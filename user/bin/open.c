/* open - open files with the default desktop application; edit - open in the text editor */
#include <stdio.h>
#include <string.h>
#include <gui.h>

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "edit")) return gui_launch("/bin/editor", argc > 1 ? argv[1] : "") < 0;
    if (argc < 2) { fprintf(stderr, "usage: open file|folder...\n"); return 1; }
    for (int i = 1; i < argc; i++) gui_launch(argv[i], 0);
    return 0;
}
