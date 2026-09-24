/* colour themes and the persistent desktop configuration */
#include "wm.h"
#include <vfs.h>
#include <boot.h>

#define C(x) (0xFF000000u | (x))

static const gui_theme_t themes[] = {
    {
        .name = "Dark", .accent = C(0xD97757), .accent_text = C(0xFFFFFF), .desktop_bg = C(0x1B1D24),
        .window_bg = C(0x202125), .window_text = C(0xE9EAEE), .window_text_dim = C(0x9DA1AA),
        .title_bg = C(0x2B2C31), .title_bg_inactive = C(0x232428), .title_text = C(0xF2F2F4),
        .title_text_inactive = C(0x8D919A), .border = C(0x44464D), .border_inactive = C(0x35363B),
        .button_bg = C(0x2F3136), .button_hover = C(0x3A3D43), .button_pressed = C(0x46494F),
        .button_text = C(0xF0F0F2), .button_border = C(0x484B52),
        .input_bg = C(0x17181B), .input_border = C(0x4A4D54), .input_text = C(0xEDEDEF),
        .selection_bg = C(0xD97757), .selection_text = C(0xFFFFFF),
        .menu_bg = C(0x2B2C31), .menu_text = C(0xEDEDEF), .menu_hover = C(0x3B3D44), .menu_border = C(0x46484F),
        .panel_bg = C(0x1C1D21), .panel_text = C(0xF0F0F2), .panel_hover = C(0x34363C), .panel_active = C(0x3E4047),
        .list_alt = C(0x25262A), .scroll_track = C(0x232428), .scroll_thumb = C(0x55585F),
        .tooltip_bg = C(0xEDEDEF), .tooltip_text = C(0x1C1D21), .sidebar_bg = C(0x1A1B1E),
        .dark = 1, .style = 0, .radius = 8, .shadows = 1, .panel_alpha = 225, .title_h = 32,
    },
    {
        .name = "Light", .accent = C(0xD97757), .accent_text = C(0xFFFFFF), .desktop_bg = C(0xDCE3EC),
        .window_bg = C(0xF7F7F8), .window_text = C(0x1C1D21), .window_text_dim = C(0x6B7079),
        .title_bg = C(0xEBECEF), .title_bg_inactive = C(0xF3F4F6), .title_text = C(0x1C1D21),
        .title_text_inactive = C(0x8A8F98), .border = C(0xB9BDC6), .border_inactive = C(0xD3D6DC),
        .button_bg = C(0xFFFFFF), .button_hover = C(0xF0F1F4), .button_pressed = C(0xE2E4E9),
        .button_text = C(0x1C1D21), .button_border = C(0xC9CDD4),
        .input_bg = C(0xFFFFFF), .input_border = C(0xBFC4CC), .input_text = C(0x1C1D21),
        .selection_bg = C(0xD97757), .selection_text = C(0xFFFFFF),
        .menu_bg = C(0xFCFCFD), .menu_text = C(0x1C1D21), .menu_hover = C(0xEDEFF3), .menu_border = C(0xCFD2D8),
        .panel_bg = C(0xF1F2F5), .panel_text = C(0x1C1D21), .panel_hover = C(0xFFFFFF), .panel_active = C(0xFFFFFF),
        .list_alt = C(0xF1F2F4), .scroll_track = C(0xEEEFF2), .scroll_thumb = C(0xB8BCC4),
        .tooltip_bg = C(0x2B2D31), .tooltip_text = C(0xFFFFFF), .sidebar_bg = C(0xEFF0F3),
        .dark = 0, .style = 0, .radius = 8, .shadows = 1, .panel_alpha = 225, .title_h = 32,
    },
    {
        .name = "Midnight", .accent = C(0x5B9BFF), .accent_text = C(0xFFFFFF), .desktop_bg = C(0x0F1626),
        .window_bg = C(0x1A2131), .window_text = C(0xE3E8F2), .window_text_dim = C(0x8E99AE),
        .title_bg = C(0x222B3F), .title_bg_inactive = C(0x1C2334), .title_text = C(0xEEF2FA),
        .title_text_inactive = C(0x7E89A0), .border = C(0x34405A), .border_inactive = C(0x2A3348),
        .button_bg = C(0x253048), .button_hover = C(0x2F3B56), .button_pressed = C(0x3A4764),
        .button_text = C(0xEEF2FA), .button_border = C(0x3A4764),
        .input_bg = C(0x131927), .input_border = C(0x3A4764), .input_text = C(0xE9EEF7),
        .selection_bg = C(0x5B9BFF), .selection_text = C(0xFFFFFF),
        .menu_bg = C(0x222B3F), .menu_text = C(0xE9EEF7), .menu_hover = C(0x31405E), .menu_border = C(0x3A4764),
        .panel_bg = C(0x131A29), .panel_text = C(0xEEF2FA), .panel_hover = C(0x2A3550), .panel_active = C(0x33405F),
        .list_alt = C(0x1E2638), .scroll_track = C(0x1C2334), .scroll_thumb = C(0x46557A),
        .tooltip_bg = C(0xE3E8F2), .tooltip_text = C(0x131927), .sidebar_bg = C(0x161C2A),
        .dark = 1, .style = 0, .radius = 10, .shadows = 1, .panel_alpha = 215, .title_h = 32,
    },
    {
        .name = "Ocean", .accent = C(0x0A84FF), .accent_text = C(0xFFFFFF), .desktop_bg = C(0xCFE3F7),
        .window_bg = C(0xF6F9FC), .window_text = C(0x14202E), .window_text_dim = C(0x5E6D80),
        .title_bg = C(0xE4EEF8), .title_bg_inactive = C(0xEEF3F8), .title_text = C(0x14202E),
        .title_text_inactive = C(0x8594A6), .border = C(0xA9C2DD), .border_inactive = C(0xCCD9E6),
        .button_bg = C(0xFFFFFF), .button_hover = C(0xEAF2FB), .button_pressed = C(0xD7E6F6),
        .button_text = C(0x14202E), .button_border = C(0xB9CCE0),
        .input_bg = C(0xFFFFFF), .input_border = C(0xB2C6DB), .input_text = C(0x14202E),
        .selection_bg = C(0x0A84FF), .selection_text = C(0xFFFFFF),
        .menu_bg = C(0xFBFDFF), .menu_text = C(0x14202E), .menu_hover = C(0xE3EEF9), .menu_border = C(0xC3D4E6),
        .panel_bg = C(0xE8F0F8), .panel_text = C(0x14202E), .panel_hover = C(0xFFFFFF), .panel_active = C(0xFFFFFF),
        .list_alt = C(0xEEF4FA), .scroll_track = C(0xE7EEF5), .scroll_thumb = C(0xA9BCD1),
        .tooltip_bg = C(0x14202E), .tooltip_text = C(0xFFFFFF), .sidebar_bg = C(0xEAF1F8),
        .dark = 0, .style = 0, .radius = 10, .shadows = 1, .panel_alpha = 215, .title_h = 32,
    },
    {
        .name = "Forest", .accent = C(0x3FB27F), .accent_text = C(0xFFFFFF), .desktop_bg = C(0x14201A),
        .window_bg = C(0x1C2420), .window_text = C(0xE4EDE8), .window_text_dim = C(0x93A39A),
        .title_bg = C(0x243029), .title_bg_inactive = C(0x1E2722), .title_text = C(0xEFF5F1),
        .title_text_inactive = C(0x83948A), .border = C(0x37473E), .border_inactive = C(0x2C3832),
        .button_bg = C(0x26322B), .button_hover = C(0x2F3D35), .button_pressed = C(0x3A4A40),
        .button_text = C(0xEFF5F1), .button_border = C(0x3A4A40),
        .input_bg = C(0x151C18), .input_border = C(0x3A4A40), .input_text = C(0xE9F1EC),
        .selection_bg = C(0x3FB27F), .selection_text = C(0xFFFFFF),
        .menu_bg = C(0x243029), .menu_text = C(0xE9F1EC), .menu_hover = C(0x324238), .menu_border = C(0x3A4A40),
        .panel_bg = C(0x151D19), .panel_text = C(0xEFF5F1), .panel_hover = C(0x2C3A32), .panel_active = C(0x35463C),
        .list_alt = C(0x202A25), .scroll_track = C(0x1E2722), .scroll_thumb = C(0x4A5E52),
        .tooltip_bg = C(0xE4EDE8), .tooltip_text = C(0x151C18), .sidebar_bg = C(0x18201C),
        .dark = 1, .style = 0, .radius = 8, .shadows = 1, .panel_alpha = 220, .title_h = 32,
    },
    {
        .name = "Retro", .accent = C(0x000080), .accent_text = C(0xFFFFFF), .desktop_bg = C(0x008080),
        .window_bg = C(0xC0C0C0), .window_text = C(0x000000), .window_text_dim = C(0x606060),
        .title_bg = C(0x000080), .title_bg_inactive = C(0x808080), .title_text = C(0xFFFFFF),
        .title_text_inactive = C(0xC0C0C0), .border = C(0x000000), .border_inactive = C(0x808080),
        .button_bg = C(0xC0C0C0), .button_hover = C(0xC8C8C8), .button_pressed = C(0xB0B0B0),
        .button_text = C(0x000000), .button_border = C(0x404040),
        .input_bg = C(0xFFFFFF), .input_border = C(0x808080), .input_text = C(0x000000),
        .selection_bg = C(0x000080), .selection_text = C(0xFFFFFF),
        .menu_bg = C(0xC0C0C0), .menu_text = C(0x000000), .menu_hover = C(0x000080), .menu_border = C(0x808080),
        .panel_bg = C(0xC0C0C0), .panel_text = C(0x000000), .panel_hover = C(0xD0D0D0), .panel_active = C(0xE0E0E0),
        .list_alt = C(0xFFFFFF), .scroll_track = C(0xD8D8D8), .scroll_thumb = C(0xC0C0C0),
        .tooltip_bg = C(0xFFFFE1), .tooltip_text = C(0x000000), .sidebar_bg = C(0xB8B8B8),
        .dark = 0, .style = 1, .radius = 0, .shadows = 0, .panel_alpha = 255, .title_h = 24,
    },
    {
        .name = "High Contrast", .accent = C(0xFFD400), .accent_text = C(0x000000), .desktop_bg = C(0x000000),
        .window_bg = C(0x000000), .window_text = C(0xFFFFFF), .window_text_dim = C(0xD0D0D0),
        .title_bg = C(0x1A1A1A), .title_bg_inactive = C(0x000000), .title_text = C(0xFFFFFF),
        .title_text_inactive = C(0xB0B0B0), .border = C(0xFFFFFF), .border_inactive = C(0x9A9A9A),
        .button_bg = C(0x000000), .button_hover = C(0x333333), .button_pressed = C(0x555555),
        .button_text = C(0xFFFFFF), .button_border = C(0xFFFFFF),
        .input_bg = C(0x000000), .input_border = C(0xFFFFFF), .input_text = C(0xFFFFFF),
        .selection_bg = C(0xFFD400), .selection_text = C(0x000000),
        .menu_bg = C(0x000000), .menu_text = C(0xFFFFFF), .menu_hover = C(0x333333), .menu_border = C(0xFFFFFF),
        .panel_bg = C(0x000000), .panel_text = C(0xFFFFFF), .panel_hover = C(0x333333), .panel_active = C(0x444444),
        .list_alt = C(0x111111), .scroll_track = C(0x111111), .scroll_thumb = C(0xFFFFFF),
        .tooltip_bg = C(0xFFFFFF), .tooltip_text = C(0x000000), .sidebar_bg = C(0x0A0A0A),
        .dark = 1, .style = 0, .radius = 0, .shadows = 0, .panel_alpha = 255, .title_h = 32,
    },
};

