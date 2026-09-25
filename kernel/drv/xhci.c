/* xHCI (USB 3) host controller driver: command/event/transfer rings, device enumeration
 * on root ports, control, bulk and interrupt transfers, hot plugging via the "usbd" thread. */
#include <kernel.h>
#include <pci.h>
#include <sched.h>
#include <mm.h>
#include <usb.h>
#include "drivers.h"

bool cmdline_has(const char *opt);

/* operational registers */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR   0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))

#define CMD_RUN  (1u << 0)
#define CMD_HCRST (1u << 1)
#define CMD_INTE (1u << 2)
#define STS_HCH  (1u << 0)
#define STS_EINT (1u << 3)
#define STS_CNR  (1u << 11)

#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR  (1u << 4)
#define PORT_PP  (1u << 9)
#define PORT_CSC (1u << 17)
#define PORT_PRC (1u << 21)
#define PORT_CHANGE_BITS (0x7Fu << 17)

/* TRB types */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP 12
#define TRB_EVAL_CTX 13
#define TRB_RESET_EP 14
#define TRB_SET_DEQ  16
#define TRB_EV_TRANSFER 32
#define TRB_EV_CMD   33
#define TRB_EV_PORT  34

#define TRB_IOC   (1u << 5)
#define TRB_CHAIN (1u << 4)
#define TRB_IDT   (1u << 6)
#define TRB_ISP   (1u << 2)

#define CC_SUCCESS 1
#define CC_STALL   6
#define CC_SHORT   13

#define RING_TRBS 256
#define MAX_SLOTS 64

typedef struct { uint64_t param; uint32_t status, control; } trb_t;

typedef struct {
    trb_t *trbs;
    uint64_t phys;
    uint32_t enq;
    uint32_t cycle;
} ring_t;

typedef struct xhci {
    pci_dev_t *pci;
    volatile uint8_t *base, *op, *rt, *db;
    uint32_t max_slots, max_ports, ctx_size;
    uint64_t *dcbaa;
    ring_t cmd;
    trb_t *evt;
    uint64_t evt_phys;
    uint32_t evt_deq, evt_cycle;
    bool msi;
    waitq_t wq;
    /* last command completion */
    volatile bool cmd_done;
    volatile uint8_t cmd_cc;
    volatile uint8_t cmd_slot;
    uint64_t cmd_trb;
    volatile uint32_t port_change;          /* bitmap of root ports to (re)examine */
    usb_device_t *slots[MAX_SLOTS + 1];
    usb_device_t *port_dev[64];
    mutex_t lock;
} xhci_t;

static xhci_t *controllers[4];
static int ncontrollers;

static inline uint32_t rd32(volatile uint8_t *b, int off) { return *(volatile uint32_t *)(b + off); }
static inline void wr32(volatile uint8_t *b, int off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void wr64(volatile uint8_t *b, int off, uint64_t v) {
    wr32(b, off, (uint32_t)v);
    wr32(b, off + 4, (uint32_t)(v >> 32));
}

static uint64_t dma_alloc(size_t bytes, void **virt) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t p = pmm_alloc_contig(pages, 0x100000000ULL);
    if (virt) *virt = p ? P2V(p) : 0;
    return p;
}

static bool ring_init(ring_t *r) {
    void *v;
    r->phys = dma_alloc(RING_TRBS * sizeof(trb_t), &v);
    if (!r->phys) return false;
    r->trbs = v;
    r->enq = 0;
    r->cycle = 1;
    /* link TRB back to the start, toggling the cycle bit */
    trb_t *link = &r->trbs[RING_TRBS - 1];
    link->param = r->phys;
    link->status = 0;
    link->control = (TRB_LINK << 10) | (1u << 1);
    return true;
}

