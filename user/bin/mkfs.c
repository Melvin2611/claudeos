/* mkfs - format a hard disk as FAT32 and use it for /home */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    const char *dev = 0, *label = "CLAUDEOS";
    bool yes = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y")) yes = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) label = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: mkfs [-y] [-n LABEL] [device]\n"
                            "Formats the whole disk (default: first disk) as FAT32 and mounts it on /home.\n");
            return 1;
        }
        else dev = argv[i];
    }
    if (!yes) {
        printf("\x1b[1;31mWARNING:\x1b[0m all data on %s will be erased.\nType 'yes' to continue: ",
               dev ? dev : "the first hard disk");
        fflush(stdout);
        char line[16];
        if (!fgets(line, sizeof(line), stdin) || strncmp(line, "yes", 3)) { printf("Aborted.\n"); return 1; }
    }
    printf("Formatting... ");
    fflush(stdout);
    if (mkfs_disk(dev, label) < 0) {
        if (errno == EBUSY) printf("failed: /home is already on a disk (use a blank disk)\n");
        else printf("failed: %s\n", strerror(errno));
        return 1;
    }
    printf("done. /home is now stored on disk.\n");
    return 0;
}
