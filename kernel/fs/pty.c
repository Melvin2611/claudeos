/* Pseudo terminals: /dev/ptmx creates a master/slave pair with a small line discipline */
#include <kernel.h>
#include <vfs.h>
#include <proc.h>

#define PTY_BUF 16384
#define LINE_MAX_LEN 1024

typedef struct {
    uint8_t buf[PTY_BUF];
    size_t head, tail, count;
} ring_t;

typedef struct {
    ring_t in;              /* master -> slave (after line discipline) */
    ring_t out;             /* slave -> master */
    char line[LINE_MAX_LEN];
    size_t line_len;
    int mode;               /* TTY_ECHO | TTY_ICANON | TTY_ISIG */
    kwinsize_t ws;
    int fg_pid;
    int masters, slaves;
    int eof_pending;
    waitq_t in_wq, out_wq;
    vnode_t *mvn, *svn;
    int refs;
} pty_t;

static size_t ring_put(ring_t *r, const void *data, size_t n) {
    const uint8_t *d = data;
    size_t i = 0;
    for (; i < n && r->count < PTY_BUF; i++) {
        r->buf[r->head] = d[i];
        r->head = (r->head + 1) % PTY_BUF;
        r->count++;
    }
    return i;
}

static size_t ring_get(ring_t *r, void *data, size_t n) {
    uint8_t *d = data;
    size_t i = 0;
    for (; i < n && r->count; i++) {
        d[i] = r->buf[r->tail];
        r->tail = (r->tail + 1) % PTY_BUF;
        r->count--;
    }
    return i;
}

static void echo(pty_t *p, const char *s, size_t n) {
    if (p->mode & TTY_ECHO) {
        ring_put(&p->out, s, n);
        wq_wake_all(&p->out_wq);
    }
}

/* process input typed into the terminal (master write) */
static void discipline(pty_t *p, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if ((p->mode & TTY_ISIG) && c == 0x03) {
            echo(p, "^C\r\n", 4);
            p->line_len = 0;
            if (p->fg_pid > 0) proc_kill(p->fg_pid, SIGINT);
            continue;
        }
        if (!(p->mode & TTY_ICANON)) {
            ring_put(&p->in, &c, 1);
            if (c == '\r') {
                /* raw mode still maps CR to NL for convenience */
                p->in.buf[(p->in.head + PTY_BUF - 1) % PTY_BUF] = '\n';
            }
            echo(p, (const char *)&c, 1);
            continue;
        }
        if (c == '\r') c = '\n';
        if (c == 0x7F || c == 0x08) {
            if (p->line_len) {
                /* remove a whole UTF-8 character */
                do { p->line_len--; } while (p->line_len && (p->line[p->line_len] & 0xC0) == 0x80);
                echo(p, "\b \b", 3);
            }
            continue;
        }
        if (c == 0x15) {   /* ^U */
            while (p->line_len) { p->line_len--; echo(p, "\b \b", 3); }
            continue;
        }
        if (c == 0x04) {   /* ^D */
            if (p->line_len == 0) p->eof_pending++;
            else { ring_put(&p->in, p->line, p->line_len); p->line_len = 0; }
            continue;
        }
        if (c == '\n') {
            if (p->line_len < LINE_MAX_LEN) p->line[p->line_len++] = '\n';
            ring_put(&p->in, p->line, p->line_len);
            p->line_len = 0;
            echo(p, "\r\n", 2);
            continue;
        }
        if (c < 32 && c != '\t') continue;
        if (p->line_len < LINE_MAX_LEN - 1) {
            p->line[p->line_len++] = (char)c;
            echo(p, (const char *)&c, 1);
        }
    }
    wq_wake_all(&p->in_wq);
}

/* ---- master side ---- */
static long m_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    pty_t *p = vn->priv;
    uint64_t fl = irq_save();
    while (p->out.count == 0) {
        if (p->slaves == 0) { irq_restore(fl); return 0; }
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (task_interrupted(current)) { irq_restore(fl); return -EINTR; }
        wq_wait(&p->out_wq);
    }
    size_t r = ring_get(&p->out, buf, n);
    irq_restore(fl);
    poll_notify();
    return (long)r;
}

static long m_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    pty_t *p = vn->priv;
    uint64_t fl = irq_save();
    discipline(p, buf, n);
    irq_restore(fl);
    poll_notify();
    return (long)n;
}

static int m_poll(vnode_t *vn, file_t *f) {
    pty_t *p = vn->priv;
    int m = POLLOUT;
    if (p->out.count) m |= POLLIN;
    if (!p->slaves) m |= POLLHUP | POLLIN;
    return m;
}

/* ---- slave side ---- */
static bool line_ready(pty_t *p) {
    if (!(p->mode & TTY_ICANON)) return p->in.count > 0;
    /* canonical: need a newline in the queue */
    size_t idx = p->in.tail;
    for (size_t i = 0; i < p->in.count; i++) {
        if (p->in.buf[idx] == '\n') return true;
        idx = (idx + 1) % PTY_BUF;
    }
    return p->in.count >= PTY_BUF / 2;
}

static long s_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    pty_t *p = vn->priv;
    uint64_t fl = irq_save();
    for (;;) {
        if (line_ready(p) || (!(p->mode & TTY_ICANON) && p->in.count)) break;
        if (p->eof_pending) { p->eof_pending--; irq_restore(fl); return 0; }
        if (p->masters == 0) { irq_restore(fl); return 0; }
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (task_interrupted(current)) { irq_restore(fl); return -EINTR; }
        wq_wait(&p->in_wq);
    }
    size_t r = 0;
    uint8_t *b = buf;
    if (p->mode & TTY_ICANON) {
        /* return at most one line */
        while (r < n && p->in.count) {
            ring_get(&p->in, b + r, 1);
            if (b[r++] == '\n') break;
        }
    } else {
        r = ring_get(&p->in, buf, n);
    }
    irq_restore(fl);
    return (long)r;
}

