/* User processes: ELF loading, spawn, exit, wait, kill, heap and stack growth */
#include <kernel.h>
#include <proc.h>
#include <mm.h>
#include <cpu.h>

#define EI_NIDENT 16
typedef struct {
    uint8_t ident[EI_NIDENT];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr_t;

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

extern void uthread_trampoline(void);
static void setup_user_frame(task_t *t, uint64_t rip, uint64_t rsp, uint64_t rdi, uint64_t rsi, uint64_t rdx);

static uint64_t user_flags(bool write, bool exec) {
    uint64_t f = PTE_P | PTE_U | PTE_OWNED;
    if (write) f |= PTE_W;
    if (!exec && cpu_has_nx()) f |= PTE_NX;
    return f;
}

/* map a zeroed user page at va in address space pml4; returns its kernel pointer */
static void *map_user_page(task_t *t, uint64_t va, bool write, bool exec) {
    uint64_t pte = vmm_get_pte(t->cr3, va);
    if (pte & PTE_P) {
        /* already mapped by an overlapping segment: widen permissions */
        uint64_t pa = pte & PTE_ADDR;
        uint64_t nf = pte & ~PTE_ADDR;
        if (write) nf |= PTE_W;
        if (exec) nf &= ~PTE_NX;
        vmm_map(t->cr3, va, pa, nf & ~PTE_P);
        return P2V(pa);
    }
    uint64_t pa = pmm_alloc_zero();
    if (!pa) return 0;
    if (!vmm_map(t->cr3, va, pa, user_flags(write, exec))) { pmm_free(pa); return 0; }
    t->user_pages++;
    return P2V(pa);
}

static int elf_load(task_t *t, const uint8_t *data, size_t size, uint64_t *entry) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 62)
        return -ENOEXEC;
    if (eh->type != 2) return -ENOEXEC;   /* ET_EXEC only */
    if (eh->phoff + (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > size) return -ENOEXEC;
    uint64_t max_end = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->phoff + i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->vaddr < USER_LOAD_MIN || ph->vaddr + ph->memsz >= USER_MMAP_BASE ||
            ph->filesz > ph->memsz || ph->offset + ph->filesz > size)
            return -ENOEXEC;
        bool w = ph->flags & PF_W, x = ph->flags & PF_X;
        uint64_t start = PAGE_ALIGN_DOWN(ph->vaddr), end = PAGE_ALIGN_UP(ph->vaddr + ph->memsz);
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            uint8_t *page = map_user_page(t, va, w, x);
            if (!page) return -ENOMEM;
            /* copy the part of the file image that falls into this page */
            uint64_t seg_file_end = ph->vaddr + ph->filesz;
            uint64_t cs = MAX(va, ph->vaddr), ce = MIN(va + PAGE_SIZE, seg_file_end);
            if (cs < ce) memcpy(page + (cs - va), data + ph->offset + (cs - ph->vaddr), ce - cs);
        }
        if (end > max_end) max_end = end;
    }
    if (!max_end) return -ENOEXEC;
    t->brk_start = t->brk = max_end;
    *entry = eh->entry;
    return 0;
}

