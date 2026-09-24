#pragma once
#include <sys/types.h>
#define WNOHANG 1
#define WEXITSTATUS(s) ((s) & 0xFF)
#define WIFEXITED(s) (1)
pid_t waitpid(pid_t pid, int *status, int flags);
pid_t wait(int *status);
