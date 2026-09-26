/* Network core: Ethernet, ARP, IPv4, ICMP, UDP demux, DHCP client, DNS resolver, syscalls */
#include "net.h"
#include <syscall.h>
#include <mm.h>

netif_t netif;
waitq_t net_wq;
mutex_t net_lock;

static task_t *net_task;
static waitq_t rx_wq, arp_wq, ping_wq, dns_wq;
static volatile bool rx_pending;
static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void ip_str(uint32_t ip, char *out, size_t n) {
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

uint16_t ip_checksum(const void *data, size_t len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

void net_rx_kick(void) {
    rx_pending = true;
    wq_wake_all(&rx_wq);
}

int net_send_eth(const uint8_t dst[6], uint16_t type, const void *payload, size_t len) {
    if (!netif.present || len > ETH_MTU) return -EINVAL;
    uint8_t frame[ETH_HLEN + ETH_MTU];
    memcpy(frame, dst, 6);
    memcpy(frame + 6, netif.mac, 6);
    frame[12] = (uint8_t)(type >> 8);
    frame[13] = (uint8_t)type;
    memcpy(frame + ETH_HLEN, payload, len);
    size_t total = ETH_HLEN + len;
    if (total < 60) { memset(frame + total, 0, 60 - total); total = 60; }
    return netif.send(frame, total);
}

/* ------------------------------------------------------------------ ARP */
#define ARP_SLOTS 32
static struct { uint32_t ip; uint8_t mac[6]; uint64_t time; bool valid; } arp_cache[ARP_SLOTS];

static void arp_store(uint32_t ip, const uint8_t mac[6]) {
    if (!ip || ip == 0xFFFFFFFF) return;
    int slot = -1, free_slot = -1, oldest_slot = 0;
    for (int i = 0; i < ARP_SLOTS; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { slot = i; break; }
        if (!arp_cache[i].valid && free_slot < 0) free_slot = i;
        if (arp_cache[i].time < arp_cache[oldest_slot].time) oldest_slot = i;
    }
    if (slot < 0) slot = free_slot >= 0 ? free_slot : oldest_slot;
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].time = uptime_ms();
    arp_cache[slot].valid = true;
    uint64_t f = irq_save();
    wq_wake_all(&arp_wq);
    irq_restore(f);
}

static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { memcpy(mac, arp_cache[i].mac, 6); return true; }
    return false;
}

static void arp_send(uint16_t op, const uint8_t tha[6], uint32_t tpa, const uint8_t *dst) {
    uint8_t p[28];
    p[0] = 0; p[1] = 1;             /* ethernet */
    p[2] = 0x08; p[3] = 0x00;       /* IPv4 */
    p[4] = 6; p[5] = 4;
    p[6] = 0; p[7] = (uint8_t)op;
    memcpy(p + 8, netif.mac, 6);
    memcpy(p + 14, &netif.ip, 4);
    memcpy(p + 18, tha, 6);
    memcpy(p + 24, &tpa, 4);
    net_send_eth(dst, 0x0806, p, 28);
}

static void arp_input(const uint8_t *p, size_t len) {
    if (len < 28) return;
    uint16_t op = (p[6] << 8) | p[7];
    uint32_t spa, tpa;
    memcpy(&spa, p + 14, 4);
    memcpy(&tpa, p + 24, 4);
    arp_store(spa, p + 8);
    if (op == 1 && netif.ip && tpa == netif.ip) arp_send(2, p + 8, spa, p + 8);
}

int arp_resolve(uint32_t ip, uint8_t mac[6], uint64_t timeout_ms) {
    if (ip == 0xFFFFFFFF || (netif.mask && (ip | netif.mask) == 0xFFFFFFFF)) { memcpy(mac, bcast, 6); return 0; }
    if (arp_lookup(ip, mac)) return 0;
    static const uint8_t zero[6] = { 0 };
    bool in_net_thread = current == net_task;
    for (int tries = 0; tries < 3; tries++) {
        arp_send(1, zero, ip, bcast);
        if (in_net_thread) return -EHOSTUNREACH;   /* never block the receive path */
        uint64_t end = uptime_ms() + timeout_ms / 3;
        while (uptime_ms() < end) {
            mutex_unlock(&net_lock);
            uint64_t f = irq_save();
            wq_wait_timeout(&arp_wq, 50);
            irq_restore(f);
            mutex_lock(&net_lock);
            if (arp_lookup(ip, mac)) return 0;
        }
    }
    return -EHOSTUNREACH;
}

