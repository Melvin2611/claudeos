/* sysinfo - neofetch style system overview */
#include <stdio.h>
#include <string.h>
#include <claudeos.h>

static const char *logo[] = {
    "\x1b[38;5;9m      ########      ",
    "\x1b[38;5;9m    ############    ",
    "\x1b[38;5;9m   ####      ###    ",
    "\x1b[38;5;9m  ####              ",
    "\x1b[38;5;9m  ####              ",
    "\x1b[38;5;9m  ####              ",
    "\x1b[38;5;9m   ####      ###    ",
    "\x1b[38;5;9m    ############    ",
    "\x1b[38;5;9m      ########      ",
    "                    ",
};

int main(void) {
    ksysinfo_t si;
    sys_info(&si);
    char mem_used[16], mem_total[16];
    format_size(si.mem_total - si.mem_free, mem_used, sizeof(mem_used));
    format_size(si.mem_total, mem_total, sizeof(mem_total));
    unsigned long up = (unsigned long)(si.uptime_ms / 1000);
    char lines[10][128];
    int n = 0;
    snprintf(lines[n++], 128, "\x1b[1;38;5;9muser\x1b[0m@\x1b[1;38;5;9mclaudeos\x1b[0m");
    snprintf(lines[n++], 128, "-------------");
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mOS:\x1b[0m %s %s x86_64", si.os_name, si.os_version);
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mKernel:\x1b[0m claudeos-%s (monolithic, preemptive)", si.os_version);
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mUptime:\x1b[0m %luh %lum %lus", up / 3600, up / 60 % 60, up % 60);
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mShell:\x1b[0m sh");
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mResolution:\x1b[0m %ux%u", si.screen_w, si.screen_h);
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mCPU:\x1b[0m %s (%lu MHz)", si.cpu_brand, (unsigned long)si.cpu_mhz);
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mMemory:\x1b[0m %s / %s", mem_used, mem_total);
    snprintf(lines[n++], 128, "\x1b[1;38;5;9mProcesses:\x1b[0m %u", si.nprocs);
    printf("\n");
    for (int i = 0; i < 10; i++) printf("%s\x1b[0m  %s\n", logo[i], i < n ? lines[i] : "");
    printf("                      ");
    for (int c = 0; c < 8; c++) printf("\x1b[4%dm   ", c);
    printf("\x1b[0m\n                      ");
    for (int c = 0; c < 8; c++) printf("\x1b[10%dm   ", c);
    printf("\x1b[0m\n\n");
    return 0;
}
