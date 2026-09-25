/* Task Manager: processes, CPU and memory graphs */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <claudeos.h>

#define HIST 120

static ui_window_t *win;
static ui_widget_t *list, *end_btn, *tab_proc, *tab_perf, *graphs, *info, *show_kernel;
static kprocinfo_t procs[128];
static int nprocs;
static int cpu_hist[HIST], mem_hist[HIST];
#define MAXCPU 32
static int core_hist[MAXCPU][HIST];
static int ncores;
static int hist_len;
static int page;
static ksysinfo_t si;

static const char *state_name(int s) {
    static const char *n[] = { "Ready", "Running", "Waiting", "Sleeping", "Zombie" };
    return s >= 0 && s < 5 ? n[s] : "?";
}

static void refresh(void *arg) {
    (void)arg;
    sys_info(&si);
    nprocs = 0;
    kprocinfo_t p;
    for (int i = 0; proc_info(i, &p) > 0 && nprocs < 128; i++) {
        if (!strncmp(p.name, "idle", 4) && p.is_kernel) continue;
        procs[nprocs++] = p;
    }
    kcpuinfo_t ci[MAXCPU];
    ncores = cpu_info(ci, MAXCPU);
    if (ncores < 1) ncores = 1;
    if (ncores > MAXCPU) ncores = MAXCPU;
    int cpu = 0;
    for (int i = 0; i < ncores; i++) {
        memmove(core_hist[i], core_hist[i] + 1, sizeof(int) * (HIST - 1));
        core_hist[i][HIST - 1] = ci[i].load;
        cpu += ci[i].load;
    }
    cpu /= ncores;
    int mem = si.mem_total ? (int)((si.mem_total - si.mem_free) * 100 / si.mem_total) : 0;
    if (hist_len < HIST) hist_len++;
    memmove(cpu_hist, cpu_hist + 1, sizeof(int) * (HIST - 1));
    memmove(mem_hist, mem_hist + 1, sizeof(int) * (HIST - 1));
    cpu_hist[HIST - 1] = cpu;
    mem_hist[HIST - 1] = mem;

    /* process list (keep selection by pid) */
    int sel_pid = -1;
    if (list->ival >= 0) sel_pid = (int)(intptr_t)ui_list_data(list, list->ival);
    ui_list_clear(list);
    int newsel = -1;
    for (int i = 0; i < nprocs; i++) {
        kprocinfo_t *q = &procs[i];
        if (q->is_kernel && !show_kernel->ival) continue;
        char row[200], mem_s[16];
        format_size(q->mem_bytes, mem_s, sizeof(mem_s));
        unsigned long s = (unsigned long)(q->cpu_ms / 1000);
        snprintf(row, sizeof(row), "%s\t%d\t%s\t%d %%\t%s\t%lu:%02lu", q->name, q->pid, state_name(q->state), q->cpu_percent,
                 mem_s, s / 60, s % 60);
        const char *icon = q->is_kernel ? "settings" : q->windows ? "file-exec" : "terminal";
        int idx = ui_list_add(list, row, icon, (void *)(intptr_t)q->pid);
        if (q->pid == sel_pid) newsel = idx;
    }
    list->ival = newsel;
    ui_enable(end_btn, newsel >= 0);
    char buf[400], used[16], tot[16], heap[16];
    format_size(si.mem_total - si.mem_free, used, sizeof(used));
    format_size(si.mem_total, tot, sizeof(tot));
    format_size(si.mem_kernel_heap, heap, sizeof(heap));
    unsigned long up = (unsigned long)(si.uptime_ms / 1000);
    snprintf(buf, sizeof(buf), "CPU: %s, %d core%s  \xE2\x80\xA2  %d %% used\nMemory: %s of %s (%d %%)  \xE2\x80\xA2  kernel heap %s\nProcesses: %u  \xE2\x80\xA2  Uptime: %lu:%02lu:%02lu",
             si.cpu_brand, ncores, ncores == 1 ? "" : "s", cpu, used, tot, mem, heap, si.nprocs, up / 3600, up / 60 % 60, up % 60);
    ui_set_text(info, buf);
    ui_widget_invalidate(graphs);
}

