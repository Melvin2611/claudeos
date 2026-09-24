/* sh - the ClaudeOS shell
 * line editing with history and tab completion, pipes, redirection, && || ; &, variables, scripts */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <claudeos.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define HIST_MAX 100

static bool interactive;
static int last_status;
static char *history[HIST_MAX];
static int hist_count;

/* ------------------------------------------------------------------ terminal helpers */
static void tty_mode(int mode) {
    if (interactive) ioctl(0, TIOCSMODE, &mode);
}

static void tty_fg(int pid) {
    if (interactive) ioctl(0, TIOCSPGRP, &pid);
}

static void out(const char *s) { write(1, s, strlen(s)); }

static int term_cols(void) {
    kwinsize_t ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.cols) return ws.cols;
    return 80;
}

static void prompt_string(char *buf, size_t n) {
    char cwd[256];
    getcwd(cwd, sizeof(cwd));
    const char *home = getenv("HOME");
    char shown[256];
    if (home && *home && !strncmp(cwd, home, strlen(home)) && (cwd[strlen(home)] == 0 || cwd[strlen(home)] == '/'))
        snprintf(shown, sizeof(shown), "~%s", cwd + strlen(home));
    else
        strlcpy(shown, cwd, sizeof(shown));
    const char *user = getenv("USER");
    snprintf(buf, n, "\x1b[1;32m%s@claudeos\x1b[0m:\x1b[1;34m%s\x1b[0m%s ", user ? user : "user", shown,
             last_status ? "\x1b[31m$\x1b[0m" : "$");
}

/* visible width of a string with escape sequences */
static int visible_len(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == 0x1b) { while (*s && *s != 'm') s++; if (*s) s++; continue; }
        if ((*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

/* ------------------------------------------------------------------ tab completion */
static const char *builtins[] = { "cd", "pwd", "exit", "export", "unset", "set", "echo", "help", "history",
                                  "clear", "source", "which", "true", "false", "type", 0 };

static int common_prefix(char **v, int n) {
    if (!n) return 0;
    int len = (int)strlen(v[0]);
    for (int i = 1; i < n; i++) {
        int j = 0;
        while (j < len && v[i][j] == v[0][j]) j++;
        len = j;
    }
    return len;
}

/* returns number of candidates; fills cand (malloc'd strings) */
static int complete(const char *word, bool first_word, char **cand, int max) {
    int n = 0;
    char dir[256], prefix[256];
    const char *slash = strrchr(word, '/');
    if (first_word && !slash) {
        for (int i = 0; builtins[i] && n < max; i++)
            if (!strncmp(builtins[i], word, strlen(word))) cand[n++] = strdup(builtins[i]);
        DIR *d = opendir("/bin");
        struct dirent *e;
        while (d && (e = readdir(d)) && n < max)
            if (!strncmp(e->d_name, word, strlen(word))) {
                bool dup = false;
                for (int i = 0; i < n; i++) if (!strcmp(cand[i], e->d_name)) dup = true;
                if (!dup) cand[n++] = strdup(e->d_name);
            }
        if (d) closedir(d);
        return n;
    }
    if (slash) {
        size_t dl = slash - word;
        if (dl == 0) strcpy(dir, "/");
        else { memcpy(dir, word, dl); dir[dl] = 0; }
        strlcpy(prefix, slash + 1, sizeof(prefix));
    } else {
        strcpy(dir, ".");
        strlcpy(prefix, word, sizeof(prefix));
    }
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, prefix, strlen(prefix))) continue;
        if (e->d_name[0] == '.' && prefix[0] != '.') continue;
        char full[512];
        if (slash) snprintf(full, sizeof(full), "%.*s%s%s", (int)(slash - word + 1), word, e->d_name,
                            e->d_type == DT_DIR ? "/" : "");
        else snprintf(full, sizeof(full), "%s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
        cand[n++] = strdup(full);
    }
    if (d) closedir(d);
    return n;
}

