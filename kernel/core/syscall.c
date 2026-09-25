/* System call dispatch and the process / file system calls */
#include <kernel.h>
#include <syscall.h>
#include <proc.h>
#include <mm.h>
#include <cpu.h>
#include <fb.h>
#include "drivers.h"

static syscall_fn table[SYS_MAX];

void syscall_register(int num, syscall_fn fn) {
    if (num >= 0 && num < SYS_MAX) table[num] = fn;
}

void syscall_dispatch(regs_t *r) {
    uint64_t n = r->rax;
    if (n >= SYS_MAX || !table[n]) { r->rax = (uint64_t)-ENOSYS; return; }
    r->rax = (uint64_t)table[n](r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
}

/* ------------------------------------------------------------------ helpers */

static int get_path(uint64_t uptr, char *out) {
    long r = strncpy_from_user(out, (const char *)uptr, PATH_MAX_LEN);
    return r < 0 ? (int)r : 0;
}

/* copy a NULL-terminated user string array into one kernel allocation */
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

/* ------------------------------------------------------------------ process calls */

SYSCALL_DEF(sys_exit) { SYSCALL_UNUSED_ARGS; proc_exit((int)a1); }

/* spawn(path, argv, envp, fdmap[3] or NULL, flags) */
SYSCALL_DEF(sys_spawn) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN];
    int r = get_path(a1, path);
    if (r < 0) return r;
    int err;
    char **argv = copy_strv(a2, &err);
    if (err) return err;
    char **envp = copy_strv(a3, &err);
    if (err) { kfree(argv); return err; }
    file_t *stdio[3] = { PROC(current)->fds[0], PROC(current)->fds[1], PROC(current)->fds[2] };
    if (a4) {
        int map[3];
        if (copy_from_user(map, (void *)a4, sizeof(map)) < 0) { kfree(argv); kfree(envp); return -EFAULT; }
        for (int i = 0; i < 3; i++) stdio[i] = map[i] >= 0 ? fd_get(current, map[i]) : 0;
    }
    char *defargv[2] = { path, 0 };
    r = proc_spawn(path, argv ? argv : defargv, envp, stdio, PROC(current)->cwd, PROC(current)->pid, (int)a5);
    kfree(argv);
    kfree(envp);
    return r;
}

SYSCALL_DEF(sys_waitpid) {
    SYSCALL_UNUSED_ARGS;
    int status = 0;
    if (a2 && !user_range_ok((void *)a2, sizeof(int), true)) return -EFAULT;
    int r = proc_waitpid((int)a1, &status, (int)a3);
    if (r > 0 && a2) *(int *)a2 = status;
    return r;
}

SYSCALL_DEF(sys_getpid) { SYSCALL_UNUSED_ARGS; return PROC(current)->pid; }
SYSCALL_DEF(sys_getppid) { SYSCALL_UNUSED_ARGS; return PROC(current)->ppid; }
SYSCALL_DEF(sys_kill) { SYSCALL_UNUSED_ARGS; return proc_kill((int)a1, (int)a2); }
SYSCALL_DEF(sys_sleep) { SYSCALL_UNUSED_ARGS; sleep_ms(a1); return current->killed ? -EINTR : 0; }
SYSCALL_DEF(sys_yield) { SYSCALL_UNUSED_ARGS; yield(); return 0; }
SYSCALL_DEF(sys_sbrk) { SYSCALL_UNUSED_ARGS; return proc_sbrk((long)a1); }
SYSCALL_DEF(sys_uptime) { SYSCALL_UNUSED_ARGS; return (long)uptime_ms(); }
SYSCALL_DEF(sys_time) { SYSCALL_UNUSED_ARGS; return (long)time_now(); }
SYSCALL_DEF(sys_settime) { SYSCALL_UNUSED_ARGS; time_set(a1); return 0; }

/* thread_create(entry, arg, stack_top, tid_ptr, tls) */
SYSCALL_DEF(sys_thread_create) {
    SYSCALL_UNUSED_ARGS;
    if (a1 >= USER_TOP || a3 >= USER_TOP || a4 >= USER_TOP || a5 >= USER_TOP) return -EFAULT;
    return proc_thread_create(a1, a2, a3, a4, a5, 0);
}

