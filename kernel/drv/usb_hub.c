/* USB hub class driver (USB 2 and USB 3 hubs): powers the ports, polls them for
 * connect/disconnect and enumerates the devices behind them. */
#include <kernel.h>
#include <usb.h>
#include <mm.h>
#include "drivers.h"

#define PORT_CONNECTION 0
#define PORT_RESET      4
#define PORT_POWER      8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE   17
#define C_PORT_RESET    20
#define C_BH_PORT_RESET 29

typedef struct hub {
    usb_device_t *dev;
    bool ss;
    int nports;
    usb_device_t *child[16];
    bool active;
    struct hub *next;
} hub_t;

static hub_t *hubs;

static int port_status(hub_t *hub, int port, uint16_t *status, uint16_t *change) {
    uint8_t buf[4];
    if (usb_control(hub->dev, 0xA3, 0, 0, port, buf, 4) < 4) return -EIO;
    *status = buf[0] | (buf[1] << 8);
    *change = buf[2] | (buf[3] << 8);
    return 0;
}

static void set_feature(hub_t *hub, int port, int f) { usb_control(hub->dev, 0x23, 3, f, port, 0, 0); }
static void clear_feature(hub_t *hub, int port, int f) { usb_control(hub->dev, 0x23, 1, f, port, 0, 0); }

static void port_connect(hub_t *hub, int port) {
    uint16_t st, ch;
    set_feature(hub, port, PORT_RESET);
    for (int i = 0; i < 50; i++) {
        pit_delay_ms(10);
        if (port_status(hub, port, &st, &ch) < 0) return;
        if (ch & (1u << 4)) break;
    }
    clear_feature(hub, port, C_PORT_RESET);
    if (hub->ss) clear_feature(hub, port, C_BH_PORT_RESET);
    if (port_status(hub, port, &st, &ch) < 0 || !(st & 1) || !(st & 2)) return;
    int speed;
    if (hub->ss) speed = USB_SPEED_SUPER;
    else if (st & (1u << 9)) speed = USB_SPEED_LOW;
    else if (st & (1u << 10)) speed = USB_SPEED_HIGH;
    else speed = USB_SPEED_FULL;
    pit_delay_ms(10);
    hub->child[port] = usb_enumerate(hub->dev->hc, hub->dev, port, speed);
}

static void check_port(hub_t *hub, int port) {
    uint16_t st, ch;
    if (port_status(hub, port, &st, &ch) < 0) return;
    if (ch & 1) clear_feature(hub, port, C_PORT_CONNECTION);
    if (ch & 2) clear_feature(hub, port, C_PORT_ENABLE);
    bool connected = st & 1;
    if (hub->child[port] && (!connected || (ch & 1))) {
        klog("[usb] hub port %d: %s disconnected\n", port, hub->child[port]->product);
        usb_device_remove(hub->child[port]);
        hub->child[port] = 0;
    }
    if (connected && !hub->child[port]) port_connect(hub, port);
}

void usb_hub_poll(void) {
    for (hub_t *h = hubs; h; h = h->next) {
        if (!h->active || h->dev->gone) continue;
        for (int p = 1; p <= h->nports; p++) check_port(h, p);
    }
}

static void hub_start(usb_device_t *d, void *ctx) {
    hub_t *hub = ctx;
    if (hub->ss) usb_control(d, 0x20, 12, d->depth, 0, 0, 0);     /* SET_HUB_DEPTH */
    for (int p = 1; p <= hub->nports; p++) set_feature(hub, p, PORT_POWER);
    pit_delay_ms(100);
    klog("[usb]   hub with %d ports ready\n", hub->nports);
    hub->active = true;
    for (int p = 1; p <= hub->nports; p++) check_port(hub, p);
}

static void hub_stop(usb_device_t *d, void *ctx) {
    UNUSED(d);
    hub_t *hub = ctx;
    hub->active = false;
    for (int p = 1; p <= hub->nports; p++) {
        if (hub->child[p]) {
            usb_device_remove(hub->child[p]);
            hub->child[p] = 0;
        }
    }
}

bool usb_hub_probe(usb_device_t *d, usb_interface_desc_t *intf, uint8_t *extra, int extra_len) {
    if (intf->bInterfaceClass != 9) return false;
    if (d->depth >= 5) return false;
    hub_t *hub = kzalloc(sizeof(hub_t));
    hub->dev = d;
    hub->ss = d->speed >= USB_SPEED_SUPER;
    uint8_t desc[16];
    int n = usb_control(d, 0xA0, USB_REQ_GET_DESCRIPTOR, (hub->ss ? 0x2A : 0x29) << 8, 0, desc, sizeof(desc));
    if (n < 7) { kfree(hub); return false; }
    hub->nports = MIN(desc[2], (uint8_t)15);
    uint16_t chars = desc[3] | (desc[4] << 8);
    d->is_hub = true;
    d->hub_ports = hub->nports;
    d->hub_ttt = (chars >> 5) & 3;
    d->hub_mtt = false;
    /* the status change endpoint is registered so the slot is fully configured; ports are polled */
    for (uint8_t *p = extra; p + 2 <= extra + extra_len && p[0]; p += p[0]) {
        if (p[1] == USB_DT_ENDPOINT && (p[2] & 0x80) && (p[3] & 3) == 3) {
            usb_add_endpoint(d, (usb_endpoint_desc_t *)p);
            break;
        }
    }
    hub->next = hubs;
    hubs = hub;
    usb_bind(d, hub_start, hub_stop, hub);
    return true;
}