/* ------------------------------------------------------------------ line editor */
static char *read_line_interactive(const char *prompt) {
    static char buf[MAX_LINE];
    int len = 0, pos = 0;
    int hist_idx = hist_count;
    char saved[MAX_LINE] = "";
    buf[0] = 0;
    tty_mode(0);   /* raw, no echo, no signals */
    out(prompt);
    int plen = visible_len(prompt);
    (void)plen;

    for (;;) {
        unsigned char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) { tty_mode(TTY_ECHO | TTY_ICANON | TTY_ISIG); return 0; }
        if (c == 0x1b) {
            unsigned char seq[4] = { 0 };
            if (read(0, &seq[0], 1) <= 0) continue;
            if (seq[0] != '[' && seq[0] != 'O') continue;
            if (read(0, &seq[1], 1) <= 0) continue;
            if (seq[1] >= '0' && seq[1] <= '9') read(0, &seq[2], 1);   /* trailing ~ */
            switch (seq[1]) {
            case 'A': case 'B': {
                if (!hist_count) break;
                if (hist_idx == hist_count) strlcpy(saved, buf, sizeof(saved));
                if (seq[1] == 'A' && hist_idx > 0) hist_idx--;
                else if (seq[1] == 'B' && hist_idx < hist_count) hist_idx++;
                else break;
                const char *src = hist_idx == hist_count ? saved : history[hist_idx];
                /* erase current line */
                while (pos > 0) { out("\b"); pos--; }
                out("\x1b[K");
                strlcpy(buf, src, sizeof(buf));
                len = pos = (int)strlen(buf);
                out(buf);
                break;
            }
            case 'C':
                if (pos < len) {
                    int np = pos + 1;
                    while (np < len && (buf[np] & 0xC0) == 0x80) np++;
                    write(1, buf + pos, np - pos);
                    pos = np;
                }
                break;
            case 'D':
                if (pos > 0) {
                    pos--;
                    while (pos > 0 && (buf[pos] & 0xC0) == 0x80) pos--;
                    out("\b");
                }
                break;
            case 'H': case '1': while (pos > 0) { out("\b"); pos--; while (pos > 0 && (buf[pos] & 0xC0) == 0x80) pos--; } break;
            case 'F': case '4': if (pos < len) { write(1, buf + pos, len - pos); pos = len; } break;
            case '3': /* delete */
                if (pos < len) {
                    int np = pos + 1;
                    while (np < len && (buf[np] & 0xC0) == 0x80) np++;
                    memmove(buf + pos, buf + np, len - np + 1);
                    len -= np - pos;
                    out("\x1b[K");
                    write(1, buf + pos, len - pos);
                    for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
                }
                break;
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            out("\r\n");
            buf[len] = 0;
            tty_mode(TTY_ECHO | TTY_ICANON | TTY_ISIG);
            return buf;
        }
        if (c == 0x03) {   /* ^C */
            out("^C\r\n");
            len = pos = 0;
            buf[0] = 0;
            hist_idx = hist_count;
            last_status = 130;
            prompt_string((char *)saved, sizeof(saved));
            out(saved);
            saved[0] = 0;
            continue;
        }
        if (c == 0x04) {   /* ^D */
            if (len == 0) { out("exit\r\n"); tty_mode(TTY_ECHO | TTY_ICANON | TTY_ISIG); return 0; }
            continue;
        }
        if (c == 0x0c) {   /* ^L */
            out("\x1b[2J\x1b[H");
            char p[300];
            prompt_string(p, sizeof(p));
            out(p);
            write(1, buf, len);
            for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
            continue;
        }
        if (c == 0x01) { while (pos > 0) { out("\b"); pos--; while (pos > 0 && (buf[pos] & 0xC0) == 0x80) pos--; } continue; }
        if (c == 0x05) { if (pos < len) { write(1, buf + pos, len - pos); pos = len; } continue; }
        if (c == 0x15) {   /* ^U: delete to start */
            int n = 0;
            for (int i = 0; i < pos; i++) if ((buf[i] & 0xC0) != 0x80) n++;
            for (int i = 0; i < n; i++) out("\b");
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos;
            pos = 0;
            out("\x1b[K");
            write(1, buf, len);
            for (int i = 0; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
            continue;
        }
        if (c == 0x0b) { buf[pos] = 0; len = pos; out("\x1b[K"); continue; }   /* ^K */
        if (c == 0x17) {   /* ^W: delete previous word */
            int start = pos;
            while (start > 0 && buf[start - 1] == ' ') start--;
            while (start > 0 && buf[start - 1] != ' ') start--;
            int n = 0;
            for (int i = start; i < pos; i++) if ((buf[i] & 0xC0) != 0x80) n++;
            for (int i = 0; i < n; i++) out("\b");
            memmove(buf + start, buf + pos, len - pos + 1);
            len -= pos - start;
            pos = start;
            out("\x1b[K");
            write(1, buf + pos, len - pos);
            for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
            continue;
        }
        if (c == '\t') {
            int ws = pos;
            while (ws > 0 && buf[ws - 1] != ' ' && buf[ws - 1] != '|' && buf[ws - 1] != ';') ws--;
            bool first = true;
            for (int i = 0; i < ws; i++) if (buf[i] != ' ') { first = false; break; }
            for (int i = ws - 1; i >= 0; i--) {
                if (buf[i] == ' ') continue;
                if (buf[i] == '|' || buf[i] == ';' || buf[i] == '&') first = true;
                break;
            }
            char word[256];
            int wl = MIN(pos - ws, 255);
            memcpy(word, buf + ws, wl);
            word[wl] = 0;
            char *cand[128];
            int n = complete(word, first, cand, 128);
            if (n == 1 || (n > 1 && common_prefix(cand, n) > wl)) {
                int cl = n == 1 ? (int)strlen(cand[0]) : common_prefix(cand, n);
                char add[256];
                int al = cl - wl;
                memcpy(add, cand[0] + wl, al);
                if (n == 1 && cand[0][cl - 1] != '/') add[al++] = ' ';
                add[al] = 0;
                if (len + al < MAX_LINE - 1) {
                    memmove(buf + pos + al, buf + pos, len - pos + 1);
                    memcpy(buf + pos, add, al);
                    len += al;
                    write(1, buf + pos, len - pos);
                    pos += al;
                    for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
                }
            } else if (n > 1) {
                out("\r\n");
                int cols = term_cols(), maxw = 0;
                for (int i = 0; i < n; i++) maxw = MAX(maxw, (int)strlen(cand[i]));
                int per = MAX(1, cols / (maxw + 2));
                for (int i = 0; i < n; i++) {
                    printf("%-*s", maxw + 2, cand[i]);
                    if ((i + 1) % per == 0 || i == n - 1) printf("\r\n");
                }
                fflush(stdout);
                char p[300];
                prompt_string(p, sizeof(p));
                out(p);
                write(1, buf, len);
                for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
            } else {
                out("\a");
            }
            for (int i = 0; i < n; i++) free(cand[i]);
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            if (pos == 0) continue;
            int start = pos - 1;
            while (start > 0 && (buf[start] & 0xC0) == 0x80) start--;
            memmove(buf + start, buf + pos, len - pos + 1);
            len -= pos - start;
            pos = start;
            out("\b\x1b[K");
            write(1, buf + pos, len - pos);
            for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
            continue;
        }
        if (c < 0x20) continue;
        if (len >= MAX_LINE - 4) continue;
        /* insert (UTF-8 continuation bytes arrive one by one) */
        memmove(buf + pos + 1, buf + pos, len - pos + 1);
        buf[pos] = (char)c;
        len++;
        pos++;
        if ((c & 0xC0) == 0x80 || c >= 0xC0) {
            /* wait until the character is complete before echoing */
            int start = pos - 1;
            while (start > 0 && (buf[start] & 0xC0) == 0x80) start--;
            unsigned char lead = (unsigned char)buf[start];
            int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (pos - start < need) continue;
            write(1, buf + start, len - start);
        } else {
            write(1, buf + pos - 1, len - pos + 1);
        }
        for (int i = pos; i < len; i++) if ((buf[i] & 0xC0) != 0x80) out("\b");
    }
}

static void add_history(const char *line) {
    if (!*line) return;
    if (hist_count && !strcmp(history[hist_count - 1], line)) return;
    if (hist_count == HIST_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HIST_MAX - 1));
        hist_count--;
    }
    history[hist_count++] = strdup(line);
}

