/* df - file system usage */
#include <stdio.h>
#include <claudeos.h>

int main(void) {
    printf("%-12s %-8s %10s %10s %10s %5s  %s\n", "Device", "Type", "Size", "Used", "Free", "Use%", "Mounted on");
    kmount_t m;
    for (int i = 0; mount_info(i, &m) > 0; i++) {
        char t[16] = "-", u[16] = "-", f[16] = "-";
        int pct = 0;
        if (m.total_bytes) {
            format_size(m.total_bytes, t, sizeof(t));
            format_size(m.total_bytes - m.free_bytes, u, sizeof(u));
            format_size(m.free_bytes, f, sizeof(f));
            pct = (int)((m.total_bytes - m.free_bytes) * 100 / m.total_bytes);
        }
        printf("%-12s %-8s %10s %10s %10s %4d%%  %s\n", m.device, m.fstype, t, u, f, pct, m.path);
    }
    return 0;
}
