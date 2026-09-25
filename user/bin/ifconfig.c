/* ifconfig - show the network interface */
#include <stdio.h>
#include <net.h>
#include <claudeos.h>

int main(void) {
    knetinfo_t ni;
    if (net_info(&ni) < 0 || !ni.present) { printf("No network adapter found.\n"); return 1; }
    char ip[20], mask[20], gw[20], dns[20], rx[16], tx[16];
    ip_to_str(ni.ip, ip, sizeof(ip));
    ip_to_str(ni.netmask, mask, sizeof(mask));
    ip_to_str(ni.gateway, gw, sizeof(gw));
    ip_to_str(ni.dns, dns, sizeof(dns));
    format_size(ni.rx_bytes, rx, sizeof(rx));
    format_size(ni.tx_bytes, tx, sizeof(tx));
    printf("eth0: %s  <%s>\n", ni.driver, ni.up ? "UP" : "DOWN (waiting for DHCP)");
    printf("      ether %02x:%02x:%02x:%02x:%02x:%02x\n", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    printf("      inet  %s  netmask %s\n", ip, mask);
    printf("      gateway %s  dns %s\n", gw, dns);
    printf("      RX %lu packets (%s)  TX %lu packets (%s)\n", (unsigned long)ni.rx_packets, rx,
           (unsigned long)ni.tx_packets, tx);
    return 0;
}