/* ------------------------------------------------------------------ parsing */
enum { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_OUT, T_APPEND, T_IN, T_ERR, T_END };
typedef struct { int type; char *text; } token_t;

static void append_var(char **o, size_t *n, size_t *cap, const char *v) {
    size_t l = strlen(v);
    while (*n + l + 1 >= *cap) { *cap *= 2; *o = realloc(*o, *cap); }
    memcpy(*o + *n, v, l);
    *n += l;
}

static int tokenize(const char *s, token_t *toks, int max) {
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (*s == '|' && s[1] == '|') { toks[n++] = (token_t){ T_OR, 0 }; s += 2; continue; }
        if (*s == '&' && s[1] == '&') { toks[n++] = (token_t){ T_AND, 0 }; s += 2; continue; }
        if (*s == '|') { toks[n++] = (token_t){ T_PIPE, 0 }; s++; continue; }
        if (*s == ';') { toks[n++] = (token_t){ T_SEMI, 0 }; s++; continue; }
        if (*s == '&') { toks[n++] = (token_t){ T_BG, 0 }; s++; continue; }
        if (*s == '>' && s[1] == '>') { toks[n++] = (token_t){ T_APPEND, 0 }; s += 2; continue; }
        if (*s == '2' && s[1] == '>') { toks[n++] = (token_t){ T_ERR, 0 }; s += 2; continue; }
        if (*s == '>') { toks[n++] = (token_t){ T_OUT, 0 }; s++; continue; }
        if (*s == '<') { toks[n++] = (token_t){ T_IN, 0 }; s++; continue; }
        size_t cap = 64, len = 0;
        char *w = malloc(cap);
        while (*s && !strchr(" \t|;&<>", *s)) {
            if (*s == '\'') {
                s++;
                while (*s && *s != '\'') { if (len + 2 >= cap) w = realloc(w, cap *= 2); w[len++] = *s++; }
                if (*s) s++;
            } else if (*s == '"') {
                s++;
                while (*s && *s != '"') {
                    if (*s == '\\' && s[1]) s++;
                    else if (*s == '$' && (isalpha((unsigned char)s[1]) || s[1] == '_' || s[1] == '?' || s[1] == '{')) goto dollar_q;
                    if (len + 2 >= cap) w = realloc(w, cap *= 2);
                    w[len++] = *s++;
                    continue;
                dollar_q:;
                    s++;
                    char name[64];
                    int k = 0;
                    bool brace = *s == '{';
                    if (brace) s++;
                    if (*s == '?') { snprintf(name, sizeof(name), "%d", last_status); s++; append_var(&w, &len, &cap, name); if (brace && *s == '}') s++; continue; }
                    while ((isalnum((unsigned char)*s) || *s == '_') && k < 63) name[k++] = *s++;
                    name[k] = 0;
                    if (brace && *s == '}') s++;
                    const char *v = getenv(name);
                    if (v) append_var(&w, &len, &cap, v);
                }
                if (*s) s++;
            } else if (*s == '\\' && s[1]) {
                s++;
                if (len + 2 >= cap) w = realloc(w, cap *= 2);
                w[len++] = *s++;
            } else if (*s == '$' && (isalpha((unsigned char)s[1]) || s[1] == '_' || s[1] == '?' || s[1] == '{' || s[1] == '$')) {
                s++;
                char name[64];
                int k = 0;
                bool brace = *s == '{';
                if (brace) s++;
                if (*s == '?' || *s == '$') {
                    snprintf(name, sizeof(name), "%d", *s == '?' ? last_status : getpid());
                    s++;
                    if (brace && *s == '}') s++;
                    append_var(&w, &len, &cap, name);
                    continue;
                }
                while ((isalnum((unsigned char)*s) || *s == '_') && k < 63) name[k++] = *s++;
                name[k] = 0;
                if (brace && *s == '}') s++;
                const char *v = getenv(name);
                if (v) append_var(&w, &len, &cap, v);
            } else if (*s == '~' && len == 0 && (s[1] == '/' || s[1] == 0 || s[1] == ' ')) {
                const char *h = getenv("HOME");
                append_var(&w, &len, &cap, h ? h : "/home");
                s++;
            } else {
                if (len + 2 >= cap) w = realloc(w, cap *= 2);
                w[len++] = *s++;
            }
        }
        w[len] = 0;
        toks[n++] = (token_t){ T_WORD, w };
    }
    toks[n] = (token_t){ T_END, 0 };
    return n;
}

