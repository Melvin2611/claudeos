#pragma once
#include <kernel.h>
#include <sched.h>
#include <vfs.h>

#define MAX_ARGS 64
#define MAX_ARG_BYTES (64 * 1024)
#define USER_MMAP_BASE 0x0000100000000000ULL

/* spawn a user process. argv/envp are kernel pointers (NULL-terminated arrays).
 * stdio: files for fd 0..2 (NULL -> /dev/null); references are taken. */
int proc_spawn(const char *path, char *const argv[], char *const envp[], file_t *stdio[3],
               const char *cwd, int ppid, int flags);
NORETURN void proc_exit(int code);
int proc_kill(int pid, int sig);
int proc_waitpid(int pid, int *status, int flags);
long proc_sbrk(long incr);
void proc_fault(regs_t *r, const char *what);
bool proc_demand_page(uint64_t addr, bool write);

/* hooks for other subsystems */
void gui_proc_exit(task_t *t);        /* window server: destroy the windows of t */
