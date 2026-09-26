/* Sockets (UDP datagrams, TCP streams) and the TCP protocol engine */
#include "net.h"
#include <syscall.h>
#include <mm.h>

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define UDP_QLEN 32
#define TCP_BUF 65536
#define TCP_MSS 1400

enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING,
       TCP_LAST_ACK, TCP_TIME_WAIT };

typedef struct dgram {
    uint32_t ip;
    uint16_t port;
    uint16_t len;
    uint8_t data[];
} dgram_t;

typedef struct tcb {
    int state;
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint32_t irs, rcv_nxt;
    uint8_t *rbuf;                 /* receive ring */
    uint32_t rhead, rtail, rcount;
    uint8_t *sbuf;                 /* bytes from snd_una on (sent but unacked + unsent) */
    uint32_t slen;
    bool close_requested, fin_sent, fin_received;
    uint32_t fin_seq;
    uint64_t rto_at;
    int rto_ms, retries;
    uint64_t closed_at;
    uint32_t adv_wnd;              /* receive window we last advertised */
    int error;
    struct sock *sock;
    struct tcb *next;
} tcb_t;

typedef struct sock {
    int type;
    uint16_t lport, rport;
    uint32_t rip;
    dgram_t *q[UDP_QLEN];
    int qh, qt, qn;
    tcb_t *tcb;
    waitq_t wq;
    struct sock *next;
} sock_t;

static sock_t *socks;
static tcb_t *tcbs;
static uint16_t next_port = 49152;

static uint16_t ephemeral(void) {
    for (;;) {
        uint16_t p = next_port++;
        if (next_port < 49152) next_port = 49152;
        bool used = false;
        for (sock_t *s = socks; s; s = s->next) if (s->lport == p) used = true;
        for (tcb_t *t = tcbs; t; t = t->next) if (t->lport == p) used = true;
        if (!used) return p;
    }
}

static void wake(sock_t *s) {
    if (!s) return;
    uint64_t f = irq_save();
    wq_wake_all(&s->wq);
    irq_restore(f);
}

/* ------------------------------------------------------------------ UDP */
void dns_input(const uint8_t *p, size_t len);

int socket_udp_deliver(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *data, size_t len) {
    if (dport == 53535) { dns_input(data, len); return 0; }
    for (sock_t *s = socks; s; s = s->next) {
        if (s->type != SOCK_DGRAM || s->lport != dport) continue;
        if (s->qn >= UDP_QLEN) return -ENOSPC;
        dgram_t *d = kmalloc(sizeof(dgram_t) + len);
        d->ip = src;
        d->port = sport;
        d->len = (uint16_t)len;
        memcpy(d->data, data, len);
        s->q[s->qt] = d;
        s->qt = (s->qt + 1) % UDP_QLEN;
        s->qn++;
        wake(s);
        return 0;
    }
    return -ENOENT;
}

/* ------------------------------------------------------------------ TCP output */
static int tcp_send_seg(tcb_t *t, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len, bool mss_opt) {
    uint8_t seg[20 + 4 + TCP_MSS];
    size_t hl = mss_opt ? 24 : 20;
    memset(seg, 0, hl);
    seg[0] = (uint8_t)(t->lport >> 8); seg[1] = (uint8_t)t->lport;
    seg[2] = (uint8_t)(t->rport >> 8); seg[3] = (uint8_t)t->rport;
    uint32_t nseq = htonl(seq), nack = htonl(t->rcv_nxt);
    memcpy(seg + 4, &nseq, 4);
    memcpy(seg + 8, &nack, 4);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    uint32_t win = TCP_BUF - (t->rbuf ? t->rcount : 0);
    if (win > 65535) win = 65535;
    t->adv_wnd = win;
    seg[14] =(uint8_t)(win >> 8); seg[15] = (uint8_t)win;
    if (mss_opt) { seg[20] = 2; seg[21] = 4; seg[22] = TCP_MSS >> 8; seg[23] = TCP_MSS & 0xFF; }
    if (len) memcpy(seg + hl, data, len);
    /* checksum over the pseudo header */
    uint8_t pseudo[12];
    memcpy(pseudo, &netif.ip, 4);
    memcpy(pseudo + 4, &t->rip, 4);
    pseudo[8] = 0; pseudo[9] = 6;
    uint16_t tl = (uint16_t)(hl + len);
    pseudo[10] = (uint8_t)(tl >> 8); pseudo[11] = (uint8_t)tl;
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (pseudo[i] << 8) | pseudo[i + 1];
    uint16_t cs = ip_checksum(seg, hl + len, sum);
    memcpy(seg + 16, &cs, 2);
    return ip_send(t->rip, 6, seg, hl + len);
}

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