/* ------------------------------------------------------------------ IPv4 */
static uint16_t ip_id;

int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    if (len + 20 > ETH_MTU) return -EINVAL;
    uint8_t pkt[ETH_MTU];
    pkt[0] = 0x45;
    pkt[1] = 0;
    uint16_t total = (uint16_t)(len + 20);
    pkt[2] = (uint8_t)(total >> 8);
    pkt[3] = (uint8_t)total;
    uint16_t id = ip_id++;
    pkt[4] = (uint8_t)(id >> 8);
    pkt[5] = (uint8_t)id;
    pkt[6] = 0x40;   /* don't fragment */
    pkt[7] = 0;
    pkt[8] = 64;
    pkt[9] = proto;
    pkt[10] = pkt[11] = 0;
    memcpy(pkt + 12, &netif.ip, 4);
    memcpy(pkt + 16, &dst, 4);
    uint16_t cs = ip_checksum(pkt, 20, 0);
    memcpy(pkt + 10, &cs, 2);
    memcpy(pkt + 20, payload, len);
    uint32_t hop = dst;
    if (dst != 0xFFFFFFFF && netif.mask && (dst & netif.mask) != (netif.ip & netif.mask)) hop = netif.gw;
    uint8_t mac[6];
    int r = arp_resolve(hop, mac, 3000);
    if (r < 0) return r;
    return net_send_eth(mac, 0x0800, pkt, total);
}

/* ------------------------------------------------------------------ ICMP */
static volatile int ping_seq_got = -1;
static volatile uint64_t ping_rtt_start;

static void icmp_input(uint32_t src, const uint8_t *p, size_t len) {
    if (len < 8) return;
    if (p[0] == 8) {
        /* echo request -> reply */
        uint8_t reply[ETH_MTU];
        size_t n = MIN(len, sizeof(reply) - 20);
        memcpy(reply, p, n);
        reply[0] = 0;
        reply[2] = reply[3] = 0;
        uint16_t cs = ip_checksum(reply, n, 0);
        memcpy(reply + 2, &cs, 2);
        ip_send(src, 1, reply, n);
    } else if (p[0] == 0) {
        uint16_t id = (p[4] << 8) | p[5], seq = (p[6] << 8) | p[7];
        if (id == 0x4C43) {
            ping_seq_got = seq;
            uint64_t f = irq_save();
            wq_wake_all(&ping_wq);
            irq_restore(f);
        }
    }
}

/* returns round trip time in ms, or a negative error */
int icmp_ping(uint32_t dst, uint16_t seq, uint64_t timeout_ms) {
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    p[0] = 8;
    p[4] = 0x4C; p[5] = 0x43;
    p[6] = (uint8_t)(seq >> 8); p[7] = (uint8_t)seq;
    for (int i = 8; i < 64; i++) p[i] = (uint8_t)i;
    uint16_t cs = ip_checksum(p, sizeof(p), 0);
    memcpy(p + 2, &cs, 2);
    mutex_lock(&net_lock);
    ping_seq_got = -1;
    uint64_t start = uptime_ms();
    uint64_t tsc0 = rdtsc();
    int r = ip_send(dst, 1, p, sizeof(p));
    mutex_unlock(&net_lock);
    if (r < 0) return r;
    while (uptime_ms() - start < timeout_ms) {
        if (ping_seq_got == seq) {
            extern uint64_t cpu_mhz;
            uint64_t us = cpu_mhz ? (rdtsc() - tsc0) / cpu_mhz : (uptime_ms() - start) * 1000;
            return (int)us;   /* microseconds */
        }
        uint64_t f = irq_save();
        wq_wait_timeout(&ping_wq, 20);
        irq_restore(f);
        if (task_interrupted(current)) return -EINTR;
    }
    return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ UDP */
static void udp_input_ip(uint32_t src, const uint8_t *p, size_t len) {
    if (len < 8) return;
    uint16_t sport = (p[0] << 8) | p[1], dport = (p[2] << 8) | p[3];
    uint16_t ulen = (p[4] << 8) | p[5];
    if (ulen < 8 || ulen > len) return;
    if (dport == 68) { dhcp_input(p + 8, ulen - 8); return; }
    socket_udp_deliver(src, sport, dport, p + 8, ulen - 8);
}

int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len) {
    if (len + 8 + 20 > ETH_MTU) return -EINVAL;
    uint8_t p[ETH_MTU];
    p[0] = (uint8_t)(sport >> 8); p[1] = (uint8_t)sport;
    p[2] = (uint8_t)(dport >> 8); p[3] = (uint8_t)dport;
    uint16_t l = (uint16_t)(len + 8);
    p[4] = (uint8_t)(l >> 8); p[5] = (uint8_t)l;
    p[6] = p[7] = 0;   /* checksum optional for IPv4 */
    memcpy(p + 8, data, len);
    return ip_send(dst, 17, p, len + 8);
}