/* ------------------------------------------------------------------ builtins */
static int run_script(const char *path);

static int builtin(char **argv, int argc, int outfd) {
    const char *cmd = argv[0];
    if (!strcmp(cmd, "cd")) {
        const char *dir = argc > 1 ? argv[1] : getenv("HOME");
        if (!dir) dir = "/";
        if (!strcmp(dir, "-")) dir = getenv("OLDPWD") ? getenv("OLDPWD") : ".";
        char old[256];
        getcwd(old, sizeof(old));
        if (chdir(dir) < 0) { dprintf(2, "cd: %s: %s\n", dir, strerror(errno)); return 1; }
        char cwd[256];
        setenv("OLDPWD", old, 1);
        setenv("PWD", getcwd(cwd, sizeof(cwd)), 1);
        return 0;
    }
    if (!strcmp(cmd, "pwd")) {
        char cwd[256];
        dprintf(outfd, "%s\n", getcwd(cwd, sizeof(cwd)));
        return 0;
    }
    if (!strcmp(cmd, "exit")) {
        tty_mode(TTY_ECHO | TTY_ICANON | TTY_ISIG);
        exit(argc > 1 ? atoi(argv[1]) : last_status);
    }
    if (!strcmp(cmd, "export") || !strcmp(cmd, "set")) {
        if (argc == 1) {
            for (char **e = environ; e && *e; e++) dprintf(outfd, "%s\n", *e);
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = 0;
            setenv(argv[i], eq + 1, 1);
            *eq = '=';
        }
        return 0;
    }
    if (!strcmp(cmd, "unset")) {
        for (int i = 1; i < argc; i++) unsetenv(argv[i]);
        return 0;
    }
    if (!strcmp(cmd, "echo")) {
        int i = 1;
        bool nl = true;
        if (argc > 1 && !strcmp(argv[1], "-n")) { nl = false; i++; }
        for (; i < argc; i++) {
            write(outfd, argv[i], strlen(argv[i]));
            if (i < argc - 1) write(outfd, " ", 1);
        }
        if (nl) write(outfd, "\n", 1);
        return 0;
    }
    if (!strcmp(cmd, "history")) {
        for (int i = 0; i < hist_count; i++) dprintf(outfd, "%4d  %s\n", i + 1, history[i]);
        return 0;
    }
    if (!strcmp(cmd, "clear")) {
        dprintf(outfd, "\x1b[2J\x1b[H");
        return 0;
    }
    if (!strcmp(cmd, "true")) return 0;
    if (!strcmp(cmd, "false")) return 1;
    if (!strcmp(cmd, "source") || !strcmp(cmd, ".")) {
        if (argc < 2) return 1;
        return run_script(argv[1]);
    }
    if (!strcmp(cmd, "which") || !strcmp(cmd, "type")) {
        int rc = 0;
        for (int i = 1; i < argc; i++) {
            bool b = false;
            for (int k = 0; builtins[k]; k++) if (!strcmp(builtins[k], argv[i])) b = true;
            char path[256];
            snprintf(path, sizeof(path), "/bin/%s", argv[i]);
            struct stat st;
            if (b) dprintf(outfd, "%s: shell builtin\n", argv[i]);
            else if (stat(path, &st) == 0) dprintf(outfd, "%s\n", path);
            else { dprintf(outfd, "%s: not found\n", argv[i]); rc = 1; }
        }
        return rc;
    }
    if (!strcmp(cmd, "help")) {
        dprintf(outfd,
            "\x1b[1mClaudeOS shell\x1b[0m - built-in commands:\n"
            "  cd [dir]          change directory (cd - for previous)\n"
            "  pwd               print working directory\n"
            "  echo [-n] args    print arguments\n"
            "  export VAR=value  set environment variable (set: list)\n"
            "  unset VAR         remove variable\n"
            "  history           show command history\n"
            "  source file       run a script in this shell\n"
            "  which cmd         locate a command\n"
            "  clear             clear the screen\n"
            "  exit [code]       leave the shell\n"
            "\nOperators: cmd1 | cmd2, > file, >> file, < file, 2> file, &&, ||, ;, & (background)\n"
            "Keys: Tab completes, Up/Down history, Ctrl+C cancel, Ctrl+L clear, Ctrl+A/E/U/K/W edit\n"
            "\nPrograms in /bin: ");
        DIR *d = opendir("/bin");
        struct dirent *e;
        int col = 19;
        while (d && (e = readdir(d))) {
            int l = (int)strlen(e->d_name) + 1;
            if (col + l > 78) { dprintf(outfd, "\n  "); col = 2; }
            dprintf(outfd, "%s ", e->d_name);
            col += l;
        }
        if (d) closedir(d);
        dprintf(outfd, "\n");
        return 0;
    }
    return -1;   /* not a builtin */
}