/* set the FS base of the calling thread (thread-local storage) */
SYSCALL_DEF(sys_set_fs) {
    SYSCALL_UNUSED_ARGS;
    if (a1 >= USER_TOP) return -EFAULT;
    current->fs_base = a1;
    wrmsr(0xC0000100, a1);
    return 0;
}
SYSCALL_DEF(sys_thread_exit) { SYSCALL_UNUSED_ARGS; thread_exit((int)a1); }
SYSCALL_DEF(sys_gettid) { SYSCALL_UNUSED_ARGS; return current->pid; }

/* futex(addr, op, val, timeout_ms (-1 = forever)) */
SYSCALL_DEF(sys_futex) {
    SYSCALL_UNUSED_ARGS;
    if (a1 >= USER_TOP) return -EFAULT;
    if (a2 == FUTEX_WAIT) return futex_wait(a1, (uint32_t)a3, (int64_t)a4);
    if (a2 == FUTEX_WAKE) return futex_wake(current->cr3, a1, (int)a3);
    return -EINVAL;
}

/* cpuinfo(kcpuinfo_t *buf, max): returns the number of CPUs */
SYSCALL_DEF(sys_cpuinfo) {
    SYSCALL_UNUSED_ARGS;
    int n = MIN((int)a2, cpu_count);
    if (n > 0 && !user_range_ok((void *)a1, n * sizeof(kcpuinfo_t), true)) return -EFAULT;
    for (int i = 0; i < n; i++) {
        kcpuinfo_t ci = { cpus[i].apic_id, cpus[i].load, cpus[i].busy_ms, cpus[i].idle_ms };
        memcpy((kcpuinfo_t *)a1 + i, &ci, sizeof(ci));
    }
    return cpu_count;
}

static int count_windows(task_t *t);

SYSCALL_DEF(sys_procinfo) {
    SYSCALL_UNUSED_ARGS;
    kprocinfo_t info;
    if (!user_range_ok((void *)a2, sizeof(info), true)) return -EFAULT;
    uint64_t idx = 0;
    uint64_t f = irq_save();
    for (task_t *t = task_list(); t; t = t->all_next) {
        if (t->state == T_DEAD || t->leader != t) continue;   /* one entry per process */
        if (idx++ != a1) continue;
        memset(&info, 0, sizeof(info));
        info.pid = t->pid;
        info.ppid = t->ppid;
        info.state = t->state == T_RUNNING ? PS_RUNNING : t->state == T_READY ? PS_READY :
                     t->state == T_BLOCKED ? PS_BLOCKED : t->state == T_SLEEPING ? PS_SLEEPING : PS_ZOMBIE;
        info.cpu_percent = 0;
        info.cpu_ms = 0;
        for (task_t *th = task_list(); th; th = th->all_next) {
            if (th->leader != t || th->state == T_DEAD) continue;
            info.cpu_percent += th->cpu_percent;
            info.cpu_ms += th->cpu_ms;
            info.threads++;
        }
        info.start_ms = t->start_ms;
        info.mem_bytes = t->user_pages * PAGE_SIZE + KSTACK_SIZE;
        info.is_kernel = !t->is_user;
        info.windows = count_windows(t);
        strlcpy(info.name, t->name, sizeof(info.name));
        irq_restore(f);
        memcpy((void *)a2, &info, sizeof(info));
        return 1;
    }
    irq_restore(f);
    return 0;
}

__attribute__((weak)) int gui_count_windows(task_t *t) { UNUSED(t); return 0; }
static int count_windows(task_t *t) { return gui_count_windows(t); }

SYSCALL_DEF(sys_sysinfo) {
    SYSCALL_UNUSED_ARGS;
    ksysinfo_t si;
    if (!user_range_ok((void *)a1, sizeof(si), true)) return -EFAULT;
    memset(&si, 0, sizeof(si));
    si.mem_total = pmm_total_pages() * PAGE_SIZE;
    si.mem_free = pmm_free_pages() * PAGE_SIZE;
    si.mem_kernel_heap = heap_used_bytes();
    si.uptime_ms = uptime_ms();
    for (task_t *t = task_list(); t; t = t->all_next) if (t->state != T_DEAD && t->leader == t) si.nprocs++;
    si.screen_w = fb.width;
    si.screen_h = fb.height;
    strlcpy(si.cpu_vendor, cpu_vendor, sizeof(si.cpu_vendor));
    cpu_get_brand(si.cpu_brand, sizeof(si.cpu_brand));
    strlcpy(si.os_name, "ClaudeOS", sizeof(si.os_name));
    strlcpy(si.os_version, "1.0", sizeof(si.os_version));
    strlcpy(si.bootloader, bootinfo.loader, sizeof(si.bootloader));
    extern uint64_t cpu_mhz;
    si.cpu_mhz = cpu_mhz;
    si.ncpus = cpu_count;
    extern void pci_gpu_name(char *out, size_t n);
    pci_gpu_name(si.gpu, sizeof(si.gpu));
    memcpy((void *)a1, &si, sizeof(si));
    return 0;
}

