/* free - memory usage */
#include <stdio.h>
#include <claudeos.h>

int main(void) {
    ksysinfo_t si;
    sys_info(&si);
    char t[16], u[16], f[16], k[16];
    format_size(si.mem_total, t, sizeof(t));
    format_size(si.mem_total - si.mem_free, u, sizeof(u));
    format_size(si.mem_free, f, sizeof(f));
    format_size(si.mem_kernel_heap, k, sizeof(k));
    printf("%-8s %12s %12s %12s %12s\n", "", "total", "used", "free", "kernel heap");
    printf("%-8s %12s %12s %12s %12s\n", "Mem:", t, u, f, k);
    return 0;
}
