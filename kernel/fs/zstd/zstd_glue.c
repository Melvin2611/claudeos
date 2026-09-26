/* Kernel wrapper around the zstd reference decoder: errors inside the decoder jump back
 * here instead of terminating a process. */
#include <kernel.h>
#include "zstd_decompress.h"

static void *jmpbuf[5];
static const char *last_error;

void zstd_error(const char *msg) {
    last_error = msg;
    __builtin_longjmp(jmpbuf, 1);
}

/* the kernel runs this under the big kernel lock and the decoder never sleeps,
 * so one jump buffer is enough */
int zstd_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if (__builtin_setjmp(jmpbuf)) {
        klog("[zstd] %s\n", last_error ? last_error : "error");
        return -EIO;
    }
    size_t n = ZSTD_decompress(out, out_len, in, in_len);
    return (int)n;
}