__attribute__((weak)) void pci_gpu_name(char *out, size_t n) { strlcpy(out, "Unknown", n); }

__attribute__((weak)) void power_off(void) { for (;;) hlt(); }
__attribute__((weak)) void power_reboot(void) { outb(0x64, 0xFE); for (;;) hlt(); }

SYSCALL_DEF(sys_power) {
    SYSCALL_UNUSED_ARGS;
    if (a1 == POWER_OFF) power_off();
    else if (a1 == POWER_REBOOT) power_reboot();
    return -EINVAL;
}

SYSCALL_DEF(sys_dmesg) {
    SYSCALL_UNUSED_ARGS;
    /* dmesg(buf, len, offset) */
    if (!user_range_ok((void *)a1, a2, true)) return -EFAULT;
    return (long)klog_read((char *)a1, a3, a2);
}

SYSCALL_DEF(sys_beep) {
    SYSCALL_UNUSED_ARGS;
    uint64_t ms = MIN(a2, 2000);
    speaker_on((uint32_t)a1);
    sleep_ms(ms);
    speaker_off();
    return 0;
}

/* ------------------------------------------------------------------ file calls */

SYSCALL_DEF(sys_open) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN];
    int r = get_path(a1, path);
    if (r < 0) return r;
    file_t *f;
    r = vfs_open(path, (int)a2, &f);
    if (r < 0) return r;
    int fd = fd_install(current, f);
    if (fd < 0) file_close(f);
    return fd;
}

SYSCALL_DEF(sys_close) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    PROC(current)->fds[a1] = 0;
    file_close(f);
    return 0;
}

SYSCALL_DEF(sys_read) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    if (!user_range_ok((void *)a2, a3, true)) return -EFAULT;
    return file_read(f, (void *)a2, a3);
}

SYSCALL_DEF(sys_write) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    if (!user_range_ok((void *)a2, a3, false)) return -EFAULT;
    return file_write(f, (const void *)a2, a3);
}

SYSCALL_DEF(sys_lseek) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    return file_seek(f, (long)a2, (int)a3);
}

SYSCALL_DEF(sys_stat) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN];
    int r = get_path(a1, path);
    if (r < 0) return r;
    kstat_t st;
    r = vfs_stat(path, &st);
    if (r < 0) return r;
    return copy_to_user((void *)a2, &st, sizeof(st));
}

SYSCALL_DEF(sys_fstat) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    kstat_t st;
    file_stat(f, &st);
    return copy_to_user((void *)a2, &st, sizeof(st));
}

SYSCALL_DEF(sys_readdir) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    kdirent_t d;
    int r = file_readdir(f, &d);
    if (r <= 0) return r;
    if (copy_to_user((void *)a2, &d, sizeof(d)) < 0) return -EFAULT;
    return 1;
}

#define PATH_CALL(name, fn)                                  \
    SYSCALL_DEF(name) {                                      \
        SYSCALL_UNUSED_ARGS;                                 \
        char path[PATH_MAX_LEN];                             \
        int r = get_path(a1, path);                          \
        if (r < 0) return r;                                 \
        return fn(path);                                     \
    }
PATH_CALL(sys_mkdir, vfs_mkdir)
PATH_CALL(sys_unlink, vfs_unlink)
PATH_CALL(sys_rmdir, vfs_rmdir)

SYSCALL_DEF(sys_rename) {
    SYSCALL_UNUSED_ARGS;
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    int r = get_path(a1, a);
    if (r < 0) return r;
    r = get_path(a2, b);
    if (r < 0) return r;
    return vfs_rename(a, b);
}

