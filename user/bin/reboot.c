/* reboot / poweroff (the same binary checks its name) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    bool off = !strcmp(name, "poweroff") || !strcmp(name, "shutdown") || (argc > 1 && !strcmp(argv[1], "-p"));
    printf("%s...\n", off ? "Powering off" : "Rebooting");
    fflush(stdout);
    sync_all:
    msleep(300);
    power(off ? POWER_OFF : POWER_REBOOT);
    goto sync_all;
}
