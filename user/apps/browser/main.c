/* Web Browser: HTTP/HTTPS, a small HTML layout engine (text, headings, lists, links, tables,
 * preformatted text, images) and simple GET forms. Pages load in a background thread. */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <net.h>
#include <tls.h>
#include <claudeos.h>

#define W 1000
#define H 640
#define TOOLBAR_H 48
#define STATUS_H 24
#define MARGIN 24
#define MAX_PAGE (8 * 1024 * 1024)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* page colours: web pages assume a light page */
#define C_BG     0xFFFFFFFF
#define C_TEXT   0xFF1F2328
#define C_LINK   0xFF1A56DB
#define C_DIM    0xFF6B7280
#define C_RULE   0xFFD0D7DE
#define C_PRE_BG 0xFFF3F4F6

static ui_window_t *win;
static ui_widget_t *addr, *view, *status, *btn_back, *btn_fwd;

/* ------------------------------------------------------------------ URLs */

typedef struct { char scheme[8]; char host[128]; int port; char path[1024]; } url_t;

static bool parse_url(const char *s, url_t *u) {
    memset(u, 0, sizeof(*u));
    const char *p = strstr(s, "://");
    if (!p) return false;
    size_t sl = (size_t)(p - s);
    if (sl >= sizeof(u->scheme)) return false;
    memcpy(u->scheme, s, sl);
    for (char *c = u->scheme; *c; c++) *c = (char)tolower(*c);
    const char *h = p + 3, *e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    size_t hl = MIN((size_t)(e - h), sizeof(u->host) - 1);
    memcpy(u->host, h, hl);
    char *colon = strchr(u->host, ':');
    u->port = !strcmp(u->scheme, "https") ? 443 : 80;
    if (colon) { *colon = 0; u->port = atoi(colon + 1); }
    if (*e == '/') strlcpy(u->path, e, sizeof(u->path));
    else { u->path[0] = '/'; strlcpy(u->path + 1, e, sizeof(u->path) - 1); }
    char *hash = strchr(u->path, '#');
    if (hash) *hash = 0;
    return u->host[0] != 0;
}

/* resolve a (possibly relative) link against the page URL */
static void resolve_url(const char *base, const char *rel, char *out, size_t n) {
    while (*rel == ' ' || *rel == '\n' || *rel == '\t') rel++;
    if (strstr(rel, "://") || !strncmp(rel, "about:", 6)) { strlcpy(out, rel, n); return; }
    url_t b;
    if (!parse_url(base, &b)) { strlcpy(out, rel, n); return; }
    char port[16] = "";
    if ((!strcmp(b.scheme, "https") && b.port != 443) || (!strcmp(b.scheme, "http") && b.port != 80))
        snprintf(port, sizeof(port), ":%d", b.port);
    if (rel[0] == '/' && rel[1] == '/') { snprintf(out, n, "%s:%s", b.scheme, rel); return; }
    if (rel[0] == '/') { snprintf(out, n, "%s://%s%s%s", b.scheme, b.host, port, rel); return; }
    if (rel[0] == '#') { snprintf(out, n, "%s://%s%s%s", b.scheme, b.host, port, b.path); return; }
    char dir[1024];
    strlcpy(dir, b.path, sizeof(dir));
    if (rel[0] == '?') { char *q = strchr(dir, '?'); if (q) *q = 0; snprintf(out, n, "%s://%s%s%s%s", b.scheme, b.host, port, dir, rel); return; }
    char *q = strchr(dir, '?');
    if (q) *q = 0;
    char *sl = strrchr(dir, '/');
    if (sl) sl[1] = 0;
    char path[2048];
    snprintf(path, sizeof(path), "%s%s", dir, rel);
    /* normalise . and .. */
    char norm[2048] = "";
    char *save, *tok = strtok_r(path, "/", &save);
    bool trailing = path[0] && rel[strlen(rel) - 1] == '/';
    while (tok) {
        if (!strcmp(tok, ".")) {}
        else if (!strcmp(tok, "..")) { char *l = strrchr(norm, '/'); if (l) *l = 0; }
        else { strlcat(norm, "/", sizeof(norm)); strlcat(norm, tok, sizeof(norm)); }
        tok = strtok_r(0, "/", &save);
    }
    if (!norm[0] || trailing) strlcat(norm, "/", sizeof(norm));
    snprintf(out, n, "%s://%s%s%s", b.scheme, b.host, port, norm);
}

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    char *data;
    size_t len;
    int status;
    char ctype[96];
    char url[1024];
    char err[200];
} resp_t;

typedef struct { int fd; tls_t *tls; } conn_t;
static long c_read(conn_t *c, void *b, size_t n) { return c->tls ? tls_read(c->tls, b, n) : read(c->fd, b, n); }
static long c_write(conn_t *c, const void *b, size_t n) { return c->tls ? tls_write(c->tls, b, n) : write(c->fd, b, n); }
static void c_close(conn_t *c) { if (c->tls) tls_close(c->tls); else if (c->fd >= 0) close(c->fd); }

static char *header_value(char *hdr, const char *name) {
    size_t nl = strlen(name);
    for (char *p = hdr; (p = strchr(p, '\n')); ) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            char *v = p + nl + 1;
            while (*v == ' ') v++;
            return v;
        }
    }
    return 0;
}

static size_t dechunk(char *d, size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        size_t sz = strtoul(d + in, 0, 16);
        char *nl = memchr(d + in, '\n', len - in);
        if (!nl) break;
        in = (size_t)(nl - d) + 1;
        if (!sz) break;
        if (in + sz > len) sz = len - in;
        memmove(d + out, d + in, sz);
        out += sz;
        in += sz + 2;
    }
    return out;
}

static volatile int cancel_gen;

