#pragma once
#include <claudeos/abi.h>

static inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define syscall0(n) __syscall6((n), 0, 0, 0, 0, 0, 0)
#define syscall1(n, a) __syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define syscall2(n, a, b) __syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define syscall3(n, a, b, c) __syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define syscall4(n, a, b, c, d) __syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define syscall5(n, a, b, c, d, e) __syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
