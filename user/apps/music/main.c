/* Music Player (WAV) */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <claudeos.h>
#include <claudeos/wav.h>

static ui_window_t *win;
static ui_widget_t *playlist, *btn_play, *progress_c, *vol_slider, *viz;
static int audio_fd = -1;
static int16_t *pcm;               /* 48 kHz stereo */
static uint32_t frames, written;   /* frames total / handed to the driver */
static bool playing;
static char cur_path[256];
static char folder[256] = "/home/Music";
static char names[128][128];
static int nnames, cur_index = -1;
static int levels[32];

static uint32_t played_frames(void) {
    int queued = 0;
    if (audio_fd >= 0) ioctl(audio_fd, FIONREAD, &queued);
    uint32_t q = (uint32_t)queued / 4;
    return written > q ? written - q : 0;
}

static void fmt_time(uint32_t f, char *out, size_t n) {
    uint32_t s = f / 48000;
    snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

static void stop(void) {
    playing = false;
    if (audio_fd >= 0) { close(audio_fd); audio_fd = -1; }
    strlcpy(btn_play->glyph, "play", sizeof(btn_play->glyph));
    ui_widget_invalidate(btn_play);
}

static bool load(const char *path) {
    size_t size;
    char *data = ui_read_file(path, &size);
    if (!data) return false;
    wav_info_t w;
    if (!wav_parse(data, size, &w)) { free(data); return false; }
    free(pcm);
    frames = wav_out_frames(&w);
    pcm = malloc((size_t)frames * 4 + 16);
    wav_convert(&w, pcm);
    free(data);
    strlcpy(cur_path, path, sizeof(cur_path));
    written = 0;
    char t[300];
    snprintf(t, sizeof(t), "%s - Music Player", ui_basename(path));
    ui_set_title(win, t);
    ui_invalidate(win);
    return true;
}

static void play(void) {
    if (!pcm) {
        if (nnames == 0) return;
        if (cur_index < 0) cur_index = 0;
        char p[400];
        snprintf(p, sizeof(p), "%s/%s", folder, names[cur_index]);
        if (!load(p)) return;
    }
    if (audio_fd < 0) {
        audio_fd = open("/dev/audio", O_WRONLY | O_NONBLOCK);
        if (audio_fd < 0) {
            ui_msgbox(win, "Music Player", "No sound card found.\n\nClaudeOS plays audio through an AC'97 sound card (QEMU: -device AC97).", "OK");
            return;
        }
    }
    if (written >= frames) written = 0;
    playing = true;
    strlcpy(btn_play->glyph, "pause", sizeof(btn_play->glyph));
    ui_widget_invalidate(btn_play);
}

static void pause_playback(void) {
    /* keep the position: drop the queued audio by closing the stream */
    uint32_t pos = played_frames();
    playing = false;
    if (audio_fd >= 0) { close(audio_fd); audio_fd = -1; }
    written = pos;
    strlcpy(btn_play->glyph, "play", sizeof(btn_play->glyph));
    ui_widget_invalidate(btn_play);
}

static void select_track(int i, bool autoplay) {
    if (i < 0 || i >= nnames) return;
    stop();
    cur_index = i;
    ui_list_select(playlist, i);
    char p[400];
    snprintf(p, sizeof(p), "%s/%s", folder, names[i]);
    if (!load(p)) {
        ui_msgbox(win, "Music Player", "This file is not a supported WAV file (PCM 8/16 bit).", "OK");
        return;
    }
    if (autoplay) play();
}

static void tick(void *arg) {
    (void)arg;
    if (playing && audio_fd >= 0) {
        /* keep about 0.25 s queued */
        int queued = 0;
        ioctl(audio_fd, FIONREAD, &queued);
        while (queued < 48000 && written < frames) {
            uint32_t n = MIN(frames - written, 4096u);
            long w = write(audio_fd, pcm + written * 2, n * 4);
            if (w <= 0) break;
            written += (uint32_t)w / 4;
            queued += (int)w;
        }
        if (written >= frames && queued == 0) {
            /* track finished: next one */
            stop();
            if (cur_index + 1 < nnames) select_track(cur_index + 1, true);
            else { written = 0; ui_invalidate(win); }
        }
        /* level meter from the samples being played */
        uint32_t pos = played_frames();
        for (int b = 0; b < 32; b++) {
            int64_t sum = 0;
            uint32_t start = pos + b * 64;
            for (uint32_t k = 0; k < 64 && start + k < frames; k++) {
                int v = pcm[(start + k) * 2];
                sum += v < 0 ? -v : v;
            }
            int lv = (int)(sum / 64 / 180);
            levels[b] = MAX(lv, levels[b] - 3);
        }
    } else {
        for (int b = 0; b < 32; b++) levels[b] = MAX(0, levels[b] - 4);
    }
    ui_widget_invalidate(progress_c);
    ui_widget_invalidate(viz);
}

static void draw_progress(ui_widget_t *w, surface_t *s) {
    uint32_t pos = playing ? played_frames() : written;
    char a[16], b[16];
    fmt_time(pos, a, sizeof(a));
    fmt_time(frames, b, sizeof(b));
    font_draw(s, &ui_font, w->r.x, w->r.y, a, ui_theme.window_text_dim);
    int bw = font_text_width(&ui_font, b);
    font_draw(s, &ui_font, w->r.x + w->r.w - bw, w->r.y, b, ui_theme.window_text_dim);
    int bar_x = w->r.x + 50, bar_w = w->r.w - 100, y = w->r.y + ui_font.height / 2;
    gfx_fill_rounded(s, bar_x, y - 2, bar_w, 5, 2, ui_theme.scroll_track);
    int fw = frames ? (int)((uint64_t)bar_w * pos / frames) : 0;
    gfx_fill_rounded(s, bar_x, y - 2, fw, 5, 2, ui_theme.accent);
    gfx_fill_circle(s, bar_x + fw, y, 6, ui_theme.accent);
}

static bool progress_event(ui_widget_t *w, gui_event_t *ev) {
    if ((ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) || (ev->type == EV_MOUSE_MOVE && (ev->buttons & MOUSE_LEFT))) {
        int bar_x = w->r.x + 50, bar_w = w->r.w - 100;
        if (!frames) return true;
        int x = MAX(0, MIN(bar_w, ev->x - bar_x));
        bool was = playing;
        if (playing) pause_playback();
        written = (uint32_t)((uint64_t)frames * x / bar_w);
        if (was && ev->type == EV_MOUSE_DOWN) play();
        ui_widget_invalidate(w);
        return true;
    }
    if (ev->type == EV_MOUSE_UP && !playing && audio_fd < 0 && written > 0 && written < frames) return true;
    return false;
}

static void draw_viz(ui_widget_t *w, surface_t *s) {
    gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, 10, ui_theme.dark ? 0xFF15161A : 0xFFE8EAEE);
    int bw = (w->r.w - 20) / 32;
    for (int b = 0; b < 32; b++) {
        int h = MIN(w->r.h - 16, levels[b] * (w->r.h - 16) / 100 + 2);
        uint32_t c = gfx_mix(ui_theme.accent, 0xFFB267E6, b * 255 / 31);
        gfx_fill_rounded(s, w->r.x + 10 + b * bw, w->r.y + w->r.h - 8 - h, bw - 3, h, 2, c);
    }
    const char *title = cur_path[0] ? ui_basename(cur_path) : "No track loaded";
    font_draw_fit(s, &ui_font_title, w->r.x + 14, w->r.y + 10, title, w->r.w - 28, ui_theme.window_text);
}