int theme_count(void) { return (int)ARRAY_SIZE(themes); }
const char *theme_name(int i) { return i >= 0 && i < theme_count() ? themes[i].name : 0; }

bool theme_by_name(const char *name, gui_theme_t *out) {
    for (int i = 0; i < theme_count(); i++) {
        if (!strcasecmp(themes[i].name, name)) { *out = themes[i]; return true; }
    }
    *out = themes[0];
    return false;
}

void theme_apply_config(gui_theme_t *t, const gui_config_t *c) {
    if (c->accent && t->style == 0) {
        uint32_t a = c->accent | 0xFF000000u;
        t->accent = a;
        t->selection_bg = a;
        t->accent_text = gfx_luma(a) > 170 ? 0xFF000000u : 0xFFFFFFFFu;
        t->selection_text = t->accent_text;
    }
    if (c->radius >= 0 && t->style == 0) t->radius = (uint8_t)MIN(c->radius, 16);
    if (c->shadows >= 0) t->shadows = c->shadows ? 1 : 0;
    if (c->transparency >= 0 && !c->transparency) t->panel_alpha = 255;
}

void config_defaults(gui_config_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->theme, "Dark", sizeof(c->theme));
    c->accent = 0;
    c->wallpaper = WP_WAVES;
    c->wp_color1 = C(0x2A1B3D);
    c->wp_color2 = C(0xD97757);
    c->wp_mode = WPM_FILL;
    c->taskbar_top = 0;
    c->taskbar_size = 1;
    c->taskbar_labels = 1;
    c->clock_24h = 1;
    c->clock_seconds = 0;
    c->clock_date = 1;
    c->desktop_icons = 1;
    c->mouse_speed = 5;
    c->double_click_ms = 450;
    strlcpy(c->kbd_layout, "de", sizeof(c->kbd_layout));
    c->volume = 70;
    c->sounds = 1;
    c->radius = -1;
    c->shadows = -1;
    c->transparency = 1;
}

