#pragma once
#include <kernel.h>

/* serial.c */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);

/* pit.c */
void pit_delay_ms(uint32_t ms);
void speaker_on(uint32_t freq);
void speaker_off(void);

/* rtc.c */
void rtc_init(void);
uint64_t rtc_read_epoch(void);
void time_set(uint64_t epoch);

/* devfs.c */
uint64_t krandom(void);