static void on_play(ui_widget_t *w) { (void)w; if (playing) pause_playback(); else play(); }
static void on_stop(ui_widget_t *w) { (void)w; stop(); written = 0; ui_invalidate(win); }
static void on_prev(ui_widget_t *w) { (void)w; select_track(cur_index > 0 ? cur_index - 1 : 0, true); }
static void on_next(ui_widget_t *w) { (void)w; if (cur_index + 1 < nnames) select_track(cur_index + 1, true); }
static void on_activate(ui_widget_t *w) { select_track(w->ival, true); }
static void on_volume(ui_widget_t *w) {
    syscall1(SYS_AUDIO_VOLUME, w->ival);
    gui_config_t c;
    gui_config_get(&c);
    c.volume = (uint32_t)w->ival;
    gui_config_set(&c, 0);
}

static void scan_folder(const char *dir) {
    strlcpy(folder, dir, sizeof(folder));
    nnames = 0;
    ui_list_clear(playlist);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nnames < 128) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && !strcasecmp(dot, ".wav")) strlcpy(names[nnames++], e->d_name, 128);
    }
    if (d) closedir(d);
    for (int i = 0; i < nnames; i++)
        for (int j = i + 1; j < nnames; j++)
            if (strcasecmp(names[i], names[j]) > 0) { char t[128]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t); }
    for (int i = 0; i < nnames; i++) ui_list_add(playlist, names[i], "file-audio", 0);
}

