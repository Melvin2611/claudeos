#pragma once
#include <kernel.h>
#include <sched.h>
#include <vfs.h>

#define MAX_ARGS 64
#define MAX_ARG_BYTES (64 * 1024)
#define USER_MMAP_BASE 0x0000100000000000ULL
#define O_CLOEXEC_K 0x80000          /* Linux O_CLOEXEC, kept in file flags */

/* spawn a user process. argv/envp are kernel pointers (NULL-terminated arrays).
 * stdio: files for fd 0..2 (NULL -> /dev/null); references are taken. */
int proc_spawn(const char *path, char *const argv[], char *const envp[], file_t *stdio[3],
               const char *cwd, int ppid, int flags);
NORETURN void proc_exit(int code);
NORETURN void thread_exit(int code);
int proc_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_top, uint64_t tid_ptr, uint64_t tls,
                       const regs_t *clone_regs);
int futex_wait(uint64_t addr, uint32_t val, int64_t timeout_ms);
int futex_wake(uint64_t cr3, uint64_t addr, int n);
int proc_kill(int pid, int sig);
int proc_waitpid(int pid, int *status, int flags);
long proc_sbrk(long incr);
void proc_fault(regs_t *r, const char *what);
bool proc_demand_page(uint64_t addr, bool write);

/* memory regions created by mmap (Linux programs) */
#define VMA_READ  1
#define VMA_WRITE 2
#define VMA_EXEC  4
typedef struct vma {
    uint64_t start, end;
    int prot;
    struct vma *next;
} vma_t;

typedef struct ksigaction {
    uint64_t handler, flags, restorer, mask;
} ksigaction_t;

/* image loading shared by spawn and execve */
typedef struct {
    uint64_t entry, phdr, phent, phnum, base;
    bool linux_abi;
} exec_info_t;

int proc_fork(regs_t *r);
int proc_execve(regs_t *r, const char *path, char *const argv[], char *const envp[]);
uint64_t proc_mmap(uint64_t addr, uint64_t len, int prot, bool fixed, bool *ok);
int proc_munmap(uint64_t addr, uint64_t len);
int proc_mprotect(uint64_t addr, uint64_t len, int prot);
void linux_send_signal(task_t *t, int sig);
bool linux_signal_pending(task_t *t);
void linux_deliver_signals(regs_t *r);

/* hooks for other subsystems */
void gui_proc_exit(task_t *t);        /* window server: destroy the windows of t */
