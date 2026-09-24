#pragma once
#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int dup(int fd);
int dup2(int a, int b);
pid_t getpid(void);
pid_t getppid(void);
unsigned sleep(unsigned sec);
int usleep(unsigned long usec);
int isatty(int fd);
int ftruncate(int fd, off_t len);
int fsync(int fd);
void *sbrk(long incr);
int access(const char *path, int mode);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