/* place one TRB on the ring (cycle bit set last); returns its physical address */
static uint64_t ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t control) {
    trb_t *t = &r->trbs[r->enq];
    t->param = param;
    t->status = status;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->control = (control & ~1u) | r->cycle;
    uint64_t phys = r->phys + r->enq * sizeof(trb_t);
    if (++r->enq == RING_TRBS - 1) {
        trb_t *link = &r->trbs[RING_TRBS - 1];
        link->control = (link->control & ~1u) | r->cycle | (control & TRB_CHAIN);
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

static void *ctx_at(xhci_t *h, void *ctx, int index) { return (uint8_t *)ctx + index * h->ctx_size; }

/* ------------------------------------------------------------------ events */

static usb_endpoint_t *find_ep(xhci_t *h, int slot, int dci) {
    usb_device_t *d = slot <= MAX_SLOTS ? h->slots[slot] : 0;
    if (!d) return 0;
    if (dci == 1) return &d->ep0;
    for (int i = 0; i < d->neps; i++)
        if (d->eps[i].dci == dci) return &d->eps[i];
    return 0;
}

static void requeue_interrupt(usb_endpoint_t *ep);

static void process_events(xhci_t *h) {
    int n = 0;
    for (;;) {
        trb_t *e = &h->evt[h->evt_deq];
        if ((e->control & 1) != h->evt_cycle) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t type = (e->control >> 10) & 0x3F;
        uint8_t cc = e->status >> 24;
        if (type == TRB_EV_CMD) {
            if (e->param == h->cmd_trb) {
                h->cmd_cc = cc;
                h->cmd_slot = e->control >> 24;
                h->cmd_done = true;
            }
        } else if (type == TRB_EV_TRANSFER) {
            int slot = e->control >> 24, dci = (e->control >> 16) & 0x1F;
            usb_endpoint_t *ep = find_ep(h, slot, dci);
            if (ep) {
                ep->cc = cc;
                ep->residual = e->status & 0xFFFFFF;
                ep->done = true;
                if (ep->on_data && !ep->dev->gone) {
                    if (cc == CC_SUCCESS || cc == CC_SHORT) ep->on_data(ep, ep->buf, ep->buf_len - ep->residual);
                    if (cc != CC_STALL) requeue_interrupt(ep);
                }
            }
        } else if (type == TRB_EV_PORT) {
            int port = (e->param >> 24) & 0xFF;
            if (port >= 1 && port <= 32) h->port_change |= 1u << (port - 1);
        }
        if (++h->evt_deq == RING_TRBS) {
            h->evt_deq = 0;
            h->evt_cycle ^= 1;
        }
        n++;
    }
    if (n) wr64(h->rt, 0x20 + 0x18, (h->evt_phys + h->evt_deq * sizeof(trb_t)) | (1u << 3));
    wq_wake_all(&h->wq);
}

static void xhci_irq(void *ctx) {
    xhci_t *h = ctx;
    wr32(h->op, OP_USBSTS, STS_EINT);
    wr32(h->rt, 0x20, rd32(h->rt, 0x20) | 1);   /* IMAN: clear interrupt pending */
    process_events(h);
}

/* wait (up to ms) until *flag becomes true, processing events meanwhile */
static bool wait_flag(xhci_t *h, volatile bool *flag, uint32_t ms) {
    uint64_t end = uptime_ms() + ms;
    for (;;) {
        process_events(h);
        if (*flag) return true;
        if (uptime_ms() >= end) return false;
        uint64_t f = irq_save();
        if (!*flag) wq_wait_timeout(&h->wq, h->msi ? 5 : 1);
        irq_restore(f);
    }
}

static int command(xhci_t *h, uint64_t param, uint32_t status, uint32_t control, uint8_t *slot_out) {
    h->cmd_done = false;
    h->cmd_trb = ring_push(&h->cmd, param, status, control);
    wr32(h->db, 0, 0);
    if (!wait_flag(h, &h->cmd_done, 2000)) {
        klog("[xhci] command %u timed out\n", (control >> 10) & 0x3F);
        return -1;
    }
    if (slot_out) *slot_out = h->cmd_slot;
    return h->cmd_cc;
}

/* ------------------------------------------------------------------ transfers */

static void ring_doorbell(usb_endpoint_t *ep) {
    xhci_t *h = ep->dev->hc;
    wr32(h->db, ep->dev->slot * 4, ep->dci);
}

/* bring a halted endpoint back to the running state, continuing after the last queued TRB */
static void ep_reset(usb_endpoint_t *ep) {
    usb_device_t *d = ep->dev;
    xhci_t *h = d->hc;
    command(h, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)d->slot << 24) | ((uint32_t)ep->dci << 16), 0);
    ring_t *r = ep->ring;
    uint64_t deq = r->phys + r->enq * sizeof(trb_t);
    command(h, deq | r->cycle, 0, (TRB_SET_DEQ << 10) | ((uint32_t)d->slot << 24) | ((uint32_t)ep->dci << 16), 0);
}

