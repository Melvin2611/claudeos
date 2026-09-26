/* Linux binary compatibility: the x86-64 Linux system call ABI for statically linked
 * programs (glibc, musl, BusyBox, ...). Calls are translated onto the ClaudeOS kernel;
 * signals are delivered with real Linux rt_sigframes. */
#include <kernel.h>
#include <syscall.h>
#include <proc.h>
#include <mm.h>
#include <cpu.h>
#include "drivers.h"

bool gui_event_pending(task_t *t);

/* Linux errno values match ours; syscall numbers: */
enum {
    L_READ = 0, L_WRITE, L_OPEN, L_CLOSE, L_STAT, L_FSTAT, L_LSTAT, L_POLL, L_LSEEK, L_MMAP, L_MPROTECT,
    L_MUNMAP, L_BRK, L_RT_SIGACTION, L_RT_SIGPROCMASK, L_RT_SIGRETURN, L_IOCTL, L_PREAD64, L_PWRITE64,
    L_READV, L_WRITEV, L_ACCESS, L_PIPE, L_SELECT, L_SCHED_YIELD, L_MREMAP, L_MSYNC, L_MINCORE, L_MADVISE,
    L_DUP = 32, L_DUP2 = 33, L_PAUSE = 34, L_NANOSLEEP = 35, L_GETITIMER = 36, L_ALARM = 37,
    L_SETITIMER = 38, L_GETPID = 39, L_SENDFILE = 40, L_SOCKET = 41, L_CONNECT = 42, L_ACCEPT = 43,
    L_SENDTO = 44, L_RECVFROM = 45, L_SENDMSG = 46, L_RECVMSG = 47, L_SHUTDOWN = 48, L_BIND = 49,
    L_LISTEN = 50, L_GETSOCKNAME = 51, L_GETPEERNAME = 52, L_SOCKETPAIR = 53, L_SETSOCKOPT = 54,
    L_GETSOCKOPT = 55, L_CLONE = 56, L_FORK = 57, L_VFORK = 58, L_EXECVE = 59, L_EXIT = 60, L_WAIT4 = 61,
    L_KILL = 62, L_UNAME = 63, L_FCNTL = 72, L_FLOCK = 73, L_FSYNC = 74, L_FDATASYNC = 75,
    L_TRUNCATE = 76, L_FTRUNCATE = 77, L_GETDENTS = 78, L_GETCWD = 79, L_CHDIR = 80, L_FCHDIR = 81,
    L_RENAME = 82, L_MKDIR = 83, L_RMDIR = 84, L_CREAT = 85, L_LINK = 86, L_UNLINK = 87, L_SYMLINK = 88,
    L_READLINK = 89, L_CHMOD = 90, L_FCHMOD = 91, L_CHOWN = 92, L_FCHOWN = 93, L_LCHOWN = 94, L_UMASK = 95,
    L_GETTIMEOFDAY = 96, L_GETRLIMIT = 97, L_GETRUSAGE = 98, L_SYSINFO = 99, L_TIMES = 100,
    L_GETUID = 102, L_GETGID = 104, L_SETUID = 105, L_SETGID = 106, L_GETEUID = 107, L_GETEGID = 108,
    L_SETPGID = 109, L_GETPPID = 110, L_GETPGRP = 111, L_SETSID = 112, L_GETGROUPS = 115,
    L_GETPGID = 121, L_GETSID = 124, L_SIGALTSTACK = 131, L_UTIME = 132, L_STATFS = 137, L_FSTATFS = 138,
    L_PRCTL = 157, L_ARCH_PRCTL = 158, L_SETRLIMIT = 160, L_SYNC = 162, L_GETTID = 186, L_TKILL = 200,
    L_TIME = 201, L_FUTEX = 202, L_SCHED_GETAFFINITY = 204, L_GETDENTS64 = 217, L_SET_TID_ADDRESS = 218,
    L_FADVISE64 = 221, L_CLOCK_GETTIME = 228, L_CLOCK_GETRES = 229, L_CLOCK_NANOSLEEP = 230,
    L_EXIT_GROUP = 231, L_TGKILL = 234, L_UTIMES = 235, L_WAITID = 247, L_OPENAT = 257, L_MKDIRAT = 258,
    L_FCHOWNAT = 260, L_NEWFSTATAT = 262, L_UNLINKAT = 263, L_RENAMEAT = 264, L_READLINKAT = 267,
    L_FCHMODAT = 268, L_FACCESSAT = 269, L_PSELECT6 = 270, L_PPOLL = 271, L_SET_ROBUST_LIST = 273,
    L_UTIMENSAT = 280, L_ACCEPT4 = 288, L_DUP3 = 292, L_PIPE2 = 293, L_PRLIMIT64 = 302,
    L_RENAMEAT2 = 316, L_GETRANDOM = 318, L_STATX = 332, L_RSEQ = 334, L_CLONE3 = 435,
    L_FACCESSAT2 = 439,
};

#define AT_FDCWD (-100)
#define AT_EMPTY_PATH 0x1000
#define AT_REMOVEDIR 0x200

static inline long call(int nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    syscall_fn fn = syscall_get(nr);
    return fn ? fn(a, b, c, d, e, 0) : -ENOSYS;
}

static bool uok(uint64_t p, size_t n, bool w) { return user_range_ok((void *)p, n, w); }

/* resolve a path argument relative to a directory fd (only AT_FDCWD or absolute paths) */
static int at_path(int dirfd, uint64_t upath, char *out) {
    long r = strncpy_from_user(out, (const char *)upath, PATH_MAX_LEN);
    if (r < 0) return (int)r;
    if (out[0] != '/' && dirfd != AT_FDCWD) return -ENOSYS;
    return 0;
}

/* ------------------------------------------------------------------ stat */

static uint32_t mode_of(const kstat_t *st, const char *path) {
    uint32_t perm = st->mode & 07777;
    switch (st->type) {
    case FT_DIR: return 040000 | (perm ? perm : 0755);
    case FT_CHR: return 020000 | (perm ? perm : 0666);
    case FT_PIPE: return 010000 | 0600;
    case FT_SOCK: return 0140000 | 0777;
    default: {
        bool exec = path && (!strncmp(path, "/bin/", 5) || !strncmp(path, "/usr/bin/", 9));
        return 0100000 | (perm ? perm : (exec ? 0755 : 0644));
    }
    }
}