/* copy bytes into the (not current) address space of t */
static int copy_to_space(task_t *t, uint64_t va, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        uint64_t pa = vmm_translate(t->cr3, va);
        if (!pa) return -EFAULT;
        size_t chunk = MIN(n, PAGE_SIZE - (va & 0xFFF));
        memcpy(P2V(pa), s, chunk);
        va += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

static int setup_stack(task_t *t, char *const argv[], char *const envp[], uint64_t *sp_out,
                       uint64_t *argv_out, uint64_t *envp_out, int *argc_out) {
    uint64_t low = USER_STACK_TOP - 64 * 1024;
    for (uint64_t va = low; va < USER_STACK_TOP; va += PAGE_SIZE)
        if (!map_user_page(t, va, true, false)) return -ENOMEM;
    t->stack_low = low;

    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    uint64_t sp = USER_STACK_TOP;
    uint64_t *ptrs = kmalloc(sizeof(uint64_t) * (argc + envc + 2));
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp[i]) + 1;
        sp -= l;
        copy_to_space(t, sp, envp[i], l);
        ptrs[argc + 1 + i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        sp -= l;
        copy_to_space(t, sp, argv[i], l);
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;
    ptrs[argc + 1 + envc] = 0;
    sp &= ~15ULL;
    /* layout: argc, argv[], NULL, envp[], NULL, auxv(0,0) ; keep rsp 16-aligned */
    size_t words = 1 + (argc + 1) + (envc + 1) + 2;
    if (words & 1) sp -= 8;
    sp -= words * 8;
    uint64_t p = sp;
    uint64_t v = argc;
    copy_to_space(t, p, &v, 8); p += 8;
    uint64_t argv_addr = p;
    copy_to_space(t, p, ptrs, (argc + 1) * 8); p += (argc + 1) * 8;
    uint64_t envp_addr = p;
    copy_to_space(t, p, ptrs + argc + 1, (envc + 1) * 8); p += (envc + 1) * 8;
    uint64_t zero[2] = { 0, 0 };
    copy_to_space(t, p, zero, 16);
    kfree(ptrs);
    *sp_out = sp;
    *argv_out = argv_addr;
    *envp_out = envp_addr;
    *argc_out = argc;
    return 0;
}

static file_t *open_dev(const char *path, int flags) {
    file_t *f = 0;
    vfs_open(path, flags, &f);
    return f;
}

int proc_spawn(const char *path, char *const argv[], char *const envp[], file_t *stdio[3],
               const char *cwd, int ppid, int flags) {
    char abs[PATH_MAX_LEN];
    size_t size = 0;
    uint8_t *img = 0;
    if (!strchr(path, '/')) {
        snprintf(abs, sizeof(abs), "/bin/%s", path);
        img = vfs_read_all(abs, &size);
    }
    if (!img) {
        if (vfs_normalize(cwd, path, abs) < 0) return -ENAMETOOLONG;
        kstat_t st;
        int r = vfs_stat(abs, &st);
        if (r < 0) return r;
        if (st.type == FT_DIR) return -EISDIR;
        img = vfs_read_all(abs, &size);
        if (!img) return -ENOENT;
    }
    const char *base = strrchr(abs, '/');
    base = base ? base + 1 : abs;

    task_t *t = task_alloc(base);
    if (!t) { kfree(img); return -ENOMEM; }
    t->is_user = true;
    t->ppid = ppid;
    t->waited = (flags & SPAWN_DETACH) != 0;
    t->cr3 = vmm_new_space();
    t->mmap_next = USER_MMAP_BASE;
    strlcpy(t->cwd, cwd ? cwd : "/", sizeof(t->cwd));
    uint64_t entry;
    int r = elf_load(t, img, size, &entry);
    kfree(img);
    uint64_t sp = 0, uargv = 0, uenvp = 0;
    int argc = 0;
    if (r == 0) r = setup_stack(t, argv, envp, &sp, &uargv, &uenvp, &argc);
    if (r < 0) {
        vmm_free_space(t->cr3);
        t->cr3 = kernel_pml4;
        task_free(t);
        return r;
    }
    for (int i = 0; i < 3; i++) {
        file_t *f = stdio ? stdio[i] : 0;
        if (f) file_ref(f);
        else f = open_dev(i == 0 ? "/dev/null" : "/dev/kmsg", i == 0 ? O_RDONLY : O_WRONLY);
        t->fds[i] = f;
    }

    setup_user_frame(t, entry, sp, argc, uargv, uenvp);
    klog("[proc] spawned pid %d: %s\n", t->pid, abs);
    sched_add(t);
    return t->pid;
}

void proc_release_resources(task_t *t) {
    if (t->leader != t) return;       /* threads share the leader's address space */
    if (t->cr3 && t->cr3 != kernel_pml4) {
        vmm_free_space(t->cr3);
        t->cr3 = kernel_pml4;
    }
}

__attribute__((weak)) void gui_proc_exit(task_t *t) { UNUSED(t); }

/* ask every other thread of the group to terminate */
static void kill_other_threads(task_t *leader, task_t *except) {
    for (task_t *th = task_list(); th; th = th->all_next) {
        if (th->leader != leader || th == except || th->state == T_DEAD || th->state == T_ZOMBIE) continue;
        th->killed = true;
        task_wake(th);
    }
}

int futex_wake(uint64_t cr3, uint64_t addr, int n);

/* terminate the calling thread only (the process lives on) */
NORETURN void thread_exit(int code) {
    task_t *self = current, *L = PROC(self);
    if (self == L) {
        /* the main thread: wait for the other threads, then end the process */
        while (L->nthreads > 1 && !L->group_exit) wq_wait_timeout(&L->thread_wq, 100);
        proc_exit(L->group_exit ? L->exit_code : code);
    }
    self->exit_code = code;
    if (self->clear_tid) {
        uint32_t zero = 0;
        if (copy_to_user((void *)self->clear_tid, &zero, 4) == 0) futex_wake(self->cr3, self->clear_tid, 0x7FFFFFFF);
    }
    sched_exit(T_DEAD);               /* the reaper updates the leader's thread count */
}

/* terminate the whole process (exit_group semantics) */
NORETURN void proc_exit(int code) {
    task_t *self = current, *t = PROC(self);
    if (!t->group_exit) {
        t->group_exit = true;
        t->exit_code = code;
    }
    if (t->nthreads > 1 || self != t) {
        kill_other_threads(t, self);
        if (self != t) {
            /* the leader finishes the teardown once all threads are gone */
            t->killed = true;
            task_wake(t);
            thread_exit(code);
        }
        while (t->nthreads > 1) {
            kill_other_threads(t, self);
            wq_wait_timeout(&t->thread_wq, 50);
        }
    }
    gui_proc_exit(t);
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i]) {
            file_close(t->fds[i]);
            t->fds[i] = 0;
        }
    }
    uint64_t f = irq_save();
    /* orphan our children: they will be reaped automatically */
    task_t *dead[32];
    int ndead = 0;
    for (task_t *c = task_list(); c; c = c->all_next) {
        if (c->leader == c && c->ppid == t->pid) {
            c->ppid = 0;
            c->waited = true;
            if (c->state == T_ZOMBIE && !c->kstack && ndead < 32) dead[ndead++] = c;
        }
    }
    for (int i = 0; i < ndead; i++) task_free(dead[i]);
    task_t *parent = t->ppid ? task_find(t->ppid) : 0;
    bool keep = parent && !t->waited;
    if (parent) wq_wake_all(&parent->child_wq);
    irq_restore(f);
    poll_notify();
    sched_exit(keep ? T_ZOMBIE : T_DEAD);
}