int usb_control(usb_device_t *d, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, void *data,
                uint16_t len) {
    if (d->gone) return -ENODEV;
    xhci_t *h = d->hc;
    usb_endpoint_t *ep = &d->ep0;
    ring_t *r = ep->ring;
    bool in = reqtype & 0x80;
    if (len > PAGE_SIZE) return -EINVAL;
    if (!in && len) memcpy(ep->buf, data, len);
    uint64_t setup = reqtype | ((uint64_t)req << 8) | ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)len << 48);
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ep->done = false;
    ring_push(r, setup, 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16));
    if (len) ring_push(r, ep->buf_phys, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(r, 0, 0, (TRB_STATUS << 10) | TRB_IOC | ((len && in) ? 0 : (1u << 16)));
    ring_doorbell(ep);
    if (!wait_flag(h, &ep->done, 3000)) return -ETIMEDOUT;
    if (ep->cc == CC_STALL) {
        ep_reset(ep);                            /* a stalled control pipe recovers with the next SETUP */
        return -EPIPE;
    }
    if (ep->cc != CC_SUCCESS && ep->cc != CC_SHORT) return -EIO;
    if (in && len) memcpy(data, ep->buf, len);
    return len;
}

/* bulk transfer through the endpoint's DMA buffer (split at 64 KiB boundaries) */
int usb_bulk(usb_endpoint_t *ep, void *data, uint32_t len, uint32_t *actual) {
    usb_device_t *d = ep->dev;
    if (d->gone) return -ENODEV;
    xhci_t *h = d->hc;
    bool in = ep->addr & 0x80;
    if (len > ep->buf_len) return -EINVAL;
    if (!in) memcpy(ep->buf, data, len);
    ep->done = false;
    uint32_t off = 0;
    do {
        uint64_t p = ep->buf_phys + off;
        uint32_t chunk = MIN(len - off, 0x10000 - (uint32_t)(p & 0xFFFF));
        bool last = off + chunk >= len;
        ring_push(ep->ring, p, chunk, (TRB_NORMAL << 10) | (last ? TRB_IOC : TRB_CHAIN) | (in ? TRB_ISP : 0));
        off += chunk;
    } while (off < len);
    ring_doorbell(ep);
    if (!wait_flag(h, &ep->done, 10000)) return -ETIMEDOUT;
    if (ep->cc == CC_STALL) return -EPIPE;
    if (ep->cc != CC_SUCCESS && ep->cc != CC_SHORT) return -EIO;
    uint32_t got = len - ep->residual;
    if (in) memcpy(data, ep->buf, got);
    if (actual) *actual = got;
    return 0;
}