static int put_stat(uint64_t ubuf, const kstat_t *st, const char *path) {
    uint8_t s[144];
    memset(s, 0, sizeof(s));
    *(uint64_t *)(s + 0) = st->dev + 1;
    *(uint64_t *)(s + 8) = st->ino;
    *(uint64_t *)(s + 16) = st->type == FT_DIR ? 2 : 1;
    *(uint32_t *)(s + 24) = mode_of(st, path);
    *(uint32_t *)(s + 28) = 1000;
    *(uint32_t *)(s + 32) = 1000;
    *(int64_t *)(s + 48) = (int64_t)st->size;
    *(int64_t *)(s + 56) = 4096;
    *(int64_t *)(s + 64) = (int64_t)((st->size + 511) / 512);
    *(uint64_t *)(s + 72) = st->mtime;
    *(uint64_t *)(s + 88) = st->mtime;
    *(uint64_t *)(s + 104) = st->ctime ? st->ctime : st->mtime;
    return copy_to_user((void *)ubuf, s, sizeof(s));
}

static int put_statx(uint64_t ubuf, const kstat_t *st, const char *path) {
    uint8_t s[256];
    memset(s, 0, sizeof(s));
    *(uint32_t *)(s + 0) = 0x7FF;                      /* STATX_BASIC_STATS */
    *(uint32_t *)(s + 4) = 4096;
    *(uint32_t *)(s + 16) = st->type == FT_DIR ? 2 : 1;
    *(uint32_t *)(s + 20) = 1000;
    *(uint32_t *)(s + 24) = 1000;
    *(uint16_t *)(s + 28) = (uint16_t)mode_of(st, path);
    *(uint64_t *)(s + 32) = st->ino;
    *(uint64_t *)(s + 40) = st->size;
    *(uint64_t *)(s + 48) = (st->size + 511) / 512;
    *(int64_t *)(s + 64) = (int64_t)st->mtime;          /* atime */
    *(int64_t *)(s + 80) = (int64_t)st->ctime;          /* btime */
    *(int64_t *)(s + 96) = (int64_t)(st->ctime ? st->ctime : st->mtime);
    *(int64_t *)(s + 112) = (int64_t)st->mtime;
    *(uint32_t *)(s + 136) = 8;
    *(uint32_t *)(s + 140) = st->dev + 1;
    return copy_to_user((void *)ubuf, s, sizeof(s));
}

static long do_stat_path(int dirfd, uint64_t upath, uint64_t ubuf, int flags, bool statx) {
    char path[PATH_MAX_LEN];
    kstat_t st;
    if (flags & AT_EMPTY_PATH) {
        char c = 0;
        if (upath) copy_from_user(&c, (void *)upath, 1);
        if (!c) {
            file_t *f = fd_get(current, dirfd);
            if (!f) return -EBADF;
            int r = file_stat(f, &st);
            if (r < 0) return r;
            return statx ? put_statx(ubuf, &st, 0) : put_stat(ubuf, &st, 0);
        }
    }
    int r = at_path(dirfd, upath, path);
    if (r) return r;
    char abs[PATH_MAX_LEN];
    vfs_normalize(PROC(current)->cwd, path, abs);
    if ((r = vfs_stat(path, &st)) < 0) return r;
    return statx ? put_statx(ubuf, &st, abs) : put_stat(ubuf, &st, abs);
}

/* ------------------------------------------------------------------ directories */

static long do_getdents64(int fd, uint64_t ubuf, size_t count) {
    file_t *f = fd_get(current, fd);
    if (!f) return -EBADF;
    if (!uok(ubuf, count, true)) return -EFAULT;
    uint8_t *out = (uint8_t *)ubuf;
    size_t pos = 0;
    kdirent_t e;
    for (;;) {
        uint64_t save = f->off;
        int r = file_readdir(f, &e);
        if (r < 0) return pos ? (long)pos : r;
        if (r == 0) break;
        size_t nl = strlen(e.name);
        size_t reclen = ALIGN_UP(19 + nl + 1, 8);
        if (pos + reclen > count) { f->off = save; if (!pos) return -EINVAL; break; }
        uint8_t *d = out + pos;
        memset(d, 0, reclen);
        *(uint64_t *)d = e.ino ? e.ino : 1;
        *(int64_t *)(d + 8) = (int64_t)f->off;
        *(uint16_t *)(d + 16) = (uint16_t)reclen;
        d[18] = e.type == FT_DIR ? 4 : e.type == FT_CHR ? 2 : e.type == FT_PIPE ? 1 : 8;
        memcpy(d + 19, e.name, nl + 1);
        pos += reclen;
    }
    return (long)pos;
}

/* ------------------------------------------------------------------ terminal ioctls */

#define L_TCGETS 0x5401
#define L_TCSETS 0x5402
#define L_TCSETSW 0x5403
#define L_TCSETSF 0x5404
#define L_TIOCGPGRP 0x540F
#define L_TIOCSPGRP 0x5410
#define L_TIOCGWINSZ 0x5413
#define L_TIOCSWINSZ 0x5414
#define L_FIONREAD 0x541B
#define L_FIONBIO 0x5421

static long do_ioctl(int fd, unsigned long req, uint64_t arg) {
    file_t *f = fd_get(current, fd);
    if (!f) return -EBADF;
    int mode;
    switch (req) {
    case L_TCGETS: {
        if (file_ioctl(f, TIOCGMODE, &mode) < 0) return -ENOTTY;
        uint8_t t[36];
        memset(t, 0, sizeof(t));
        *(uint32_t *)(t + 0) = 0x100 | 0x400;             /* ICRNL | IXON */
        *(uint32_t *)(t + 4) = 0x1 | 0x4;                 /* OPOST | ONLCR */
        *(uint32_t *)(t + 8) = 0xBF;                      /* B38400 | CS8 | CREAD */
        uint32_t lflag = 0x8000 | 0x10 | 0x20;            /* IEXTEN ECHOE ECHOK */
        if (mode & TTY_ISIG) lflag |= 0x1;
        if (mode & TTY_ICANON) lflag |= 0x2;
        if (mode & TTY_ECHO) lflag |= 0x8;
        *(uint32_t *)(t + 12) = lflag;
        uint8_t *cc = t + 17;
        cc[0] = 3; cc[1] = 28; cc[2] = 127; cc[3] = 21; cc[4] = 4; cc[5] = 0; cc[6] = 1; cc[8] = 17; cc[9] = 19;
        cc[10] = 26;
        return copy_to_user((void *)arg, t, sizeof(t)) < 0 ? -EFAULT : 0;
    }
    case L_TCSETS: case L_TCSETSW: case L_TCSETSF: {
        uint8_t t[36];
        if (copy_from_user(t, (void *)arg, sizeof(t)) < 0) return -EFAULT;
        uint32_t lflag = *(uint32_t *)(t + 12);
        mode = ((lflag & 0x1) ? TTY_ISIG : 0) | ((lflag & 0x2) ? TTY_ICANON : 0) | ((lflag & 0x8) ? TTY_ECHO : 0);
        return file_ioctl(f, TIOCSMODE, &mode) < 0 ? -ENOTTY : 0;
    }
    case L_TIOCGWINSZ: case L_TIOCSWINSZ: {
        kwinsize_t ws;
        if (req == L_TIOCSWINSZ && copy_from_user(&ws, (void *)arg, sizeof(ws)) < 0) return -EFAULT;
        int r = file_ioctl(f, req, &ws);
        if (r < 0) return -ENOTTY;
        if (req == L_TIOCGWINSZ && copy_to_user((void *)arg, &ws, sizeof(ws)) < 0) return -EFAULT;
        return 0;
    }
    case L_TIOCGPGRP: {
        if (file_ioctl(f, TIOCGMODE, &mode) < 0) return -ENOTTY;
        int pg = PROC(current)->pid;
        return copy_to_user((void *)arg, &pg, 4) < 0 ? -EFAULT : 0;
    }
    case L_TIOCSPGRP: {
        int pg;
        if (copy_from_user(&pg, (void *)arg, 4) < 0) return -EFAULT;
        return file_ioctl(f, TIOCSPGRP, &pg) < 0 ? -ENOTTY : 0;
    }
    case L_FIONREAD: {
        int n = 0;
        if (file_ioctl(f, FIONREAD, &n) < 0) return -ENOTTY;
        return copy_to_user((void *)arg, &n, 4) < 0 ? -EFAULT : 0;
    }
    case L_FIONBIO: {
        int on = 0;
        if (copy_from_user(&on, (void *)arg, 4) < 0) return -EFAULT;
        if (on) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK;
        return 0;
    }
    }
    return -ENOTTY;
}

