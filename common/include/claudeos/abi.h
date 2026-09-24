#pragma once
/* ClaudeOS kernel <-> userland ABI: syscall numbers, structures, flags, errno values.
 * Syscalls: int 0x80, rax = number, args in rdi, rsi, rdx, r10, r8, r9, result in rax
 * (negative errno on failure). */
#include <stdint.h>

enum {
    SYS_EXIT = 0, SYS_SPAWN, SYS_WAITPID, SYS_GETPID, SYS_GETPPID, SYS_KILL, SYS_SLEEP, SYS_YIELD,
    SYS_SBRK, SYS_UPTIME, SYS_TIME, SYS_PROCINFO, SYS_SYSINFO, SYS_POWER,
    SYS_OPEN = 20, SYS_CLOSE, SYS_READ, SYS_WRITE, SYS_LSEEK, SYS_STAT, SYS_FSTAT, SYS_READDIR,
    SYS_MKDIR, SYS_UNLINK, SYS_RMDIR, SYS_RENAME, SYS_CHDIR, SYS_GETCWD, SYS_PIPE, SYS_DUP, SYS_DUP2,
    SYS_IOCTL, SYS_FTRUNCATE, SYS_STATFS, SYS_POLL, SYS_FSYNC, SYS_MOUNTS,
    SYS_WIN_CREATE = 50, SYS_WIN_DESTROY, SYS_WIN_UPDATE, SYS_WIN_SET_TITLE, SYS_WIN_RESIZE,
    SYS_WIN_ACTION, SYS_GUI_EVENT, SYS_SCREEN_INFO, SYS_THEME_GET, SYS_THEME_SET, SYS_WM_CONFIG_GET,
    SYS_WM_CONFIG_SET, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET, SYS_NOTIFY, SYS_SET_RESOLUTION,
    SYS_WIN_GET_RECT, SYS_WIN_SET_CURSOR, SYS_WIN_MOVE, SYS_LAUNCH,
    SYS_SOCKET = 80, SYS_CONNECT, SYS_BIND, SYS_SENDTO, SYS_RECVFROM, SYS_NET_INFO, SYS_NET_PING,
    SYS_NET_RESOLVE, SYS_LISTEN, SYS_ACCEPT, SYS_NET_CONFIG,
    SYS_BEEP = 100, SYS_KBD_LAYOUT, SYS_MOUSE_CONFIG, SYS_DMESG, SYS_PCI_INFO, SYS_SOUND_PLAY,
    SYS_AUDIO_VOLUME, SYS_MKFS, SYS_SETTIME,
    SYS_MAX = 128
};

/* errno values (Linux compatible numbering) */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ENOTSOCK 88
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ENETDOWN 100
#define ENETUNREACH 101
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

/* open flags */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* file types */
#define FT_FILE 1
#define FT_DIR  2
#define FT_CHR  3
#define FT_PIPE 4
#define FT_SOCK 5

typedef struct {
    uint64_t ino;
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t dev;
    uint32_t reserved;
} kstat_t;

typedef struct {
    uint64_t ino;
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
    uint64_t mtime;
    char name[256];
} kdirent_t;

typedef struct {
    uint64_t total_bytes, free_bytes;
    uint32_t block_size;
    char fstype[16];
} kstatfs_t;

typedef struct {
    char path[64];
    char fstype[16];
    char device[32];
    uint64_t total_bytes, free_bytes;
} kmount_t;

/* process info */
#define PS_READY 0
#define PS_RUNNING 1
#define PS_BLOCKED 2
#define PS_SLEEPING 3
#define PS_ZOMBIE 4

typedef struct {
    int32_t pid, ppid;
    int32_t state;
    int32_t cpu_percent;
    uint64_t cpu_ms;
    uint64_t start_ms;
    uint64_t mem_bytes;
    uint32_t is_kernel;
    uint32_t windows;
    char name[32];
} kprocinfo_t;

typedef struct {
    uint64_t mem_total, mem_free, mem_kernel_heap;
    uint64_t uptime_ms;
    uint32_t nprocs;
    uint32_t screen_w, screen_h;
    char cpu_vendor[16];
    char cpu_brand[64];
    char os_name[32];
    char os_version[32];
    char bootloader[64];
    uint64_t cpu_mhz;
} ksysinfo_t;

/* spawn flags */
#define SPAWN_DETACH 1          /* child is not waited for by the parent (reaped automatically) */

/* ioctl requests */
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGMODE  0x5401       /* get tty mode flags (TTY_*) */
#define TIOCSMODE  0x5402       /* set tty mode flags */
#define TIOCSPGRP  0x5410       /* set foreground process */
#define TIOCPTYNEW 0x5420       /* on /dev/ptmx: returns slave fd via arg */
#define FIONREAD   0x541B

#define TTY_ECHO   1
#define TTY_ICANON 2
#define TTY_ISIG   4

typedef struct { uint16_t rows, cols, xpixel, ypixel; } kwinsize_t;

/* signals (kill) */
#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15

/* poll */
#define POLLIN  1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
typedef struct { int32_t fd; int16_t events, revents; } kpollfd_t;
#define POLL_GUI 1              /* flag: also return when a GUI event is pending */

/* power */
#define POWER_OFF 0
#define POWER_REBOOT 1

typedef struct {
    uint8_t bus, dev, func, class_code, subclass, prog_if, irq, reserved;
    uint16_t vendor, device;
    char description[48];
} kpciinfo_t;