SYSCALL_DEF(sys_chdir) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN], abs[PATH_MAX_LEN];
    int r = get_path(a1, path);
    if (r < 0) return r;
    r = vfs_normalize(PROC(current)->cwd, path, abs);
    if (r < 0) return r;
    kstat_t st;
    r = vfs_stat(abs, &st);
    if (r < 0) return r;
    if (st.type != FT_DIR) return -ENOTDIR;
    strlcpy(PROC(current)->cwd, abs, sizeof(PROC(current)->cwd));
    return 0;
}

SYSCALL_DEF(sys_getcwd) {
    SYSCALL_UNUSED_ARGS;
    size_t l = strlen(PROC(current)->cwd) + 1;
    if (l > a2) return -ERANGE;
    return copy_to_user((void *)a1, PROC(current)->cwd, l) < 0 ? -EFAULT : (long)l;
}

SYSCALL_DEF(sys_pipe) {
    SYSCALL_UNUSED_ARGS;
    if (!user_range_ok((void *)a1, sizeof(int) * 2, true)) return -EFAULT;
    file_t *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r < 0) return r;
    int fr = fd_install(current, rd);
    if (fr < 0) { file_close(rd); file_close(wr); return fr; }
    int fw = fd_install(current, wr);
    if (fw < 0) { PROC(current)->fds[fr] = 0; file_close(rd); file_close(wr); return fw; }
    if (a2 & O_NONBLOCK) { rd->flags |= O_NONBLOCK; wr->flags |= O_NONBLOCK; }
    ((int *)a1)[0] = fr;
    ((int *)a1)[1] = fw;
    return 0;
}

SYSCALL_DEF(sys_dup) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    int fd = fd_install(current, f);
    if (fd >= 0) file_ref(f);
    return fd;
}

SYSCALL_DEF(sys_dup2) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f || a2 >= MAX_FDS) return -EBADF;
    if (a1 == a2) return (long)a2;
    file_ref(f);
    if (PROC(current)->fds[a2]) file_close(PROC(current)->fds[a2]);
    PROC(current)->fds[a2] = f;
    return (long)a2;
}

SYSCALL_DEF(sys_ioctl) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    /* all ioctl arguments are small structs/ints: copy through a kernel buffer */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    size_t sz = (a2 == TIOCGWINSZ || a2 == TIOCSWINSZ) ? sizeof(kwinsize_t) : sizeof(int);
    if (a3) {
        if (!user_range_ok((void *)a3, sz, true)) return -EFAULT;
        memcpy(buf, (void *)a3, sz);
    }
    int r = file_ioctl(f, a2, buf);
    if (a3 && r >= 0) memcpy((void *)a3, buf, sz);
    return r;
}

SYSCALL_DEF(sys_ftruncate) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    return file_truncate(f, a2);
}

SYSCALL_DEF(sys_statfs) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN];
    int r = get_path(a1, path);
    if (r < 0) return r;
    kstatfs_t st;
    r = vfs_statfs(path, &st);
    if (r < 0) return r;
    return copy_to_user((void *)a2, &st, sizeof(st));
}

SYSCALL_DEF(sys_fsync) {
    SYSCALL_UNUSED_ARGS;
    file_t *f = fd_get(current, (int)a1);
    if (!f) return -EBADF;
    if (f->vn->mnt && f->vn->mnt->ops && f->vn->mnt->ops->sync) {
        mutex_lock(&f->vn->mnt->lock);
        int r = f->vn->mnt->ops->sync(f->vn->mnt);
        mutex_unlock(&f->vn->mnt->lock);
        return r;
    }
    return 0;
}

SYSCALL_DEF(sys_mounts) {
    SYSCALL_UNUSED_ARGS;
    /* mounts(index, kmount_t*) -> 1 / 0 at end */
    uint64_t i = 0;
    for (mount_t *m = vfs_mounts(); m; m = m->next, i++) {
        if (i != a1) continue;
        kmount_t km;
        memset(&km, 0, sizeof(km));
        strlcpy(km.path, m->path, sizeof(km.path));
        strlcpy(km.fstype, m->fstype, sizeof(km.fstype));
        strlcpy(km.device, m->device, sizeof(km.device));
        if (m->ops && m->ops->statfs) {
            kstatfs_t st;
            memset(&st, 0, sizeof(st));
            m->ops->statfs(m, &st);
            km.total_bytes = st.total_bytes;
            km.free_bytes = st.free_bytes;
        }
        if (copy_to_user((void *)a2, &km, sizeof(km)) < 0) return -EFAULT;
        return 1;
    }
    return 0;
}