/* ------------------------------------------------------------------ vectors, time */

static long do_iov(int fd, uint64_t uiov, int cnt, bool write) {
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    if (!uok(uiov, (size_t)cnt * 16, false)) return -EFAULT;
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t base = ((uint64_t *)uiov)[i * 2], len = ((uint64_t *)uiov)[i * 2 + 1];
        if (!len) continue;
        long r = call(write ? SYS_WRITE : SYS_READ, fd, base, len, 0, 0);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < len) break;
    }
    return total;
}

static void realtime(uint64_t *sec, uint64_t *nsec) {
    static uint64_t base_sec, base_ms;
    if (!base_sec) { base_sec = time_now(); base_ms = uptime_ms(); }
    uint64_t ms = uptime_ms() - base_ms;
    *sec = base_sec + ms / 1000;
    *nsec = (ms % 1000) * 1000000ULL;
}

static long do_clock_gettime(int clk, uint64_t uts) {
    uint64_t ts[2];
    if (clk == 0 || clk == 5 || clk == 8 || clk == 11) realtime(&ts[0], &ts[1]);
    else if (clk == 2 || clk == 3) { uint64_t ms = current->cpu_ms; ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000ULL; }
    else {
        uint64_t hz = cpu_mhz;
        uint64_t ms = uptime_ms();
        ts[0] = ms / 1000;
        ts[1] = (ms % 1000) * 1000000ULL;
        if (hz) {
            /* finer resolution from the TSC inside the current millisecond */
            uint64_t us = (rdtsc() / hz) % 1000;
            ts[1] += us * 1000;
        }
    }
    return copy_to_user((void *)uts, ts, 16) < 0 ? -EFAULT : 0;
}

static uint64_t ts_to_ms(uint64_t uts, bool *ok) {
    int64_t ts[2];
    *ok = copy_from_user(ts, (void *)uts, 16) == 0;
    if (!*ok || ts[0] < 0) return 0;
    return (uint64_t)ts[0] * 1000 + ((uint64_t)ts[1] + 999999) / 1000000;
}

/* sleep that wakes up early for signals */
static long sleep_interruptible(uint64_t ms) {
    uint64_t end = uptime_ms() + ms;
    while (uptime_ms() < end) {
        if (task_interrupted(current)) return -EINTR;
        sleep_ms(MIN(end - uptime_ms(), (uint64_t)50));
    }
    return 0;
}

/* ------------------------------------------------------------------ select */

static long do_select(int nfds, uint64_t rset, uint64_t wset, uint64_t eset, int64_t timeout_ms) {
    if (nfds < 0 || nfds > 64) nfds = 64;
    uint64_t rin = 0, win = 0, rout, wout;
    if (rset && copy_from_user(&rin, (void *)rset, 8) < 0) return -EFAULT;
    if (wset && copy_from_user(&win, (void *)wset, 8) < 0) return -EFAULT;
    if (nfds < 64) { uint64_t m = (1ULL << nfds) - 1; rin &= m; win &= m; }
    uint64_t deadline = timeout_ms < 0 ? ~0ULL : uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        rout = wout = 0;
        int n = 0;
        for (int fd = 0; fd < nfds; fd++) {
            if (!((rin | win) & (1ULL << fd))) continue;
            file_t *f = fd_get(current, fd);
            if (!f) return -EBADF;
            int m = file_poll(f);
            if ((rin & (1ULL << fd)) && (m & (POLLIN | POLLHUP | POLLERR))) { rout |= 1ULL << fd; n++; }
            if ((win & (1ULL << fd)) && (m & POLLOUT)) { wout |= 1ULL << fd; n++; }
        }
        if (n || uptime_ms() >= deadline) {
            uint64_t zero = 0;
            if (rset) copy_to_user((void *)rset, &rout, 8);
            if (wset) copy_to_user((void *)wset, &wout, 8);
            if (eset) copy_to_user((void *)eset, &zero, 8);
            return n;
        }
        if (task_interrupted(current)) return -EINTR;
        uint64_t f = irq_save();
        wq_wait_timeout(&poll_wq, MIN(deadline - uptime_ms(), (uint64_t)1000));
        irq_restore(f);
    }
}

/* ------------------------------------------------------------------ sockets */

static long do_sockaddr_in(uint64_t uaddr, uint32_t *ip, uint16_t *port) {
    uint8_t sa[16];
    if (copy_from_user(sa, (void *)uaddr, 16) < 0) return -EFAULT;
    if (*(uint16_t *)sa != 2) return -EAFNOSUPPORT;
    *port = (uint16_t)((sa[2] << 8) | sa[3]);
    memcpy(ip, sa + 4, 4);
    return 0;
}

static void put_sockaddr_in(uint64_t uaddr, uint64_t ulen, uint32_t ip, uint16_t port) {
    if (!uaddr) return;
    uint8_t sa[16] = { 2, 0, (uint8_t)(port >> 8), (uint8_t)port };
    memcpy(sa + 4, &ip, 4);
    copy_to_user((void *)uaddr, sa, 16);
    if (ulen) { uint32_t l = 16; copy_to_user((void *)ulen, &l, 4); }
}

/* ------------------------------------------------------------------ misc structures */

static long do_uname(uint64_t ubuf) {
    char u[6][65];
    memset(u, 0, sizeof(u));
    strlcpy(u[0], "Linux", 65);
    strlcpy(u[1], "claudeos", 65);
    strlcpy(u[2], "6.1.0-claudeos", 65);
    strlcpy(u[3], "#1 SMP ClaudeOS 1.0", 65);
    strlcpy(u[4], "x86_64", 65);
    strlcpy(u[5], "(none)", 65);
    return copy_to_user((void *)ubuf, u, sizeof(u)) < 0 ? -EFAULT : 0;
}