void proc_check_killed(regs_t *r) {
    UNUSED(r);
    task_t *L = PROC(current);
    if (current != L) thread_exit(137);
    proc_exit(L->exit_code ? L->exit_code : 137);
}

int proc_kill(int pid, int sig) {
    task_t *t = task_find(pid);
    if (t) t = PROC(t);
    if (!t || t->state == T_ZOMBIE) return -ESRCH;
    if (!t->is_user) return -EPERM;
    if (!t->group_exit) t->exit_code = 128 + sig;
    t->killed = true;
    task_wake(t);
    kill_other_threads(t, 0);
    return 0;
}

/* free zombie tasks whose parent vanished (called from waitpid scans) */
static void free_zombie(task_t *c) {
    task_free(c);
}

int proc_waitpid(int pid, int *status, int flags) {
    task_t *me = PROC(current);
    for (;;) {
        uint64_t f = irq_save();
        bool have_child = false;
        for (task_t *c = task_list(); c; c = c->all_next) {
            if (c->leader != c || c->ppid != me->pid || c->waited) continue;
            if (pid > 0 && c->pid != pid) continue;
            have_child = true;
            if (c->state == T_ZOMBIE && !c->kstack) {
                int cpid = c->pid;
                if (status) *status = c->exit_code;
                c->waited = true;
                irq_restore(f);
                free_zombie(c);
                return cpid;
            }
        }
        if (!have_child) { irq_restore(f); return -ECHILD; }
        if (flags & 1) { irq_restore(f); return 0; }
        if (current->killed) { irq_restore(f); return -EINTR; }
        wq_wait_timeout(&me->child_wq, 100);
        irq_restore(f);
    }
}

/* ------------------------------------------------------------------ threads */

static void setup_user_frame(task_t *t, uint64_t rip, uint64_t rsp, uint64_t rdi, uint64_t rsi, uint64_t rdx) {
    uint64_t *ksp = (uint64_t *)t->kstack_top;
    regs_t *frame = (regs_t *)((uint8_t *)ksp - sizeof(regs_t));
    memset(frame, 0, sizeof(*frame));
    frame->rip = rip;
    frame->cs = USER_CS;
    frame->rflags = 0x202;
    frame->rsp = rsp;
    frame->ss = USER_DS;
    frame->rdi = rdi;
    frame->rsi = rsi;
    frame->rdx = rdx;
    uint64_t *s = (uint64_t *)frame;
    *--s = (uint64_t)uthread_trampoline;
    for (int i = 0; i < 6; i++) *--s = 0;   /* rbp rbx r12 r13 r14 r15 */
    t->rsp = (uint64_t)s;
}

/* create a thread in the current process; returns its id */
int proc_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_top, uint64_t tid_ptr, uint64_t tls,
                       const regs_t *clone_regs) {
    task_t *L = PROC(current);
    if (L->group_exit) return -EINTR;
    if (L->nthreads >= 512) return -EAGAIN;
    task_t *t = task_alloc(L->name);
    if (!t) return -ENOMEM;
    t->is_user = true;
    t->leader = L;
    t->cr3 = L->cr3;
    t->prio = current->prio;
    t->waited = true;
    t->clear_tid = tid_ptr;
    t->fs_base = tls ? tls : current->fs_base;
    t->gs_base = current->gs_base;
    L->nthreads++;
    if (clone_regs) {
        /* Linux clone(): the child continues at the same place with rax = 0 */
        setup_user_frame(t, 0, 0, 0, 0, 0);
        regs_t *frame = (regs_t *)(t->kstack_top - sizeof(regs_t));
        *frame = *clone_regs;
        frame->rax = 0;
        if (stack_top) frame->rsp = stack_top;
    } else {
        setup_user_frame(t, entry, (stack_top & ~15ULL) - 8, arg, 0, 0);
    }
    if (tid_ptr) {
        uint32_t tid = t->pid;
        copy_to_user((void *)tid_ptr, &tid, 4);
    }
    sched_add(t);
    return t->pid;
}