static void requeue_interrupt(usb_endpoint_t *ep) {
    ring_push(ep->ring, ep->buf_phys, ep->buf_len, (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
    ring_doorbell(ep);
}

bool usb_interrupt_start(usb_endpoint_t *ep, uint32_t len) {
    ep->buf_len = MIN(len, (uint32_t)PAGE_SIZE);
    requeue_interrupt(ep);
    return true;
}

/* reset a halted endpoint and clear the halt on the device */
int usb_clear_halt(usb_endpoint_t *ep) {
    ep_reset(ep);
    return usb_control(ep->dev, 0x02, USB_REQ_CLEAR_FEATURE, 0, ep->addr, 0, 0);
}

/* ------------------------------------------------------------------ device setup */

static bool ep_alloc(usb_endpoint_t *ep, uint32_t buf_bytes) {
    ring_t *r = kzalloc(sizeof(ring_t));
    if (!ring_init(r)) { kfree(r); return false; }
    ep->ring = r;
    void *v;
    ep->buf_phys = dma_alloc(buf_bytes, &v);
    if (!ep->buf_phys) return false;
    ep->buf = v;
    ep->buf_len = buf_bytes;
    return true;
}

usb_endpoint_t *usb_add_endpoint(usb_device_t *d, const usb_endpoint_desc_t *e) {
    if (d->neps >= 8) return 0;
    usb_endpoint_t *ep = &d->eps[d->neps];
    memset(ep, 0, sizeof(*ep));
    ep->dev = d;
    ep->addr = e->bEndpointAddress;
    ep->type = e->bmAttributes & 3;
    ep->max_packet = e->wMaxPacketSize & 0x7FF;
    ep->interval = e->bInterval;
    ep->dci = (e->bEndpointAddress & 0xF) * 2 + ((e->bEndpointAddress & 0x80) ? 1 : 0);
    if (!ep_alloc(ep, ep->type == 2 ? 65536 : PAGE_SIZE)) return 0;
    d->neps++;
    return ep;
}

void usb_bind(usb_device_t *d, void (*start)(usb_device_t *, void *), void (*stop)(usb_device_t *, void *), void *ctx) {
    if (d->nbound >= 4) return;
    d->start[d->nbound] = start;
    d->stop[d->nbound] = stop;
    d->ctx[d->nbound] = ctx;
    d->nbound++;
}

static uint32_t xhci_interval(usb_device_t *d, usb_endpoint_t *ep) {
    uint32_t iv = ep->interval ? ep->interval : 1;
    if (d->speed == USB_SPEED_HIGH || d->speed >= USB_SPEED_SUPER) return iv - 1 < 15 ? iv - 1 : 15;
    /* full/low speed: interval in frames (1 ms) -> 2^n * 125 us */
    uint32_t us125 = iv * 8, n = 0;
    while ((1u << (n + 1)) <= us125 && n < 10) n++;
    return n < 3 ? 3 : n;
}

int usb_configure_endpoints(usb_device_t *d) {
    xhci_t *h = d->hc;
    void *in;
    uint64_t in_phys = dma_alloc(33 * h->ctx_size, &in);
    if (!in_phys) return -ENOMEM;
    uint32_t *icc = in;
    int max_dci = 1;
    icc[1] = 1;                               /* add slot context */
    for (int i = 0; i < d->neps; i++) {
        usb_endpoint_t *ep = &d->eps[i];
        icc[1] |= 1u << ep->dci;
        if (ep->dci > max_dci) max_dci = ep->dci;
        uint32_t *c = ctx_at(h, in, 1 + ep->dci);
        bool isin = ep->addr & 0x80;
        uint32_t type = ep->type == 2 ? (isin ? 6 : 2) : ep->type == 3 ? (isin ? 7 : 3) : (isin ? 5 : 1);
        c[0] = ep->type == 3 ? xhci_interval(d, ep) << 16 : 0;
        c[1] = (3u << 1) | (type << 3) | ((uint32_t)ep->max_packet << 16);
        ring_t *r = ep->ring;
        c[2] = (uint32_t)r->phys | 1;
        c[3] = (uint32_t)(r->phys >> 32);
        c[4] = ep->type == 3 ? ep->max_packet : 3072;
        if (ep->type == 3) c[4] |= (uint32_t)ep->max_packet << 16;   /* max ESIT payload */
    }
    /* slot context: copy the current one from the output context, update context entries */
    uint32_t *slot_in = ctx_at(h, in, 1);
    uint32_t *slot_out = P2V(h->dcbaa[d->slot]);
    memcpy(slot_in, slot_out, h->ctx_size);
    slot_in[0] = (slot_in[0] & ~(0x1Fu << 27)) | ((uint32_t)max_dci << 27);
    slot_in[3] = 0;
    if (d->is_hub) {
        slot_in[0] |= 1u << 26;
        if (d->hub_mtt) slot_in[0] |= 1u << 25;
        slot_in[1] = (slot_in[1] & 0x00FFFFFF) | ((uint32_t)d->hub_ports << 24);
        slot_in[2] = (slot_in[2] & ~(3u << 16)) | ((uint32_t)d->hub_ttt << 16);
    }
    int cc = command(h, in_phys, 0, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), 0);
    pmm_free_contig(in_phys, (33 * h->ctx_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (cc != CC_SUCCESS) {
        klog("[usb] configure endpoint failed (cc %d)\n", cc);
        return -EIO;
    }
    return 0;
}

static void read_string(usb_device_t *d, uint8_t idx, char *out, size_t n) {
    out[0] = 0;
    if (!idx) return;
    uint8_t buf[256];
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) | idx, 0x0409, buf, 255) < 4) return;
    size_t j = 0;
    for (int i = 2; i + 1 < buf[0] && j + 1 < n; i += 2) {
        uint16_t c = buf[i] | (buf[i + 1] << 8);
        out[j++] = c < 128 ? (char)c : '?';
    }
    out[j] = 0;
}

