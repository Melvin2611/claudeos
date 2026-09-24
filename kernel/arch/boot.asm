; ClaudeOS boot code: Multiboot2 header, 32-bit bootstrap, switch to long mode,
; jump into the higher-half kernel.

KERNEL_VMA equ 0xFFFFFFFF80000000

MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0

section .multiboot
align 8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd mb2_header_end - mb2_header_start
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header_start))

    ; framebuffer request (optional): 1024x768x32
    align 8
    dw 5
    dw 1
    dd 20
    dd 1024
    dd 768
    dd 32

    ; page-align modules
    align 8
    dw 6
    dw 0
    dd 8

    ; end tag
    align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .boot.bss nobits
align 4096
boot_pml4:      resb 4096
boot_pdpt_low:  resb 4096
boot_pdpt_high: resb 4096
boot_pd:        resb 4096 * 4       ; identity map of the first 4 GiB (2 MiB pages)
boot_stack:     resb 4096
boot_stack_top:

section .boot.data
align 16
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF           ; 0x08 kernel code (64-bit)
    dq 0x00CF92000000FFFF           ; 0x10 kernel data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

msg_nolm: db "ClaudeOS: this CPU does not support 64-bit long mode.", 0

section .boot.text
bits 32
global _start
_start:
    cli
    cld
    mov esp, boot_stack_top
    mov edi, eax                    ; multiboot2 magic
    mov esi, ebx                    ; multiboot2 info (physical)

    ; --- check for long mode ---
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21                ; ID flag -> CPUID available?
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_long_mode
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; --- build boot page tables ---
    ; 4 page directories with 2 MiB pages mapping 0..4 GiB
    mov ecx, 0
.fill_pd:
    mov eax, ecx
    shl eax, 21                     ; ecx * 2 MiB (low 32 bits)
    or eax, 0x83                    ; present | writable | huge
    mov [boot_pd + ecx * 8], eax
    mov eax, ecx
    shr eax, 11                     ; high bits of (ecx * 2 MiB)
    mov [boot_pd + ecx * 8 + 4], eax
    inc ecx
    cmp ecx, 2048
    jne .fill_pd

    ; PDPT low: 4 entries -> the 4 page directories
    mov eax, boot_pd
    or eax, 3
    mov [boot_pdpt_low], eax
    add eax, 4096
    mov [boot_pdpt_low + 8], eax
    add eax, 4096
    mov [boot_pdpt_low + 16], eax
    add eax, 4096
    mov [boot_pdpt_low + 24], eax

    ; PDPT high: entry 510 -> first page directory (maps 0xFFFFFFFF80000000 -> 0)
    mov eax, boot_pd
    or eax, 3
    mov [boot_pdpt_high + 510 * 8], eax

    ; PML4: [0] identity, [256] physmap, [511] kernel
    mov eax, boot_pdpt_low
    or eax, 3
    mov [boot_pml4], eax
    mov [boot_pml4 + 256 * 8], eax
    mov eax, boot_pdpt_high
    or eax, 3
    mov [boot_pml4 + 511 * 8], eax

    ; --- enable PAE + long mode + paging ---
    mov eax, boot_pml4
    mov cr3, eax
    mov eax, cr4
    or eax, (1 << 5)                ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, (1 << 8)                ; LME
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)   ; PG | WP
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp 0x08:.long_mode

.no_long_mode:
    ; print a message on the serial port and halt
    mov esi, msg_nolm
.nlm_loop:
    lodsb
    test al, al
    jz .halt
    mov dx, 0x3F8
    out dx, al
    jmp .nlm_loop
.halt:
    hlt
    jmp .halt

bits 64
.long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rax, higher_half_entry
    jmp rax

section .text
bits 64
extern kmain
higher_half_entry:
    mov rsp, kernel_boot_stack_top
    xor rbp, rbp
    mov edi, edi                    ; zero-extend magic
    mov esi, esi                    ; zero-extend mbi pointer
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
global kernel_boot_stack
global kernel_boot_stack_top
kernel_boot_stack:
    resb 32768
kernel_boot_stack_top:
