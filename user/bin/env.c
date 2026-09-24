/* env - print the environment */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    for (char **e = environ; e && *e; e++) puts(*e);
    return 0;
}