void usb_device_remove(usb_device_t *d) {
    xhci_t *h = d->hc;
    d->gone = true;
    for (int i = 0; i < d->nbound; i++)
        if (d->stop[i]) d->stop[i](d, d->ctx[i]);
    if (d->slot) {
        command(h, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)d->slot << 24), 0);
        h->slots[d->slot] = 0;
        h->dcbaa[d->slot] = 0;
    }
    /* rings and buffers are kept: an in-flight DMA must never hit reused memory */
}

/* enumerate a newly connected device on a root port (parent NULL) or on a hub port */
usb_device_t *usb_enumerate(void *hc, usb_device_t *parent, int port, int speed) {
    xhci_t *h = hc;
    usb_device_t *d = kzalloc(sizeof(usb_device_t));
    d->hc = h;
    d->port = port;
    d->speed = speed;
    d->parent = parent;
    if (parent) {
        d->root_port = parent->root_port;
        d->depth = parent->depth + 1;
        d->route = parent->route | ((uint32_t)MIN(port, 15) << (4 * parent->depth));
    } else {
        d->root_port = port;
    }
    uint8_t slot = 0;
    if (command(h, 0, 0, TRB_ENABLE_SLOT << 10, &slot) != CC_SUCCESS || !slot || slot > MAX_SLOTS) {
        klog("[usb] port %d: no free device slot\n", port);
        kfree(d);
        return 0;
    }
    d->slot = slot;
    h->slots[slot] = d;
    if (!parent) h->port_dev[port] = d;

    /* output device context */
    void *out;
    uint64_t out_phys = dma_alloc(32 * h->ctx_size, &out);
    h->dcbaa[slot] = out_phys;

    /* default control endpoint */
    d->ep0.dev = d;
    d->ep0.dci = 1;
    d->ep0.max_packet = speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    if (!ep_alloc(&d->ep0, PAGE_SIZE)) return d;

    void *in;
    uint64_t in_phys = dma_alloc(33 * h->ctx_size, &in);
    uint32_t *icc = in;
    icc[1] = 3;                                   /* add slot + EP0 */
    uint32_t *slotc = ctx_at(h, in, 1);
    slotc[0] = (1u << 27) | ((uint32_t)speed << 20) | d->route;
    slotc[1] = (uint32_t)d->root_port << 16;
    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
        /* behind a high-speed hub the hub's transaction translator talks to us */
        usb_device_t *child = d;
        for (usb_device_t *p = parent; p; child = p, p = p->parent) {
            if (p->speed == USB_SPEED_HIGH) {
                slotc[2] = (uint32_t)p->slot | ((uint32_t)child->port << 8);
                if (p->hub_mtt) slotc[0] |= 1u << 25;
                break;
            }
        }
    }
    uint32_t *ep0c = ctx_at(h, in, 2);
    ring_t *r = d->ep0.ring;
    ep0c[1] = (3u << 1) | (4u << 3) | ((uint32_t)d->ep0.max_packet << 16);
    ep0c[2] = (uint32_t)r->phys | 1;
    ep0c[3] = (uint32_t)(r->phys >> 32);
    ep0c[4] = 8;
    int cc = command(h, in_phys, 0, (TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24), 0);
    if (cc != CC_SUCCESS) {
        klog("[usb] port %d: address device failed (cc %d)\n", port, cc);
        usb_device_remove(d);
        return 0;
    }
    pit_delay_ms(2);

    /* device descriptor (first 8 bytes tell the real EP0 packet size) */
    uint8_t buf[18];
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, buf, 8) < 8) {
        klog("[usb] port %d: cannot read device descriptor\n", port);
        usb_device_remove(d);
        return 0;
    }
    uint16_t mps = speed >= USB_SPEED_SUPER ? (1u << buf[7]) : buf[7];
    if (mps && mps != d->ep0.max_packet) {
        d->ep0.max_packet = mps;
        memset(in, 0, 33 * h->ctx_size);
        icc[1] = 2;
        ep0c[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
        ep0c[2] = (uint32_t)r->phys | 1;
        ep0c[4] = 8;
        command(h, in_phys, 0, (TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24), 0);
    }
    pmm_free_contig(in_phys, (33 * h->ctx_size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &d->desc, 18) < 18) {
        usb_device_remove(d);
        return 0;
    }
    read_string(d, d->desc.iProduct, d->product, sizeof(d->product));
    if (!d->product[0]) snprintf(d->product, sizeof(d->product), "USB device %04x:%04x", d->desc.idVendor,
                                  d->desc.idProduct);

    /* configuration descriptor */
    usb_config_desc_t cfg;
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &cfg, 9) < 9) {
        usb_device_remove(d);
        return 0;
    }
    d->config_len = MIN(cfg.wTotalLength, (uint16_t)PAGE_SIZE);
    d->config = kmalloc(d->config_len);
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, d->config, d->config_len) <
        d->config_len) {
        usb_device_remove(d);
        return 0;
    }
    static const char *speeds[] = { "?", "full", "low", "high", "super", "super+" };
    klog("[usb] port %d: %s (%04x:%04x, %s speed)\n", port, d->product, d->desc.idVendor, d->desc.idProduct,
         speeds[speed < 6 ? speed : 0]);

    /* offer each interface to the class drivers */
    uint8_t *p = d->config, *end = d->config + d->config_len;
    while (p + 2 <= end && p[0]) {
        if (p[1] == USB_DT_INTERFACE && p + 9 <= end) {
            usb_interface_desc_t *intf = (usb_interface_desc_t *)p;
            uint8_t *extra = p + p[0];
            uint8_t *q = extra;
            while (q + 2 <= end && q[0] && q[1] != USB_DT_INTERFACE) q += q[0];
            if (intf->bAlternateSetting == 0) {
                if (!usb_hid_probe(d, intf, extra, (int)(q - extra)) &&
                    !usb_msc_probe(d, intf, extra, (int)(q - extra)))
                    usb_hub_probe(d, intf, extra, (int)(q - extra));
            }
            p = q;
            continue;
        }
        p += p[0];
    }
    if (!d->nbound) {
        klog("[usb]   no driver for this device (class %02x)\n", d->desc.bDeviceClass);
        return d;
    }
    if (usb_control(d, 0x00, USB_REQ_SET_CONFIG, cfg.bConfigurationValue, 0, 0, 0) < 0 ||
        usb_configure_endpoints(d) < 0) {
        klog("[usb]   configuration failed\n");
        return d;
    }
    for (int i = 0; i < d->nbound; i++) d->start[i](d, d->ctx[i]);
    return d;
}

