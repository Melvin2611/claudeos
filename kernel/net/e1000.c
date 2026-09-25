/* Intel PRO/1000 (e1000 / 82540EM family) network driver */
#include "net.h"
#include <pci.h>
#include <mm.h>
#include <cpu.h>

#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_IMS 0x00D0
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL 0x5400
#define REG_RAH 0x5404

#define NRX 64
#define NTX 32
#define BUFSZ 2048

typedef struct { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed)) rxd_t;
typedef struct { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed)) txd_t;

static volatile uint8_t *mmio;
static rxd_t *rx;
static txd_t *tx;
static uint8_t *rxbuf, *txbuf;
static uint64_t rxbuf_phys, txbuf_phys;
static uint32_t rx_cur, tx_cur;
static mutex_t tx_lock;

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static uint16_t eeprom_read(uint8_t addr) {
    wr(REG_EERD, 1 | ((uint32_t)addr << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1 << 4)) return (uint16_t)(v >> 16);
    }
    return 0;
}

static int e1000_send(const void *frame, size_t len) {
    if (len > BUFSZ) return -EINVAL;
    mutex_lock(&tx_lock);
    txd_t *d = &tx[tx_cur];
    /* wait for the descriptor to be free (descriptor done) */
    for (int i = 0; i < 1000000 && d->cmd && !(d->status & 1); i++) cpu_pause();
    memcpy(txbuf + tx_cur * BUFSZ, frame, len);
    d->addr = txbuf_phys + tx_cur * BUFSZ;
    d->len = (uint16_t)len;
    d->cso = 0;
    d->status = 0;
    d->cmd = 0x01 | 0x02 | 0x08;   /* EOP | IFCS | RS */
    tx_cur = (tx_cur + 1) % NTX;
    wr(REG_TDT, tx_cur);
    mutex_unlock(&tx_lock);
    netif.tx_packets++;
    netif.tx_bytes += len;
    return 0;
}

static int e1000_poll(void (*deliver)(const uint8_t *frame, size_t len)) {
    int n = 0;
    for (;;) {
        rxd_t *d = &rx[rx_cur];
        if (!(d->status & 1)) break;
        uint16_t len = d->len;
        if ((d->status & 2) && !d->errors && len >= 14) {
            netif.rx_packets++;
            netif.rx_bytes += len;
            deliver(rxbuf + rx_cur * BUFSZ, len);
        }
        d->status = 0;
        uint32_t old = rx_cur;
        rx_cur = (rx_cur + 1) % NRX;
        wr(REG_RDT, old);
        n++;
    }
    return n;
}

static void e1000_irq(regs_t *r, void *ctx) {
    UNUSED(r);
    UNUSED(ctx);
    uint32_t icr = rd(REG_ICR);   /* reading clears */
    if (icr) net_rx_kick();
}

void e1000_init(void) {
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x100C, 0x1015, 0x1019, 0x101E, 0x10D3, 0x107C, 0x1076 };
    pci_dev_t *dev = 0;
    for (size_t i = 0; i < ARRAY_SIZE(ids) && !dev; i++) dev = pci_find(0x8086, ids[i]);
    if (!dev) { klog("[net] no supported network adapter (e1000) found\n"); return; }
    pci_enable_bus_master(dev);
    uint64_t base = dev->bar[0] & ~0xFULL;
    if ((dev->bar[0] & 0x6) == 0x4) base |= (uint64_t)dev->bar[1] << 32;
    mmio = ioremap(base, 128 * 1024, CACHE_UC);
    /* reset */
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26));
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & (1u << 26)); i++) cpu_pause();
    wr(REG_IMC, 0xFFFFFFFF);
    rd(REG_ICR);
    wr(REG_CTRL, (rd(REG_CTRL) | (1 << 6) | (1 << 5)) & ~((1u << 3) | (1u << 31) | (1u << 7)));   /* SLU, ASDE */
    /* MAC address */
    uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
    if (rah & (1u << 31)) {
        for (int i = 0; i < 4; i++) netif.mac[i] = (uint8_t)(ral >> (i * 8));
        netif.mac[4] = (uint8_t)rah;
        netif.mac[5] = (uint8_t)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read((uint8_t)i);
            netif.mac[i * 2] = (uint8_t)w;
            netif.mac[i * 2 + 1] = (uint8_t)(w >> 8);
        }
        wr(REG_RAL, netif.mac[0] | (netif.mac[1] << 8) | (netif.mac[2] << 16) | ((uint32_t)netif.mac[3] << 24));
        wr(REG_RAH, netif.mac[4] | (netif.mac[5] << 8) | (1u << 31));
    }
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);
    /* descriptor rings and buffers (below 4 GiB) */
    uint64_t rxp = pmm_alloc_contig(1, 0x100000000ULL), txp = pmm_alloc_contig(1, 0x100000000ULL);
    rxbuf_phys = pmm_alloc_contig(NRX * BUFSZ / 4096, 0x100000000ULL);
    txbuf_phys = pmm_alloc_contig(NTX * BUFSZ / 4096, 0x100000000ULL);
    if (!rxp || !txp || !rxbuf_phys || !txbuf_phys) { klog("[net] out of DMA memory\n"); return; }
    rx = P2V(rxp);
    tx = P2V(txp);
    rxbuf = P2V(rxbuf_phys);
    txbuf = P2V(txbuf_phys);
    for (int i = 0; i < NRX; i++) { rx[i].addr = rxbuf_phys + i * BUFSZ; rx[i].status = 0; }
    for (int i = 0; i < NTX; i++) { tx[i].addr = txbuf_phys + i * BUFSZ; tx[i].cmd = 0; tx[i].status = 1; }
    wr(REG_RDBAL, (uint32_t)rxp);
    wr(REG_RDBAH, (uint32_t)(rxp >> 32));
    wr(REG_RDLEN, NRX * sizeof(rxd_t));
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    rx_cur = 0;
    wr(REG_RCTL, (1 << 1) | (1 << 15) | (1 << 26) | (1 << 4));   /* EN, BAM, SECRC, MPE */
    wr(REG_TDBAL, (uint32_t)txp);
    wr(REG_TDBAH, (uint32_t)(txp >> 32));
    wr(REG_TDLEN, NTX * sizeof(txd_t));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_cur = 0;
    wr(REG_TCTL, (1 << 1) | (1 << 3) | (0x10 << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);
    netif.present = true;
    netif.send = e1000_send;
    netif.poll = e1000_poll;
    snprintf(netif.driver, sizeof(netif.driver), "Intel PRO/1000 (%04x)", dev->device);
    if (dev->irq && dev->irq < 16) {
        irq_register(dev->irq, e1000_irq, 0);
        wr(REG_IMS, (1 << 7) | (1 << 2) | (1 << 4) | (1 << 6));   /* RXT0, LSC, RXDMT0, RXO */
    }
    uint32_t st = rd(REG_STATUS);
    klog("[net] %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s, irq %u\n", netif.driver, netif.mac[0], netif.mac[1],
         netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5], (st & 2) ? "up" : "down", dev->irq);
}
