/* ps - list processes */
#include <stdio.h>
#include <string.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    bool all = argc > 1 && (!strcmp(argv[1], "-a") || !strcmp(argv[1], "aux") || !strcmp(argv[1], "-e"));
    static const char *states[] = { "ready", "run", "wait", "sleep", "zombie" };
    printf("%5s %5s %-7s %4s %9s %8s  %s\n", "PID", "PPID", "STATE", "CPU%", "MEM", "TIME", "NAME");
    kprocinfo_t p;
    for (int i = 0; proc_info(i, &p) > 0; i++) {
        if (p.is_kernel && !all) continue;
        char mem[16];
        format_size(p.mem_bytes, mem, sizeof(mem));
        unsigned long s = (unsigned long)(p.cpu_ms / 1000);
        printf("%5d %5d %-7s %3d%% %9s %4lu:%02lu  %s%s\n", p.pid, p.ppid, states[p.state % 5], p.cpu_percent, mem,
               s / 60, s % 60, p.name, p.is_kernel ? " [kernel]" : "");
    }
    if (!all) printf("(use 'ps -a' to include kernel threads)\n");
    return 0;
}