/* ------------------------------------------------------------------ futex */

#define FUTEX_BUCKETS 64
static waitq_t futex_wq[FUTEX_BUCKETS];

static waitq_t *futex_bucket(uint64_t cr3, uint64_t addr) {
    return &futex_wq[((addr >> 2) ^ (cr3 >> 12)) % FUTEX_BUCKETS];
}

/* wait while *addr == val; timeout_ms < 0: forever. 0 = woken, -EAGAIN, -ETIMEDOUT, -EINTR */
int futex_wait(uint64_t addr, uint32_t val, int64_t timeout_ms) {
    if (addr & 3) return -EINVAL;
    uint32_t cur;
    uint64_t f = irq_save();
    if (copy_from_user(&cur, (void *)addr, 4) < 0) { irq_restore(f); return -EFAULT; }
    if (cur != val) { irq_restore(f); return -EAGAIN; }
    if (current->killed) { irq_restore(f); return -EINTR; }
    task_t *me = current;
    me->futex_key = addr;
    me->futex_cr3 = me->cr3;
    bool ok = true;
    if (timeout_ms < 0) wq_wait(futex_bucket(me->cr3, addr));
    else ok = wq_wait_timeout(futex_bucket(me->cr3, addr), (uint64_t)timeout_ms);
    me->futex_key = 0;
    irq_restore(f);
    if (me->killed) return -EINTR;
    return ok ? 0 : -ETIMEDOUT;
}

int futex_wake(uint64_t cr3, uint64_t addr, int n) {
    waitq_t *wq = futex_bucket(cr3, addr);
    int woken = 0;
    uint64_t f = irq_save();
    while (woken < n) {
        /* wake the longest waiter with a matching key */
        task_t *match = 0;
        for (task_t *t = wq->head; t; t = t->wq_next)
            if (t->futex_key == addr && t->futex_cr3 == cr3) match = t;
        if (!match) break;
        match->futex_key = 0;
        task_wake(match);
        woken++;
    }
    irq_restore(f);
    return woken;
}

long proc_sbrk(long incr) {
    task_t *t = PROC(current);
    uint64_t old = t->brk;
    if (incr == 0) return (long)old;
    uint64_t nb = old + incr;
    if (incr > 0) {
        if (nb >= USER_MMAP_BASE || nb < old) return -ENOMEM;
        for (uint64_t va = PAGE_ALIGN_UP(old); va < PAGE_ALIGN_UP(nb); va += PAGE_SIZE) {
            if (!map_user_page(t, va, true, false)) return -ENOMEM;
        }
    } else {
        if (nb < t->brk_start) return -EINVAL;
        for (uint64_t va = PAGE_ALIGN_UP(nb); va < PAGE_ALIGN_UP(old); va += PAGE_SIZE) {
            uint64_t pte = vmm_unmap(t->cr3, va);
            if ((pte & PTE_P) && (pte & PTE_OWNED)) { pmm_free(pte & PTE_ADDR); t->user_pages--; }
        }
    }
    t->brk = nb;
    return (long)old;
}

bool proc_demand_page(uint64_t addr, bool write) {
    task_t *t = current;
    if (!t || !t->is_user) return false;
    t = PROC(t);
    uint64_t lim = USER_STACK_TOP - USER_STACK_MAX;
    if (addr < lim || addr >= USER_STACK_TOP) return false;
    if (addr >= t->stack_low) return false;
    for (uint64_t va = PAGE_ALIGN_DOWN(addr); va < t->stack_low; va += PAGE_SIZE)
        if (!map_user_page(t, va, true, false)) return false;
    t->stack_low = PAGE_ALIGN_DOWN(addr);
    return true;
}

void proc_fault(regs_t *r, const char *what) {
    task_t *t = current;
    char msg[160];
    snprintf(msg, sizeof(msg), "\n%s: %s at %lx (address %lx) - process terminated\n", t->name, what,
             r->rip, read_cr2());
    klog("[proc] pid %d (%s): %s at rip=%lx cr2=%lx err=%lx\n", t->pid, t->name, what, r->rip, read_cr2(),
         r->error);
    if (t->fds[2]) {
        sti();
        file_write(t->fds[2], msg, strlen(msg));
    }
    proc_exit(r->vector == 14 ? 139 : 134);
}
