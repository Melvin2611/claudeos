/* About ClaudeOS */
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <claudeos.h>

static ui_window_t *win;
static ksysinfo_t si;

static void paint(ui_window_t *w, surface_t *s) {
    /* header band with the logo */
    gfx_gradient_h(s, 0, 0, w->w, 150, 0xFF2A1B3D, 0xFFD97757);
    ui_draw_icon(s, "logo", 48, 32, 50);
    font_draw(s, &ui_font_big, 96, 46, "ClaudeOS", 0xFFFFFFFF);
    font_draw(s, &ui_font, 98, 84, "Version 1.0  \xE2\x80\xA2  64-bit", 0xE0FFFFFF);
    int y = 176;
    char mem[16], line[160];
    format_size(si.mem_total, mem, sizeof(mem));
    const char *rows[][2] = {
        { "Processor", si.cpu_brand }, { "Memory", mem }, { "Boot loader", si.bootloader }, { 0, 0 },
    };
    for (int i = 0; rows[i][0]; i++) {
        font_draw(s, &ui_font_bold, 32, y, rows[i][0], ui_theme.window_text);
        font_draw_fit(s, &ui_font, 150, y, rows[i][1], w->w - 180, ui_theme.window_text);
        y += 24;
    }
    snprintf(line, sizeof(line), "%u x %u", si.screen_w, si.screen_h);
    font_draw(s, &ui_font_bold, 32, y, "Display", ui_theme.window_text);
    font_draw(s, &ui_font, 150, y, line, ui_theme.window_text);
    y += 40;
    const char *text[] = {
        "ClaudeOS is a hobby operating system written from scratch in C:",
        "a preemptive multitasking kernel with virtual memory, a VFS with",
        "RAM and FAT32 file systems, drivers for keyboard, mouse, disks,",
        "sound and network, a compositing window server and a set of",
        "desktop applications.",
    };
    for (int i = 0; i < 5; i++) {
        font_draw(s, &ui_font, 32, y, text[i], ui_theme.window_text_dim);
        y += 20;
    }
}

static void on_ok(ui_widget_t *w) { (void)w; ui_window_close(win); ui_quit(); }

int main(void) {
    sys_info(&si);
    win = ui_window("About ClaudeOS", 480, 450, WF_DIALOG | WF_CENTER, "about");
    if (!win) return 1;
    win->on_paint = paint;
    ui_widget_t *ok = ui_button(win, 480 - 32 - 100, 450 - 52, 100, 34, "OK", on_ok);
    ui_button_style(ok, BTN_PRIMARY);
    ui_focus(win, ok);
    ui_run();
    return 0;
}