static bool is_builtin(const char *cmd) {
    for (int i = 0; builtins[i]; i++) if (!strcmp(builtins[i], cmd)) return true;
    return !strcmp(cmd, ".");
}

/* ------------------------------------------------------------------ execution */
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *in, *out, *err;
    bool append;
} cmd_t;

static int reap_background(void) {
    int st, pid, n = 0;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (interactive) printf("[%d] done (exit %d)\n", pid, st);
        n++;
    }
    return n;
}

/* run a pipeline of n commands; returns exit status of the last one */
static int run_pipeline(cmd_t *cmds, int n, bool background) {
    if (n == 1 && cmds[0].argc > 0 && is_builtin(cmds[0].argv[0]) && !background) {
        int outfd = 1;
        if (cmds[0].out) {
            outfd = open(cmds[0].out, O_WRONLY | O_CREAT | (cmds[0].append ? O_APPEND : O_TRUNC));
            if (outfd < 0) { dprintf(2, "sh: %s: %s\n", cmds[0].out, strerror(errno)); return 1; }
        }
        int r = builtin(cmds[0].argv, cmds[0].argc, outfd);
        if (outfd != 1) close(outfd);
        return r;
    }
    int pids[16], npids = 0;
    int prev_read = -1;
    int status = 0;
    for (int i = 0; i < n; i++) {
        cmd_t *c = &cmds[i];
        int fdin = prev_read >= 0 ? prev_read : 0;
        int fdout = 1, fderr = 2;
        int pipefd[2] = { -1, -1 };
        if (i < n - 1) {
            if (pipe(pipefd) < 0) { perror("sh: pipe"); break; }
            fdout = pipefd[1];
        }
        int opened[3] = { -1, -1, -1 };
        if (c->in) {
            fdin = opened[0] = open(c->in, O_RDONLY);
            if (fdin < 0) { dprintf(2, "sh: %s: %s\n", c->in, strerror(errno)); status = 1; goto next; }
        }
        if (c->out) {
            fdout = opened[1] = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC));
            if (fdout < 0) { dprintf(2, "sh: %s: %s\n", c->out, strerror(errno)); status = 1; goto next; }
        }
        if (c->err) {
            fderr = opened[2] = open(c->err, O_WRONLY | O_CREAT | O_TRUNC);
            if (fderr < 0) fderr = 2;
        }
        if (c->argc == 0) goto next;
        if (is_builtin(c->argv[0])) {
            /* builtin inside a pipeline: run it here with the pipe as output */
            status = builtin(c->argv, c->argc, fdout);
            goto next;
        }
        {
            int map[3] = { fdin, fdout, fderr };
            int pid = spawn(c->argv[0], c->argv, environ, map, 0);
            if (pid < 0) {
                if (errno == ENOENT) dprintf(2, "sh: %s: command not found\n", c->argv[0]);
                else dprintf(2, "sh: %s: %s\n", c->argv[0], strerror(errno));
                status = errno == ENOENT ? 127 : 126;
            } else if (npids < 16) {
                pids[npids++] = pid;
            }
        }
    next:
        for (int k = 0; k < 3; k++) if (opened[k] >= 0) close(opened[k]);
        if (prev_read >= 0) close(prev_read);
        if (pipefd[1] >= 0) close(pipefd[1]);
        prev_read = pipefd[0];
    }
    if (prev_read >= 0) close(prev_read);
    if (background) {
        for (int i = 0; i < npids; i++) printf("[%d] started\n", pids[i]);
        return 0;
    }
    if (npids) {
        tty_fg(pids[npids - 1]);
        for (int i = 0; i < npids; i++) {
            int st = 0;
            if (waitpid(pids[i], &st, 0) > 0 && i == npids - 1) status = st;
        }
        tty_fg(getpid());
        if (status == 130 + 0 && interactive) { /* interrupted */ }
    }
    return status;
}

