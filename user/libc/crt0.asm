; ClaudeOS user program entry: rdi = argc, rsi = argv, rdx = envp (also on the stack)
bits 64
section .text.start
global _start
extern __libc_start
_start:
    xor rbp, rbp
    and rsp, -16
    call __libc_start
.hang:
    jmp .hang

; marks the binary as a ClaudeOS program (anything without it runs with the Linux ABI)
section .note.claudeos noalloc
    db "ClaudeOS", 0
