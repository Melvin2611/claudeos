#pragma once
#include <stdarg.h>
#include <stddef.h>

/* Generic printf-style formatter shared by kernel and libc.
 * The output callback receives one character at a time. Returns number of chars emitted. */
typedef void (*fmt_out_fn)(char c, void *ctx);
int fmt_format(fmt_out_fn out, void *ctx, const char *fmt, va_list ap);