static void port_disconnect(xhci_t *h, int port) {
    usb_device_t *d = h->port_dev[port];
    if (!d) return;
    klog("[usb] port %d: %s disconnected\n", port, d->product);
    h->port_dev[port] = 0;
    usb_device_remove(d);
}

static void port_check(xhci_t *h, int port) {
    uint32_t sc = rd32(h->op, OP_PORTSC(port));
    uint32_t neutral = sc & ~(PORT_PED | PORT_CHANGE_BITS | PORT_PR);
    wr32(h->op, OP_PORTSC(port), neutral | (sc & PORT_CHANGE_BITS));   /* acknowledge changes */
    bool connected = sc & PORT_CCS;
    if (!connected) {
        port_disconnect(h, port);
        return;
    }
    if (h->port_dev[port]) {
        if (!(sc & PORT_CSC)) return;                /* nothing new */
        port_disconnect(h, port);                    /* re-plugged quickly */
    }
    if (!(sc & PORT_PED)) {
        /* USB 2 port: reset it to enable */
        wr32(h->op, OP_PORTSC(port), neutral | PORT_PR);
        for (int i = 0; i < 100; i++) {
            pit_delay_ms(2);
            sc = rd32(h->op, OP_PORTSC(port));
            if (sc & PORT_PRC) break;
        }
        wr32(h->op, OP_PORTSC(port), (sc & ~(PORT_PED | PORT_CHANGE_BITS | PORT_PR)) | PORT_PRC);
        if (!(rd32(h->op, OP_PORTSC(port)) & PORT_PED)) return;
    }
    pit_delay_ms(10);
    sc = rd32(h->op, OP_PORTSC(port));
    usb_enumerate(h, 0, port, (sc >> 10) & 0xF);
}