static long s_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    pty_t *p = vn->priv;
    const uint8_t *b = buf;
    size_t done = 0;
    uint64_t fl = irq_save();
    while (done < n) {
        if (p->masters == 0) { irq_restore(fl); return done ? (long)done : -EPIPE; }
        if (p->out.count >= PTY_BUF - 2) {
            if (task_interrupted(current)) break;
            wq_wake_all(&p->out_wq);
            wq_wait_timeout(&p->out_wq, 50);
            continue;
        }
        uint8_t c = b[done];
        if (c == '\n') ring_put(&p->out, "\r\n", 2);
        else ring_put(&p->out, &c, 1);
        done++;
    }
    wq_wake_all(&p->out_wq);
    irq_restore(fl);
    poll_notify();
    return (long)done;
}

static int s_poll(vnode_t *vn, file_t *f) {
    pty_t *p = vn->priv;
    int m = POLLOUT;
    if (line_ready(p) || p->eof_pending) m |= POLLIN;
    if (!p->masters) m |= POLLHUP | POLLIN;
    return m;
}

static int pty_ioctl(vnode_t *vn, file_t *f, unsigned long req, void *arg) {
    pty_t *p = vn->priv;
    switch (req) {
    case TIOCGWINSZ: memcpy(arg, &p->ws, sizeof(p->ws)); return 0;
    case TIOCSWINSZ: memcpy(&p->ws, arg, sizeof(p->ws)); return 0;
    case TIOCGMODE: *(int *)arg = p->mode; return 0;
    case TIOCSMODE: {
        uint64_t fl = irq_save();
        int old = p->mode;
        p->mode = *(int *)arg;
        if ((old & TTY_ICANON) && !(p->mode & TTY_ICANON) && p->line_len) {
            ring_put(&p->in, p->line, p->line_len);
            p->line_len = 0;
        }
        irq_restore(fl);
        return 0;
    }
    case TIOCSPGRP: p->fg_pid = *(int *)arg; return 0;
    case FIONREAD: *(int *)arg = (int)(vn == p->mvn ? p->out.count : p->in.count); return 0;
    }
    return -ENOTTY;
}

static void pty_put(pty_t *p) {
    if (--p->refs == 0) kfree(p);
}

static void m_close(vnode_t *vn, file_t *f) {
    pty_t *p = vn->priv;
    uint64_t fl = irq_save();
    p->masters--;
    wq_wake_all(&p->in_wq);
    wq_wake_all(&p->out_wq);
    irq_restore(fl);
    /* hang up: terminate the foreground job */
    if (p->masters == 0 && p->fg_pid > 0) proc_kill(p->fg_pid, SIGKILL);
    poll_notify();
}

static void s_close(vnode_t *vn, file_t *f) {
    pty_t *p = vn->priv;
    uint64_t fl = irq_save();
    p->slaves--;
    wq_wake_all(&p->out_wq);
    irq_restore(fl);
    poll_notify();
}

static void m_release(vnode_t *vn) { pty_t *p = vn->priv; kfree(vn); pty_put(p); }
static void s_release(vnode_t *vn) { pty_t *p = vn->priv; kfree(vn); pty_put(p); }

static int s_open(vnode_t *vn, file_t *f) { pty_t *p = vn->priv; p->slaves++; return 0; }

static const vnode_ops_t master_ops = {
    .read = m_read, .write = m_write, .poll = m_poll, .ioctl = 0, .close = m_close, .release = m_release,
};
static const vnode_ops_t slave_ops = {
    .read = s_read, .write = s_write, .poll = s_poll, .ioctl = pty_ioctl, .close = s_close,
    .release = s_release, .open = s_open,
};

static int master_ioctl(vnode_t *vn, file_t *f, unsigned long req, void *arg) {
    pty_t *p = vn->priv;
    if (req == TIOCPTYNEW) {
        vnode_ref(p->svn);
        file_t *sf = file_alloc(p->svn, O_RDWR);
        p->slaves++;
        int fd = fd_install(current, sf);
        if (fd < 0) { file_close(sf); return fd; }
        *(int *)arg = fd;
        return 0;
    }
    return pty_ioctl(vn, f, req, arg);
}

static vnode_ops_t master_ops_full;

/* opening /dev/ptmx: replace the file's vnode with a fresh master */
static int ptmx_open(vnode_t *vn, file_t *f) {
    pty_t *p = kzalloc(sizeof(pty_t));
    p->mode = TTY_ECHO | TTY_ICANON | TTY_ISIG;
    p->ws.rows = 24;
    p->ws.cols = 80;
    p->masters = 1;
    master_ops_full = master_ops;
    master_ops_full.ioctl = master_ioctl;
    p->mvn = vnode_alloc(FT_CHR, &master_ops_full, p);
    p->svn = vnode_alloc(FT_CHR, &slave_ops, p);
    p->refs = 2;
    /* the slave vnode is only referenced by opened slave files */
    p->svn->refcount = 0;
    vnode_unref(f->vn);   /* drop /dev/ptmx reference taken by vfs_open */
    f->vn = p->mvn;
    f->flags = (f->flags & ~O_ACCMODE) | O_RDWR;
    return 0;
}

static const vnode_ops_t ptmx_ops = { .open = ptmx_open };

void pty_init(void) {
    devfs_register("ptmx", &ptmx_ops, 0);
}