static int http_get(const char *start_url, resp_t *r, int gen, volatile size_t *progress) {
    memset(r, 0, sizeof(*r));
    strlcpy(r->url, start_url, sizeof(r->url));
    for (int redirects = 0; redirects < 8; redirects++) {
        url_t u;
        if (!parse_url(r->url, &u)) { snprintf(r->err, sizeof(r->err), "Invalid address"); return -1; }
        bool https = !strcmp(u.scheme, "https");
        if (!https && strcmp(u.scheme, "http")) { snprintf(r->err, sizeof(r->err), "Unsupported protocol %s", u.scheme); return -1; }
        conn_t c = { -1, 0 };
        if (https) {
            c.tls = tls_connect(u.host, (uint16_t)u.port, 10000, r->err, sizeof(r->err));
            if (!c.tls) return -1;
        } else {
            uint32_t ip;
            if (net_resolve(u.host, &ip) < 0) { snprintf(r->err, sizeof(r->err), "Cannot find %s", u.host); return -1; }
            c.fd = socket(AF_INET, SOCK_STREAM);
            if (c.fd < 0 || connect_ip(c.fd, ip, (uint16_t)u.port, 10000) < 0) {
                snprintf(r->err, sizeof(r->err), "Cannot connect to %s", u.host);
                c_close(&c);
                return -1;
            }
        }
        char req[1500];
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (ClaudeOS; x86_64) ClaudeBrowser/1.0\r\n"
                         "Accept: text/html,text/plain,image/*;q=0.8,*/*;q=0.5\r\nAccept-Language: de,en;q=0.8\r\n"
                         "Connection: close\r\n\r\n", u.path, u.host);
        if (c_write(&c, req, n) < 0) { snprintf(r->err, sizeof(r->err), "Connection failed"); c_close(&c); return -1; }
        size_t cap = 65536, len = 0;
        char *buf = malloc(cap + 1);
        for (;;) {
            if (cancel_gen != gen) { free(buf); c_close(&c); snprintf(r->err, sizeof(r->err), "Cancelled"); return -1; }
            if (len == cap) {
                if (cap >= MAX_PAGE) break;
                cap *= 2;
                buf = realloc(buf, cap + 1);
            }
            long k = c_read(&c, buf + len, cap - len);
            if (k <= 0) break;
            len += k;
            if (progress) *progress = len;
        }
        c_close(&c);
        buf[len] = 0;
        char *body = strstr(buf, "\r\n\r\n");
        if (!body) { free(buf); snprintf(r->err, sizeof(r->err), "Invalid server response"); return -1; }
        *body = 0;
        body += 4;
        char *sp = strchr(buf, ' ');
        r->status = sp ? atoi(sp + 1) : 0;
        char *loc = header_value(buf, "Location");
        if (r->status >= 300 && r->status < 400 && loc) {
            char next[1024], lv[1024];
            size_t ll = strcspn(loc, "\r\n");
            memcpy(lv, loc, MIN(ll, sizeof(lv) - 1));
            lv[MIN(ll, sizeof(lv) - 1)] = 0;
            resolve_url(r->url, lv, next, sizeof(next));
            strlcpy(r->url, next, sizeof(r->url));
            free(buf);
            continue;
        }
        char *ct = header_value(buf, "Content-Type");
        if (ct) { size_t cl = strcspn(ct, "\r\n;"); memcpy(r->ctype, ct, MIN(cl, sizeof(r->ctype) - 1)); }
        char *te = header_value(buf, "Transfer-Encoding");
        bool chunked = te && !strncasecmp(te, "chunked", 7);
        size_t blen = len - (size_t)(body - buf);
        memmove(buf, body, blen);
        if (chunked) blen = dechunk(buf, blen);
        buf[blen] = 0;
        r->data = buf;
        r->len = blen;
        return 0;
    }
    snprintf(r->err, sizeof(r->err), "Too many redirects");
    return -1;
}

/* ------------------------------------------------------------------ document model */

enum { IT_TEXT, IT_RULE, IT_BOX, IT_BULLET };

typedef struct {
    int type;
    int x, y, w, h;
    const font_t *f;
    uint32_t color, bg;
    int link;               /* -1 none */
    bool underline, strike;
    char *text;
} item_t;

typedef struct { int x, y, w, h, img, link; } imgbox_t;
typedef struct { char *src; uint32_t *px; int w, h; int dw, dh; volatile int state; } image_t;   /* 0 wait 1 ok 2 fail */
typedef struct { int x, y, w, h, form; char name[64]; char value[256]; bool submit; bool hidden; } input_t;
typedef struct { char action[1024]; bool post; } form_t;

typedef struct {
    char url[1024];
    char title[160];
    char *html;
    bool plain;
    item_t *items; int nitems, capitems;
    imgbox_t *boxes; int nboxes, capboxes;
    char **links; int nlinks, caplinks;
    image_t *images; int nimages;
    input_t inputs[64]; int ninputs;
    form_t forms[16]; int nforms;
    int height;
    int layout_w;
} doc_t;

static doc_t *doc;
static int scroll_y;
static int hover_link = -1;
static int focus_input = -1;

static void doc_free(doc_t *d) {
    if (!d) return;
    for (int i = 0; i < d->nitems; i++) free(d->items[i].text);
    free(d->items);
    free(d->boxes);
    for (int i = 0; i < d->nlinks; i++) free(d->links[i]);
    free(d->links);
    for (int i = 0; i < d->nimages; i++) { free(d->images[i].src); free(d->images[i].px); }
    free(d->images);
    free(d->html);
    free(d);
}

/* ------------------------------------------------------------------ HTML entities */

static const struct { const char *name; uint32_t cp; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC },
    { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "szlig", 0xDF }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "bull", 0x2022 },
    { "middot", 0xB7 }, { "euro", 0x20AC }, { "pound", 0xA3 }, { "deg", 0xB0 }, { "times", 0xD7 },
    { "rarr", 0x2192 }, { "larr", 0x2190 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "bdquo", 0x201E }, { "sect", 0xA7 }, { "para", 0xB6 }, { "shy", 0xAD }, { "thinsp", 0x2009 },
};