static void on_open(ui_widget_t *w) {
    (void)w;
    char *p = ui_file_dialog(win, false, "Open Audio File", folder, 0, "wav");
    if (!p) return;
    char dir[256];
    strlcpy(dir, p, sizeof(dir));
    char *sl = strrchr(dir, '/');
    if (sl && sl != dir) *sl = 0;
    scan_folder(dir);
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i], ui_basename(p))) select_track(i, true);
    free(p);
}

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->key == KEY_SPACE) on_play(0);
    else if (ev->key == KEY_RIGHT) on_next(0);
    else if (ev->key == KEY_LEFT) on_prev(0);
}

static bool on_close(ui_window_t *w) { (void)w; stop(); ui_quit(); return true; }

int main(int argc, char **argv) {
    int W = 520, H = 480;
    win = ui_window("Music Player", W, H, 0, "music");
    if (!win) return 1;
    win->on_key = on_key;
    win->on_close = on_close;
    viz = ui_canvas(win, 16, 16, W - 32, 150, draw_viz, 0);
    viz->focusable = false;
    progress_c = ui_canvas(win, 16, 180, W - 32, 24, draw_progress, progress_event);
    progress_c->focusable = false;
    int by = 214, cx = W / 2;
    ui_widget_t *b;
    b = ui_toolbutton(win, cx - 110, by, 40, "prev", "Previous", on_prev);
    btn_play = ui_toolbutton(win, cx - 60, by, 40, "play", "Play", on_play);
    b = ui_toolbutton(win, cx - 10, by, 40, "stop", "Stop", on_stop);
    b = ui_toolbutton(win, cx + 40, by, 40, "next", "Next", on_next);
    (void)b;
    ui_toolbutton(win, 16, by + 4, 32, "open", "Open", on_open);
    ui_widget_t *vl = ui_label(win, W - 190, by + 8, 30, 24, "");
    (void)vl;
    vol_slider = ui_slider(win, W - 150, by + 8, 134, 24, 0, 100, 70);
    vol_slider->on_activate = on_volume;
    long v = syscall1(SYS_AUDIO_VOLUME, -1);
    if (v >= 0) vol_slider->ival = (int)v;
    playlist = ui_list(win, 16, by + 56, W - 32, H - by - 72);
    ui_list_set_row_height(playlist, 30);
    playlist->on_activate = on_activate;
    scan_folder("/home/Music");
    if (argc > 1 && argv[1][0]) {
        char dir[256];
        strlcpy(dir, argv[1], sizeof(dir));
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) { *sl = 0; scan_folder(dir); }
        for (int i = 0; i < nnames; i++)
            if (!strcmp(names[i], ui_basename(argv[1]))) select_track(i, true);
        if (cur_index < 0 && load(argv[1])) play();
    } else if (nnames) {
        cur_index = 0;
        ui_list_select(playlist, 0);
        char p[400];
        snprintf(p, sizeof(p), "%s/%s", folder, names[0]);
        load(p);
    }
    ui_focus(win, playlist);
    ui_timer(40, tick, 0);
    ui_run();
    return 0;
}