static long do_rlimit(int res, uint64_t unew, uint64_t uold) {
    UNUSED(unew);
    if (!uold) return 0;
    uint64_t lim[2] = { ~0ULL, ~0ULL };
    if (res == 3) lim[0] = lim[1] = USER_STACK_MAX;          /* RLIMIT_STACK */
    if (res == 7) lim[0] = lim[1] = MAX_FDS;                  /* RLIMIT_NOFILE */
    return copy_to_user((void *)uold, lim, 16) < 0 ? -EFAULT : 0;
}

static char **copy_strv(uint64_t uptr, int *err) {
    *err = 0;
    if (!uptr) return 0;
    char **uv = (char **)uptr;
    int n = 0;
    size_t total = 0;
    for (;; n++) {
        if (n >= MAX_ARGS) { *err = -E2BIG; return 0; }
        if (!user_range_ok(&uv[n], sizeof(char *), false)) { *err = -EFAULT; return 0; }
        if (!uv[n]) break;
        if (!user_str_ok(uv[n], 4096)) { *err = -EFAULT; return 0; }
        total += strlen(uv[n]) + 1;
        if (total > MAX_ARG_BYTES) { *err = -E2BIG; return 0; }
    }
    char **kv = kmalloc(sizeof(char *) * (n + 1) + total);
    char *s = (char *)(kv + n + 1);
    for (int i = 0; i < n; i++) {
        size_t l = strlen(uv[i]) + 1;
        memcpy(s, uv[i], l);
        kv[i] = s;
        s += l;
    }
    kv[n] = 0;
    return kv;
}

/* ------------------------------------------------------------------ signals */

#define SIG_DFL 0
#define SIG_IGN 1
#define SA_NODEFER 0x40000000ULL
#define SA_RESETHAND 0x80000000ULL
#define SA_RESTORER 0x04000000ULL
#define SA_ONSTACK 0x08000000ULL
#define SIGKILL 9
#define SIGSTOP 19
#define SIGCHLD 17
#define UNBLOCKABLE ((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)))

static ksigaction_t *sigact_of(task_t *t) {
    task_t *l = PROC(t);
    if (!l->sigact) l->sigact = kzalloc(65 * sizeof(ksigaction_t));
    return l->sigact;
}

static bool default_ignored(int sig) { return sig == SIGCHLD || sig == 18 || sig == 23 || sig == 28 || (sig >= 19 && sig <= 22); }

bool linux_signal_pending(task_t *t) { return (t->sigpending & ~t->sigmask) != 0; }

/* post a signal to a Linux process (or thread) */
void linux_send_signal(task_t *t, int sig) {
    if (sig < 1 || sig > 64) return;
    task_t *l = PROC(t);
    ksigaction_t *a = l->sigact ? &l->sigact[sig] : 0;
    uint64_t h = a ? a->handler : SIG_DFL;
    if (sig != SIGKILL && (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig)))) return;
    if (sig == SIGKILL || h == SIG_DFL) {
        if (!l->group_exit) l->exit_code = 128 + sig;
        l->killed = true;
        task_wake(l);
        for (task_t *th = task_list(); th; th = th->all_next)
            if (th->leader == l && th != l) { th->killed = true; task_wake(th); }
        return;
    }
    t->sigpending |= 1ULL << (sig - 1);
    task_wake(t);
}

/* sigcontext offsets inside ucontext (uc_mcontext starts at 40) */
#define UC_MCONTEXT 40
#define UC_SIGMASK (40 + 256)
#define UC_SIZE (UC_SIGMASK + 8)
#define FRAME_SIZE (8 + UC_SIZE + 128)

void linux_deliver_signals(regs_t *r) {
    task_t *cur = current;
    uint64_t ready = cur->sigpending & ~cur->sigmask;
    if (!ready) return;
    int sig = __builtin_ctzll(ready) + 1;
    cur->sigpending &= ~(1ULL << (sig - 1));
    ksigaction_t *act = &sigact_of(cur)[sig];
    if (act->handler == SIG_IGN) return;
    if (act->handler == SIG_DFL) {
        if (default_ignored(sig)) return;
        task_t *l = PROC(cur);
        if (!l->group_exit) l->exit_code = 128 + sig;
        proc_exit(128 + sig);
    }
    if (!(act->flags & SA_RESTORER)) proc_exit(128 + sig);

    uint64_t sp = r->rsp - 128;
    if ((act->flags & SA_ONSTACK) && cur->alt_stack_size && !(sp >= cur->alt_stack && sp < cur->alt_stack + cur->alt_stack_size))
        sp = cur->alt_stack + cur->alt_stack_size;
    /* FPU state */
    sp = (sp - 512) & ~63ULL;
    uint64_t fx = sp;
    fpu_save(cur->fpu);
    /* rt_sigframe: pretcode, ucontext, siginfo */
    sp = (sp - FRAME_SIZE) & ~15ULL;
    sp -= 8;
    uint8_t frame[FRAME_SIZE];
    memset(frame, 0, sizeof(frame));
    *(uint64_t *)frame = act->restorer;
    uint8_t *uc = frame + 8;
    *(uint64_t *)(uc + 16) = cur->alt_stack;
    *(uint64_t *)(uc + 32) = cur->alt_stack_size;
    uint64_t *mc = (uint64_t *)(uc + UC_MCONTEXT);
    mc[0] = r->r8; mc[1] = r->r9; mc[2] = r->r10; mc[3] = r->r11; mc[4] = r->r12; mc[5] = r->r13;
    mc[6] = r->r14; mc[7] = r->r15; mc[8] = r->rdi; mc[9] = r->rsi; mc[10] = r->rbp; mc[11] = r->rbx;
    mc[12] = r->rdx; mc[13] = r->rax; mc[14] = r->rcx; mc[15] = r->rsp; mc[16] = r->rip; mc[17] = r->rflags;
    mc[18] = 0x33;                                         /* cs, gs, fs, ss (16-bit each) */
    mc[22] = fx;                                           /* fpstate */
    *(uint64_t *)(uc + UC_SIGMASK) = cur->sigmask;
    uint8_t *si = uc + UC_SIZE;
    *(int32_t *)si = sig;
    *(int32_t *)(si + 8) = 0;                              /* SI_USER */
    if (copy_to_user((void *)fx, cur->fpu, 512) < 0 || copy_to_user((void *)sp, frame, sizeof(frame)) < 0)
        proc_exit(128 + 11);
    /* the handler runs with the action's mask and a clean FPU */
    cur->sigmask |= act->mask;
    if (!(act->flags & SA_NODEFER)) cur->sigmask |= 1ULL << (sig - 1);
    cur->sigmask &= ~UNBLOCKABLE;
    if (act->flags & SA_RESETHAND) act->handler = SIG_DFL;
    memcpy(cur->fpu, fpu_initial_state, fpu_size);
    fpu_restore(cur->fpu);
    r->rip = act->handler;
    r->rsp = sp;
    r->rdi = sig;
    r->rsi = sp + 8 + UC_SIZE;
    r->rdx = sp + 8;
    r->rax = 0;
    r->rflags &= ~0x500ULL;                                /* DF, TF */
}