static void arm_rto(tcb_t *t) { t->rto_at = uptime_ms() + t->rto_ms; }

static void tcp_output(tcb_t *t) {
    if (t->state == TCP_SYN_SENT || t->state == TCP_CLOSED) return;
    uint32_t in_flight = t->snd_nxt - t->snd_una;
    if (t->fin_sent && in_flight) in_flight--;   /* the FIN occupies one sequence number */
    while (in_flight < t->slen) {
        uint32_t wnd = t->snd_wnd ? t->snd_wnd : 1;
        if (in_flight >= wnd) break;
        uint32_t n = MIN(t->slen - in_flight, MIN((uint32_t)TCP_MSS, wnd - in_flight));
        tcp_send_seg(t, F_ACK | F_PSH, t->snd_una + in_flight, t->sbuf + in_flight, n, false);
        in_flight += n;
        t->snd_nxt = t->snd_una + in_flight;
        if (!t->rto_at) arm_rto(t);
    }
    if (t->close_requested && !t->fin_sent && in_flight == t->slen) {
        t->fin_seq = t->snd_una + t->slen;
        tcp_send_seg(t, F_FIN | F_ACK, t->fin_seq, 0, 0, false);
        t->fin_sent = true;
        t->snd_nxt = t->fin_seq + 1;
        if (t->state == TCP_ESTABLISHED) t->state = TCP_FIN_WAIT_1;
        else if (t->state == TCP_CLOSE_WAIT) t->state = TCP_LAST_ACK;
        arm_rto(t);
    }
}

