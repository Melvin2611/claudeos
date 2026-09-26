#pragma once
#include <kernel.h>
#include <claudeos/abi.h>

typedef long (*syscall_fn)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
void syscall_register(int num, syscall_fn fn);
syscall_fn syscall_get(int num);
void syscall_init(void);

#define SYSCALL_DEF(name) static long name(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
#define SYSCALL_UNUSED_ARGS (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6