static void do_sigreturn(regs_t *r) {
    task_t *cur = current;
    uint64_t ucp = r->rsp;                                 /* the handler's "ret" popped pretcode */
    uint8_t uc[UC_SIZE];
    if (copy_from_user(uc, (void *)ucp, sizeof(uc)) < 0) proc_exit(128 + 11);
    uint64_t *mc = (uint64_t *)(uc + UC_MCONTEXT);
    r->r8 = mc[0]; r->r9 = mc[1]; r->r10 = mc[2]; r->r11 = mc[3]; r->r12 = mc[4]; r->r13 = mc[5];
    r->r14 = mc[6]; r->r15 = mc[7]; r->rdi = mc[8]; r->rsi = mc[9]; r->rbp = mc[10]; r->rbx = mc[11];
    r->rdx = mc[12]; r->rax = mc[13]; r->rcx = mc[14]; r->rsp = mc[15]; r->rip = mc[16];
    r->rflags = (mc[17] & 0xDD5) | 0x202;                  /* user-changeable flags, IF always on */
    r->cs = USER_CS;
    r->ss = USER_DS;
    cur->sigmask = *(uint64_t *)(uc + UC_SIGMASK) & ~UNBLOCKABLE;
    if (mc[22] && copy_from_user(cur->fpu, (void *)mc[22], 512) == 0) {
        ((uint32_t *)cur->fpu)[6] &= 0xFFBF;               /* sanitize MXCSR reserved bits */
        if (fpu_xsave) cur->fpu[512] |= 3;                  /* XSTATE_BV: x87 + SSE from the legacy area */
        fpu_restore(cur->fpu);
    }
}

static long do_sigaction(int sig, uint64_t uact, uint64_t uold) {
    if (sig < 1 || sig > 64) return -EINVAL;
    ksigaction_t *a = &sigact_of(current)[sig];
    if (uold && copy_to_user((void *)uold, a, sizeof(*a)) < 0) return -EFAULT;
    if (uact) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        ksigaction_t n;
        if (copy_from_user(&n, (void *)uact, sizeof(n)) < 0) return -EFAULT;
        *a = n;
    }
    return 0;
}

static long do_sigprocmask(int how, uint64_t uset, uint64_t uold) {
    task_t *cur = current;
    if (uold && copy_to_user((void *)uold, &cur->sigmask, 8) < 0) return -EFAULT;
    if (!uset) return 0;
    uint64_t set;
    if (copy_from_user(&set, (void *)uset, 8) < 0) return -EFAULT;
    if (how == 0) cur->sigmask |= set;
    else if (how == 1) cur->sigmask &= ~set;
    else if (how == 2) cur->sigmask = set;
    else return -EINVAL;
    cur->sigmask &= ~UNBLOCKABLE;
    return 0;
}

static long do_kill(int pid, int sig, bool thread) {
    if (pid <= 0) pid = PROC(current)->pid;              /* process groups: our own process */
    task_t *t = task_find(pid);
    if (!t || t->state == T_ZOMBIE) return -ESRCH;
    if (!sig) return 0;
    if (!t->is_user) return -EPERM;
    if (PROC(t)->linux_abi) { linux_send_signal(thread ? t : PROC(t), sig); return 0; }
    return proc_kill(PROC(t)->pid, sig);
}

/* ------------------------------------------------------------------ futex */

static long do_futex(uint64_t uaddr, int op, uint32_t val, uint64_t utimeout, uint64_t uaddr2, uint32_t val3) {
    UNUSED(uaddr2);
    int cmd = op & 0x7F;
    int64_t ms = -1;
    bool ok;
    switch (cmd) {
    case 0: case 9:                                        /* WAIT, WAIT_BITSET */
        if (utimeout) {
            uint64_t t = ts_to_ms(utimeout, &ok);
            if (!ok) return -EFAULT;
            if (cmd == 9) {                                /* absolute */
                uint64_t now_s, now_ns, now;
                if (op & 256) { realtime(&now_s, &now_ns); now = now_s * 1000 + now_ns / 1000000; }
                else now = uptime_ms();
                t = t > now ? t - now : 0;
            }
            ms = (int64_t)t;
        }
        return futex_wait(uaddr, val, ms);
    case 1: case 10:                                       /* WAKE, WAKE_BITSET */
        return futex_wake(current->cr3, uaddr, (int)val);
    case 3: case 4: {                                      /* REQUEUE, CMP_REQUEUE: wake everyone */
        if (cmd == 4) {
            uint32_t cur;
            if (copy_from_user(&cur, (void *)uaddr, 4) < 0) return -EFAULT;
            if (cur != val3) return -EAGAIN;
        }
        return futex_wake(current->cr3, uaddr, 0x7FFFFFFF);
    }
    }
    return -ENOSYS;
}

/* ------------------------------------------------------------------ dispatcher */

static long do_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off) {
    bool ok;
    bool fixed = flags & 0x10;
    bool anon = flags & 0x20;
    int vprot = (prot & 1 ? VMA_READ : 0) | (prot & 2 ? VMA_WRITE : 0) | (prot & 4 ? VMA_EXEC : 0);
    if (anon) {
        uint64_t a = proc_mmap(addr, len, vprot, fixed, &ok);
        return ok ? (long)a : -ENOMEM;
    }
    /* file mapping: private copy of the file contents */
    file_t *f = fd_get(current, fd);
    if (!f) return -EBADF;
    uint64_t a = proc_mmap(addr, len, VMA_READ | VMA_WRITE, fixed, &ok);
    if (!ok) return -ENOMEM;
    uint64_t save = f->off;
    f->off = off;
    uint8_t *buf = kmalloc(65536);
    uint64_t done = 0;
    while (done < len) {
        long n = file_read(f, buf, MIN(len - done, (uint64_t)65536));
        if (n <= 0) break;
        if (copy_to_user((void *)(a + done), buf, n) < 0) break;
        done += n;
    }
    kfree(buf);
    f->off = save;
    proc_mprotect(a, len, vprot);
    return (long)a;
}

