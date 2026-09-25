#pragma once
/* ClaudeOS network stack: internal interfaces */
#include <kernel.h>
#include <sched.h>
#include <vfs.h>

#define ETH_MTU 1500
#define ETH_HLEN 14

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#define IP4(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct netif {
    bool present, up;
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;      /* network byte order */
    uint32_t lease;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    char driver[24];
    int (*send)(const void *frame, size_t len);
    int (*poll)(void (*deliver)(const uint8_t *frame, size_t len));
} netif_t;

extern netif_t netif;

/* driver */
void e1000_init(void);

/* core */
void net_init(void);
void net_rx_kick(void);                              /* called from the driver IRQ */
int net_send_eth(const uint8_t dst[6], uint16_t type, const void *payload, size_t len);
int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);
uint16_t ip_checksum(const void *data, size_t len, uint32_t initial);
int arp_resolve(uint32_t ip, uint8_t mac[6], uint64_t timeout_ms);
void ip_str(uint32_t ip, char *out, size_t n);

/* udp */
typedef struct udp_sock udp_sock_t;
void udp_input(uint32_t src, const uint8_t *pkt, size_t len);
int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len);

/* tcp */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_timer(void);

/* sockets */
void socket_init(void);
int socket_udp_deliver(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *data, size_t len);

/* services */
int dns_resolve(const char *name, uint32_t *ip);
void dhcp_start(void);
void dhcp_input(const uint8_t *data, size_t len);
int icmp_ping(uint32_t dst, uint16_t seq, uint64_t timeout_ms);
extern waitq_t net_wq;
extern mutex_t net_lock;