/* decode entities in place (s is modified; the result is never longer) */
static void decode_entities(char *s) {
    char *w = s;
    for (char *r = s; *r;) {
        if (*r != '&') { *w++ = *r++; continue; }
        char *semi = strchr(r, ';');
        uint32_t cp = 0;
        if (semi && semi - r < 12) {
            if (r[1] == '#') cp = (r[2] == 'x' || r[2] == 'X') ? (uint32_t)strtoul(r + 3, 0, 16) : (uint32_t)strtoul(r + 2, 0, 10);
            else {
                for (size_t i = 0; i < ARRAY_SIZE(entities); i++) {
                    size_t l = strlen(entities[i].name);
                    if ((size_t)(semi - r - 1) == l && !strncmp(r + 1, entities[i].name, l)) { cp = entities[i].cp; break; }
                }
            }
        }
        if (cp) {
            if (cp == 0xAD) { r = semi + 1; continue; }
            char tmp[5];
            int n = utf8_encode(cp == 0xA0 ? ' ' : cp, tmp);
            if (n <= semi + 1 - r) { memcpy(w, tmp, n); w += n; r = semi + 1; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* ------------------------------------------------------------------ layout engine */

typedef struct {
    const font_t *f;
    uint32_t color;
    int link;
    bool underline, strike, pre, center;
    int indent;
} style_t;

typedef struct {
    doc_t *d;
    int width;
    int x, y;
    int line_start;          /* first item of the current line */
    int line_h;
    style_t st[48];
    int sp;
    int list_num[16];
    int list_depth;
    bool space_pending;
    int margin_pending;
    int cur_form;
    bool in_title;
} lay_t;

static item_t *add_item(doc_t *d) {
    if (d->nitems == d->capitems) {
        d->capitems = d->capitems ? d->capitems * 2 : 256;
        d->items = realloc(d->items, d->capitems * sizeof(item_t));
    }
    item_t *it = &d->items[d->nitems++];
    memset(it, 0, sizeof(*it));
    it->link = -1;
    return it;
}

static int add_link(doc_t *d, const char *href) {
    if (d->nlinks == d->caplinks) {
        d->caplinks = d->caplinks ? d->caplinks * 2 : 64;
        d->links = realloc(d->links, d->caplinks * sizeof(char *));
    }
    char full[1024];
    resolve_url(d->url, href, full, sizeof(full));
    d->links[d->nlinks] = strdup(full);
    return d->nlinks++;
}

static style_t *S(lay_t *L) { return &L->st[L->sp]; }

static void flush_line(lay_t *L) {
    doc_t *d = L->d;
    int h = L->line_h ? L->line_h : 0;
    int maxx = 0;
    for (int i = L->line_start; i < d->nitems; i++) {
        item_t *it = &d->items[i];
        if (it->y != L->y) continue;
        if (it->x + it->w > maxx) maxx = it->x + it->w;
    }
    int shift = S(L)->center && maxx < L->width ? (L->width - maxx) / 2 : 0;
    for (int i = L->line_start; i < d->nitems; i++) {
        item_t *it = &d->items[i];
        if (it->y != L->y) continue;
        it->x += shift;
        if (it->type == IT_TEXT || it->type == IT_BULLET) it->y += h - it->h;       /* bottom-align on the line */
    }
    for (int i = 0; i < d->nboxes; i++)
        if (d->boxes[i].y == L->y) d->boxes[i].x += shift;
    L->y += h;
    L->x = S(L)->indent;
    L->line_start = d->nitems;
    L->line_h = 0;
    L->space_pending = false;
}

static void newline(lay_t *L) {
    if (L->x > S(L)->indent || L->line_h) flush_line(L);
}

static void block_break(lay_t *L, int margin) {
    newline(L);
    if (margin > L->margin_pending) L->margin_pending = margin;
}

static void apply_margin(lay_t *L) {
    if (L->margin_pending && L->y > 0) L->y += L->margin_pending;
    L->margin_pending = 0;
}

static void place_text(lay_t *L, const char *word, int bytes, int w) {
    style_t *s = S(L);
    doc_t *d = L->d;
    apply_margin(L);
    int sw = font_text_width(s->f, " ");
    if (L->space_pending && L->x > s->indent) L->x += sw;
    L->space_pending = false;
    if (L->x + w > L->width && L->x > s->indent) flush_line(L);
    item_t *it = add_item(d);
    it->type = IT_TEXT;
    it->x = L->x;
    it->y = L->y;
    it->w = w;
    it->h = s->f->height;
    it->f = s->f;
    it->color = s->link >= 0 ? C_LINK : s->color;
    it->link = s->link;
    it->underline = s->link >= 0 || s->underline;
    it->strike = s->strike;
    it->text = malloc(bytes + 1);
    memcpy(it->text, word, bytes);
    it->text[bytes] = 0;
    if (s->pre) it->bg = C_PRE_BG;
    L->x += w;
    if (it->h > L->line_h) L->line_h = it->h;
}

static void add_text(lay_t *L, const char *t) {
    style_t *s = S(L);
    if (s->pre) {
        /* keep spaces, break at newlines */
        const char *p = t;
        while (*p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len) place_text(L, p, len, font_text_width_n(s->f, p, len));
            if (!nl) break;
            if (!L->line_h) L->line_h = s->f->height;
            flush_line(L);
            p = nl + 1;
        }
        return;
    }
    const char *p = t;
    while (*p) {
        if (isspace((unsigned char)*p)) { L->space_pending = true; p++; continue; }
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        int len = (int)(e - p);
        int w = font_text_width_n(s->f, p, len);
        if (w > L->width - s->indent) {
            /* very long word: break it by characters */
            int i = 0;
            while (i < len) {
                int j = i, ww = 0;
                while (j < len) {
                    int k = utf8_next(p, j);
                    int cw = font_text_width_n(s->f, p + j, k - j);
                    if (ww + cw > L->width - s->indent && j > i) break;
                    ww += cw;
                    j = k;
                }
                place_text(L, p + i, j - i, ww);
                i = j;
            }
        } else {
            place_text(L, p, len, w);
        }
        p = e;
    }
}

static void push(lay_t *L) { if (L->sp < 47) { L->st[L->sp + 1] = L->st[L->sp]; L->sp++; } }
static void pop(lay_t *L) { if (L->sp > 0) L->sp--; }

/* attribute lookup inside a tag's text */
static bool get_attr(const char *tag, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    const char *p = tag;
    while ((p = strcasestr(p, name))) {
        if ((p == tag || isspace((unsigned char)p[-1])) && (p[nl] == '=' || isspace((unsigned char)p[nl]))) {
            const char *v = p + nl;
            while (isspace((unsigned char)*v)) v++;
            if (*v != '=') { p += nl; continue; }
            v++;
            while (isspace((unsigned char)*v)) v++;
            char q = 0;
            if (*v == '"' || *v == '\'') q = *v++;
            size_t i = 0;
            while (*v && (q ? *v != q : !isspace((unsigned char)*v) && *v != '>') && i + 1 < n) out[i++] = *v++;
            out[i] = 0;
            decode_entities(out);
            return true;
        }
        p += nl;
    }
    return false;
}

static void add_box(lay_t *L, int w, int h, int img, int link) {
    doc_t *d = L->d;
    apply_margin(L);
    if (L->space_pending && L->x > S(L)->indent) L->x += 4;
    L->space_pending = false;
    if (w > L->width) { h = h * L->width / w; w = L->width; }
    if (L->x + w > L->width && L->x > S(L)->indent) flush_line(L);
    if (d->nboxes == d->capboxes) {
        d->capboxes = d->capboxes ? d->capboxes * 2 : 32;
        d->boxes = realloc(d->boxes, d->capboxes * sizeof(imgbox_t));
    }
    imgbox_t *b = &d->boxes[d->nboxes++];
    b->x = L->x;
    b->y = L->y;
    b->w = w;
    b->h = h;
    b->img = img;
    b->link = link;
    /* a placeholder item on the line keeps the line height */
    item_t *it = add_item(d);
    it->type = IT_BOX;
    it->x = L->x;
    it->y = L->y;
    it->w = w;
    it->h = h;
    L->x += w;
    if (h > L->line_h) L->line_h = h;
}

static int find_image(doc_t *d, const char *src) {
    for (int i = 0; i < d->nimages; i++) if (!strcmp(d->images[i].src, src)) return i;
    return -1;
}

static void handle_tag(lay_t *L, char *tag, bool closing, bool first_pass) {
    doc_t *d = L->d;
    char name[16];
    int i = 0;
    while (tag[i] && !isspace((unsigned char)tag[i]) && tag[i] != '/' && i < 15) { name[i] = (char)tolower(tag[i]); i++; }
    name[i] = 0;
    style_t *s = S(L);
    static const char *blocks[] = { "p", "div", "section", "article", "header", "footer", "nav", "main", "aside",
                                    "form", "figure", "figcaption", "dl", "address", "center", "details", "summary",
                                    "fieldset", "table", "caption" };
    bool is_block = false;
    for (size_t k = 0; k < ARRAY_SIZE(blocks); k++) if (!strcmp(name, blocks[k])) is_block = true;

    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block_break(L, 14);
        if (closing) { pop(L); block_break(L, 8); return; }
        push(L);
        int lvl = name[1] - '0';
        S(L)->f = lvl == 1 ? &ui_font_display : lvl == 2 ? &ui_font_big : lvl == 3 ? &ui_font_title : &ui_font_bold;
        return;
    }
    if (is_block) {
        block_break(L, strcmp(name, "div") ? 10 : 0);
        if (!strcmp(name, "center") || !strcmp(name, "caption")) {
            if (closing) pop(L); else { push(L); S(L)->center = true; }
        } else if (!closing) {
            char al[16];
            if (get_attr(tag, "align", al, sizeof(al)) && !strcasecmp(al, "center")) { push(L); S(L)->center = true; }
            else push(L);
        } else pop(L);
        if (!strcmp(name, "form")) {
            if (!closing && d->nforms < 16) {
                form_t *f = &d->forms[d->nforms];
                char a[1024] = "", m[8] = "";
                get_attr(tag, "action", a, sizeof(a));
                get_attr(tag, "method", m, sizeof(m));
                resolve_url(d->url, a[0] ? a : d->url, f->action, sizeof(f->action));
                f->post = !strcasecmp(m, "post");
                L->cur_form = d->nforms++;
            } else if (closing) L->cur_form = -1;
        }
        return;
    }
    if (!strcmp(name, "br")) { if (!L->line_h) L->line_h = s->f->height; flush_line(L); return; }
    if (!strcmp(name, "hr")) {
        block_break(L, 8);
        apply_margin(L);
        item_t *it = add_item(d);
        it->type = IT_RULE;
        it->x = s->indent;
        it->y = L->y + 4;
        it->w = L->width - s->indent;
        it->h = 1;
        L->y += 9;
        block_break(L, 8);
        return;
    }
    if (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "th") || !strcmp(name, "dt")) {
        if (!strcmp(name, "th")) { if (!closing) { L->x += L->x > s->indent ? 24 : 0; } }
        if (closing) pop(L); else { push(L); S(L)->f = s->f == &ui_font_mono ? &ui_font_mono_bold : &ui_font_bold; }
        return;
    }
    if (!strcmp(name, "i") || !strcmp(name, "em") || !strcmp(name, "cite") || !strcmp(name, "q")) {
        if (closing) pop(L); else { push(L); S(L)->color = 0xFF4B5563; }
        return;
    }
    if (!strcmp(name, "u") || !strcmp(name, "ins")) { if (closing) pop(L); else { push(L); S(L)->underline = true; } return; }
    if (!strcmp(name, "s") || !strcmp(name, "del") || !strcmp(name, "strike")) { if (closing) pop(L); else { push(L); S(L)->strike = true; } return; }
    if (!strcmp(name, "small") || !strcmp(name, "sup") || !strcmp(name, "sub")) {
        if (closing) pop(L); else { push(L); S(L)->color = C_DIM; }
        return;
    }
    if (!strcmp(name, "code") || !strcmp(name, "tt") || !strcmp(name, "kbd") || !strcmp(name, "samp")) {
        if (closing) pop(L); else { push(L); S(L)->f = &ui_font_mono; }
        return;
    }
    if (!strcmp(name, "pre")) {
        block_break(L, 10);
        if (closing) pop(L); else { push(L); S(L)->f = &ui_font_mono; S(L)->pre = true; }
        return;
    }
    if (!strcmp(name, "blockquote") || !strcmp(name, "dd")) {
        block_break(L, 8);
        if (closing) pop(L); else { push(L); S(L)->indent += 32; S(L)->color = s->color == C_TEXT ? 0xFF374151 : s->color; }
        L->x = S(L)->indent;
        return;
    }
    if (!strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "menu")) {
        block_break(L, L->list_depth ? 0 : 8);
        if (closing) { pop(L); if (L->list_depth) L->list_depth--; }
        else {
            push(L);
            S(L)->indent += 28;
            if (L->list_depth < 15) L->list_num[++L->list_depth] = !strcmp(name, "ol") ? 1 : 0;
        }
        L->x = S(L)->indent;
        return;
    }
    if (!strcmp(name, "li")) {
        newline(L);
        if (closing) return;
        apply_margin(L);
        int num = L->list_num[L->list_depth];
        char mark[16];
        if (num) { snprintf(mark, sizeof(mark), "%d.", num); L->list_num[L->list_depth]++; }
        else strcpy(mark, L->list_depth > 1 ? "\xE2\x97\xA6" : "\xE2\x80\xA2");
        item_t *it = add_item(d);
        it->type = IT_BULLET;
        it->f = s->f;
        it->w = font_text_width(s->f, mark);
        it->x = s->indent - it->w - 8;
        it->y = L->y;
        it->h = s->f->height;
        it->color = s->color;
        it->text = strdup(mark);
        L->line_h = s->f->height;
        return;
    }
    if (!strcmp(name, "tr")) { newline(L); return; }
    if (!strcmp(name, "td")) { if (!closing && L->x > s->indent) { L->x += 24; L->space_pending = false; } return; }
    if (!strcmp(name, "a")) {
        if (closing) { pop(L); return; }
        push(L);
        char href[1024];
        if (get_attr(tag, "href", href, sizeof(href)) && strncasecmp(href, "javascript:", 11))
            S(L)->link = add_link(d, href);
        return;
    }
    if (!strcmp(name, "img") && !closing) {
        char src[1024], alt[200] = "", ws[16], hs[16];
        if (!get_attr(tag, "src", src, sizeof(src))) return;
        get_attr(tag, "alt", alt, sizeof(alt));
        char full[1024];
        resolve_url(d->url, src, full, sizeof(full));
        int idx = find_image(d, full);
        if (idx < 0 && first_pass && d->nimages < 48) {
            d->images = realloc(d->images, (d->nimages + 1) * sizeof(image_t));
            memset(&d->images[d->nimages], 0, sizeof(image_t));
            d->images[d->nimages].src = strdup(full);
            idx = d->nimages++;
        }
        int w = get_attr(tag, "width", ws, sizeof(ws)) ? atoi(ws) : 0;
        int h = get_attr(tag, "height", hs, sizeof(hs)) ? atoi(hs) : 0;
        image_t *im = idx >= 0 ? &d->images[idx] : 0;
        if (im && im->state == 1) {
            if (!w && !h) { w = im->w; h = im->h; }
            else if (!w) w = im->w * h / MAX(im->h, 1);
            else if (!h) h = im->h * w / MAX(im->w, 1);
        }
        if (w <= 0 || h <= 0) { w = w > 0 ? w : 48; h = h > 0 ? h : 48; }
        if (im && im->state == 2) {
            if (alt[0]) { push(L); S(L)->color = C_DIM; add_text(L, "["); add_text(L, alt); add_text(L, "]"); pop(L); }
            return;
        }
        if (w < 2 || h < 2) return;                          /* tracking pixels */
        add_box(L, w, h, idx, s->link);
        return;
    }
    if ((!strcmp(name, "input") || !strcmp(name, "button") || !strcmp(name, "textarea")) && !closing) {
        char type[16] = "text", nm[64] = "", val[256] = "";
        get_attr(tag, "type", type, sizeof(type));
        get_attr(tag, "name", nm, sizeof(nm));
        get_attr(tag, "value", val, sizeof(val));
        bool submit = !strcasecmp(type, "submit") || !strcmp(name, "button");
        if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio") || !strcasecmp(type, "image") ||
            !strcasecmp(type, "file") || !strcasecmp(type, "password"))
            return;
        if (d->ninputs >= 64) return;
        input_t *in = &d->inputs[d->ninputs];
        bool hidden = !strcasecmp(type, "hidden");
        if (first_pass) {
            memset(in, 0, sizeof(*in));
            strlcpy(in->name, nm, sizeof(in->name));
            strlcpy(in->value, val[0] || !submit ? val : "Submit", sizeof(in->value));
            in->submit = submit;
            in->hidden = hidden;
            in->form = L->cur_form;
        }
        d->ninputs++;
        if (hidden) return;
        int w = submit ? font_text_width(&ui_font, in->value) + 24 : 240;
        add_box(L, w, ui_font.height + 12, -2 - (d->ninputs - 1), -1);
        return;
    }
    if (!strcmp(name, "title")) { L->in_title = !closing; return; }
}