static void tcb_free(tcb_t *t) {
    for (tcb_t **pp = &tcbs; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    if (t->sock) t->sock->tcb = 0;
    vfree(t->rbuf);
    vfree(t->sbuf);
    kfree(t);
}

static void tcp_set_closed(tcb_t *t, int err) {
    t->state = TCP_CLOSED;
    if (err) t->error = err;
    t->closed_at = uptime_ms();
    wake(t->sock);
}

/* ------------------------------------------------------------------ TCP input */
static void send_rst(uint32_t src, uint16_t sport, uint16_t dport, uint32_t ack, uint32_t seq, bool has_ack) {
    tcb_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.rip = src;
    tmp.lport = dport;
    tmp.rport = sport;
    tmp.rcv_nxt = ack;
    tcp_send_seg(&tmp, has_ack ? F_RST : (F_RST | F_ACK), has_ack ? seq : 0, 0, 0, false);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    if (len < 20) return;
    uint16_t sport = (seg[0] << 8) | seg[1], dport = (seg[2] << 8) | seg[3];
    uint32_t seq, ack;
    memcpy(&seq, seg + 4, 4);
    memcpy(&ack, seg + 8, 4);
    seq = ntohl(seq);
    ack = ntohl(ack);
    size_t hl = (seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint32_t wnd = (seg[14] << 8) | seg[15];
    if (hl < 20 || hl > len) return;
    const uint8_t *data = seg + hl;
    uint32_t dlen = (uint32_t)(len - hl);
    UNUSED(dst);
    tcb_t *t = 0;
    for (tcb_t *x = tcbs; x; x = x->next)
        if (x->rip == src && x->rport == sport && x->lport == dport && x->state != TCP_CLOSED) { t = x; break; }
    if (!t) {
        if (!(flags & F_RST)) send_rst(src, sport, dport, seq + dlen + ((flags & (F_SYN | F_FIN)) ? 1 : 0), ack, flags & F_ACK);
        return;
    }
    if (flags & F_RST) {
        tcp_set_closed(t, t->state == TCP_SYN_SENT ? ECONNREFUSED : ECONNRESET);
        return;
    }
    if (t->state == TCP_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == t->iss + 1) {
            t->irs = seq;
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_nxt = ack;
            t->snd_wnd = wnd;
            t->state = TCP_ESTABLISHED;
            t->rto_at = 0;
            t->retries = 0;
            t->rto_ms = 1000;
            tcp_send_seg(t, F_ACK, t->snd_nxt, 0, 0, false);
            wake(t->sock);
            tcp_output(t);
        }
        return;
    }
    /* ACK processing */
    if (flags & F_ACK) {
        int32_t acked = (int32_t)(ack - t->snd_una);
        int32_t outstanding = (int32_t)(t->snd_nxt - t->snd_una);
        if (acked > 0 && acked <= outstanding) {
            uint32_t data_acked = (uint32_t)acked;
            if (t->fin_sent && ack == t->fin_seq + 1) data_acked--;
            data_acked = MIN(data_acked, t->slen);
            memmove(t->sbuf, t->sbuf + data_acked, t->slen - data_acked);
            t->slen -= data_acked;
            t->snd_una = ack;
            t->retries = 0;
            t->rto_ms = 1000;
            t->rto_at = t->snd_una != t->snd_nxt ? uptime_ms() + t->rto_ms : 0;
            wake(t->sock);
            if (t->fin_sent && ack == t->fin_seq + 1) {
                if (t->state == TCP_FIN_WAIT_1) t->state = TCP_FIN_WAIT_2;
                else if (t->state == TCP_CLOSING) { t->state = TCP_TIME_WAIT; t->closed_at = uptime_ms(); }
                else if (t->state == TCP_LAST_ACK) tcp_set_closed(t, 0);
            }
        }
        t->snd_wnd = wnd;
    }
    /* data */
    bool need_ack = false;
    if (dlen) {
        /* drop the part we already have (retransmissions that overlap new data) */
        int32_t dup = (int32_t)(t->rcv_nxt - seq);
        if (dup > 0 && (uint32_t)dup < dlen) {
            data += dup;
            dlen -= (uint32_t)dup;
            seq = t->rcv_nxt;
        }
        if (seq == t->rcv_nxt && t->state != TCP_CLOSE_WAIT && t->state != TCP_LAST_ACK) {
            uint32_t space = TCP_BUF - t->rcount;
            uint32_t n = MIN(dlen, space);
            for (uint32_t i = 0; i < n; i++) {
                t->rbuf[t->rhead] = data[i];
                t->rhead = (t->rhead + 1) % TCP_BUF;
            }
            t->rcount += n;
            t->rcv_nxt += n;
            if (n) wake(t->sock);
        } else {
            static int ooo;
            if (++ooo % 50 == 1) klog("[tcp] out of order: seq %u expected %u len %u (%d)\n", seq, t->rcv_nxt, dlen, ooo);
        }
        need_ack = true;
    }
    if ((flags & F_FIN) && seq + dlen == t->rcv_nxt && !t->fin_received) {
        t->rcv_nxt++;
        t->fin_received = true;
        need_ack = true;
        if (t->state == TCP_ESTABLISHED) t->state = TCP_CLOSE_WAIT;
        else if (t->state == TCP_FIN_WAIT_1) t->state = TCP_CLOSING;
        else if (t->state == TCP_FIN_WAIT_2) { t->state = TCP_TIME_WAIT; t->closed_at = uptime_ms(); }
        wake(t->sock);
    }
    if (need_ack) tcp_send_seg(t, F_ACK, t->snd_nxt, 0, 0, false);
    tcp_output(t);
}

void tcp_timer(void) {
    uint64_t now = uptime_ms();
    for (tcb_t *t = tcbs, *n; t; t = n) {
        n = t->next;
        if ((t->state == TCP_TIME_WAIT && now - t->closed_at > 2000) ||
            (t->state == TCP_CLOSED && !t->sock && now - t->closed_at > 100)) {
            tcb_free(t);
            continue;
        }
        if (!t->rto_at || now < t->rto_at) continue;
        if (++t->retries > 8) {
            tcp_set_closed(t, ETIMEDOUT);
            continue;
        }
        t->rto_ms = MIN(t->rto_ms * 2, 8000);
        if (t->state == TCP_SYN_SENT) {
            tcp_send_seg(t, F_SYN, t->iss, 0, 0, true);
        } else {
            /* go back N: resend from the oldest unacknowledged byte */
            t->snd_nxt = t->snd_una;
            bool fin = t->fin_sent;
            t->fin_sent = false;
            if (fin && t->state == TCP_FIN_WAIT_1) t->state = TCP_ESTABLISHED;
            if (fin && t->state == TCP_LAST_ACK) t->state = TCP_CLOSE_WAIT;
            if (fin && t->state == TCP_CLOSING) t->state = TCP_CLOSE_WAIT;
            tcp_output(t);
        }
        arm_rto(t);
    }
}

/* ------------------------------------------------------------------ socket file operations */
static long s_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off);
static long s_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off);

