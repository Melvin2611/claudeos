/* icon cache: loads .icn files from /system/icons on demand */
#include "wm.h"
#include <vfs.h>
#include <claudeos/image.h>

typedef struct icon_entry {
    char key[64];
    icon_t icon;
    bool missing;
    struct icon_entry *next;
} icon_entry_t;

static icon_entry_t *cache;

static icon_t *load(const char *path) {
    for (icon_entry_t *e = cache; e; e = e->next)
        if (!strcmp(e->key, path)) return e->missing ? 0 : &e->icon;
    icon_entry_t *e = kzalloc(sizeof(icon_entry_t));
    strlcpy(e->key, path, sizeof(e->key));
    size_t size;
    void *data = vfs_read_all(path, &size);
    const uint32_t *px;
    if (data && icn_parse(data, size, &e->icon.w, &e->icon.h, &px)) {
        e->icon.px = px;
        e->icon.data = data;
    } else {
        kfree(data);
        e->missing = true;
    }
    e->next = cache;
    cache = e;
    return e->missing ? 0 : &e->icon;
}

icon_t *icon_get(const char *name, int size) {
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/%s-%d.icn", name, size);
    return load(path);
}

icon_t *glyph_get(const char *name, int size) {
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/glyph/%s-%d.icn", name, size);
    return load(path);
}

static const char *cursor_names[CUR_COUNT] = {
    "arrow", "text", "hand", "wait", "resize-h", "resize-v", "resize-nwse", "resize-nesw", "move", "cross"
};
static const int8_t hotspots[CUR_COUNT][2] = {
    { 1, 1 }, { 16, 16 }, { 13, 2 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 16, 16 }
};

icon_t *cursor_icon(int shape, int *hx, int *hy) {
    if (shape < 0 || shape >= CUR_COUNT) shape = 0;
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/cursor/%s.icn", cursor_names[shape]);
    *hx = hotspots[shape][0];
    *hy = hotspots[shape][1];
    return load(path);
}

void draw_icon(surface_t *s, icon_t *ic, int x, int y) {
    if (ic) gfx_draw_image(s, x, y, ic->px, ic->w, ic->h);
}

void draw_glyph(surface_t *s, const char *name, int size, int x, int y, uint32_t color) {
    icon_t *ic = glyph_get(name, size);
    if (ic) gfx_draw_image_tinted(s, x, y, ic->px, ic->w, ic->h, color);
}