/* lay the document out for the given width */
static void layout(doc_t *d, int width, bool first_pass) {
    for (int i = 0; i < d->nitems; i++) free(d->items[i].text);
    d->nitems = 0;
    d->nboxes = 0;
    int nlinks_keep = first_pass ? 0 : d->nlinks;
    if (first_pass) {
        for (int i = 0; i < d->nlinks; i++) free(d->links[i]);
        d->nlinks = 0;
    } else {
        for (int i = 0; i < d->nlinks; i++) free(d->links[i]);
        d->nlinks = 0;
    }
    (void)nlinks_keep;
    d->ninputs = 0;
    d->nforms = 0;
    d->layout_w = width;
    static lay_t L;
    memset(&L, 0, sizeof(L));
    L.d = d;
    L.width = width;
    L.st[0].f = &ui_font;
    L.st[0].color = C_TEXT;
    L.st[0].link = -1;
    L.cur_form = -1;
    if (d->plain) {
        L.st[0].f = &ui_font_mono;
        L.st[0].pre = true;
        add_text(&L, d->html);
        newline(&L);
        d->height = L.y + 32;
        return;
    }
    char *p = d->html;
    char *textbuf = malloc(65536);
    bool skip = false;          /* inside script/style/head */
    char skip_until[16] = "";
    while (*p) {
        if (*p == '<') {
            if (!strncmp(p, "<!--", 4)) { char *e = strstr(p + 4, "-->"); p = e ? e + 3 : p + strlen(p); continue; }
            char *e = strchr(p, '>');
            if (!e) break;
            *e = 0;
            char *tag = p + 1;
            bool closing = *tag == '/';
            if (closing) tag++;
            char nm[16];
            int i = 0;
            while (tag[i] && !isspace((unsigned char)tag[i]) && tag[i] != '/' && i < 15) { nm[i] = (char)tolower(tag[i]); i++; }
            nm[i] = 0;
            if (skip) {
                if (closing && !strcmp(nm, skip_until)) skip = false;
            } else if (!closing && (!strcmp(nm, "script") || !strcmp(nm, "style") || !strcmp(nm, "svg") ||
                                    !strcmp(nm, "template") || !strcmp(nm, "select"))) {
                skip = true;
                strlcpy(skip_until, nm, sizeof(skip_until));
            } else if (tag[0] != '!' && tag[0] != '?') {
                handle_tag(&L, tag, closing, first_pass);
            }
            *e = '>';
            p = e + 1;
            continue;
        }
        char *e = strchr(p, '<');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!skip && len) {
            size_t n = MIN(len, (size_t)65535);
            memcpy(textbuf, p, n);
            textbuf[n] = 0;
            decode_entities(textbuf);
            if (L.in_title) {
                if (first_pass) {
                    strlcpy(d->title, textbuf, sizeof(d->title));
                    for (char *c = d->title; *c; c++) if (*c == '\n' || *c == '\t') *c = ' ';
                }
            } else {
                add_text(&L, textbuf);
            }
        }
        p += len;
    }
    free(textbuf);
    newline(&L);
    d->height = L.y + 48;
}