__attribute__((weak)) bool gui_event_pending(task_t *t) { UNUSED(t); return false; }

/* poll(fds, nfds, timeout_ms (-1 = forever), flags) */
SYSCALL_DEF(sys_poll) {
    SYSCALL_UNUSED_ARGS;
    int nfds = (int)a2;
    if (nfds < 0 || nfds > 64) return -EINVAL;
    kpollfd_t *ufds = (kpollfd_t *)a1;
    if (nfds && !user_range_ok(ufds, sizeof(kpollfd_t) * nfds, true)) return -EFAULT;
    int64_t timeout = (int64_t)a3;
    uint64_t deadline = timeout < 0 ? ~0ULL : uptime_ms() + (uint64_t)timeout;
    for (;;) {
        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            ufds[i].revents = 0;
            file_t *f = fd_get(current, ufds[i].fd);
            if (!f) { ufds[i].revents = POLLERR; ready++; continue; }
            int m = file_poll(f) & (ufds[i].events | POLLHUP | POLLERR);
            ufds[i].revents = (int16_t)m;
            if (m) ready++;
        }
        bool gui = (a4 & POLL_GUI) && gui_event_pending(current);
        if (ready || gui) return ready + (gui ? 1 : 0);
        if (current->killed) return -EINTR;
        uint64_t now = uptime_ms();
        if (now >= deadline) return 0;
        uint64_t f = irq_save();
        wq_wait_timeout(&poll_wq, MIN(deadline - now, 1000));
        irq_restore(f);
    }
}

void syscall_init(void) {
    syscall_register(SYS_EXIT, sys_exit);
    syscall_register(SYS_SPAWN, sys_spawn);
    syscall_register(SYS_WAITPID, sys_waitpid);
    syscall_register(SYS_GETPID, sys_getpid);
    syscall_register(SYS_GETPPID, sys_getppid);
    syscall_register(SYS_KILL, sys_kill);
    syscall_register(SYS_SLEEP, sys_sleep);
    syscall_register(SYS_YIELD, sys_yield);
    syscall_register(SYS_SBRK, sys_sbrk);
    syscall_register(SYS_UPTIME, sys_uptime);
    syscall_register(SYS_TIME, sys_time);
    syscall_register(SYS_SETTIME, sys_settime);
    syscall_register(SYS_PROCINFO, sys_procinfo);
    syscall_register(SYS_SYSINFO, sys_sysinfo);
    syscall_register(SYS_POWER, sys_power);
    syscall_register(SYS_THREAD_CREATE, sys_thread_create);
    syscall_register(SYS_THREAD_EXIT, sys_thread_exit);
    syscall_register(SYS_FUTEX, sys_futex);
    syscall_register(SYS_GETTID, sys_gettid);
    syscall_register(SYS_CPUINFO, sys_cpuinfo);
    syscall_register(SYS_SET_FS, sys_set_fs);
    syscall_register(SYS_DMESG, sys_dmesg);
    syscall_register(SYS_BEEP, sys_beep);
    syscall_register(SYS_OPEN, sys_open);
    syscall_register(SYS_CLOSE, sys_close);
    syscall_register(SYS_READ, sys_read);
    syscall_register(SYS_WRITE, sys_write);
    syscall_register(SYS_LSEEK, sys_lseek);
    syscall_register(SYS_STAT, sys_stat);
    syscall_register(SYS_FSTAT, sys_fstat);
    syscall_register(SYS_READDIR, sys_readdir);
    syscall_register(SYS_MKDIR, sys_mkdir);
    syscall_register(SYS_UNLINK, sys_unlink);
    syscall_register(SYS_RMDIR, sys_rmdir);
    syscall_register(SYS_RENAME, sys_rename);
    syscall_register(SYS_CHDIR, sys_chdir);
    syscall_register(SYS_GETCWD, sys_getcwd);
    syscall_register(SYS_PIPE, sys_pipe);
    syscall_register(SYS_DUP, sys_dup);
    syscall_register(SYS_DUP2, sys_dup2);
    syscall_register(SYS_IOCTL, sys_ioctl);
    syscall_register(SYS_FTRUNCATE, sys_ftruncate);
    syscall_register(SYS_STATFS, sys_statfs);
    syscall_register(SYS_FSYNC, sys_fsync);
    syscall_register(SYS_MOUNTS, sys_mounts);
    syscall_register(SYS_POLL, sys_poll);
}