static void draw_graph(surface_t *s, rect_t r, int *data, uint32_t color, const char *title, int value) {
    gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 8, ui_theme.input_bg);
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, 8, ui_theme.input_border);
    bool small = r.h < 90;
    rect_t g = small ? mkrect(r.x + 6, r.y + 26, r.w - 12, r.h - 32) : mkrect(r.x + 12, r.y + 36, r.w - 24, r.h - 48);
    for (int i = 1; i < 4; i++) gfx_hline(s, g.x, g.y + g.h * i / 4, g.w, WITH_ALPHA(ui_theme.window_text_dim, 40));
    char t[64];
    snprintf(t, sizeof(t), "%s  %d %%", title, value);
    font_draw(s, small ? &ui_font : &ui_font_bold, r.x + (small ? 8 : 12), r.y + (small ? 5 : 10), t, ui_theme.window_text);
    if (hist_len < 2) return;
    int prev_x = 0, prev_y = 0;
    for (int i = HIST - hist_len; i < HIST; i++) {
        int x = g.x + (i * (g.w - 1)) / (HIST - 1);
        int y = g.y + g.h - 1 - data[i] * (g.h - 1) / 100;
        if (i > HIST - hist_len) {
            /* area fill */
            for (int xx = prev_x; xx <= x; xx++) {
                int yy = x == prev_x ? y : prev_y + (y - prev_y) * (xx - prev_x) / (x - prev_x);
                gfx_vline(s, xx, yy, g.y + g.h - yy, WITH_ALPHA(color, 50));
            }
            gfx_line_aa(s, prev_x, prev_y, x, y, color);
            gfx_line_aa(s, prev_x, prev_y + 1, x, y + 1, color);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void draw_graphs(ui_widget_t *w, surface_t *s) {
    /* overall CPU, one small graph per core, memory */
    int cols = ncores <= 4 ? ncores : ncores <= 8 ? 4 : 8;
    int rows = (ncores + cols - 1) / cols;
    int core_h = ncores > 1 ? MIN(80, (w->r.h / 3) / rows) : 0;
    int cores_h = ncores > 1 ? rows * (core_h + 8) : 0;
    int h = (w->r.h - 12 - cores_h) / 2;
    draw_graph(s, mkrect(w->r.x, w->r.y, w->r.w, h), cpu_hist, ui_theme.accent, "CPU", cpu_hist[HIST - 1]);
    if (ncores > 1) {
        int cw = (w->r.w - (cols - 1) * 8) / cols;
        for (int i = 0; i < ncores; i++) {
            int cx = w->r.x + (i % cols) * (cw + 8), cy = w->r.y + h + 8 + (i / cols) * (core_h + 8);
            char t[16];
            snprintf(t, sizeof(t), "Core %d", i);
            draw_graph(s, mkrect(cx, cy, cw, core_h), core_hist[i], ui_theme.accent, t, core_hist[i][HIST - 1]);
        }
    }
    draw_graph(s, mkrect(w->r.x, w->r.y + h + 12 + cores_h, w->r.w, h), mem_hist, 0xFF4ADE80, "Memory",
               mem_hist[HIST - 1]);
}

static void set_page(int p) {
    page = p;
    tab_proc->ival = p == 0;
    tab_perf->ival = p == 1;
    ui_show(list, p == 0);
    ui_show(end_btn, p == 0);
    ui_show(show_kernel, p == 0);
    ui_show(graphs, p == 1);
    ui_invalidate(win);
}

static void on_tab(ui_widget_t *w) { set_page(w == tab_perf); }

static void on_end(ui_widget_t *w) {
    (void)w;
    if (list->ival < 0) return;
    int pid = (int)(intptr_t)ui_list_data(list, list->ival);
    const char *name = "";
    for (int i = 0; i < nprocs; i++) if (procs[i].pid == pid) name = procs[i].name;
    char msg[200];
    snprintf(msg, sizeof(msg), "End the process \"%s\" (PID %d)? Unsaved data will be lost.", name, pid);
    if (ui_msgbox(win, "End task", msg, "End task|Cancel") != 0) return;
    if (kill(pid, SIGKILL) < 0) ui_msgbox(win, "End task", "The process could not be terminated.", "OK");
    refresh(0);
}

static void on_select(ui_widget_t *w) { ui_enable(end_btn, w->ival >= 0); }
static void on_kernel(ui_widget_t *w) { (void)w; refresh(0); }

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->key == KEY_DELETE && page == 0) on_end(0);
}

int main(void) {
    int W = 720, H = 520;
    win = ui_window("Task Manager", W, H, WF_RESIZABLE, "taskmgr");
    if (!win) return 1;
    win->on_key = on_key;
    tab_proc = ui_button(win, 12, 10, 120, 32, "Processes", on_tab);
    tab_perf = ui_button(win, 138, 10, 120, 32, "Performance", on_tab);
    tab_proc->focusable = tab_perf->focusable = false;
    info = ui_label(win, 12, 52, W - 24, 58, "");
    ui_set_anchor(info, A_LEFT | A_RIGHT | A_TOP);
    list = ui_list(win, 12, 116, W - 24, H - 116 - 56);
    ui_set_anchor(list, A_ALL);
    static const ui_column_t cols[] = { { "Name", 200 }, { "PID", 70 }, { "Status", 110 }, { "CPU", 80 }, { "Memory", 110 }, { "CPU time", -1 } };
    ui_list_set_columns(list, cols, 6);
    list->on_change = on_select;
    show_kernel = ui_checkbox(win, 12, H - 44, 260, 32, "Show kernel threads", false);
    ui_set_anchor(show_kernel, A_LEFT | A_BOTTOM);
    show_kernel->on_change = on_kernel;
    end_btn = ui_button(win, W - 12 - 120, H - 46, 120, 34, "End task", on_end);
    ui_set_anchor(end_btn, A_RIGHT | A_BOTTOM);
    ui_button_style(end_btn, BTN_PRIMARY);
    graphs = ui_canvas(win, 12, 116, W - 24, H - 116 - 12, draw_graphs, 0);
    ui_set_anchor(graphs, A_ALL);
    graphs->focusable = false;
    set_page(0);
    refresh(0);
    ui_timer(1000, refresh, 0);
    ui_focus(win, list);
    ui_run();
    return 0;
}