static long do_clone(regs_t *r, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    if ((flags & 0x100) && (flags & 0x10000)) {            /* CLONE_VM | CLONE_THREAD */
        long tid = proc_thread_create(0, 0, newsp, (flags & 0x200000) ? ctid : 0, (flags & 0x80000) ? tls : 0, r);
        if (tid > 0 && (flags & 0x100000) && ptid) copy_to_user((void *)ptid, &tid, 4);
        return tid;
    }
    /* processes (fork, vfork, posix_spawn): the child gets a copy of the memory */
    long pid = proc_fork(r);
    if (pid > 0 && newsp) {
        task_t *c = task_find((int)pid);
        if (c) ((regs_t *)(c->kstack_top - sizeof(regs_t)))->rsp = newsp;
    }
    if (pid > 0 && (flags & 0x100000) && ptid) copy_to_user((void *)ptid, &pid, 4);
    return pid;
}

static long do_wait4(int pid, uint64_t ustatus, int options) {
    int status = 0;
    int r = proc_waitpid(pid, &status, options & 1);
    if (r > 0 && ustatus) {
        int ls = status >= 128 && status < 128 + 65 ? status - 128 : (status & 0xFF) << 8;
        if (copy_to_user((void *)ustatus, &ls, 4) < 0) return -EFAULT;
    }
    return r;
}