static int s_poll(vnode_t *vn, file_t *f) {
    sock_t *s = vn->priv;
    int m = 0;
    mutex_lock(&net_lock);
    if (s->type == SOCK_DGRAM) {
        if (s->qn) m |= POLLIN;
        m |= POLLOUT;
    } else if (s->tcb) {
        tcb_t *t = s->tcb;
        if (t->rcount || t->fin_received || t->state == TCP_CLOSED) m |= POLLIN;
        if (t->state == TCP_ESTABLISHED && t->slen < TCP_BUF) m |= POLLOUT;
        if (t->state == TCP_CLOSED) m |= POLLHUP;
    }
    mutex_unlock(&net_lock);
    return m;
}

static int s_ioctl(vnode_t *vn, file_t *f, unsigned long req, void *arg) {
    sock_t *s = vn->priv;
    if (req == FIONREAD) {
        mutex_lock(&net_lock);
        *(int *)arg = s->type == SOCK_DGRAM ? (s->qn ? s->q[s->qh]->len : 0) : (s->tcb ? (int)s->tcb->rcount : 0);
        mutex_unlock(&net_lock);
        return 0;
    }
    return -ENOTTY;
}

static void s_release(vnode_t *vn) {
    sock_t *s = vn->priv;
    mutex_lock(&net_lock);
    for (sock_t **pp = &socks; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
    while (s->qn) { kfree(s->q[s->qh]); s->qh = (s->qh + 1) % UDP_QLEN; s->qn--; }
    if (s->tcb) {
        tcb_t *t = s->tcb;
        t->sock = 0;
        if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
            t->close_requested = true;
            tcp_output(t);
        } else if (t->state == TCP_SYN_SENT) {
            tcp_set_closed(t, 0);
        }
        if (t->state == TCP_CLOSED) t->closed_at = uptime_ms();
    }
    mutex_unlock(&net_lock);
    kfree(s);
    kfree(vn);
}

static const vnode_ops_t sock_ops = { .read = s_read, .write = s_write, .poll = s_poll, .ioctl = s_ioctl, .release = s_release };

static sock_t *sock_of(int fd, file_t **fout) {
    file_t *f = fd_get(current, fd);
    if (!f || f->vn->type != FT_SOCK) return 0;
    if (fout) *fout = f;
    return f->vn->priv;
}

/* wait for a condition with net_lock held; returns false on timeout/kill */
static bool wait_on(sock_t *s, uint64_t deadline) {
    mutex_unlock(&net_lock);
    uint64_t f = irq_save();
    uint64_t now = uptime_ms();
    if (now < deadline) wq_wait_timeout(&s->wq, MIN(deadline - now, 200));
    irq_restore(f);
    mutex_lock(&net_lock);
    return !task_interrupted(current) && uptime_ms() < deadline;
}

static long tcp_recv(sock_t *s, file_t *f, void *buf, size_t n, uint64_t timeout) {
    uint64_t deadline = timeout ? uptime_ms() + timeout : ~0ULL;
    mutex_lock(&net_lock);
    tcb_t *t = s->tcb;
    while (t && !t->rcount && !t->fin_received && t->state != TCP_CLOSED) {
        if (f && (f->flags & O_NONBLOCK)) { mutex_unlock(&net_lock); return -EAGAIN; }
        if (!wait_on(s, deadline)) { mutex_unlock(&net_lock); return task_interrupted(current) ? -EINTR : -ETIMEDOUT; }
        t = s->tcb;
    }
    if (!t) { mutex_unlock(&net_lock); return -ENOTCONN; }
    if (!t->rcount) {
        int err = t->error;
        mutex_unlock(&net_lock);
        return err && err != ECONNRESET ? -err : 0;   /* EOF */
    }
    uint8_t *b = buf;
    size_t m = MIN(n, t->rcount);
    for (size_t i = 0; i < m; i++) {
        b[i] = t->rbuf[t->rtail];
        t->rtail = (t->rtail + 1) % TCP_BUF;
    }
    t->rcount -= (uint32_t)m;
    /* window update once the window has grown noticeably since the last advertisement */
    uint32_t now_wnd = MIN((uint32_t)65535, TCP_BUF - t->rcount);
    if (t->state != TCP_CLOSED && now_wnd >= t->adv_wnd + 2 * TCP_MSS)
        tcp_send_seg(t, F_ACK, t->snd_nxt, 0, 0, false);
    mutex_unlock(&net_lock);
    return (long)m;
}