/* ------------------------------------------------------------------ loading (background thread) */

typedef struct {
    int gen;
    volatile int finished;      /* the thread no longer touches its document */
    char url[1024];
    resp_t resp;
    volatile int done;          /* 1 page loaded (or failed) */
    volatile size_t progress;
    doc_t *doc;                 /* image loading target */
    volatile int images_changed;
} job_t;

static job_t *job;
static int gen_counter;
static char history[64][1024];
static int hist_len, hist_pos = -1;

static const char *home_html =
    "<html><head><title>Start</title></head><body>"
    "<h1>Willkommen im ClaudeOS Browser</h1>"
    "<p>Ein einfacher Webbrowser mit <b>HTTPS</b>, Bildern und Formularen. Tippe eine Adresse "
    "oder einen Suchbegriff in die Adressleiste.</p>"
    "<h2>Lesezeichen</h2><ul>"
    "<li><a href=\"https://example.com/\">example.com</a> &ndash; die klassische Testseite</li>"
    "<li><a href=\"https://de.m.wikipedia.org/wiki/Betriebssystem\">Wikipedia: Betriebssystem</a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a> &ndash; Websuche</li>"
    "<li><a href=\"https://text.npr.org/\">NPR Text</a> &ndash; Nachrichten als reiner Text</li>"
    "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
    "<li><a href=\"https://wiby.me/\">wiby.me</a> &ndash; Suchmaschine f&uuml;r einfache Webseiten</li>"
    "</ul><hr><p><small>JavaScript und CSS werden nicht ausgef&uuml;hrt; Seiten werden in einer "
    "vereinfachten Darstellung gezeigt.</small></p></body></html>";

