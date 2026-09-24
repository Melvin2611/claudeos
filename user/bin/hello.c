/* first user program: exercises libc + syscalls */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>
#include <sys/wait.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    printf("Hello from user space! pid=%d argc=%d argv[0]=%s\n", getpid(), argc, argv[0]);
    char *p = malloc(100000);
    memset(p, 'x', 100000);
    free(p);
    printf("float: %.3f sqrt(2)=%.6f sin(1)=%.4f pow(2,10)=%g\n", 3.14159, sqrt(2.0), sin(1.0), pow(2, 10));

    FILE *f = fopen("/tmp/test.txt", "w");
    if (f) { fprintf(f, "line %d\n", 42); fclose(f); }
    f = fopen("/tmp/test.txt", "r");
    char buf[64];
    if (f && fgets(buf, sizeof(buf), f)) printf("read back: %s", buf);
    if (f) fclose(f);

    DIR *d = opendir("/");
    struct dirent *e;
    printf("root:");
    while (d && (e = readdir(d))) printf(" %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
    printf("\n");
    if (d) closedir(d);

    if (argc > 1 && !strcmp(argv[1], "child")) return 7;

    int fds[2];
    pipe(fds);
    int map[3] = { 0, fds[1], 2 };
    char *cargv[] = { "hello", "child", 0 };
    int pid = spawn("/bin/hello", cargv, 0, map, 0);
    close(fds[1]);
    char out[256];
    int n = 0, r;
    while ((r = read(fds[0], out + n, sizeof(out) - 1 - n)) > 0) n += r;
    out[n] = 0;
    int st = 0;
    waitpid(pid, &st, 0);
    printf("child %d exited with %d, wrote %d bytes via pipe\n", pid, st, n);

    /* stack growth */
    volatile char big[200000];
    big[0] = 1;
    big[199999] = 2;
    printf("stack growth ok (%d)\n", big[0] + big[199999]);
    printf("USERTEST PASSED\n");
    return 0;
}