static int execute_line(const char *line) {
    token_t toks[256];
    int nt = tokenize(line, toks, 256);
    int i = 0;
    int status = last_status;
    int skip_until_sep = 0;   /* for && / || short circuit */
    while (i < nt) {
        cmd_t cmds[16];
        int nc = 0;
        memset(cmds, 0, sizeof(cmds));
        cmd_t *c = &cmds[nc++];
        int sep = T_END;
        bool syntax_error = false;
        for (; i < nt; i++) {
            token_t *t = &toks[i];
            if (t->type == T_WORD) {
                if (c->argc < MAX_ARGS - 1) c->argv[c->argc++] = t->text;
            } else if (t->type == T_PIPE) {
                if (nc >= 16) { syntax_error = true; break; }
                c = &cmds[nc++];
            } else if (t->type == T_OUT || t->type == T_APPEND || t->type == T_IN || t->type == T_ERR) {
                if (i + 1 >= nt || toks[i + 1].type != T_WORD) { syntax_error = true; break; }
                char *f = toks[++i].text;
                if (t->type == T_IN) c->in = f;
                else if (t->type == T_ERR) c->err = f;
                else { c->out = f; c->append = t->type == T_APPEND; }
            } else {
                sep = t->type;
                i++;
                break;
            }
        }
        if (syntax_error) { dprintf(2, "sh: syntax error\n"); status = 2; break; }
        bool run = true;
        if (skip_until_sep == T_AND && status != 0) run = false;
        if (skip_until_sep == T_OR && status == 0) run = false;
        if (run && (cmds[0].argc || nc > 1)) status = run_pipeline(cmds, nc, sep == T_BG);
        skip_until_sep = (sep == T_AND || sep == T_OR) ? sep : 0;
    }
    for (int k = 0; k < nt; k++) free(toks[k].text);
    last_status = status;
    return status;
}

