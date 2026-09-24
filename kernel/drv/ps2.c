/* i8042 PS/2 controller: keyboard, mouse (with wheel) and the VMware/QEMU absolute pointer */
#include <kernel.h>
#include <cpu.h>
#include <input.h>
#include <boot.h>

#define DATA 0x60
#define STATUS 0x64
#define CMD 0x64

int mouse_speed = 5;
bool mouse_absolute;

/* ------------------------------------------------------------------ input queue */
#define QSIZE 512
static input_event_t queue[QSIZE];
static volatile uint32_t qhead, qtail;
static waitq_t input_wq;

void input_push(const input_event_t *ev) {
    uint64_t f = irq_save();
    uint32_t next = (qhead + 1) % QSIZE;
    if (next != qtail) {
        queue[qhead] = *ev;
        qhead = next;
    }
    wq_wake_all(&input_wq);
    irq_restore(f);
}

bool input_pop(input_event_t *ev) {
    uint64_t f = irq_save();
    bool ok = qtail != qhead;
    if (ok) {
        *ev = queue[qtail];
        qtail = (qtail + 1) % QSIZE;
    }
    irq_restore(f);
    return ok;
}

bool input_wait(uint64_t timeout_ms) {
    uint64_t f = irq_save();
    if (qtail == qhead) wq_wait_timeout(&input_wq, timeout_ms);
    bool r = qtail != qhead;
    irq_restore(f);
    return r;
}

/* wake the window server when anything else happens (window updates etc.) */
void input_kick(void) {
    uint64_t f = irq_save();
    wq_wake_all(&input_wq);
    irq_restore(f);
}

/* ------------------------------------------------------------------ controller helpers */
static bool wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(STATUS) & 2)) return true;
        io_wait();
    }
    return false;
}

static bool wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(STATUS) & 1) return true;
        io_wait();
    }
    return false;
}

static void ctrl_cmd(uint8_t c) { wait_write(); outb(CMD, c); }
static void ctrl_data(uint8_t d) { wait_write(); outb(DATA, d); }
static int read_data(void) { return wait_read() ? inb(DATA) : -1; }
static void flush(void) { for (int i = 0; i < 32 && (inb(STATUS) & 1); i++) inb(DATA); }

static int kbd_cmd(uint8_t c) {
    for (int tries = 0; tries < 3; tries++) {
        ctrl_data(c);
        int r = read_data();
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}

static int mouse_cmd(uint8_t c) {
    for (int tries = 0; tries < 3; tries++) {
        ctrl_cmd(0xD4);
        ctrl_data(c);
        int r = read_data();
        if (r == 0xFA) return 0;
        if (r != 0xFE) return -1;
    }
    return -1;
}

/* ------------------------------------------------------------------ keyboard */
static uint32_t mods = MOD_NUM;
static bool e0_prefix;
static uint32_t dead_key;
static uint8_t e1_skip;

static void kbd_leds(void) {
    uint8_t leds = ((mods & MOD_CAPS) ? 4 : 0) | ((mods & MOD_NUM) ? 2 : 0);
    ctrl_data(0xED);
    ctrl_data(leds);
}

static void emit_key(int kc, bool pressed, uint32_t ch) {
    input_event_t ev = { 0 };
    ev.type = IN_KEY;
    ev.keycode = (uint16_t)kc;
    ev.pressed = pressed;
    ev.ch = ch;
    ev.mods = mods;
    input_push(&ev);
}

static void kbd_byte(uint8_t sc) {
    if (e1_skip) { e1_skip--; return; }     /* Pause key sequence */
    if (sc == 0xE1) { e1_skip = 5; return; }
    if (sc == 0xE0) { e0_prefix = true; return; }
    if (sc == 0xFA || sc == 0xFE || sc == 0x00 || sc == 0xFF) return;
    bool released = sc & 0x80;
    int kc = sc & 0x7F;
    if (e0_prefix) {
        e0_prefix = false;
        if (kc == 0x2A || kc == 0x36) return;   /* fake shifts */
        kc |= 0x80;
    }
    uint32_t bit = 0;
    switch (kc) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL: case KEY_RCTRL: bit = MOD_CTRL; break;
    case KEY_LALT: bit = MOD_ALT; break;
    case KEY_RALT: bit = MOD_ALTGR; break;
    case KEY_LSUPER: case KEY_RSUPER: bit = MOD_SUPER; break;
    }
    if (bit) {
        if (released) mods &= ~bit; else mods |= bit;
        emit_key(kc, !released, 0);
        return;
    }
    if (!released && kc == KEY_CAPSLOCK) { mods ^= MOD_CAPS; kbd_leds(); }
    if (!released && kc == KEY_NUMLOCK) { mods ^= MOD_NUM; kbd_leds(); }

    /* keypad without numlock acts as navigation */
    if (!(mods & MOD_NUM) && kc >= 0x47 && kc <= 0x53 && kc != 0x4A && kc != 0x4E && kc != 0x4C) {
        static const uint8_t nav[] = { KEY_HOME, KEY_UP, KEY_PGUP, 0, KEY_LEFT, 0, KEY_RIGHT, 0,
                                       KEY_END, KEY_DOWN, KEY_PGDN, KEY_INSERT, KEY_DELETE };
        kc = nav[kc - 0x47];
    }
    if (released) { emit_key(kc, false, 0); return; }

    bool dead;
    uint32_t ch = keymap_translate(kc, mods, &dead);
    if (dead && !(mods & MOD_CTRL)) {
        if (dead_key) {
            /* two dead keys: emit the first one as a normal character */
            emit_key(kc, true, dead_key);
            dead_key = 0;
            return;
        }
        dead_key = ch;
        emit_key(kc, true, 0);
        return;
    }
    if (dead_key && ch && ch >= ' ') {
        uint32_t composed = keymap_compose(dead_key, ch);
        if (composed) {
            dead_key = 0;
            emit_key(kc, true, composed);
            return;
        }
        emit_key(0, true, dead_key);
        dead_key = 0;
    }
    emit_key(kc, true, ch);
}

