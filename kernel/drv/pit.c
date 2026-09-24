/* PIT channel 0 system timer + PC speaker (channel 2) */
#include <kernel.h>
#include <cpu.h>
#include "drivers.h"

volatile uint64_t ticks;   /* ms since boot */
static uint32_t pit_hz = 1000;

void sched_tick(void);

static void pit_irq(regs_t *r, void *ctx) {
    ticks += 1000 / pit_hz;
    sched_tick();
}

void pit_init(uint32_t hz) {
    pit_hz = hz;
    uint32_t div = 1193182 / hz;
    outb(0x43, 0x34);              /* channel 0, lobyte/hibyte, mode 2 */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_register(0, pit_irq, 0);
}

uint64_t uptime_ms(void) { return ticks; }

/* busy-wait delay usable before the scheduler runs */
void pit_delay_ms(uint32_t ms) {
    uint64_t end = ticks + ms;
    if (ints_enabled()) {
        while (ticks < end) hlt();
    } else {
        /* interrupts off: poll counter via channel 0 readback, roughly */
        for (uint32_t i = 0; i < ms; i++)
            for (int j = 0; j < 1000; j++) io_wait();   /* ~1us each */
    }
}

/* ---- PC speaker ---- */
void speaker_on(uint32_t freq) {
    if (freq < 20) freq = 20;
    uint32_t div = 1193182 / freq;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    uint8_t v = inb(0x61);
    if ((v & 3) != 3) outb(0x61, v | 3);
}

void speaker_off(void) {
    outb(0x61, inb(0x61) & 0xFC);
}