static void *load_thread(void *arg) {
    job_t *j = arg;
    if (!strcmp(j->url, "about:home")) {
        j->resp.data = strdup(home_html);
        j->resp.len = strlen(home_html);
        strlcpy(j->resp.ctype, "text/html", sizeof(j->resp.ctype));
        strlcpy(j->resp.url, j->url, sizeof(j->resp.url));
        j->resp.status = 200;
    } else {
        http_get(j->url, &j->resp, j->gen, &j->progress);
    }
    j->done = 1;
    /* wait for the UI to build the document, then load its images */
    while (!j->doc && cancel_gen == j->gen) usleep(20000);
    doc_t *d = j->doc;
    if (!d) { j->finished = 1; return 0; }
    for (int i = 0; i < d->nimages && cancel_gen == j->gen; i++) {
        image_t *im = &d->images[i];
        resp_t r;
        if (http_get(im->src, &r, j->gen, 0) == 0 && r.data) {
            int w, h;
            uint32_t *px = ui_decode_image(r.data, r.len, &w, &h);
            free(r.data);
            if (px) {
                /* keep images reasonably small */
                int maxw = 900;
                if (w > maxw) {
                    int nh = h * maxw / w;
                    uint32_t *sc = ui_scale_image(px, w, h, maxw, nh);
                    free(px);
                    px = sc;
                    w = maxw;
                    h = nh;
                }
                im->px = px;
                im->w = w;
                im->h = h;
                im->state = px ? 1 : 2;
            } else im->state = 2;
        } else {
            free(r.data);
            im->state = 2;
        }
        j->images_changed = 1;
    }
    j->finished = 1;
    return 0;
}

/* documents replaced while their loader thread still runs are freed later */
static struct { doc_t *d; job_t *j; } retired[32];
static job_t *doc_job;

static void retire_doc(doc_t *d, job_t *j) {
    if (!d) return;
    if (!j || j->finished) { doc_free(d); return; }
    for (int i = 0; i < 32; i++) if (!retired[i].d) { retired[i].d = d; retired[i].j = j; return; }
    /* table full: leak rather than risk a use after free */
}

static void reap_docs(void) {
    for (int i = 0; i < 32; i++)
        if (retired[i].d && retired[i].j->finished) { doc_free(retired[i].d); retired[i].d = 0; }
}

static void update_buttons(void) {
    ui_enable(btn_back, hist_pos > 0);
    ui_enable(btn_fwd, hist_pos + 1 < hist_len);
}

static void set_status(const char *s) { ui_set_text(status, s); }

static void start_load(const char *url, bool add_history) {
    cancel_gen = ++gen_counter;
    job_t *j = calloc(1, sizeof(job_t));
    j->gen = gen_counter;
    strlcpy(j->url, url, sizeof(j->url));
    job = j;                    /* old jobs finish on their own and are leaked safely */
    if (add_history) {
        if (hist_pos + 1 < 64) hist_pos++;
        strlcpy(history[hist_pos], url, sizeof(history[0]));
        hist_len = hist_pos + 1;
    }
    update_buttons();
    ui_set_text(addr, url);
    char msg[1100];
    snprintf(msg, sizeof(msg), "Loading %s ...", url);
    set_status(msg);
    ui_set_cursor(win, CUR_WAIT);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, load_thread, j);
}

static int content_width(void) { return view->r.w - 2 * MARGIN - 14; }

static void relayout(bool first) {
    if (!doc) return;
    layout(doc, content_width(), first);
    if (scroll_y > doc->height - view->r.h) scroll_y = MAX(0, doc->height - view->r.h);
    ui_widget_invalidate(view);
}

static void poll_job(void *arg) {
    (void)arg;
    reap_docs();
    job_t *j = job;
    if (!j) return;
    if (j->done == 1) {
        j->done = 2;
        ui_set_cursor(win, CUR_ARROW);
        doc_t *d = calloc(1, sizeof(doc_t));
        resp_t *r = &j->resp;
        strlcpy(d->url, r->url[0] ? r->url : j->url, sizeof(d->url));
        if (!r->data) {
            char *html = malloc(2048);
            snprintf(html, 2048, "<h1>Seite nicht erreichbar</h1><p>%s</p><p><small>%s</small></p>", r->err, j->url);
            d->html = html;
        } else if (!strncmp(r->ctype, "image/", 6)) {
            /* a single image: show it on its own */
            d->html = malloc(1200);
            snprintf(d->html, 1200, "<center><img src=\"%s\"></center>", d->url);
            free(r->data);
        } else {
            d->html = r->data;
            d->plain = r->ctype[0] && strncasecmp(r->ctype, "text/html", 9) && strncasecmp(r->ctype, "application/xhtml", 17);
        }
        retire_doc(doc, doc_job);
        doc = d;
        doc_job = j;
        scroll_y = 0;
        hover_link = -1;
        focus_input = -1;
        layout(doc, content_width(), true);
        if (!doc->title[0]) strlcpy(doc->title, doc->url, sizeof(doc->title));
        char t[200];
        snprintf(t, sizeof(t), "%s - Browser", doc->title);
        ui_set_title(win, t);
        if (strcmp(j->url, "about:home")) ui_set_text(addr, doc->url);
        if (hist_pos >= 0) strlcpy(history[hist_pos], doc->url, sizeof(history[0]));
        char msg[160];
        if (r->data || d->plain) snprintf(msg, sizeof(msg), "Fertig - %zu KB, %d Bilder", r->len / 1024, doc->nimages);
        else snprintf(msg, sizeof(msg), "Fehler");
        set_status(msg);
        j->doc = doc;
        ui_widget_invalidate(view);
    } else if (!j->done && j->progress) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Loading ... %zu KB", j->progress / 1024);
        set_status(msg);
    }
    if (j->images_changed && j->doc == doc) {
        j->images_changed = 0;
        relayout(false);
    }
}