/* ------------------------------------------------------------------ mouse */
static uint8_t mpacket[4];
static int mindex, mpacket_len = 3;
static uint32_t last_buttons;

/* VMware backdoor (vmmouse) */
#define VMW_MAGIC 0x564D5868
#define VMW_PORT 0x5658
typedef struct { uint32_t a, b, c, d; } vmw_regs_t;
static void vmw_call(uint32_t cmd, uint32_t arg, vmw_regs_t *r) {
    uint32_t a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT;
    __asm__ volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d) :: "memory");
    r->a = a; r->b = b; r->c = c; r->d = d;
}

static bool vmmouse_init(void) {
    if (cmdline_has("novmmouse")) return false;
    vmw_regs_t r;
    vmw_call(10, ~VMW_MAGIC, &r);                 /* GETVERSION */
    if (r.b != VMW_MAGIC || r.a == 0xFFFFFFFF) return false;
    vmw_call(41, 0x45414552, &r);                 /* ABSPOINTER_COMMAND: enable */
    vmw_call(40, 0, &r);                          /* status */
    if ((r.a & 0xFFFF) == 0) return false;
    vmw_call(39, 1, &r);                          /* read version id */
    if (r.a != 0x3442554A) return false;
    vmw_call(41, 0x53424152, &r);                 /* request absolute */
    return true;
}

static void vmmouse_poll(void) {
    for (int guard = 0; guard < 64; guard++) {
        vmw_regs_t r;
        vmw_call(40, 0, &r);
        if ((r.a & 0xFFFF0000) == 0xFFFF0000) {    /* error: re-enable */
            vmmouse_init();
            return;
        }
        uint32_t queued = r.a & 0xFFFF;
        if (queued < 4) return;
        vmw_call(39, 4, &r);
        uint32_t status = r.a;
        input_event_t ev = { 0 };
        uint32_t b = 0;
        if (status & 0x20) b |= MOUSE_LEFT;
        if (status & 0x10) b |= MOUSE_RIGHT;
        if (status & 0x08) b |= MOUSE_MIDDLE;
        ev.buttons = b;
        ev.wheel = (int8_t)(r.d & 0xFF);
        if (status & 0x00010000) {
            ev.type = IN_MOUSE_REL;
            ev.dx = (int32_t)r.b;
            ev.dy = (int32_t)r.c;
        } else {
            ev.type = IN_MOUSE_ABS;
            ev.dx = (int32_t)(r.b & 0xFFFF);
            ev.dy = (int32_t)(r.c & 0xFFFF);
        }
        input_push(&ev);
    }
}

