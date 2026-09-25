; Interrupt entry stubs, common save/restore path and context switch.
bits 64
section .text

extern isr_dispatch

%assign i 0
%rep 256
isr_stub_%[i]:
%if i = 8 || i = 10 || i = 11 || i = 12 || i = 13 || i = 14 || i = 17 || i = 21 || i = 29 || i = 30
%else
    push 0
%endif
    push i
    jmp isr_common
%assign i i+1
%endrep

isr_common:
    ; coming from user mode: switch GS to the per-CPU block
    test qword [rsp + 24], 3
    jz .from_kernel
    swapgs
.from_kernel:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    call isr_dispatch
global trap_return
trap_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    test qword [rsp + 8], 3
    jz .to_kernel
    swapgs
.to_kernel:
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%[i]
%assign i i+1
%endrep

section .text
; void swtch(uint64_t *save_rsp, uint64_t new_rsp)
global swtch
swtch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; entry point of new kernel threads: r12 = function, r13 = argument
extern kthread_exit
extern sched_tail
global kthread_trampoline
kthread_trampoline:
    call sched_tail
    sti
    mov rdi, r13
    call r12
    mov rdi, rax
    call kthread_exit
.hang:
    hlt
    jmp .hang

; entry for new user processes: stack holds a regs_t frame
global uthread_trampoline
uthread_trampoline:
    call sched_tail
    jmp trap_return

; void gdt_flush(void *gdtr)
global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    pop rdi
    push 0x08
    push rdi
    retfq