/* ------------------------------------------------------------------ navigation */

static void url_encode(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else o += snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

static void navigate(const char *input) {
    char url[1024];
    while (*input == ' ') input++;
    if (!*input) return;
    if (strstr(input, "://") || !strncmp(input, "about:", 6)) strlcpy(url, input, sizeof(url));
    else if (strchr(input, '.') && !strchr(input, ' ')) snprintf(url, sizeof(url), "https://%s", input);
    else {
        char q[600];
        url_encode(input, q, sizeof(q));
        snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", q);
    }
    start_load(url, true);
}

static void submit_form(int form) {
    if (!doc || form < 0 || form >= doc->nforms) return;
    char url[2048];
    strlcpy(url, doc->forms[form].action, sizeof(url));
    char *q = strchr(url, '?');
    if (q) *q = 0;
    strlcat(url, "?", sizeof(url));
    bool first = true;
    for (int i = 0; i < doc->ninputs; i++) {
        input_t *in = &doc->inputs[i];
        if (in->form != form || !in->name[0] || in->submit) continue;
        char enc[600];
        url_encode(in->value, enc, sizeof(enc));
        if (!first) strlcat(url, "&", sizeof(url));
        strlcat(url, in->name, sizeof(url));
        strlcat(url, "=", sizeof(url));
        strlcat(url, enc, sizeof(url));
        first = false;
    }
    start_load(url, true);
}

static void go_back(ui_widget_t *w) { (void)w; if (hist_pos > 0) { hist_pos--; start_load(history[hist_pos], false); } }
static void go_fwd(ui_widget_t *w) { (void)w; if (hist_pos + 1 < hist_len) { hist_pos++; start_load(history[hist_pos], false); } }
static void do_reload(ui_widget_t *w) { (void)w; if (hist_pos >= 0) start_load(history[hist_pos], false); }
static void go_home(ui_widget_t *w) { (void)w; start_load("about:home", true); }
static void addr_enter(ui_widget_t *w) { navigate(ui_get_text(w)); ui_focus(win, view); }

/* ------------------------------------------------------------------ drawing and input */

static void draw_view(ui_widget_t *w, surface_t *s) {
    rect_t r = w->r;
    gfx_fill(s, r.x, r.y, r.w, r.h, C_BG);
    if (!doc) return;
    gfx_set_clip(s, r);
    int ox = r.x + MARGIN, oy = r.y + 16 - scroll_y;
    for (int i = 0; i < doc->nitems; i++) {
        item_t *it = &doc->items[i];
        int y = oy + it->y;
        if (y > r.y + r.h || y + it->h < r.y) continue;
        if (it->type == IT_RULE) { gfx_hline(s, ox + it->x, y, it->w, C_RULE); continue; }
        if (it->type == IT_BOX || !it->text) continue;
        if (it->bg) gfx_fill(s, ox + it->x - 2, y, it->w + 4, it->h, it->bg);
        uint32_t col = it->color;
        if (it->link >= 0 && it->link == hover_link) col = 0xFF0B3AA8;
        font_draw(s, it->f, ox + it->x, y, it->text, col);
        if (it->underline) gfx_hline(s, ox + it->x, y + it->f->ascent + 2, it->w, it->link == hover_link ? col : WITH_ALPHA(col, 120));
        if (it->strike) gfx_hline(s, ox + it->x, y + it->f->ascent / 2 + 3, it->w, col);
    }
    for (int i = 0; i < doc->nboxes; i++) {
        imgbox_t *b = &doc->boxes[i];
        int x = ox + b->x, y = oy + b->y;
        if (y > r.y + r.h || y + b->h < r.y) continue;
        if (b->img <= -2) {
            /* form control */
            int idx = -2 - b->img;
            if (idx < 0 || idx >= doc->ninputs) continue;
            input_t *in = &doc->inputs[idx];
            in->x = b->x; in->y = b->y; in->w = b->w; in->h = b->h;
            if (in->submit) {
                gfx_fill_rounded(s, x, y + 2, b->w, b->h - 4, 6, 0xFFE5E7EB);
                gfx_rounded_rect(s, x, y + 2, b->w, b->h - 4, 6, 0xFF9CA3AF);
                ui_draw_text_center(s, &ui_font, mkrect(x, y + 2, b->w, b->h - 4), in->value, C_TEXT);
            } else {
                gfx_fill(s, x, y + 2, b->w, b->h - 4, 0xFFFFFFFF);
                gfx_rect(s, x, y + 2, b->w, b->h - 4, focus_input == idx ? C_LINK : 0xFF9CA3AF);
                font_draw_fit(s, &ui_font, x + 6, y + 8, in->value, b->w - 12, C_TEXT);
                if (focus_input == idx) {
                    int cx = x + 6 + MIN(font_text_width(&ui_font, in->value), b->w - 14);
                    gfx_vline(s, cx, y + 7, ui_font.height, C_TEXT);
                }
            }
            continue;
        }
        image_t *im = b->img >= 0 && b->img < doc->nimages ? &doc->images[b->img] : 0;
        if (im && im->state == 1 && im->px) {
            if (im->w == b->w && im->h == b->h) gfx_draw_image(s, x, y, im->px, im->w, im->h);
            else {
                surface_t src;
                gfx_init(&src, im->px, im->w, im->h, im->w);
                gfx_blit_scaled(s, mkrect(x, y, b->w, b->h), &src, mkrect(0, 0, im->w, im->h), true, true);
            }
        } else {
            gfx_fill(s, x, y, b->w, b->h, 0xFFF3F4F6);
            gfx_rect(s, x, y, b->w, b->h, 0xFFE5E7EB);
        }
    }
    gfx_reset_clip(s);
    /* scrollbar */
    if (doc->height > r.h)
        ui_draw_scrollbar(s, mkrect(r.x + r.w - 12, r.y, 12, r.h), doc->height, r.h, scroll_y, false);
}

static void scroll_to(int y) {
    int max = doc ? MAX(0, doc->height - view->r.h) : 0;
    scroll_y = MAX(0, MIN(y, max));
    ui_widget_invalidate(view);
}

static int hit_link(int mx, int my, int *input) {
    *input = -1;
    if (!doc) return -1;
    int x = mx - view->r.x - MARGIN, y = my - view->r.y - 16 + scroll_y;
    for (int i = 0; i < doc->ninputs; i++) {
        input_t *in = &doc->inputs[i];
        if (!in->hidden && in->w && x >= in->x && x < in->x + in->w && y >= in->y && y < in->y + in->h) { *input = i; return -1; }
    }
    for (int i = 0; i < doc->nitems; i++) {
        item_t *it = &doc->items[i];
        if (it->link < 0) continue;
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->link;
    }
    for (int i = 0; i < doc->nboxes; i++) {
        imgbox_t *b = &doc->boxes[i];
        if (b->link >= 0 && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return b->link;
    }
    return -1;
}

static bool view_event(ui_widget_t *w, gui_event_t *ev) {
    int input;
    if (ev->type == EV_MOUSE_WHEEL) { scroll_to(scroll_y + ev->wheel * 60); return true; }
    if (ev->type == EV_MOUSE_MOVE) {
        int l = hit_link(ev->x, ev->y, &input);
        if (l != hover_link) {
            hover_link = l;
            set_status(l >= 0 ? doc->links[l] : "");
            ui_widget_invalidate(w);
        }
        ui_set_cursor(win, l >= 0 || (input >= 0 && doc->inputs[input].submit) ? CUR_HAND : input >= 0 ? CUR_TEXT : CUR_ARROW);
        return true;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
        /* scrollbar drag */
        if (ev->x >= w->r.x + w->r.w - 14 && doc && doc->height > w->r.h) {
            scroll_to((ev->y - w->r.y) * doc->height / w->r.h - w->r.h / 2);
            return true;
        }
        int l = hit_link(ev->x, ev->y, &input);
        ui_focus(win, w);
        if (input >= 0) {
            if (doc->inputs[input].submit) submit_form(doc->inputs[input].form);
            else { focus_input = input; ui_widget_invalidate(w); }
            return true;
        }
        focus_input = -1;
        if (l >= 0) {
            char url[1024];
            strlcpy(url, doc->links[l], sizeof(url));
            start_load(url, true);
        }
        ui_widget_invalidate(w);
        return true;
    }
    if (ev->type == EV_KEY_DOWN) {
        if (focus_input >= 0 && doc && focus_input < doc->ninputs) {
            input_t *in = &doc->inputs[focus_input];
            size_t l = strlen(in->value);
            if (ev->key == KEY_ENTER) { submit_form(in->form); return true; }
            if (ev->key == KEY_BACKSPACE) { if (l) { int p = utf8_prev(in->value, (int)l); in->value[p] = 0; } }
            else if (ev->key == KEY_ESC) focus_input = -1;
            else if (ev->ch >= ' ' && l + 5 < sizeof(in->value)) { char b[5]; int n = utf8_encode(ev->ch, b); memcpy(in->value + l, b, n); in->value[l + n] = 0; }
            ui_widget_invalidate(w);
            return true;
        }
        int page = w->r.h - 60;
        switch (ev->key) {
        case KEY_DOWN: scroll_to(scroll_y + 48); return true;
        case KEY_UP: scroll_to(scroll_y - 48); return true;
        case KEY_PGDN: case KEY_SPACE: scroll_to(scroll_y + page); return true;
        case KEY_PGUP: scroll_to(scroll_y - page); return true;
        case KEY_HOME: scroll_to(0); return true;
        case KEY_END: scroll_to(1 << 30); return true;
        case KEY_BACKSPACE: go_back(0); return true;
        }
    }
    return false;
}

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    bool ctrl = ev->mods & MOD_CTRL, alt = ev->mods & MOD_ALT;
    if (ctrl && (ev->ch == 'l' || ev->ch == 'L')) { ui_focus(win, addr); ui_textbox_select_all(addr); }
    else if (ctrl && (ev->ch == 'r' || ev->ch == 'R')) do_reload(0);
    else if (ev->key == KEY_F5) do_reload(0);
    else if (alt && ev->key == KEY_LEFT) go_back(0);
    else if (alt && ev->key == KEY_RIGHT) go_fwd(0);
    else if (alt && ev->key == KEY_HOME) go_home(0);
}

static void on_resize(ui_window_t *w) {
    (void)w;
    if (doc && doc->layout_w != content_width()) relayout(false);
}

int main(int argc, char **argv) {
    ui_init();
    win = ui_window("Browser", W, H, WF_RESIZABLE, "browser");
    win->on_key = on_key;
    win->on_resize = on_resize;
    btn_back = ui_toolbutton(win, 8, 8, 32, "back", "Back (Alt+Left)", go_back);
    btn_fwd = ui_toolbutton(win, 42, 8, 32, "forward", "Forward (Alt+Right)", go_fwd);
    ui_toolbutton(win, 76, 8, 32, "refresh", "Reload (F5)", do_reload);
    ui_toolbutton(win, 110, 8, 32, "home", "Home", go_home);
    addr = ui_textbox(win, 150, 8, W - 158, 32, "");
    ui_textbox_set_placeholder(addr, "Adresse oder Suchbegriff eingeben");
    ui_set_anchor(addr, A_LEFT | A_RIGHT | A_TOP);
    addr->on_activate = addr_enter;
    view = ui_canvas(win, 0, TOOLBAR_H, W, H - TOOLBAR_H - STATUS_H, draw_view, view_event);
    ui_set_anchor(view, A_ALL);
    view->focusable = true;
    status = ui_label(win, 10, H - STATUS_H + 2, W - 20, STATUS_H - 4, "");
    status->color = ui_theme.window_text_dim;
    ui_set_anchor(status, A_LEFT | A_RIGHT | A_BOTTOM);
    ui_timer(100, poll_job, 0);
    start_load(argc > 1 ? argv[1] : "about:home", true);
    ui_run();
    return 0;
}