static void mouse_packet(void) {
    uint8_t b0 = mpacket[0];
    if (b0 & 0xC0) return;   /* overflow */
    int dx = mpacket[1] - ((b0 << 4) & 0x100);
    int dy = mpacket[2] - ((b0 << 3) & 0x100);
    input_event_t ev = { 0 };
    ev.type = IN_MOUSE_REL;
    ev.dx = dx * mouse_speed / 5;
    ev.dy = -dy * mouse_speed / 5;
    ev.buttons = (b0 & 1 ? MOUSE_LEFT : 0) | (b0 & 2 ? MOUSE_RIGHT : 0) | (b0 & 4 ? MOUSE_MIDDLE : 0);
    if (mpacket_len == 4) ev.wheel = (int8_t)((mpacket[3] & 0x08) ? (mpacket[3] | 0xF0) : (mpacket[3] & 0x0F));
    if (ev.dx || ev.dy || ev.wheel || ev.buttons != last_buttons) input_push(&ev);
    last_buttons = ev.buttons;
}

static void mouse_byte(uint8_t b) {
    if (mindex == 0 && !(b & 0x08)) return;   /* resync: bit 3 of first byte is always set */
    mpacket[mindex++] = b;
    if (mindex == mpacket_len) {
        mindex = 0;
        if (mouse_absolute) vmmouse_poll();
        else mouse_packet();
    }
}

/* ------------------------------------------------------------------ IRQ handlers */
static void ps2_irq(regs_t *r, void *ctx) {
    UNUSED(r);
    bool is_mouse_irq = ctx != 0;
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(STATUS);
        if (!(st & 1)) break;
        uint8_t data = inb(DATA);
        if (st & 0x20) mouse_byte(data);
        else kbd_byte(data);
    }
    if (is_mouse_irq && mouse_absolute) vmmouse_poll();
}

void ps2_init(void) {
    ctrl_cmd(0xAD);          /* disable keyboard */
    ctrl_cmd(0xA7);          /* disable mouse */
    flush();
    ctrl_cmd(0x20);
    int cfg = read_data();
    if (cfg < 0) cfg = 0x47;
    cfg &= ~0x03;            /* IRQs off while configuring */
    cfg |= 0x40;             /* translation on */
    ctrl_cmd(0x60);
    ctrl_data(cfg);
    ctrl_cmd(0xAE);          /* enable keyboard port */
    ctrl_cmd(0xA8);          /* enable mouse port */

    /* keyboard: defaults + enable scanning */
    flush();
    kbd_cmd(0xF6);
    kbd_cmd(0xF4);
    kbd_leds();
    flush();

    /* mouse: defaults, try IntelliMouse (wheel) */
    bool mouse_ok = mouse_cmd(0xF6) == 0;
    if (mouse_ok) {
        mouse_cmd(0xF3); mouse_cmd(200);
        mouse_cmd(0xF3); mouse_cmd(100);
        mouse_cmd(0xF3); mouse_cmd(80);
        mouse_cmd(0xF2);
        int id = read_data();
        if (id == 3 || id == 4) mpacket_len = 4;
        mouse_cmd(0xF3); mouse_cmd(100);
        mouse_cmd(0xF4);
    }
    flush();

    ctrl_cmd(0x20);
    cfg = read_data();
    if (cfg < 0) cfg = 0x47;
    cfg |= 0x03;             /* IRQ1 + IRQ12 */
    cfg &= ~0x30;            /* clocks enabled */
    ctrl_cmd(0x60);
    ctrl_data(cfg);

    const char *layout = cmdline_get("kbd");
    if (layout) keymap_select(layout);

    mouse_absolute = mouse_ok && vmmouse_init();
    irq_register(1, ps2_irq, 0);
    irq_register(12, ps2_irq, (void *)1);
    klog("[ps2] keyboard (layout %s), mouse %s%s\n", keymap_name(), mouse_ok ? "ok" : "not found",
         mouse_absolute ? " + vmmouse absolute pointer" : mpacket_len == 4 ? " (wheel)" : "");
}
