/* USB HID class driver: boot keyboards (with software key repeat) and mice / tablets
 * described by their report descriptor (relative or absolute pointers, wheel, buttons). */
#include <kernel.h>
#include <usb.h>
#include <input.h>
#include <mm.h>

/* HID usage (keyboard page) -> ClaudeOS key code (PC set 1, 0x80 = E0 prefix) */
static const uint8_t usage_to_key[256] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22,
    [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31,
    [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16,
    [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08,
    [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D,
    [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29,
    [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41,
    [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = 0xB7, [0x47] = 0x46, [0x49] = 0xD2, [0x4A] = 0xC7, [0x4B] = 0xC9, [0x4C] = 0xD3, [0x4D] = 0xCF,
    [0x4E] = 0xD1, [0x4F] = 0xCD, [0x50] = 0xCB, [0x51] = 0xD0, [0x52] = 0xC8,
    [0x53] = 0x45, [0x54] = 0xB5, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x9C, [0x59] = 0x4F,
    [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48,
    [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = 0xDD,
};
static const uint8_t mod_keys[8] = { 0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC };

typedef struct {
    usb_device_t *dev;
    usb_endpoint_t *ep;
    uint8_t iface;
    bool keyboard;
    /* keyboard state */
    uint8_t last[8];
    int repeat_key;
    uint64_t repeat_at;
    /* pointer report layout (bit offsets inside the report, after the report id byte) */
    bool parsed, absolute;
    uint8_t report_id;
    int btn_off, btn_count;
    int x_off, x_size, y_off, y_size, w_off, w_size;
    int32_t x_max, y_max;
    uint32_t last_buttons;
} hid_t;

static hid_t *keyboards[8];
static int nkeyboards;

/* ------------------------------------------------------------------ report descriptor parser */

static void parse_report_desc(hid_t *h, const uint8_t *d, int len) {
    uint32_t usage_page = 0, report_size = 0, report_count = 0;
    int32_t logical_max = 0;
    uint32_t usages[16];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    uint8_t report_id = 0;
    int bitpos = 0;
    bool chose_id = false;
    for (int i = 0; i < len;) {
        uint8_t prefix = d[i];
        if (prefix == 0xFE) { i += 3 + (i + 1 < len ? d[i + 1] : 0); continue; }   /* long item */
        int size = prefix & 3;
        if (size == 3) size = 4;
        uint32_t val = 0;
        for (int k = 0; k < size && i + 1 + k < len; k++) val |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t sval = size == 1 ? (int8_t)val : size == 2 ? (int16_t)val : (int32_t)val;
        uint8_t tag = prefix & 0xFC;
        i += 1 + size;
        switch (tag) {
        case 0x04: usage_page = val; break;                 /* usage page */
        case 0x24: logical_max = sval; if (size == 2 && sval < 0) logical_max = (int32_t)(uint16_t)val; break;
        case 0x74: report_size = val; break;
        case 0x94: report_count = val; break;
        case 0x84:                                          /* report id */
            if (chose_id && report_id != val) { i = len; break; }   /* only the first pointer report */
            report_id = (uint8_t)val;
            bitpos = 0;
            break;
        case 0x08: if (nusages < 16) usages[nusages++] = size == 4 ? val : (usage_page << 16) | val; break;
        case 0x18: umin = size == 4 ? val : (usage_page << 16) | val; break;
        case 0x28: umax = size == 4 ? val : (usage_page << 16) | val; break;
        case 0x80: {                                        /* input */
            bool constant = val & 1, relative = val & 4;
            for (uint32_t n = 0; n < report_count; n++) {
                uint32_t u = 0;
                if (nusages) u = usages[n < (uint32_t)nusages ? n : (uint32_t)nusages - 1];
                else if (umax) u = umin + n <= umax ? umin + n : umax;
                if (!constant) {
                    if ((u >> 16) == 0x09 && h->btn_count == 0) {
                        h->btn_off = bitpos;
                        h->btn_count = MIN(report_count, 8u);
                        chose_id = true;
                        h->report_id = report_id;
                    } else if (u == 0x10030 && !h->x_size) {
                        h->x_off = bitpos; h->x_size = report_size; h->x_max = logical_max; h->absolute = !relative;
                        chose_id = true;
                        h->report_id = report_id;
                    } else if (u == 0x10031 && !h->y_size) {
                        h->y_off = bitpos; h->y_size = report_size; h->y_max = logical_max;
                    } else if (u == 0x10038 && !h->w_size) {
                        h->w_off = bitpos; h->w_size = report_size;
                    }
                }
                bitpos += report_size;
            }
            nusages = 0;
            umin = umax = 0;
            break;
        }
        case 0x90: case 0xB0:                               /* output / feature: not in input reports */
            nusages = 0;
            umin = umax = 0;
            break;
        case 0xA0: case 0xC0: nusages = 0; umin = umax = 0; break;   /* collection */
        default: break;
        }
    }
    h->parsed = h->x_size > 0 && h->y_size > 0;
}

static int32_t get_bits(const uint8_t *r, int len, int off, int size, bool sign) {
    uint32_t v = 0;
    for (int b = 0; b < size && b < 32; b++) {
        int bit = off + b;
        if (bit / 8 >= len) break;
        if (r[bit / 8] & (1u << (bit % 8))) v |= 1u << b;
    }
    if (sign && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (int32_t)v;
}

/* ------------------------------------------------------------------ reports */

static void kbd_report(usb_endpoint_t *ep, uint8_t *r, uint32_t len) {
    hid_t *h = ep->driver;
    if (len < 8) return;
    if (r[2] == 1) return;                                  /* rollover error */
    /* modifiers */
    uint8_t changed = r[0] ^ h->last[0];
    for (int b = 0; b < 8; b++)
        if (changed & (1u << b)) kbd_key(mod_keys[b], !(r[0] & (1u << b)));
    /* released keys */
    for (int i = 2; i < 8; i++) {
        uint8_t u = h->last[i];
        if (!u) continue;
        bool still = false;
        for (int j = 2; j < 8; j++) if (r[j] == u) still = true;
        if (!still && usage_to_key[u]) {
            kbd_key(usage_to_key[u], true);
            if (h->repeat_key == usage_to_key[u]) h->repeat_key = 0;
        }
    }
    /* pressed keys */
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (!u) continue;
        bool was = false;
        for (int j = 2; j < 8; j++) if (h->last[j] == u) was = true;
        if (!was && usage_to_key[u]) {
            kbd_key(usage_to_key[u], false);
            h->repeat_key = usage_to_key[u];
            h->repeat_at = uptime_ms() + 500;
        }
    }
    memcpy(h->last, r, 8);
}

static void pointer_report(usb_endpoint_t *ep, uint8_t *r, uint32_t len) {
    hid_t *h = ep->driver;
    input_event_t ev = { 0 };
    if (h->parsed) {
        if (h->report_id) {
            if (!len || r[0] != h->report_id) return;
            r++;
            len--;
        }
        uint32_t b = 0;
        for (int i = 0; i < h->btn_count; i++)
            if (get_bits(r, len, h->btn_off + i, 1, false)) b |= 1u << i;
        ev.buttons = (b & 1 ? MOUSE_LEFT : 0) | (b & 2 ? MOUSE_RIGHT : 0) | (b & 4 ? MOUSE_MIDDLE : 0);
        if (h->w_size) ev.wheel = -get_bits(r, len, h->w_off, h->w_size, true);
        if (h->absolute) {
            int32_t x = get_bits(r, len, h->x_off, h->x_size, false), y = get_bits(r, len, h->y_off, h->y_size, false);
            ev.type = IN_MOUSE_ABS;
            ev.dx = h->x_max > 0 ? (int32_t)((int64_t)x * 65535 / h->x_max) : x;
            ev.dy = h->y_max > 0 ? (int32_t)((int64_t)y * 65535 / h->y_max) : y;
        } else {
            ev.type = IN_MOUSE_REL;
            ev.dx = get_bits(r, len, h->x_off, h->x_size, true) * mouse_speed / 5;
            ev.dy = get_bits(r, len, h->y_off, h->y_size, true) * mouse_speed / 5;
        }
    } else {
        /* boot protocol: buttons, dx, dy, [wheel] */
        if (len < 3) return;
        ev.type = IN_MOUSE_REL;
        ev.buttons = (r[0] & 1 ? MOUSE_LEFT : 0) | (r[0] & 2 ? MOUSE_RIGHT : 0) | (r[0] & 4 ? MOUSE_MIDDLE : 0);
        ev.dx = (int8_t)r[1] * mouse_speed / 5;
        ev.dy = (int8_t)r[2] * mouse_speed / 5;
        if (len >= 4) ev.wheel = -(int8_t)r[3];
    }
    if (ev.type == IN_MOUSE_REL && !ev.dx && !ev.dy && !ev.wheel && ev.buttons == h->last_buttons) return;
    h->last_buttons = ev.buttons;
    input_push(&ev);
}

/* software typematic repeat: 500 ms delay, 30 characters per second */
void usb_hid_tick(void) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < nkeyboards; i++) {
        hid_t *h = keyboards[i];
        if (!h || !h->repeat_key || h->dev->gone || now < h->repeat_at) continue;
        kbd_key(h->repeat_key, false);
        h->repeat_at = now + 33;
    }
}

/* ------------------------------------------------------------------ probing */

static void hid_start(usb_device_t *d, void *ctx) {
    hid_t *h = ctx;
    if (h->keyboard) {
        usb_control(d, 0x21, 0x0B, 0, h->iface, 0, 0);     /* SET_PROTOCOL boot */
        usb_control(d, 0x21, 0x0A, 0, h->iface, 0, 0);     /* SET_IDLE: report only on change */
        if (nkeyboards < 8) keyboards[nkeyboards++] = h;
        klog("[usb]   keyboard ready\n");
    } else {
        uint8_t *rd = kmalloc(1024);
        int n = usb_control(d, 0x81, USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, h->iface, rd, 1024);
        if (n > 0) parse_report_desc(h, rd, n);
        kfree(rd);
        if (!h->parsed) usb_control(d, 0x21, 0x0B, 0, h->iface, 0, 0);   /* fall back to the boot protocol */
        usb_control(d, 0x21, 0x0A, 0, h->iface, 0, 0);
        klog("[usb]   %s ready%s\n", h->parsed && h->absolute ? "tablet (absolute pointer)" : "mouse",
             h->w_size || !h->parsed ? ", wheel" : "");
    }
    h->ep->on_data = h->keyboard ? kbd_report : pointer_report;
    h->ep->driver = h;
    usb_interrupt_start(h->ep, h->ep->max_packet ? h->ep->max_packet : 8);
}

static void hid_stop(usb_device_t *d, void *ctx) {
    UNUSED(d);
    hid_t *h = ctx;
    if (h->keyboard) {
        /* release everything that was held */
        for (int b = 0; b < 8; b++) if (h->last[0] & (1u << b)) kbd_key(mod_keys[b], true);
        h->repeat_key = 0;
        for (int i = 0; i < nkeyboards; i++) if (keyboards[i] == h) keyboards[i] = 0;
    }
}

bool usb_hid_probe(usb_device_t *d, usb_interface_desc_t *intf, uint8_t *extra, int extra_len) {
    if (intf->bInterfaceClass != 3) return false;
    bool kbd = intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 1;
    bool mouse = intf->bInterfaceProtocol == 2 || intf->bInterfaceSubClass == 0;
    if (!kbd && !mouse) return false;
    usb_endpoint_desc_t *ep = 0;
    for (uint8_t *p = extra; p + 2 <= extra + extra_len && p[0]; p += p[0]) {
        if (p[1] == USB_DT_ENDPOINT && (p[2] & 0x80) && (p[3] & 3) == 3) { ep = (usb_endpoint_desc_t *)p; break; }
    }
    if (!ep) return false;
    hid_t *h = kzalloc(sizeof(hid_t));
    h->dev = d;
    h->iface = intf->bInterfaceNumber;
    h->keyboard = kbd;
    h->ep = usb_add_endpoint(d, ep);
    if (!h->ep) { kfree(h); return false; }
    usb_bind(d, hid_start, hid_stop, h);
    return true;
}
