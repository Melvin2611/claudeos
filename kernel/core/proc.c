/* User processes: ELF loading, spawn, exit, wait, kill, heap and stack growth */
#include <kernel.h>
#include <proc.h>
#include <mm.h>
#include <cpu.h>
#include "drivers.h"

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

#define PT_INTERP 3
#define PT_PHDR 6
#define ET_DYN 3
#define PIE_BASE 0x10000000ULL

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} elf64_shdr_t;

/* ClaudeOS programs carry a ".note.claudeos" section (added by crt0); everything else is
 * treated as a Linux program */
static bool elf_is_native(const uint8_t *data, size_t size) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (!eh->shoff || !eh->shnum || eh->shstrndx >= eh->shnum || eh->shentsize != sizeof(elf64_shdr_t)) return false;
    if (eh->shoff + (uint64_t)eh->shnum * sizeof(elf64_shdr_t) > size) return false;
    const elf64_shdr_t *sh = (const elf64_shdr_t *)(data + eh->shoff);
    uint64_t strtab = sh[eh->shstrndx].offset, strsz = sh[eh->shstrndx].size;
    if (strtab + strsz > size) return false;
    for (int i = 0; i < eh->shnum; i++) {
        if (sh[i].name + 15 > strsz) continue;
        if (!memcmp(data + strtab + sh[i].name, ".note.claudeos", 15)) return true;
    }
    return false;
}

static int elf_load(task_t *t, const uint8_t *data, size_t size, exec_info_t *ei) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 62)
        return -ENOEXEC;
    if (eh->type != 2 && eh->type != ET_DYN) return -ENOEXEC;
    if (eh->phentsize != sizeof(elf64_phdr_t)) return -ENOEXEC;
    if (eh->phoff + (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > size) return -ENOEXEC;
    memset(ei, 0, sizeof(*ei));
    ei->linux_abi = !elf_is_native(data, size);
    uint64_t base = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->phoff + i * eh->phentsize);
        if (ph->type == PT_INTERP) {
            klog("[proc] dynamically linked programs are not supported (need a static binary)\n");
            return -ENOEXEC;
        }
    }
    if (eh->type == ET_DYN) base = PIE_BASE;
    uint64_t max_end = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->phoff + i * eh->phentsize);
        if (ph->type == PT_PHDR) ei->phdr = base + ph->vaddr;
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        uint64_t vaddr = base + ph->vaddr;
        if (vaddr < USER_LOAD_MIN || vaddr + ph->memsz >= USER_MMAP_BASE || ph->filesz > ph->memsz ||
            ph->offset + ph->filesz > size)
            return -ENOEXEC;
        if (!ei->phdr && ph->offset == 0) ei->phdr = vaddr + eh->phoff;
        bool w = ph->flags & PF_W, x = ph->flags & PF_X;
        uint64_t start = PAGE_ALIGN_DOWN(vaddr), end = PAGE_ALIGN_UP(vaddr + ph->memsz);
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            uint8_t *page = map_user_page(t, va, w, x);
            if (!page) return -ENOMEM;
            uint64_t seg_file_end = vaddr + ph->filesz;
            uint64_t cs = MAX(va, vaddr), ce = MIN(va + PAGE_SIZE, seg_file_end);
            if (cs < ce) memcpy(page + (cs - va), data + ph->offset + (cs - vaddr), ce - cs);
        }
        if (end > max_end) max_end = end;
    }
    if (!max_end) return -ENOEXEC;
    t->brk_start = t->brk = max_end;
    ei->entry = base + eh->entry;
    ei->phent = eh->phentsize;
    ei->phnum = eh->phnum;
    ei->base = base;
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

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