/* ------------------------------------------------------------------ receive path */
static void deliver(const uint8_t *f, size_t len) {
    uint16_t type = (f[12] << 8) | f[13];
    const uint8_t *p = f + ETH_HLEN;
    size_t plen = len - ETH_HLEN;
    if (type == 0x0806) { arp_input(p, plen); return; }
    if (type != 0x0800 || plen < 20) return;
    uint8_t ihl = (p[0] & 0x0F) * 4;
    if ((p[0] >> 4) != 4 || ihl < 20 || plen < ihl) return;
    uint16_t total = (p[2] << 8) | p[3];
    if (total > plen || total < ihl) return;
    if (((p[6] & 0x3F) || p[7]) && !(p[6] & 0x40)) {
        /* fragments are not supported */
        if ((p[6] & 0x20) || p[7] || (p[6] & 0x1F)) return;
    }
    uint32_t src, dst;
    memcpy(&src, p + 12, 4);
    memcpy(&dst, p + 16, 4);
    bool for_us = dst == netif.ip || dst == 0xFFFFFFFF || !netif.ip ||
                  (netif.mask && (dst | netif.mask) == 0xFFFFFFFF);
    if (!for_us) return;
    /* learn the sender's MAC when it is on our network */
    if (netif.mask && (src & netif.mask) == (netif.ip & netif.mask)) arp_store(src, f + 6);
    switch (p[9]) {
    case 1: icmp_input(src, p + ihl, total - ihl); break;
    case 17: udp_input_ip(src, p + ihl, total - ihl); break;
    case 6: tcp_input(src, dst, p + ihl, total - ihl); break;
    }
}

/* ------------------------------------------------------------------ DHCP */
static uint32_t dhcp_xid, dhcp_offer_ip, dhcp_server;
static int dhcp_state;       /* 0 idle, 1 discover sent, 2 request sent, 3 bound */
static uint64_t dhcp_timer;

