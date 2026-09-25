#pragma once
/* ClaudeOS sockets (IPv4) */
#include <stdint.h>
#include <stddef.h>
#include <claudeos/abi.h>

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2

int socket(int domain, int type);
int connect_ip(int fd, uint32_t ip, uint16_t port, int timeout_ms);
int bind_port(int fd, uint16_t port);
long sendto_ip(int fd, const void *buf, size_t len, uint32_t ip, uint16_t port);
long recvfrom_ip(int fd, void *buf, size_t len, uint32_t *ip, uint16_t *port, int timeout_ms);
int net_info(knetinfo_t *out);
int net_ping(uint32_t ip, uint16_t seq, int timeout_ms);   /* returns microseconds */
int net_resolve(const char *name, uint32_t *ip);
void ip_to_str(uint32_t ip, char *out, size_t n);
int str_to_ip(const char *s, uint32_t *ip);
