/* sync - write all cached file system data to disk */
#include <unistd.h>

int main(void) { return fsync(-1) < 0; }