static void dhcp_send(int type) {
    uint8_t p[300];
    memset(p, 0, sizeof(p));
    p[0] = 1; p[1] = 1; p[2] = 6;
    memcpy(p + 4, &dhcp_xid, 4);
    p[10] = 0x80;   /* broadcast flag */
    memcpy(p + 28, netif.mac, 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;   /* magic cookie */
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (uint8_t)type;
    p[o++] = 61; p[o++] = 7; p[o++] = 1; memcpy(p + o, netif.mac, 6); o += 6;
    p[o++] = 12; p[o++] = 8; memcpy(p + o, "claudeos", 8); o += 8;
    if (type == 3) {
        p[o++] = 50; p[o++] = 4; memcpy(p + o, &dhcp_offer_ip, 4); o += 4;
        p[o++] = 54; p[o++] = 4; memcpy(p + o, &dhcp_server, 4); o += 4;
    }
    p[o++] = 55; p[o++] = 4; p[o++] = 1; p[o++] = 3; p[o++] = 6; p[o++] = 51;
    p[o++] = 255;
    /* sent from 0.0.0.0:68 to 255.255.255.255:67 */
    uint32_t saved = netif.ip;
    netif.ip = 0;
    udp_send(0xFFFFFFFF, 68, 67, p, (size_t)MAX(o, 300));
    netif.ip = saved;
}

void dhcp_input(const uint8_t *p, size_t len) {
    if (len < 240 || p[0] != 2 || memcmp(p + 4, &dhcp_xid, 4)) return;
    if (p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) return;
    uint32_t yiaddr;
    memcpy(&yiaddr, p + 16, 4);
    int type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0, lease = 0;
    for (size_t o = 240; o + 1 < len && p[o] != 255;) {
        uint8_t opt = p[o], l = p[o + 1];
        if (opt == 0) { o++; continue; }
        const uint8_t *v = p + o + 2;
        if (o + 2 + l > len) break;
        if (opt == 53 && l >= 1) type = v[0];
        else if (opt == 1 && l >= 4) memcpy(&mask, v, 4);
        else if (opt == 3 && l >= 4) memcpy(&router, v, 4);
        else if (opt == 6 && l >= 4) memcpy(&dns, v, 4);
        else if (opt == 54 && l >= 4) memcpy(&server, v, 4);
        else if (opt == 51 && l >= 4) lease = ntohl(*(const uint32_t *)v);
        o += 2 + l;
    }
    if (type == 2 && dhcp_state == 1) {
        dhcp_offer_ip = yiaddr;
        dhcp_server = server;
        dhcp_state = 2;
        dhcp_send(3);
        dhcp_timer = uptime_ms();
    } else if (type == 5 && dhcp_state == 2) {
        netif.ip = yiaddr;
        netif.mask = mask ? mask : IP4(255, 255, 255, 0);
        netif.gw = router;
        netif.dns = dns ? dns : router;
        netif.lease = lease;
        netif.up = true;
        dhcp_state = 3;
        char a[20], g[20], d[20];
        ip_str(netif.ip, a, sizeof(a));
        ip_str(netif.gw, g, sizeof(g));
        ip_str(netif.dns, d, sizeof(d));
        klog("[net] DHCP: address %s, gateway %s, DNS %s, lease %u s\n", a, g, d, lease);
    } else if (type == 6) {
        dhcp_state = 0;   /* NAK: start over */
    }
}

void dhcp_start(void) {
    dhcp_xid = (uint32_t)rdtsc();
    dhcp_state = 1;
    dhcp_timer = uptime_ms();
    dhcp_send(1);
}

static void dhcp_tick(void) {
    if (dhcp_state == 3) return;
    if (uptime_ms() - dhcp_timer > 3000) dhcp_start();
}

/* ------------------------------------------------------------------ DNS */
#define DNS_CACHE 16
static struct { char name[64]; uint32_t ip; uint64_t time; } dns_cache[DNS_CACHE];
static int dns_next;
static uint16_t dns_txid;
static volatile uint32_t dns_answer;
static volatile int dns_rcode = -1;
static volatile uint16_t dns_wait_id;

/* raw DNS response delivered by the socket layer for port 53535 */
void dns_input(const uint8_t *p, size_t len) {
    if (len < 12) return;
    uint16_t id = (p[0] << 8) | p[1];
    if (id != dns_wait_id) return;
    int rcode = p[3] & 0x0F;
    uint16_t qd = (p[4] << 8) | p[5], an = (p[6] << 8) | p[7];
    size_t o = 12;
    for (int i = 0; i < qd && o < len; i++) {
        while (o < len && p[o]) { if ((p[o] & 0xC0) == 0xC0) { o++; break; } o += p[o] + 1; }
        o += 5;
    }
    uint32_t ip = 0;
    for (int i = 0; i < an && o + 10 < len; i++) {
        /* name (possibly compressed) */
        while (o < len) {
            if ((p[o] & 0xC0) == 0xC0) { o += 2; break; }
            if (!p[o]) { o++; break; }
            o += p[o] + 1;
        }
        if (o + 10 > len) break;
        uint16_t type = (p[o] << 8) | p[o + 1];
        uint16_t rdlen = (p[o + 8] << 8) | p[o + 9];
        o += 10;
        if (type == 1 && rdlen == 4 && o + 4 <= len) { memcpy(&ip, p + o, 4); break; }
        o += rdlen;
    }
    dns_answer = ip;
    dns_rcode = ip ? 0 : (rcode ? rcode : 3);
    uint64_t f = irq_save();
    wq_wake_all(&dns_wq);
    irq_restore(f);
}

static bool parse_ip(const char *s, uint32_t *out) {
    uint32_t parts[4];
    int n = 0;
    while (n < 4) {
        if (!isdigit(*s)) return false;
        uint32_t v = 0;
        while (isdigit(*s)) v = v * 10 + (*s++ - '0');
        if (v > 255) return false;
        parts[n++] = v;
        if (n < 4) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = IP4(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

int dns_resolve(const char *name, uint32_t *ip) {
    if (parse_ip(name, ip)) return 0;
    if (!strcasecmp(name, "localhost")) { *ip = IP4(127, 0, 0, 1); return 0; }
    for (int i = 0; i < DNS_CACHE; i++)
        if (dns_cache[i].name[0] && !strcasecmp(dns_cache[i].name, name) && uptime_ms() - dns_cache[i].time < 300000) {
            *ip = dns_cache[i].ip;
            return 0;
        }
    if (!netif.up || !netif.dns) return -ENETDOWN;
    uint8_t q[300];
    size_t n = 12;
    memset(q, 0, 12);
    for (int attempt = 0; attempt < 3; attempt++) {
        uint16_t id = ++dns_txid ^ (uint16_t)rdtsc();
        q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
        q[2] = 0x01;   /* recursion desired */
        q[5] = 1;      /* one question */
        n = 12;
        const char *p = name;
        while (*p && n < 280) {
            const char *dot = strchr(p, '.');
            size_t l = dot ? (size_t)(dot - p) : strlen(p);
            if (l == 0 || l > 63) return -EINVAL;
            q[n++] = (uint8_t)l;
            memcpy(q + n, p, l);
            n += l;
            p += l + (dot ? 1 : 0);
        }
        q[n++] = 0;
        q[n++] = 0; q[n++] = 1;   /* A */
        q[n++] = 0; q[n++] = 1;   /* IN */
        dns_wait_id = id;
        dns_rcode = -1;
        mutex_lock(&net_lock);
        int r = udp_send(netif.dns, 53535, 53, q, n);
        mutex_unlock(&net_lock);
        if (r < 0) return r;
        uint64_t start = uptime_ms();
        while (dns_rcode < 0 && uptime_ms() - start < 2000) {
            uint64_t f = irq_save();
            wq_wait_timeout(&dns_wq, 50);
            irq_restore(f);
            if (task_interrupted(current)) return -EINTR;
        }
        if (dns_rcode == 0) {
            *ip = dns_answer;
            strlcpy(dns_cache[dns_next].name, name, 64);
            dns_cache[dns_next].ip = dns_answer;
            dns_cache[dns_next].time = uptime_ms();
            dns_next = (dns_next + 1) % DNS_CACHE;
            return 0;
        }
        if (dns_rcode > 0) return -ENOENT;
    }
    return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ thread */
static int net_thread(void *arg) {
    UNUSED(arg);
    net_task = current;
    dhcp_start();
    uint64_t last_tick = 0;
    for (;;) {
        uint64_t f = irq_save();
        if (!rx_pending) wq_wait_timeout(&rx_wq, 20);
        rx_pending = false;
        irq_restore(f);
        mutex_lock(&net_lock);
        netif.poll(deliver);
        uint64_t now = uptime_ms();
        if (now - last_tick >= 100) {
            last_tick = now;
            dhcp_tick();
            tcp_timer();
        }
        mutex_unlock(&net_lock);
        poll_notify();
    }
    return 0;
}

bool net_status(char *ip, size_t n) {
    if (!netif.up) { if (n) ip[0] = 0; return false; }
    ip_str(netif.ip, ip, n);
    return true;
}

/* ------------------------------------------------------------------ syscalls */
SYSCALL_DEF(sys_net_info) {
    SYSCALL_UNUSED_ARGS;
    knetinfo_t ni;
    memset(&ni, 0, sizeof(ni));
    memcpy(ni.mac, netif.mac, 6);
    ni.present = netif.present;
    ni.up = netif.up;
    ni.ip = netif.ip;
    ni.netmask = netif.mask;
    ni.gateway = netif.gw;
    ni.dns = netif.dns;
    ni.rx_packets = netif.rx_packets;
    ni.tx_packets = netif.tx_packets;
    ni.rx_bytes = netif.rx_bytes;
    ni.tx_bytes = netif.tx_bytes;
    ni.lease_seconds = netif.lease;
    strlcpy(ni.driver, netif.driver, sizeof(ni.driver));
    return copy_to_user((void *)a1, &ni, sizeof(ni));
}

/* ping(ip, seq, timeout_ms) -> round trip time in microseconds */
SYSCALL_DEF(sys_net_ping) {
    SYSCALL_UNUSED_ARGS;
    if (!netif.up) return -ENETDOWN;
    return icmp_ping((uint32_t)a1, (uint16_t)a2, a3 ? a3 : 2000);
}

/* resolve(name, &ip) */
SYSCALL_DEF(sys_net_resolve) {
    SYSCALL_UNUSED_ARGS;
    char name[128];
    if (strncpy_from_user(name, (const char *)a1, sizeof(name)) < 0) return -EFAULT;
    uint32_t ip;
    int r = dns_resolve(name, &ip);
    if (r < 0) return r;
    return copy_to_user((void *)a2, &ip, 4);
}

void net_init(void) {
    syscall_register(SYS_NET_INFO, sys_net_info);
    syscall_register(SYS_NET_PING, sys_net_ping);
    syscall_register(SYS_NET_RESOLVE, sys_net_resolve);
    socket_init();
    e1000_init();
    if (!netif.present) return;
    task_t *t = kthread_create("netd", net_thread, 0);
    t->prio = 1;
}
