; Application processor start-up code. It is copied to physical 0x8000 and entered
; in real mode after an INIT-SIPI-SIPI sequence. It switches straight to long mode
; using the kernel page tables (which temporarily identity-map the first 2 MiB),
; then jumps to ap_entry(cpu) on the stack prepared by the boot CPU.

TRAMPOLINE_PHYS equ 0x8000
%define TR(x) ((x) - ap_trampoline_start + TRAMPOLINE_PHYS)

section .rodata
global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_data

bits 16
ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    lgdt [TR(tr_gdtr)]
    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)     ; PAE | PGE
    mov cr4, eax
    mov eax, [TR(tr_cr3)]
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)    ; LME | NXE
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16) | 1   ; PG | WP | PE
    mov cr0, eax
    jmp dword 0x08:TR(tr_long)

bits 64
default abs
tr_long:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [TR(tr_stack)]
    mov rdi, [TR(tr_arg)]
    mov rax, [TR(tr_entry)]
    xor rbp, rbp
    push 0                          ; fake return address, keeps the ABI alignment
    jmp rax

align 16
tr_gdt:
    dq 0
    dq 0x00AF9A000000FFFF           ; 0x08 64-bit code
    dq 0x00CF92000000FFFF           ; 0x10 data
tr_gdtr:
    dw 23
    dd TR(tr_gdt)

align 8
ap_trampoline_data:
tr_cr3:   dq 0
tr_stack: dq 0
tr_arg:   dq 0
tr_entry: dq 0
ap_trampoline_end:
