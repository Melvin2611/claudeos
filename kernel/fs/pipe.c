/* Anonymous pipes */
#include <kernel.h>
#include <vfs.h>

#define PIPE_SIZE 65536

typedef struct {
    uint8_t *buf;
    size_t head, tail, count;
    int readers, writers;
    waitq_t rq, wq;
} pipe_t;

static long p_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    pipe_t *p = vn->priv;
    if (n == 0) return 0;
    uint64_t fl = irq_save();
    while (p->count == 0) {
        if (p->writers == 0) { irq_restore(fl); return 0; }
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (current->killed) { irq_restore(fl); return -EINTR; }
        wq_wait(&p->rq);
    }
    size_t m = MIN(n, p->count);
    uint8_t *b = buf;
    for (size_t i = 0; i < m; i++) {
        b[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_SIZE;
    }
    p->count -= m;
    wq_wake_all(&p->wq);
    irq_restore(fl);
    poll_notify();
    return (long)m;
}

static long p_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    pipe_t *p = vn->priv;
    const uint8_t *b = buf;
    size_t done = 0;
    uint64_t fl = irq_save();
    while (done < n) {
        if (p->readers == 0) { irq_restore(fl); return done ? (long)done : -EPIPE; }
        if (p->count == PIPE_SIZE) {
            if (f->flags & O_NONBLOCK) break;
            if (current->killed) { irq_restore(fl); return done ? (long)done : -EINTR; }
            wq_wait(&p->wq);
            continue;
        }
        while (done < n && p->count < PIPE_SIZE) {
            p->buf[p->head] = b[done++];
            p->head = (p->head + 1) % PIPE_SIZE;
            p->count++;
        }
        wq_wake_all(&p->rq);
    }
    irq_restore(fl);
    poll_notify();
    if (done == 0 && n > 0) return -EAGAIN;
    return (long)done;
}

static int p_poll(vnode_t *vn, file_t *f) {
    pipe_t *p = vn->priv;
    int m = 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        if (p->count) m |= POLLIN;
        if (!p->writers) m |= POLLIN | POLLHUP;
    } else {
        if (p->count < PIPE_SIZE) m |= POLLOUT;
        if (!p->readers) m |= POLLERR;
    }
    return m;
}

static int p_ioctl(vnode_t *vn, file_t *f, unsigned long req, void *arg) {
    pipe_t *p = vn->priv;
    if (req == FIONREAD) { *(int *)arg = (int)p->count; return 0; }
    return -ENOTTY;
}

static void p_close(vnode_t *vn, file_t *f) {
    pipe_t *p = vn->priv;
    uint64_t fl = irq_save();
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--;
    else p->writers--;
    wq_wake_all(&p->rq);
    wq_wake_all(&p->wq);
    irq_restore(fl);
    poll_notify();
}

static void p_release(vnode_t *vn) {
    pipe_t *p = vn->priv;
    vfree(p->buf);
    kfree(p);
    kfree(vn);
}

static const vnode_ops_t pipe_ops = {
    .read = p_read, .write = p_write, .poll = p_poll, .ioctl = p_ioctl, .close = p_close, .release = p_release,
};

int pipe_create(file_t **rd, file_t **wr) {
    pipe_t *p = kzalloc(sizeof(pipe_t));
    p->buf = vmalloc(PIPE_SIZE);
    if (!p->buf) { kfree(p); return -ENOMEM; }
    p->readers = 1;
    p->writers = 1;
    vnode_t *vn = vnode_alloc(FT_PIPE, &pipe_ops, p);   /* refcount 1 for rd */
    vnode_ref(vn);                                        /* +1 for wr */
    *rd = file_alloc(vn, O_RDONLY);
    *wr = file_alloc(vn, O_WRONLY);
    return 0;
}
