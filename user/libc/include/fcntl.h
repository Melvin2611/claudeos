#pragma once
#include <claudeos/abi.h>
int open(const char *path, int flags, ...);
int creat(const char *path, int mode);