static long dispatch(regs_t *r, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6) {
    char path[PATH_MAX_LEN], path2[PATH_MAX_LEN];
    long ret;
    switch (nr) {
    case L_READ: return call(SYS_READ, a1, a2, a3, 0, 0);
    case L_WRITE: return call(SYS_WRITE, a1, a2, a3, 0, 0);
    case L_OPEN: return call(SYS_OPEN, a1, a2, a3, 0, 0);
    case L_CREAT: return call(SYS_OPEN, a1, O_CREAT | O_WRONLY | O_TRUNC, a2, 0, 0);
    case L_OPENAT:
        if ((ret = at_path((int)a1, a2, path))) return ret;
        return call(SYS_OPEN, (uint64_t)a2, a3, a4, 0, 0);
    case L_CLOSE: return call(SYS_CLOSE, a1, 0, 0, 0, 0);
    case L_STAT: case L_LSTAT: return do_stat_path(AT_FDCWD, a1, a2, 0, false);
    case L_FSTAT: return do_stat_path((int)a1, 0, a2, AT_EMPTY_PATH, false);
    case L_NEWFSTATAT: return do_stat_path((int)a1, a2, a3, (int)a4, false);
    case L_STATX: return do_stat_path((int)a1, a2, a5, (int)a3, true);
    case L_POLL: return call(SYS_POLL, a1, a2, (uint64_t)(int64_t)(int)a3, 0, 0);
    case L_PPOLL: {
        int64_t ms = -1;
        bool ok;
        if (a3) { ms = (int64_t)ts_to_ms(a3, &ok); if (!ok) return -EFAULT; }
        return call(SYS_POLL, a1, a2, (uint64_t)ms, 0, 0);
    }
    case L_SELECT: {
        int64_t ms = -1;
        if (a5) {
            int64_t tv[2];
            if (copy_from_user(tv, (void *)a5, 16) < 0) return -EFAULT;
            ms = tv[0] * 1000 + tv[1] / 1000;
        }
        return do_select((int)a1, a2, a3, a4, ms);
    }
    case L_PSELECT6: {
        int64_t ms = -1;
        bool ok;
        if (a5) { ms = (int64_t)ts_to_ms(a5, &ok); if (!ok) return -EFAULT; }
        return do_select((int)a1, a2, a3, a4, ms);
    }
    case L_LSEEK: return call(SYS_LSEEK, a1, a2, a3, 0, 0);
    case L_MMAP: return do_mmap(a1, a2, (int)a3, (int)a4, (int)a5, a6);
    case L_MPROTECT: return proc_mprotect(a1, a2, (a3 & 1 ? VMA_READ : 0) | (a3 & 2 ? VMA_WRITE : 0) | (a3 & 4 ? VMA_EXEC : 0));
    case L_MUNMAP: return proc_munmap(a1, a2);
    case L_BRK: {
        task_t *t = PROC(current);
        if (a1 && a1 >= t->brk_start) proc_sbrk((long)(a1 - t->brk));
        return (long)t->brk;
    }
    case L_RT_SIGACTION: return do_sigaction((int)a1, a2, a3);
    case L_RT_SIGPROCMASK: return do_sigprocmask((int)a1, a2, a3);
    case L_IOCTL: return do_ioctl((int)a1, a2, a3);
    case L_PREAD64: case L_PWRITE64: {
        file_t *f = fd_get(current, (int)a1);
        if (!f) return -EBADF;
        uint64_t save = f->off;
        f->off = a4;
        ret = call(nr == L_PREAD64 ? SYS_READ : SYS_WRITE, a1, a2, a3, 0, 0);
        f->off = save;
        return ret;
    }
    case L_READV: return do_iov((int)a1, a2, (int)a3, false);
    case L_WRITEV: return do_iov((int)a1, a2, (int)a3, true);
    case L_ACCESS: case L_FACCESSAT: case L_FACCESSAT2: {
        uint64_t up = nr == L_ACCESS ? a1 : a2;
        if ((ret = at_path(nr == L_ACCESS ? AT_FDCWD : (int)a1, up, path))) return ret;
        kstat_t st;
        return vfs_stat(path, &st);
    }
    case L_PIPE: return call(SYS_PIPE, a1, 0, 0, 0, 0);
    case L_PIPE2: return call(SYS_PIPE, a1, a2 & O_NONBLOCK, 0, 0, 0);
    case L_SCHED_YIELD: yield(); return 0;
    case L_MREMAP: return -ENOMEM;
    case L_MSYNC: case L_MADVISE: case L_MINCORE: case L_FADVISE64: return 0;
    case L_DUP: return call(SYS_DUP, a1, 0, 0, 0, 0);
    case L_DUP2: return a1 == a2 ? (fd_get(current, (int)a1) ? (long)a1 : -EBADF) : call(SYS_DUP2, a1, a2, 0, 0, 0);
    case L_DUP3: return a1 == a2 ? -EINVAL : call(SYS_DUP2, a1, a2, 0, 0, 0);
    case L_PAUSE: return sleep_interruptible(~0ULL >> 2);
    case L_NANOSLEEP: {
        bool ok;
        uint64_t ms = ts_to_ms(a1, &ok);
        return ok ? sleep_interruptible(ms) : -EFAULT;
    }
    case L_CLOCK_NANOSLEEP: {
        bool ok;
        uint64_t ms = ts_to_ms(a3, &ok);
        if (!ok) return -EFAULT;
        if (a2 & 1) {                                      /* TIMER_ABSTIME */
            uint64_t s, ns, now;
            if (a1 == 0) { realtime(&s, &ns); now = s * 1000 + ns / 1000000; } else now = uptime_ms();
            ms = ms > now ? ms - now : 0;
        }
        return sleep_interruptible(ms);
    }
    case L_GETITIMER: case L_SETITIMER: case L_ALARM: return 0;
    case L_GETPID: return PROC(current)->pid;
    case L_GETTID: return current->pid;
    case L_GETPPID: return PROC(current)->ppid;
    case L_GETUID: case L_GETGID: case L_GETEUID: case L_GETEGID: return 1000;
    case L_SETUID: case L_SETGID: case L_SETPGID: return 0;
    case L_GETPGRP: case L_GETPGID: case L_GETSID: case L_SETSID: return PROC(current)->pid;
    case L_GETGROUPS: return 0;
    case L_SOCKET: {
        int type = (int)(a2 & 0xF);
        ret = call(SYS_SOCKET, a1, type, 0, 0, 0);
        if (ret >= 0 && (a2 & 0x800)) fd_get(current, (int)ret)->flags |= O_NONBLOCK;
        return ret;
    }
    case L_CONNECT: {
        uint32_t ip;
        uint16_t port;
        if ((ret = do_sockaddr_in(a2, &ip, &port))) return ret;
        return call(SYS_CONNECT, a1, ip, port, 10000, 0);
    }
    case L_BIND: {
        uint32_t ip;
        uint16_t port;
        if ((ret = do_sockaddr_in(a2, &ip, &port))) return ret;
        return port ? call(SYS_BIND, a1, port, 0, 0, 0) : 0;
    }
    case L_SENDTO: {
        uint32_t ip = 0;
        uint16_t port = 0;
        if (a5 && (ret = do_sockaddr_in(a5, &ip, &port))) return ret;
        return call(SYS_SENDTO, a1, a2, a3, ip, port);
    }
    case L_RECVFROM: {
        file_t *f = fd_get(current, (int)a1);
        uint64_t timeout = (f && ((f->flags & O_NONBLOCK) || (a4 & 0x40))) ? 1 : 0;   /* MSG_DONTWAIT */
        /* the native call stores (ip, port) in user memory: borrow the caller's sockaddr buffer */
        if (a5 && !uok(a5, 16, true)) return -EFAULT;
        ret = call(SYS_RECVFROM, a1, a2, a3, a5, timeout);
        if (ret >= 0 && a5) {
            uint32_t from[2];
            copy_from_user(from, (void *)a5, 8);
            put_sockaddr_in(a5, a6, from[0], (uint16_t)from[1]);
        }
        return ret == -ETIMEDOUT ? -EAGAIN : ret;
    }
    case L_SHUTDOWN: case L_LISTEN: case L_SETSOCKOPT: return 0;
    case L_GETSOCKOPT: {
        int zero = 0;
        if (a4) copy_to_user((void *)a4, &zero, 4);
        return 0;
    }
    case L_GETSOCKNAME: case L_GETPEERNAME: put_sockaddr_in(a2, a3, 0, 0); return 0;
    case L_CLONE: return do_clone(r, a1, a2, a3, a4, a5);
    case L_FORK: case L_VFORK: return proc_fork(r);
    case L_CLONE3: return -ENOSYS;
    case L_EXECVE: {
        if ((ret = at_path(AT_FDCWD, a1, path))) return ret;
        int err;
        char **argv = copy_strv(a2, &err);
        if (err) return err;
        char **envp = copy_strv(a3, &err);
        if (err) { kfree(argv); return err; }
        char *defargv[2] = { path, 0 };
        ret = proc_execve(r, path, argv ? argv : defargv, envp);
        kfree(argv);
        kfree(envp);
        return ret;
    }
    case L_EXIT: thread_exit((int)(a1 & 0xFF));
    case L_EXIT_GROUP: proc_exit((int)(a1 & 0xFF));
    case L_WAIT4: return do_wait4((int)a1, a2, (int)a3);
    case L_WAITID: return -ENOSYS;
    case L_KILL: return do_kill((int)a1, (int)a2, false);
    case L_TKILL: return do_kill((int)a1, (int)a2, true);
    case L_TGKILL: return do_kill((int)a2, (int)a3, true);
    case L_UNAME: return do_uname(a1);
    case L_FCNTL: {
        file_t *f = fd_get(current, (int)a1);
        if (!f) return -EBADF;
        switch ((int)a2) {
        case 0: case 1030: {                               /* F_DUPFD, F_DUPFD_CLOEXEC */
            task_t *l = PROC(current);
            for (int fd = (int)a3; fd < MAX_FDS; fd++) {
                if (!l->fds[fd]) { file_ref(f); l->fds[fd] = f; return fd; }
            }
            return -EMFILE;
        }
        case 1: return (f->flags & O_CLOEXEC_K) ? 1 : 0;
        case 2: if (a3 & 1) f->flags |= O_CLOEXEC_K; else f->flags &= ~O_CLOEXEC_K; return 0;
        case 3: return f->flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
        case 4: f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (a3 & (O_APPEND | O_NONBLOCK)); return 0;
        default: return 0;                                 /* locks: always granted */
        }
    }
    case L_FLOCK: return 0;
    case L_FSYNC: case L_FDATASYNC: return call(SYS_FSYNC, a1, 0, 0, 0, 0);
    case L_SYNC: return call(SYS_FSYNC, (uint64_t)-1, 0, 0, 0, 0);
    case L_TRUNCATE: {
        file_t *f;
        if ((ret = at_path(AT_FDCWD, a1, path))) return ret;
        if ((ret = vfs_open(path, O_WRONLY, &f)) < 0) return ret;
        ret = file_truncate(f, a2);
        file_close(f);
        return ret;
    }
    case L_FTRUNCATE: return call(SYS_FTRUNCATE, a1, a2, 0, 0, 0);
    case L_GETDENTS64: return do_getdents64((int)a1, a2, a3);
    case L_GETCWD: return call(SYS_GETCWD, a1, a2, 0, 0, 0);
    case L_CHDIR: return call(SYS_CHDIR, a1, 0, 0, 0, 0);
    case L_FCHDIR: return -ENOSYS;
    case L_RENAME: return call(SYS_RENAME, a1, a2, 0, 0, 0);
    case L_RENAMEAT: case L_RENAMEAT2:
        if ((ret = at_path((int)a1, a2, path)) || (ret = at_path((int)a3, a4, path2))) return ret;
        return vfs_rename(path, path2);
    case L_MKDIR: return call(SYS_MKDIR, a1, 0, 0, 0, 0);
    case L_MKDIRAT:
        if ((ret = at_path((int)a1, a2, path))) return ret;
        return vfs_mkdir(path);
    case L_RMDIR: return call(SYS_RMDIR, a1, 0, 0, 0, 0);
    case L_UNLINK: return call(SYS_UNLINK, a1, 0, 0, 0, 0);
    case L_UNLINKAT:
        if ((ret = at_path((int)a1, a2, path))) return ret;
        return (a3 & AT_REMOVEDIR) ? vfs_rmdir(path) : vfs_unlink(path);
    case L_LINK: case L_SYMLINK: return -EPERM;
    case L_READLINK: case L_READLINKAT: return -EINVAL;
    case L_CHMOD: case L_FCHMOD: case L_CHOWN: case L_FCHOWN: case L_LCHOWN: case L_FCHMODAT: case L_FCHOWNAT:
    case L_UTIME: case L_UTIMES: case L_UTIMENSAT:
        return 0;
    case L_UMASK: { task_t *l = PROC(current); uint32_t old = l->umask; l->umask = a1 & 0777; return old; }
    case L_GETTIMEOFDAY: {
        if (a1) {
            uint64_t s, ns;
            realtime(&s, &ns);
            uint64_t tv[2] = { s, ns / 1000 };
            if (copy_to_user((void *)a1, tv, 16) < 0) return -EFAULT;
        }
        return 0;
    }
    case L_TIME: {
        uint64_t s, ns;
        realtime(&s, &ns);
        if (a1 && copy_to_user((void *)a1, &s, 8) < 0) return -EFAULT;
        return (long)s;
    }
    case L_CLOCK_GETTIME: return do_clock_gettime((int)a1, a2);
    case L_CLOCK_GETRES: {
        uint64_t ts[2] = { 0, 1000000 };
        if (a2 && copy_to_user((void *)a2, ts, 16) < 0) return -EFAULT;
        return 0;
    }
    case L_GETRLIMIT: return do_rlimit((int)a1, 0, a2);
    case L_SETRLIMIT: return 0;
    case L_PRLIMIT64: return do_rlimit((int)a2, a3, a4);
    case L_GETRUSAGE: {
        uint8_t ru[144];
        memset(ru, 0, sizeof(ru));
        uint64_t ms = current->cpu_ms;
        *(uint64_t *)ru = ms / 1000;
        *(uint64_t *)(ru + 8) = (ms % 1000) * 1000;
        return copy_to_user((void *)a2, ru, sizeof(ru)) < 0 ? -EFAULT : 0;
    }
    case L_TIMES: {
        uint64_t ticks100 = uptime_ms() / 10;
        if (a1) {
            uint64_t tms[4] = { current->cpu_ms / 10, 0, 0, 0 };
            if (copy_to_user((void *)a1, tms, 32) < 0) return -EFAULT;
        }
        return (long)ticks100;
    }
    case L_SYSINFO: {
        uint8_t si[112];
        memset(si, 0, sizeof(si));
        *(int64_t *)si = (int64_t)(uptime_ms() / 1000);
        *(uint64_t *)(si + 32) = pmm_total_pages() * PAGE_SIZE;
        *(uint64_t *)(si + 40) = pmm_free_pages() * PAGE_SIZE;
        *(uint16_t *)(si + 80) = 64;
        *(uint32_t *)(si + 104) = 1;
        return copy_to_user((void *)a1, si, sizeof(si)) < 0 ? -EFAULT : 0;
    }
    case L_STATFS: case L_FSTATFS: {
        kstatfs_t st;
        if (nr == L_STATFS) {
            if ((ret = at_path(AT_FDCWD, a1, path))) return ret;
            if ((ret = vfs_statfs(path, &st)) < 0) return ret;
        } else {
            if ((ret = vfs_statfs("/", &st)) < 0) return ret;
        }
        uint64_t s[15];
        memset(s, 0, sizeof(s));
        uint64_t bs = st.block_size ? st.block_size : 4096;
        s[0] = 0x9123683E;
        s[1] = bs;
        s[2] = st.total_bytes / bs;
        s[3] = s[4] = st.free_bytes / bs;
        s[5] = 1 << 20;
        s[6] = 1 << 19;
        s[8] = 255;
        s[9] = bs;
        return copy_to_user((void *)a2, s, 120) < 0 ? -EFAULT : 0;
    }
    case L_SIGALTSTACK: {
        task_t *cur = current;
        if (a2) {
            uint64_t old[3] = { cur->alt_stack, cur->alt_stack_size ? 0 : 2, cur->alt_stack_size };
            if (copy_to_user((void *)a2, old, 24) < 0) return -EFAULT;
        }
        if (a1) {
            uint64_t ss[3];
            if (copy_from_user(ss, (void *)a1, 24) < 0) return -EFAULT;
            if (ss[1] & 2) { cur->alt_stack = 0; cur->alt_stack_size = 0; }
            else { cur->alt_stack = ss[0]; cur->alt_stack_size = ss[2]; }
        }
        return 0;
    }
    case L_PRCTL: return a1 == 15 || a1 == 16 ? 0 : -EINVAL;       /* PR_SET_NAME / PR_GET_NAME */
    case L_ARCH_PRCTL:
        switch (a1) {
        case 0x1002: current->fs_base = a2; wrmsr(0xC0000100, a2); return 0;
        case 0x1003: return copy_to_user((void *)a2, &current->fs_base, 8) < 0 ? -EFAULT : 0;
        case 0x1001: current->gs_base = a2; wrmsr(0xC0000102, a2); return 0;
        case 0x1004: return copy_to_user((void *)a2, &current->gs_base, 8) < 0 ? -EFAULT : 0;
        }
        return -EINVAL;
    case L_FUTEX: return do_futex(a1, (int)a2, (uint32_t)a3, a4, a5, (uint32_t)a6);
    case L_SCHED_GETAFFINITY: {
        uint64_t mask = cpu_count >= 64 ? ~0ULL : (1ULL << cpu_count) - 1;
        if (a2 < 8) return -EINVAL;
        if (copy_to_user((void *)a3, &mask, 8) < 0) return -EFAULT;
        return 8;
    }
    case L_SET_TID_ADDRESS: current->clear_tid = a1; return current->pid;
    case L_SET_ROBUST_LIST: return 0;
    case L_RSEQ: return -ENOSYS;
    case L_GETRANDOM: {
        if (!uok(a1, a2, true)) return -EFAULT;
        uint8_t *p = (uint8_t *)a1;
        for (uint64_t i = 0; i < a2; i++) p[i] = (uint8_t)krandom();
        return (long)a2;
    }
    case L_SENDFILE: case L_ACCEPT: case L_ACCEPT4: case L_SENDMSG: case L_RECVMSG: case L_SOCKETPAIR:
        return -ENOSYS;
    }
    static uint64_t reported[8];
    if (nr < 512 && !(reported[nr / 64] & (1ULL << (nr % 64)))) {
        reported[nr / 64] |= 1ULL << (nr % 64);
        klog("[linux] %s: unsupported system call %lu\n", PROC(current)->name, nr);
    }
    return -ENOSYS;
}

void linux_syscall(regs_t *r) {
    if (!PROC(current)->linux_abi) { r->rax = (uint64_t)-ENOSYS; return; }
    uint64_t nr = r->rax;
    if (nr == L_RT_SIGRETURN) { do_sigreturn(r); return; }
    long ret = dispatch(r, nr, r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
    r->rax = (uint64_t)ret;
}