static int run_file(FILE *f) {
    char line[MAX_LINE];
    int status = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        status = execute_line(line);
    }
    return status;
}

static int run_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { dprintf(2, "sh: %s: %s\n", path, strerror(errno)); return 127; }
    int r = run_file(f);
    fclose(f);
    return r;
}

int main(int argc, char **argv) {
    if (!getenv("PATH")) setenv("PATH", "/bin", 1);
    if (!getenv("HOME")) setenv("HOME", "/home", 1);
    if (!getenv("USER")) setenv("USER", "user", 1);
    setenv("SHELL", "/bin/sh", 1);
    if (argc > 2 && !strcmp(argv[1], "-c")) return execute_line(argv[2]);
    if (argc > 1) return run_script(argv[1]);
    interactive = isatty(0);
    if (!interactive) return run_file(stdin);

    tty_fg(getpid());
    char cwd[256];
    if (!strcmp(getcwd(cwd, sizeof(cwd)), "/")) chdir(getenv("HOME"));
    FILE *motd = fopen("/etc/motd", "r");
    if (motd) {
        char line[256];
        while (fgets(line, sizeof(line), motd)) fputs(line, stdout);
        fclose(motd);
        fflush(stdout);
    }
    for (;;) {
        reap_background();
        fflush(stdout);
        char prompt[300];
        prompt_string(prompt, sizeof(prompt));
        char *line = read_line_interactive(prompt);
        if (!line) break;
        char *p = line;
        while (*p == ' ') p++;
        if (!*p) continue;
        add_history(p);
        char copy[MAX_LINE];
        strlcpy(copy, p, sizeof(copy));
        execute_line(copy);
        fflush(stdout);
    }
    return 0;
}