/* ------------------------------------------------------------------ controller */

static void bios_handoff(xhci_t *h, uint32_t hccp1) {
    uint32_t off = (hccp1 >> 16) << 2;
    for (int n = 0; off && n < 32; n++) {
        volatile uint32_t *cap = (volatile uint32_t *)(h->base + off);
        uint32_t v = cap[0];
        if ((v & 0xFF) == 1) {
            cap[0] = v | (1u << 24);                 /* OS owned */
            for (int i = 0; i < 100 && (cap[0] & (1u << 16)); i++) pit_delay_ms(10);
            cap[0] &= ~(1u << 16);
            cap[1] &= 0x1F1EE;                       /* disable SMIs */
            break;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

static int usbd_thread(void *arg) {
    UNUSED(arg);
    for (;;) {
        for (int c = 0; c < ncontrollers; c++) {
            xhci_t *h = controllers[c];
            process_events(h);
            uint32_t ch = __atomic_exchange_n(&h->port_change, 0, __ATOMIC_ACQ_REL);
            for (uint32_t p = 1; p <= h->max_ports && p <= 32; p++)
                if (ch & (1u << (p - 1))) port_check(h, p);
        }
        usb_hid_tick();
        static uint64_t last_hub_poll;
        if (uptime_ms() - last_hub_poll >= 250) {
            last_hub_poll = uptime_ms();
            usb_hub_poll();
        }
        uint64_t f = irq_save();
        wq_wait_timeout(&controllers[0]->wq, 10);
        irq_restore(f);
    }
    return 0;
}

static void xhci_init_ctrl(pci_dev_t *pd) {
    xhci_t *h = kzalloc(sizeof(xhci_t));
    h->pci = pd;
    pci_enable_bus_master(pd);
    h->base = pci_map_bar(pd, 0, 0);
    if (!h->base) { klog("[xhci] cannot map registers\n"); return; }
    uint8_t caplen = h->base[0];
    uint32_t hcs1 = rd32(h->base, 4), hcs2 = rd32(h->base, 8), hcc1 = rd32(h->base, 0x10);
    h->op = h->base + caplen;
    h->db = h->base + (rd32(h->base, 0x14) & ~3u);
    h->rt = h->base + (rd32(h->base, 0x18) & ~0x1Fu);
    h->max_slots = MIN(hcs1 & 0xFF, (uint32_t)MAX_SLOTS);
    h->max_ports = (hcs1 >> 24) & 0xFF;
    h->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    bios_handoff(h, hcc1);

    /* halt and reset */
    wr32(h->op, OP_USBCMD, rd32(h->op, OP_USBCMD) & ~CMD_RUN);
    for (int i = 0; i < 100 && !(rd32(h->op, OP_USBSTS) & STS_HCH); i++) pit_delay_ms(1);
    wr32(h->op, OP_USBCMD, CMD_HCRST);
    for (int i = 0; i < 500 && (rd32(h->op, OP_USBCMD) & CMD_HCRST); i++) pit_delay_ms(1);
    for (int i = 0; i < 500 && (rd32(h->op, OP_USBSTS) & STS_CNR); i++) pit_delay_ms(1);
    if (rd32(h->op, OP_USBSTS) & STS_CNR) { klog("[xhci] controller not ready after reset\n"); return; }

    wr32(h->op, OP_CONFIG, h->max_slots);
    void *v;
    uint64_t dcbaa_phys = dma_alloc(2048, &v);
    h->dcbaa = v;
    /* scratchpad buffers */
    uint32_t nsp = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nsp) {
        void *arr;
        uint64_t arr_phys = dma_alloc(nsp * 8, &arr);
        for (uint32_t i = 0; i < nsp; i++) ((uint64_t *)arr)[i] = dma_alloc(PAGE_SIZE, 0);
        h->dcbaa[0] = arr_phys;
    }
    wr64(h->op, OP_DCBAAP, dcbaa_phys);
    if (!ring_init(&h->cmd)) return;
    wr64(h->op, OP_CRCR, h->cmd.phys | 1);

    /* event ring with one segment */
    h->evt_phys = dma_alloc(RING_TRBS * sizeof(trb_t), &v);
    h->evt = v;
    h->evt_cycle = 1;
    uint64_t *erst;
    uint64_t erst_phys = dma_alloc(64, (void **)&erst);
    erst[0] = h->evt_phys;
    erst[1] = RING_TRBS;
    wr32(h->rt, 0x20 + 0x08, 1);                    /* ERSTSZ */
    wr64(h->rt, 0x20 + 0x18, h->evt_phys);          /* ERDP */
    wr64(h->rt, 0x20 + 0x10, erst_phys);            /* ERSTBA */
    wr32(h->rt, 0x20 + 0x04, 4000);                 /* IMOD: 1 ms */
    h->msi = pci_enable_msi(pd, xhci_irq, h);
    wr32(h->rt, 0x20, rd32(h->rt, 0x20) | 3);        /* IMAN: IP clear + IE */
    wr32(h->op, OP_USBCMD, CMD_RUN | CMD_INTE);
    for (int i = 0; i < 100 && (rd32(h->op, OP_USBSTS) & STS_HCH); i++) pit_delay_ms(1);

    uint16_t ver = rd32(h->base, 0) >> 16;
    klog("[xhci] controller %04x:%04x, xHCI %x.%02x, %u ports, %u slots, %s\n", pd->vendor, pd->device, ver >> 8,
         ver & 0xFF, h->max_ports, h->max_slots, h->msi ? "MSI" : "polling");
    controllers[ncontrollers++] = h;

    /* power all ports, then look at every port once */
    for (uint32_t p = 1; p <= h->max_ports; p++) {
        uint32_t sc = rd32(h->op, OP_PORTSC(p));
        if (!(sc & PORT_PP)) wr32(h->op, OP_PORTSC(p), (sc & ~(PORT_PED | PORT_CHANGE_BITS)) | PORT_PP);
    }
    pit_delay_ms(50);
    for (uint32_t p = 1; p <= h->max_ports && p <= 32; p++) {
        if (rd32(h->op, OP_PORTSC(p)) & PORT_CCS) port_check(h, p);
    }
}

void xhci_init(void) {
    if (cmdline_has("nousb")) return;
    for (int i = 0; i < pci_count() && ncontrollers < 4; i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) xhci_init_ctrl(d);
    }
    if (ncontrollers) kthread_create("usbd", usbd_thread, 0);
}