static long tcp_send(sock_t *s, file_t *f, const void *buf, size_t n) {
    const uint8_t *b = buf;
    size_t done = 0;
    mutex_lock(&net_lock);
    while (done < n) {
        tcb_t *t = s->tcb;
        if (!t) { mutex_unlock(&net_lock); return done ? (long)done : -ENOTCONN; }
        if (t->state == TCP_CLOSED) {
            int err = t->error ? t->error : EPIPE;
            mutex_unlock(&net_lock);
            return done ? (long)done : -err;
        }
        if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) { mutex_unlock(&net_lock); return -ENOTCONN; }
        uint32_t space = TCP_BUF - t->slen;
        if (!space) {
            if (f && (f->flags & O_NONBLOCK)) break;
            if (!wait_on(s, uptime_ms() + 30000)) { mutex_unlock(&net_lock); return done ? (long)done : -ETIMEDOUT; }
            continue;
        }
        uint32_t k = (uint32_t)MIN((size_t)space, n - done);
        memcpy(t->sbuf + t->slen, b + done, k);
        t->slen += k;
        done += k;
        tcp_output(t);
    }
    mutex_unlock(&net_lock);
    return done ? (long)done : -EAGAIN;
}

static long s_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    sock_t *s = vn->priv;
    if (s->type == SOCK_STREAM) return tcp_recv(s, f, buf, n, 0);
    /* UDP: one datagram */
    mutex_lock(&net_lock);
    while (!s->qn) {
        if (f->flags & O_NONBLOCK) { mutex_unlock(&net_lock); return -EAGAIN; }
        if (!wait_on(s, ~0ULL)) { mutex_unlock(&net_lock); return -EINTR; }
    }
    dgram_t *d = s->q[s->qh];
    s->qh = (s->qh + 1) % UDP_QLEN;
    s->qn--;
    mutex_unlock(&net_lock);
    size_t m = MIN(n, (size_t)d->len);
    memcpy(buf, d->data, m);
    kfree(d);
    return (long)m;
}

static long s_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    sock_t *s = vn->priv;
    if (s->type == SOCK_STREAM) return tcp_send(s, f, buf, n);
    if (!s->rip) return -ENOTCONN;
    mutex_lock(&net_lock);
    if (!s->lport) s->lport = ephemeral();
    int r = udp_send(s->rip, s->lport, s->rport, buf, n);
    mutex_unlock(&net_lock);
    return r < 0 ? r : (long)n;
}

/* ------------------------------------------------------------------ syscalls */
SYSCALL_DEF(sys_socket) {
    SYSCALL_UNUSED_ARGS;
    int type = (int)a2;
    if (a1 != 2) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EPROTONOSUPPORT;
    if (!netif.present) return -ENETDOWN;
    sock_t *s = kzalloc(sizeof(sock_t));
    s->type = type;
    vnode_t *vn = vnode_alloc(FT_SOCK, &sock_ops, s);
    file_t *f = file_alloc(vn, O_RDWR);
    int fd = fd_install(current, f);
    if (fd < 0) { file_close(f); return fd; }
    mutex_lock(&net_lock);
    s->next = socks;
    socks = s;
    mutex_unlock(&net_lock);
    return fd;
}

/* connect(fd, ip, port, timeout_ms) */
SYSCALL_DEF(sys_connect) {
    SYSCALL_UNUSED_ARGS;
    sock_t *s = sock_of((int)a1, 0);
    if (!s) return -ENOTSOCK;
    if (!netif.up) return -ENETDOWN;
    uint32_t ip = (uint32_t)a2;
    uint16_t port = (uint16_t)a3;
    mutex_lock(&net_lock);
    s->rip = ip;
    s->rport = port;
    if (s->type == SOCK_DGRAM) {
        if (!s->lport) s->lport = ephemeral();
        mutex_unlock(&net_lock);
        return 0;
    }
    if (s->tcb) { mutex_unlock(&net_lock); return -EISCONN; }
    tcb_t *t = kzalloc(sizeof(tcb_t));
    t->rbuf = vmalloc(TCP_BUF);
    t->sbuf = vmalloc(TCP_BUF);
    t->rip = ip;
    t->rport = port;
    t->lport = s->lport ? s->lport : ephemeral();
    s->lport = t->lport;
    t->iss = (uint32_t)rdtsc();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->rto_ms = 1000;
    t->state = TCP_SYN_SENT;
    t->sock = s;
    s->tcb = t;
    t->next = tcbs;
    tcbs = t;
    int r = tcp_send_seg(t, F_SYN, t->iss, 0, 0, true);
    arm_rto(t);
    if (r < 0) { tcp_set_closed(t, -r); mutex_unlock(&net_lock); return r; }
    uint64_t deadline = uptime_ms() + (a4 ? a4 : 15000);
    while (t->state == TCP_SYN_SENT) {
        if (!wait_on(s, deadline)) {
            if (t->state == TCP_SYN_SENT) tcp_set_closed(t, ETIMEDOUT);
            break;
        }
        t = s->tcb;
        if (!t) break;
    }
    int result = !t ? -ECONNRESET : t->state == TCP_ESTABLISHED ? 0 : -(t->error ? t->error : ETIMEDOUT);
    mutex_unlock(&net_lock);
    return result;
}

