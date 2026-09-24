/* Kernel log: goes to COM1 and an in-memory ring buffer (readable via dmesg). */
#include <kernel.h>
#include <claudeos/fmt.h>
#include "drivers.h"

#define KLOG_SIZE (128 * 1024)
static char klog_buf[KLOG_SIZE];
static size_t klog_len;          /* total bytes ever written */

void (*klog_hook)(char c);       /* optional early framebuffer console */

static void klog_out(char c, void *ctx) {
    UNUSED(ctx);
    serial_putc(c);
    klog_buf[klog_len % KLOG_SIZE] = c;
    klog_len++;
    if (klog_hook) klog_hook(c);
}

void kvprintf(const char *fmt, va_list ap) {
    uint64_t f = irq_save();
    fmt_format(klog_out, 0, fmt, ap);
    irq_restore(f);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

size_t klog_size(void) { return klog_len < KLOG_SIZE ? klog_len : KLOG_SIZE; }

/* read from the retained part of the log, offset relative to the oldest retained byte */
size_t klog_read(char *buf, size_t off, size_t n) {
    uint64_t f = irq_save();
    size_t size = klog_size();
    size_t start = klog_len - size;
    size_t r = 0;
    while (r < n && off + r < size) {
        buf[r] = klog_buf[(start + off + r) % KLOG_SIZE];
        r++;
    }
    irq_restore(f);
    return r;
}
