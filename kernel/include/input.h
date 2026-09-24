#pragma once
#include <kernel.h>
#include <sched.h>
#include <claudeos/keys.h>

enum { IN_KEY = 1, IN_MOUSE_REL, IN_MOUSE_ABS };

typedef struct {
    uint8_t type;
    uint8_t pressed;      /* key events */
    uint16_t keycode;
    uint32_t ch;          /* unicode character (0 if none) */
    uint32_t mods;
    int32_t dx, dy;       /* relative motion, or absolute 0..65535 for IN_MOUSE_ABS */
    int32_t wheel;
    uint32_t buttons;
} input_event_t;

void input_init(void);
void input_push(const input_event_t *ev);          /* from IRQ context */
bool input_pop(input_event_t *ev);
bool input_wait(uint64_t timeout_ms);             /* true if events are pending */
void input_kick(void);                            /* wake the window server */
void input_set_waiter(struct waitq *wq);

void ps2_init(void);
int kbd_set_layout(const char *name);
const char *kbd_get_layout(void);
uint32_t keymap_translate(int keycode, uint32_t mods, bool *dead);
uint32_t keymap_compose(uint32_t dead, uint32_t ch);
bool keymap_select(const char *name);
const char *keymap_name(void);
extern int mouse_speed;          /* 1..10, 5 = default */
extern bool mouse_absolute;      /* vmmouse active */