/* initial stack: strings, then argc, argv[], NULL, envp[], NULL, auxv[] (Linux ABI layout) */
static int setup_stack(task_t *t, char *const argv[], char *const envp[], const exec_info_t *ei,
                       const char *execfn, uint64_t *sp_out, uint64_t *argv_out, uint64_t *envp_out, int *argc_out) {
    uint64_t low = USER_STACK_TOP - 64 * 1024;
    for (uint64_t va = low; va < USER_STACK_TOP; va += PAGE_SIZE)
        if (!map_user_page(t, va, true, false)) return -ENOMEM;
    t->stack_low = low;

    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    uint64_t sp = USER_STACK_TOP;
    uint64_t *ptrs = kmalloc(sizeof(uint64_t) * (argc + envc + 2));
    /* platform string, exec name and random bytes for the C library */
    sp -= 7;
    copy_to_space(t, sp, "x86_64", 7);
    uint64_t platform = sp;
    size_t fl = strlen(execfn) + 1;
    sp -= fl;
    copy_to_space(t, sp, execfn, fl);
    uint64_t execfn_addr = sp;
    uint64_t rnd[2] = { krandom(), krandom() };
    sp = (sp - 16) & ~15ULL;
    copy_to_space(t, sp, rnd, 16);
    uint64_t random_addr = sp;
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
    uint64_t aux[] = {
        AT_PHDR, ei->phdr, AT_PHENT, ei->phent, AT_PHNUM, ei->phnum, AT_PAGESZ, PAGE_SIZE,
        AT_BASE, 0, AT_FLAGS, 0, AT_ENTRY, ei->entry, AT_UID, 1000, AT_EUID, 1000, AT_GID, 1000,
        AT_EGID, 1000, AT_PLATFORM, platform, AT_HWCAP, 0x178BFBFF, AT_CLKTCK, 100, AT_SECURE, 0,
        AT_RANDOM, random_addr, AT_EXECFN, execfn_addr, AT_NULL, 0,
    };
    size_t naux = ARRAY_SIZE(aux);
    size_t words = 1 + (argc + 1) + (envc + 1) + naux;
    if (words & 1) sp -= 8;
    sp -= words * 8;
    uint64_t p = sp;
    uint64_t v = argc;
    copy_to_space(t, p, &v, 8); p += 8;
    uint64_t argv_addr = p;
    copy_to_space(t, p, ptrs, (argc + 1) * 8); p += (argc + 1) * 8;
    uint64_t envp_addr = p;
    copy_to_space(t, p, ptrs + argc + 1, (envc + 1) * 8); p += (envc + 1) * 8;
    copy_to_space(t, p, aux, naux * 8);
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

/* find and read an executable: bare names come from /bin */
static uint8_t *read_exec(const char *path, const char *cwd, char *abs, size_t *size, int *err) {
    uint8_t *img = 0;
    *err = 0;
    if (!strchr(path, '/')) {
        snprintf(abs, PATH_MAX_LEN, "/bin/%s", path);
        img = vfs_read_all(abs, size);
        if (img) return img;
    }
    if (vfs_normalize(cwd, path, abs) < 0) { *err = -ENAMETOOLONG; return 0; }
    kstat_t st;
    int r = vfs_stat(abs, &st);
    if (r < 0) { *err = r; return 0; }
    if (st.type == FT_DIR) { *err = -EISDIR; return 0; }
    img = vfs_read_all(abs, size);
    if (!img) *err = -ENOENT;
    return img;
}

int proc_spawn(const char *path, char *const argv[], char *const envp[], file_t *stdio[3],
               const char *cwd, int ppid, int flags) {
    char abs[PATH_MAX_LEN];
    size_t size = 0;
    int err;
    uint8_t *img = read_exec(path, cwd ? cwd : "/", abs, &size, &err);
    if (!img) return err;
    const char *base = strrchr(abs, '/');
    base = base ? base + 1 : abs;

    task_t *t = task_alloc(base);
    if (!t) { kfree(img); return -ENOMEM; }
    t->is_user = true;
    t->ppid = ppid;
    t->waited = (flags & SPAWN_DETACH) != 0;
    t->cr3 = vmm_new_space();
    t->mmap_next = USER_MMAP_BASE;
    t->umask = 022;
    strlcpy(t->cwd, cwd ? cwd : "/", sizeof(t->cwd));
    exec_info_t ei;
    int r = elf_load(t, img, size, &ei);
    kfree(img);
    uint64_t sp = 0, uargv = 0, uenvp = 0;
    int argc = 0;
    if (r == 0) r = setup_stack(t, argv, envp, &ei, abs, &sp, &uargv, &uenvp, &argc);
    if (r < 0) {
        vmm_free_space(t->cr3);
        t->cr3 = kernel_pml4;
        task_free(t);
        return r;
    }
    t->linux_abi = ei.linux_abi;
    for (int i = 0; i < 3; i++) {
        file_t *f = stdio ? stdio[i] : 0;
        if (f) file_ref(f);
        else f = open_dev(i == 0 ? "/dev/null" : "/dev/kmsg", i == 0 ? O_RDONLY : O_WRONLY);
        t->fds[i] = f;
    }

    if (ei.linux_abi) setup_user_frame(t, ei.entry, sp, 0, 0, 0);
    else setup_user_frame(t, ei.entry, sp, argc, uargv, uenvp);
    klog("[proc] spawned pid %d: %s%s\n", t->pid, abs, ei.linux_abi ? " (Linux)" : "");
    sched_add(t);
    return t->pid;
}

static void free_vmas(task_t *t) {
    for (vma_t *v = t->vmas, *n; v; v = n) { n = v->next; kfree(v); }
    t->vmas = 0;
}

void proc_release_resources(task_t *t) {
    if (t->leader != t) return;       /* threads share the leader's address space */
    free_vmas(t);
    kfree(t->sigact);
    t->sigact = 0;
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
    if (t->linux_abi) { linux_send_signal(t, sig); return 0; }
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
        if (task_interrupted(current)) { irq_restore(f); return -EINTR; }
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
    if (task_interrupted(current)) { irq_restore(f); return -EINTR; }
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
    for (vma_t *v = t->vmas; v; v = v->next) {
        if (addr < v->start || addr >= v->end) continue;
        if (!(v->prot & (VMA_READ | VMA_WRITE | VMA_EXEC)) || (write && !(v->prot & VMA_WRITE))) return false;
        return map_user_page(t, PAGE_ALIGN_DOWN(addr), (v->prot & VMA_WRITE) != 0, (v->prot & VMA_EXEC) != 0) != 0;
    }
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

/* ------------------------------------------------------------------ memory mappings */

static void vma_insert(task_t *t, uint64_t start, uint64_t end, int prot) {
    vma_t *v = kzalloc(sizeof(vma_t));
    v->start = start;
    v->end = end;
    v->prot = prot;
    vma_t **pp = &t->vmas;
    while (*pp && (*pp)->start < start) pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
}

/* cut [start, end) out of the vma list; optionally re-add it with a new protection */
static void vma_carve(task_t *t, uint64_t start, uint64_t end) {
    for (vma_t **pp = &t->vmas; *pp;) {
        vma_t *v = *pp;
        if (v->end <= start || v->start >= end) { pp = &v->next; continue; }
        if (v->start < start && v->end > end) {
            vma_t *tail = kzalloc(sizeof(vma_t));
            tail->start = end;
            tail->end = v->end;
            tail->prot = v->prot;
            tail->next = v->next;
            v->end = start;
            v->next = tail;
            return;
        }
        if (v->start < start) { v->end = start; pp = &v->next; continue; }
        if (v->end > end) { v->start = end; pp = &v->next; continue; }
        *pp = v->next;
        kfree(v);
    }
}

static void unmap_range(task_t *t, uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(t->cr3, va);
        if (!(pte & PTE_P)) continue;
        pte = vmm_unmap(t->cr3, va);
        if ((pte & PTE_P) && (pte & PTE_OWNED)) { pmm_free(pte & PTE_ADDR); t->user_pages--; }
    }
}

static bool range_free(task_t *t, uint64_t start, uint64_t end) {
    for (vma_t *v = t->vmas; v; v = v->next)
        if (v->start < end && v->end > start) return false;
    return true;
}

/* reserve an anonymous region (pages appear on first access) */
uint64_t proc_mmap(uint64_t addr, uint64_t len, int prot, bool fixed, bool *ok) {
    task_t *t = PROC(current);
    *ok = false;
    len = PAGE_ALIGN_UP(len);
    if (!len || len > (1ULL << 40)) return 0;
    uint64_t start;
    if (fixed) {
        if (addr & 0xFFF) return 0;
        start = addr;
        if (start < USER_LOAD_MIN || start + len > USER_STACK_TOP - USER_STACK_MAX) return 0;
        vma_carve(t, start, start + len);
        unmap_range(t, start, start + len);
    } else {
        start = addr && !(addr & 0xFFF) && addr >= USER_MMAP_BASE && range_free(t, addr, addr + len) ? addr : 0;
        if (!start) {
            start = PAGE_ALIGN_UP(t->mmap_next);
            while (!range_free(t, start, start + len)) start += len;
            t->mmap_next = start + len;
        }
        if (start + len > USER_STACK_TOP - USER_STACK_MAX) return 0;
    }
    vma_insert(t, start, start + len, prot);
    *ok = true;
    return start;
}

int proc_munmap(uint64_t addr, uint64_t len) {
    task_t *t = PROC(current);
    if (addr & 0xFFF) return -EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (addr < USER_MMAP_BASE && addr < t->brk) return 0;   /* never unmap the program image */
    vma_carve(t, addr, addr + len);
    unmap_range(t, addr, addr + len);
    return 0;
}

int proc_mprotect(uint64_t addr, uint64_t len, int prot) {
    task_t *t = PROC(current);
    if (addr & 0xFFF) return -EINVAL;
    uint64_t end = addr + PAGE_ALIGN_UP(len);
    bool in_vma = false;
    for (vma_t *v = t->vmas; v; v = v->next) if (v->start < end && v->end > addr) in_vma = true;
    if (in_vma) {
        vma_carve(t, addr, end);
        vma_insert(t, addr, end, prot);
    }
    /* adjust pages that already exist */
    for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(t->cr3, va);
        if (!(pte & PTE_P) || !(pte & PTE_U)) continue;
        uint64_t nf = (pte & ~PTE_ADDR & ~(PTE_W | PTE_NX)) | ((prot & VMA_WRITE) ? PTE_W : 0);
        if (!(prot & VMA_EXEC) && cpu_has_nx()) nf |= PTE_NX;
        vmm_map(t->cr3, va, pte & PTE_ADDR, nf & ~PTE_P);
    }
    return 0;
}

