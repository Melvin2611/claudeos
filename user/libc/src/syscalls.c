/* thin wrappers around the ClaudeOS system calls */
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <claudeos.h>


static long ret(long r) {
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

ssize_t read(int fd, void *buf, size_t n) { return ret(syscall3(SYS_READ, fd, buf, n)); }
ssize_t write(int fd, const void *buf, size_t n) { return ret(syscall3(SYS_WRITE, fd, buf, n)); }
int close(int fd) { return (int)ret(syscall1(SYS_CLOSE, fd)); }
off_t lseek(int fd, off_t off, int whence) { return ret(syscall3(SYS_LSEEK, fd, off, whence)); }
int unlink(const char *path) { return (int)ret(syscall1(SYS_UNLINK, path)); }
int rmdir(const char *path) { return (int)ret(syscall1(SYS_RMDIR, path)); }
int chdir(const char *path) { return (int)ret(syscall1(SYS_CHDIR, path)); }
int pipe(int fds[2]) { return (int)ret(syscall2(SYS_PIPE, fds, 0)); }
int pipe2(int fds[2], int flags) { return (int)ret(syscall2(SYS_PIPE, fds, flags)); }
int dup(int fd) { return (int)ret(syscall1(SYS_DUP, fd)); }
int dup2(int a, int b) { return (int)ret(syscall2(SYS_DUP2, a, b)); }
pid_t getpid(void) { return (pid_t)syscall0(SYS_GETPID); }
pid_t getppid(void) { return (pid_t)syscall0(SYS_GETPPID); }
int ftruncate(int fd, off_t len) { return (int)ret(syscall2(SYS_FTRUNCATE, fd, len)); }
int fsync(int fd) { return (int)ret(syscall1(SYS_FSYNC, fd)); }
int kill(pid_t pid, int sig) { return (int)ret(syscall2(SYS_KILL, pid, sig)); }
pid_t waitpid(pid_t pid, int *status, int flags) { return (pid_t)ret(syscall3(SYS_WAITPID, pid, status, flags)); }
pid_t wait(int *status) { return waitpid(-1, status, 0); }
int rename(const char *from, const char *to) { return (int)ret(syscall2(SYS_RENAME, from, to)); }

char *getcwd(char *buf, size_t size) {
    static char tmp[256];
    if (!buf) { buf = tmp; size = sizeof(tmp); }
    if (ret(syscall2(SYS_GETCWD, buf, size)) < 0) return 0;
    return buf;
}

unsigned sleep(unsigned sec) { syscall1(SYS_SLEEP, (long)sec * 1000); return 0; }
int usleep(unsigned long usec) { syscall1(SYS_SLEEP, (usec + 999) / 1000); return 0; }
int msleep(unsigned ms) { return (int)ret(syscall1(SYS_SLEEP, ms)); }
int sched_yield(void) { return (int)syscall0(SYS_YIELD); }

void *sbrk(long incr) {
    long r = syscall1(SYS_SBRK, incr);
    if (r < 0) { errno = (int)-r; return (void *)-1; }
    return (void *)r;
}

int open(const char *path, int flags, ...) { return (int)ret(syscall2(SYS_OPEN, path, flags)); }
int creat(const char *path, int mode) { (void)mode; return open(path, O_WRONLY | O_CREAT | O_TRUNC); }

static void conv_stat(const kstat_t *k, struct stat *st) {
    st->st_ino = k->ino;
    st->st_type = k->type;
    st->st_size = (off_t)k->size;
    st->st_mtime = (time_t)k->mtime;
    st->st_ctime = (time_t)k->ctime;
    st->st_dev = k->dev;
    mode_t m = k->mode & 0777;
    if (k->type == FT_DIR) m |= S_IFDIR;
    else if (k->type == FT_CHR) m |= S_IFCHR;
    else if (k->type == FT_PIPE) m |= S_IFIFO;
    else m |= S_IFREG;
    st->st_mode = m;
}

int stat(const char *path, struct stat *st) {
    kstat_t k;
    if (ret(syscall2(SYS_STAT, path, &k)) < 0) return -1;
    conv_stat(&k, st);
    return 0;
}

int fstat(int fd, struct stat *st) {
    kstat_t k;
    if (ret(syscall2(SYS_FSTAT, fd, &k)) < 0) return -1;
    conv_stat(&k, st);
    return 0;
}

int access(const char *path, int mode) {
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

int mkdir(const char *path, mode_t mode) { (void)mode; return (int)ret(syscall1(SYS_MKDIR, path)); }

int isatty(int fd) {
    kwinsize_t ws;
    return syscall3(SYS_IOCTL, fd, TIOCGWINSZ, &ws) == 0;
}

int ioctl(int fd, unsigned long req, void *arg) { return (int)ret(syscall3(SYS_IOCTL, fd, req, arg)); }

/* ---- ClaudeOS API ---- */
int spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3], int flags) {
    return (int)ret(syscall5(SYS_SPAWN, path, argv, envp, fdmap, flags));
}
int spawnv(const char *path, char *const argv[]) { return spawn(path, argv, environ, 0, 0); }
int proc_info(int index, kprocinfo_t *out) { return (int)ret(syscall2(SYS_PROCINFO, index, out)); }
int sys_info(ksysinfo_t *out) { return (int)ret(syscall1(SYS_SYSINFO, out)); }
int cpu_info(kcpuinfo_t *out, int max) { return (int)ret(syscall2(SYS_CPUINFO, out, max)); }
uint64_t uptime_ms(void) { return (uint64_t)syscall0(SYS_UPTIME); }
int power(int what) { return (int)ret(syscall1(SYS_POWER, what)); }
long dmesg(char *buf, size_t len, size_t off) { return ret(syscall3(SYS_DMESG, buf, len, off)); }
int beep(unsigned freq, unsigned ms) { return (int)ret(syscall2(SYS_BEEP, freq, ms)); }
int poll(kpollfd_t *fds, int nfds, int timeout_ms) { return (int)ret(syscall4(SYS_POLL, fds, nfds, timeout_ms, 0)); }
int poll_ex(kpollfd_t *fds, int nfds, int timeout_ms, int flags) {
    return (int)ret(syscall4(SYS_POLL, fds, nfds, timeout_ms, flags));
}
int mount_info(int index, kmount_t *out) { return (int)ret(syscall2(SYS_MOUNTS, index, out)); }
int statfs(const char *path, kstatfs_t *out) { return (int)ret(syscall2(SYS_STATFS, path, out)); }
int readdir_raw(int fd, kdirent_t *out) { return (int)ret(syscall2(SYS_READDIR, fd, out)); }
int set_time(uint64_t epoch) { return (int)ret(syscall1(SYS_SETTIME, epoch)); }
int kbd_layout(const char *set, char *get, size_t n) { return (int)ret(syscall3(SYS_KBD_LAYOUT, set, get, n)); }
int pci_info(int index, kpciinfo_t *out) { return (int)ret(syscall2(SYS_PCI_INFO, index, out)); }
int mkfs_disk(const char *device, const char *label) { return (int)ret(syscall2(SYS_MKFS, device, label)); }

time_t time(time_t *t) {
    time_t v = (time_t)syscall0(SYS_TIME);
    if (t) *t = v;
    return v;
}

clock_t clock(void) { return (clock_t)syscall0(SYS_UPTIME); }

void format_size(uint64_t b, char *out, size_t n) {
    if (b < 1024) snprintf(out, n, "%lu B", (unsigned long)b);
    else if (b < 1024 * 1024) snprintf(out, n, "%.1f KB", b / 1024.0);
    else if (b < 1024ULL * 1024 * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
    else snprintf(out, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

int system(const char *cmd) {
    char *argv[] = { "sh", "-c", (char *)cmd, 0 };
    int pid = spawnv("/bin/sh", argv);
    if (pid < 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}