SYSCALL_DEF(sys_bind) {
    SYSCALL_UNUSED_ARGS;
    sock_t *s = sock_of((int)a1, 0);
    if (!s) return -ENOTSOCK;
    mutex_lock(&net_lock);
    for (sock_t *o = socks; o; o = o->next)
        if (o != s && o->type == s->type && o->lport == (uint16_t)a2) { mutex_unlock(&net_lock); return -EADDRINUSE; }
    s->lport = (uint16_t)a2;
    mutex_unlock(&net_lock);
    return 0;
}

/* sendto(fd, buf, len, ip, port) */
SYSCALL_DEF(sys_sendto) {
    SYSCALL_UNUSED_ARGS;
    file_t *f;
    sock_t *s = sock_of((int)a1, &f);
    if (!s) return -ENOTSOCK;
    if (!user_range_ok((void *)a2, a3, false)) return -EFAULT;
    if (s->type == SOCK_STREAM) return tcp_send(s, f, (void *)a2, a3);
    if (a3 > ETH_MTU - 28) return -EINVAL;
    uint8_t buf[ETH_MTU];
    memcpy(buf, (void *)a2, a3);
    mutex_lock(&net_lock);
    if (!s->lport) s->lport = ephemeral();
    uint32_t ip = a4 ? (uint32_t)a4 : s->rip;
    uint16_t port = a5 ? (uint16_t)a5 : s->rport;
    int r = udp_send(ip, s->lport, port, buf, a3);
    mutex_unlock(&net_lock);
    return r < 0 ? r : (long)a3;
}

/* recvfrom(fd, buf, len, uint32_t from[2] (ip, port) or NULL, timeout_ms) */
SYSCALL_DEF(sys_recvfrom) {
    SYSCALL_UNUSED_ARGS;
    file_t *f;
    sock_t *s = sock_of((int)a1, &f);
    if (!s) return -ENOTSOCK;
    if (!user_range_ok((void *)a2, a3, true)) return -EFAULT;
    if (a4 && !user_range_ok((void *)a4, 8, true)) return -EFAULT;
    if (s->type == SOCK_STREAM) return tcp_recv(s, f, (void *)a2, a3, a5);
    uint64_t deadline = a5 ? uptime_ms() + a5 : ~0ULL;
    mutex_lock(&net_lock);
    while (!s->qn) {
        if (f->flags & O_NONBLOCK) { mutex_unlock(&net_lock); return -EAGAIN; }
        if (!wait_on(s, deadline)) { mutex_unlock(&net_lock); return task_interrupted(current) ? -EINTR : -ETIMEDOUT; }
    }
    dgram_t *d = s->q[s->qh];
    s->qh = (s->qh + 1) % UDP_QLEN;
    s->qn--;
    mutex_unlock(&net_lock);
    size_t m = MIN((size_t)a3, (size_t)d->len);
    memcpy((void *)a2, d->data, m);
    if (a4) { ((uint32_t *)a4)[0] = d->ip; ((uint32_t *)a4)[1] = d->port; }
    kfree(d);
    return (long)m;
}

void socket_init(void) {
    syscall_register(SYS_SOCKET, sys_socket);
    syscall_register(SYS_CONNECT, sys_connect);
    syscall_register(SYS_BIND, sys_bind);
    syscall_register(SYS_SENDTO, sys_sendto);
    syscall_register(SYS_RECVFROM, sys_recvfrom);
}