/* ------------------------------------------------------------------ fork / execve */

/* duplicate the calling process (full copy of its memory); returns the child's pid */
int proc_fork(regs_t *r) {
    task_t *p = PROC(current);
    task_t *c = task_alloc(p->name);
    if (!c) return -ENOMEM;
    c->is_user = true;
    c->ppid = p->pid;
    c->cr3 = vmm_new_space();
    if (!c->cr3 || vmm_clone_user(p->cr3, c->cr3) < 0) {
        if (c->cr3) vmm_free_space(c->cr3);
        c->cr3 = kernel_pml4;
        task_free(c);
        return -ENOMEM;
    }
    c->brk_start = p->brk_start;
    c->brk = p->brk;
    c->mmap_next = p->mmap_next;
    c->stack_low = p->stack_low;
    c->user_pages = p->user_pages;
    c->linux_abi = p->linux_abi;
    c->umask = p->umask;
    c->fs_base = current->fs_base;
    c->gs_base = current->gs_base;
    c->sigmask = current->sigmask;
    strlcpy(c->cwd, p->cwd, sizeof(c->cwd));
    for (vma_t *v = p->vmas; v; v = v->next) vma_insert(c, v->start, v->end, v->prot);
    if (p->sigact) {
        c->sigact = kmalloc(65 * sizeof(ksigaction_t));
        memcpy(c->sigact, p->sigact, 65 * sizeof(ksigaction_t));
    }
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) { file_ref(p->fds[i]); c->fds[i] = p->fds[i]; }
    }
    fpu_save(current->fpu);
    memcpy(c->fpu, current->fpu, fpu_size);
    setup_user_frame(c, 0, 0, 0, 0, 0);
    regs_t *frame = (regs_t *)(c->kstack_top - sizeof(regs_t));
    *frame = *r;
    frame->rax = 0;
    sched_add(c);
    return c->pid;
}