#define CFG_PATH "/home/.config/desktop.cfg"

static const struct { const char *key; int type; size_t off; size_t size; } fields[] = {
#define F_STR(k) { #k, 0, offsetof(gui_config_t, k), sizeof(((gui_config_t *)0)->k) }
#define F_U32(k) { #k, 1, offsetof(gui_config_t, k), 4 }
#define F_HEX(k) { #k, 2, offsetof(gui_config_t, k), 4 }
#define F_I32(k) { #k, 3, offsetof(gui_config_t, k), 4 }
    F_STR(theme), F_HEX(accent), F_U32(wallpaper), F_HEX(wp_color1), F_HEX(wp_color2), F_STR(wp_image),
    F_U32(wp_mode), F_U32(taskbar_top), F_U32(taskbar_size), F_U32(taskbar_labels), F_U32(clock_24h),
    F_U32(clock_seconds), F_U32(clock_date), F_U32(desktop_icons), F_U32(mouse_speed),
    F_U32(double_click_ms), F_STR(kbd_layout), F_U32(volume), F_U32(sounds), F_I32(radius), F_I32(shadows),
    F_I32(transparency),
};

void config_load(gui_config_t *c) {
    config_defaults(c);
    const char *kb = cmdline_get("kbd");
    size_t size;
    char *data = vfs_read_all(CFG_PATH, &size);
    if (data) {
        char *line = data;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                const char *val = eq + 1;
                for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
                    if (strcmp(fields[i].key, line)) continue;
                    uint8_t *p = (uint8_t *)c + fields[i].off;
                    switch (fields[i].type) {
                    case 0: strlcpy((char *)p, val, fields[i].size); break;
                    case 1: *(uint32_t *)p = (uint32_t)strtoul(val, 0, 10); break;
                    case 2: *(uint32_t *)p = (uint32_t)strtoul(val, 0, 16); break;
                    case 3: *(int32_t *)p = (int32_t)strtol(val, 0, 10); break;
                    }
                }
            }
            line = nl ? nl + 1 : 0;
        }
        kfree(data);
        klog("[wm] loaded settings from %s\n", CFG_PATH);
    }
    if (kb) strlcpy(c->kbd_layout, kb, sizeof(c->kbd_layout));
    if (c->mouse_speed < 1 || c->mouse_speed > 10) c->mouse_speed = 5;
    if (c->volume > 100) c->volume = 70;
    if (c->wallpaper >= WP_COUNT) c->wallpaper = WP_WAVES;
}

void config_save(const gui_config_t *c) {
    char *buf = kmalloc(4096);
    size_t n = 0;
    for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
        const uint8_t *p = (const uint8_t *)c + fields[i].off;
        switch (fields[i].type) {
        case 0: n += snprintf(buf + n, 4096 - n, "%s=%s\n", fields[i].key, (const char *)p); break;
        case 1: n += snprintf(buf + n, 4096 - n, "%s=%u\n", fields[i].key, *(const uint32_t *)p); break;
        case 2: n += snprintf(buf + n, 4096 - n, "%s=%08x\n", fields[i].key, *(const uint32_t *)p); break;
        case 3: n += snprintf(buf + n, 4096 - n, "%s=%d\n", fields[i].key, *(const int32_t *)p); break;
        }
    }
    vfs_mkdir("/home/.config");
    int r = vfs_write_all(CFG_PATH, buf, n);
    if (r < 0) klog("[wm] could not save settings: %d\n", r);
    kfree(buf);
}

void theme_init(void) {}
