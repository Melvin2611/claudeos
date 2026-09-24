/* uname - system information */
#include <stdio.h>
#include <string.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    ksysinfo_t si;
    sys_info(&si);
    if (argc > 1 && !strcmp(argv[1], "-a"))
        printf("%s %s x86_64 (%s, %lu MHz)\n", si.os_name, si.os_version, si.cpu_brand, (unsigned long)si.cpu_mhz);
    else if (argc > 1 && !strcmp(argv[1], "-r"))
        printf("%s\n", si.os_version);
    else if (argc > 1 && !strcmp(argv[1], "-m"))
        printf("x86_64\n");
    else
        printf("%s\n", si.os_name);
    return 0;
}
