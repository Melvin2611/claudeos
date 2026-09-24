#pragma once
/* ClaudeOS specific system interfaces */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <claudeos/abi.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* process control */
int spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3], int flags);
int spawnv(const char *path, char *const argv[]);          /* inherits stdio, uses environ */
int proc_info(int index, kprocinfo_t *out);                /* 1 = filled, 0 = end */
int sys_info(ksysinfo_t *out);
uint64_t uptime_ms(void);
int msleep(unsigned ms);
int sched_yield(void);
int power(int what);
long dmesg(char *buf, size_t len, size_t off);
int beep(unsigned freq, unsigned ms);
int poll(kpollfd_t *fds, int nfds, int timeout_ms);
int poll_ex(kpollfd_t *fds, int nfds, int timeout_ms, int flags);
int ioctl(int fd, unsigned long req, void *arg);
int mount_info(int index, kmount_t *out);
int statfs(const char *path, kstatfs_t *out);
int readdir_raw(int fd, kdirent_t *out);
int set_time(uint64_t epoch);
int kbd_layout(const char *set, char *get, size_t n);      /* set layout (or NULL), read current */
int pci_info(int index, kpciinfo_t *out);
int mkfs_disk(const char *device, const char *label);

/* human friendly sizes */
void format_size(uint64_t bytes, char *out, size_t n);