/* replace the program of the calling process; on success the syscall "returns" into it */
int proc_execve(regs_t *r, const char *path, char *const argv[], char *const envp[]) {
    task_t *t = PROC(current);
    if (current != t || t->nthreads > 1) return -EAGAIN;
    char abs[PATH_MAX_LEN];
    size_t size;
    int err;
    uint8_t *img = read_exec(path, t->cwd, abs, &size, &err);
    if (!img) return err;
    /* build the new address space in a scratch task first */
    task_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.cr3 = vmm_new_space();
    tmp.leader = &tmp;
    exec_info_t ei;
    int res = elf_load(&tmp, img, size, &ei);
    kfree(img);
    uint64_t sp = 0, uargv = 0, uenvp = 0;
    int argc = 0;
    if (res == 0) res = setup_stack(&tmp, argv, envp, &ei, abs, &sp, &uargv, &uenvp, &argc);
    if (res < 0) {
        vmm_free_space(tmp.cr3);
        return res;
    }
    uint64_t old = t->cr3;
    t->cr3 = tmp.cr3;
    write_cr3(t->cr3);
    vmm_free_space(old);
    free_vmas(t);
    t->brk_start = tmp.brk_start;
    t->brk = tmp.brk;
    t->stack_low = tmp.stack_low;
    t->user_pages = tmp.user_pages;
    t->mmap_next = USER_MMAP_BASE;
    t->linux_abi = ei.linux_abi;
    t->fs_base = 0;
    wrmsr(0xC0000100, 0);
    t->clear_tid = 0;
    kfree(t->sigact);
    t->sigact = 0;
    t->sigpending = 0;
    const char *base = strrchr(abs, '/');
    strlcpy(t->name, base ? base + 1 : abs, sizeof(t->name));
    /* close-on-exec descriptors */
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i] && (t->fds[i]->flags & O_CLOEXEC_K)) {
            file_close(t->fds[i]);
            t->fds[i] = 0;
        }
    }
    memcpy(current->fpu, fpu_initial_state, fpu_size);
    fpu_restore(current->fpu);
    memset(r, 0, offsetof(regs_t, vector));
    r->rip = ei.entry;
    r->rsp = sp;
    r->rflags = 0x202;
    r->cs = USER_CS;
    r->ss = USER_DS;
    if (!ei.linux_abi) { r->rdi = argc; r->rsi = uargv; r->rdx = uenvp; }
    klog("[proc] pid %d exec %s%s\n", t->pid, abs, ei.linux_abi ? " (Linux)" : "");
    return 0;
}
