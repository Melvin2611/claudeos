/* nslookup - resolve a host name */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <net.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nslookup name\n"); return 1; }
    knetinfo_t ni;
    char dns[20] = "?";
    if (net_info(&ni) == 0) ip_to_str(ni.dns, dns, sizeof(dns));
    printf("Server:  %s\n\n", dns);
    uint32_t ip;
    if (net_resolve(argv[1], &ip) < 0) { printf("** cannot find %s: %s\n", argv[1], strerror(errno)); return 1; }
    char s[20];
    ip_to_str(ip, s, sizeof(s));
    printf("Name:    %s\nAddress: %s\n", argv[1], s);
    return 0;
}
