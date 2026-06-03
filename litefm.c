/*
 * litefm - a minimal two-pane terminal file manager. Single file, depends only on
 *      libc (no ncurses).
 *
 * Two independent panes side by side. The active pane has the cursor; Tab
 * switches. F5 / c copies, F6 / m moves the selection from the ACTIVE pane
 * into the OTHER pane's directory -- "copy from left to right". File ops
 * shell out to cp / mv / rm. Files open in $LITEFM_EDITOR, else litefe if
 * installed, else $EDITOR / nano / vi.
 *
 * Build:  cc -O2 -Wall -o litefm src/litefm.c
 * Keys:   press ? inside the program.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <wchar.h>
#include <locale.h>
#include <langinfo.h>
#include <stdarg.h>
#include <poll.h>
#include <pwd.h>
#include <grp.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ */
/* Terminal globals                                                   */
/* ------------------------------------------------------------------ */
static struct termios g_orig, g_raw;
static int g_have_orig = 0, g_in_tui = 0;
static volatile sig_atomic_t g_resized = 1;
static int g_rows = 24, g_cols = 80;
static int g_utf8 = 1;

/* ------------------------------------------------------------------ */
/* Safe allocation                                                    */
/* ------------------------------------------------------------------ */
static void leave_tui(void);
static void render(void);
static int read_key(void);
static void die(const char *m) { leave_tui(); fprintf(stderr, "litefm: %s\n", m); exit(1); }
static void *xmalloc(size_t n)  { void *p = malloc(n);     if (!p) die("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) die("out of memory"); return q; }
static char *xstrdup(const char *s) { char *p = strdup(s); if (!p) die("out of memory"); return p; }

/* ------------------------------------------------------------------ */
/* Dynamic string buffer                                              */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t len, cap; } Buf;

static void buf_reserve(Buf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + need + 1) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
}
static void buf_add(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_s(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_pad(Buf *b, int n) { while (n-- > 0) buf_add(b, " ", 1); }
static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    buf_add(b, tmp, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */
static void wr(const char *s) { ssize_t r = write(STDOUT_FILENO, s, strlen(s)); (void)r; }

static int ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}
/* locale-independent human size (always '.' as decimal point) */
static void human(off_t bytes, char *out, size_t n) {
    const char *u[] = {"B", "K", "M", "G", "T", "P"};
    if (bytes < 0) bytes = 0;
    if (bytes < 1024) { snprintf(out, n, "%u", (unsigned)bytes); return; }
    double s = (double)bytes;
    int i = 0;
    while (s >= 1024.0 && i < 5) { s /= 1024.0; i++; }
    if (s >= 10.0) snprintf(out, n, "%d%s", (int)(s + 0.5), u[i]);
    else {
        int w = (int)s, f = (int)((s - w) * 10 + 0.5);
        if (f >= 10) { w++; f = 0; }
        snprintf(out, n, "%d.%d%s", w, f, u[i]);
    }
}
static void join_path(char *out, size_t n, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", dir, name);
}
static void path_parent(const char *p, char *out, size_t n) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", p);
    size_t L = strlen(tmp);
    while (L > 1 && tmp[L - 1] == '/') tmp[--L] = 0;
    char *s = strrchr(tmp, '/');
    if (!s) { snprintf(out, n, "."); return; }
    if (s == tmp) { snprintf(out, n, "/"); return; }
    *s = 0;
    snprintf(out, n, "%s", tmp);
}
static void path_base(const char *p, char *out, size_t n) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", p);
    size_t L = strlen(tmp);
    while (L > 1 && tmp[L - 1] == '/') tmp[--L] = 0;
    char *s = strrchr(tmp, '/');
    snprintf(out, n, "%s", s ? s + 1 : tmp);
}
static void abbrev_home(const char *path, char *out, size_t n) {
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (home && hl && strncmp(path, home, hl) == 0 && (path[hl] == '/' || path[hl] == 0))
        snprintf(out, n, "~%s", path + hl);
    else
        snprintf(out, n, "%s", path);
}

/* ------------------------------------------------------------------ */
/* Unicode-aware width                                                */
/* ------------------------------------------------------------------ */
static int str_width(const char *s) {
    mbstate_t ps; memset(&ps, 0, sizeof ps);
    size_t len = strlen(s);
    const char *p = s;
    int w = 0;
    while (*p) {
        wchar_t wc;
        size_t k = mbrtowc(&wc, p, len - (p - s), &ps);
        if (k == (size_t)-1 || k == (size_t)-2) { p++; w++; memset(&ps, 0, sizeof ps); continue; }
        if (k == 0) break;
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        w += cw; p += k;
    }
    return w;
}
/* append s truncated to at most maxw columns; never splits UTF-8.
   returns columns emitted. */
static int buf_add_trunc(Buf *b, const char *s, int maxw) {
    if (maxw <= 0) return 0;
    mbstate_t ps; memset(&ps, 0, sizeof ps);
    size_t len = strlen(s);
    const char *p = s;
    int w = 0;
    while (*p) {
        wchar_t wc;
        size_t k = mbrtowc(&wc, p, len - (p - s), &ps);
        if (k == (size_t)-1 || k == (size_t)-2) { k = 1; wc = (unsigned char)*p; memset(&ps, 0, sizeof ps); }
        if (k == 0) break;
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        if (w + cw > maxw) break;
        buf_add(b, p, k);
        w += cw; p += k;
    }
    return w;
}
/* append the trailing portion of s that fits in maxw columns (for input fields
   so the end being typed stays visible). returns the columns emitted. */
static int buf_add_tail(Buf *b, const char *s, int maxw) {
    int total = str_width(s);
    if (total <= maxw) { buf_s(b, s); return total; }
    int skip = total - maxw;
    mbstate_t ps; memset(&ps, 0, sizeof ps);
    size_t len = strlen(s);
    const char *p = s;
    int w = 0;
    while (*p && w < skip) {
        wchar_t wc;
        size_t k = mbrtowc(&wc, p, len - (p - s), &ps);
        if (k == (size_t)-1 || k == (size_t)-2) { k = 1; wc = (unsigned char)*p; memset(&ps, 0, sizeof ps); }
        if (k == 0) break;
        int cw = wcwidth(wc); if (cw < 0) cw = 1;
        w += cw; p += k;
    }
    buf_s(b, p);
    return str_width(p);
}
static int utf8_len(unsigned char c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

/* ------------------------------------------------------------------ */
/* Directory entries                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *name;
    mode_t mode;
    off_t  size;
    int    is_dir;
    int    is_link;
    int    broken;
    int    marked;
} Entry;

static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}
static void free_entries(Entry *e, int n) {
    for (int i = 0; i < n; i++) free(e[i].name);
    free(e);
}
static void load_dir(const char *path, Entry **out, int *outn,
                     int show_hidden, const char *filter) {
    *out = NULL; *outn = 0;
    DIR *d = opendir(path);
    if (!d) return;
    int cap = 64, n = 0;
    Entry *e = xmalloc(cap * sizeof *e);
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        if (!show_hidden && nm[0] == '.') continue;
        if (filter && filter[0] && !ci_contains(nm, filter)) continue;
        if (n >= cap) { cap *= 2; e = xrealloc(e, cap * sizeof *e); }
        char full[PATH_MAX];
        join_path(full, sizeof full, path, nm);
        Entry *it = &e[n];
        it->name = xstrdup(nm);
        it->marked = 0; it->is_link = 0; it->broken = 0;
        struct stat lst, st;
        if (lstat(full, &lst) == 0) {
            it->mode = lst.st_mode;
            it->size = lst.st_size;
            if (S_ISLNK(lst.st_mode)) {
                it->is_link = 1;
                if (stat(full, &st) == 0) { it->is_dir = S_ISDIR(st.st_mode); it->size = st.st_size; }
                else { it->broken = 1; it->is_dir = 0; }
            } else {
                it->is_dir = S_ISDIR(lst.st_mode);
            }
        } else {
            it->mode = 0; it->size = 0; it->is_dir = 0; it->broken = 1;
        }
        n++;
    }
    closedir(d);
    qsort(e, n, sizeof *e, cmp_entry);
    *out = e; *outn = n;
}

/* ------------------------------------------------------------------ */
/* Colors + type indicators                                           */
/* ------------------------------------------------------------------ */
#define C_RESET "\x1b[0m"
#define C_DIR   "\x1b[1;34m"
#define C_LINK  "\x1b[1;36m"
#define C_EXEC  "\x1b[1;32m"
#define C_BROK  "\x1b[1;31m"
#define C_SEP   "\x1b[38;5;240m"

static int has_ext(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot + 1, exts[i]) == 0) return 1;
    return 0;
}
static const char *color_for(const Entry *e) {
    if (e->broken)  return C_BROK;
    if (e->is_dir)  return C_DIR;
    if (e->is_link) return C_LINK;
    if (e->mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return C_EXEC;
    static const char *img[] = {"jpg","jpeg","png","gif","bmp","svg","webp","ico","tiff",0};
    static const char *vid[] = {"mp4","mkv","webm","avi","mov","flv","wmv","m4v",0};
    static const char *aud[] = {"mp3","flac","wav","ogg","opus","m4a","aac",0};
    static const char *arc[] = {"zip","tar","gz","xz","bz2","zst","7z","rar","tgz","lz",0};
    static const char *doc[] = {"pdf","epub","djvu","doc","docx","odt","xls","xlsx","ppt","pptx",0};
    if (has_ext(e->name, img)) return "\x1b[35m";
    if (has_ext(e->name, vid)) return "\x1b[95m";
    if (has_ext(e->name, aud)) return "\x1b[36m";
    if (has_ext(e->name, arc)) return "\x1b[31m";
    if (has_ext(e->name, doc)) return "\x1b[33m";
    return "";
}
static char suffix_for(const Entry *e) {
    if (e->is_dir)  return '/';
    if (e->is_link) return '@';
    if (e->mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return '*';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Terminal raw mode + alt screen                                     */
/* ------------------------------------------------------------------ */
static void leave_tui(void) {
    if (!g_in_tui) return;
    if (g_have_orig) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    wr("\x1b[?1000l\x1b[?1006l\x1b[?7h\x1b[?25h\x1b[?1049l");
    g_in_tui = 0;
}
static void enter_tui(void) {
    if (g_have_orig) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_raw);
    wr("\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[2J");
    g_in_tui = 1;
}
static void on_signal(int sig) { leave_tui(); _exit(128 + sig); }
static void on_winch(int sig) { (void)sig; g_resized = 1; }

static int term_init(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_orig) < 0) return -1;
    g_have_orig = 1;
    g_raw = g_orig;
    g_raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    g_raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    g_raw.c_oflag &= ~(OPOST);
    g_raw.c_cflag |= CS8;
    g_raw.c_cc[VMIN] = 1;
    g_raw.c_cc[VTIME] = 0;

    atexit(leave_tui);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    struct sigaction sw = {0};
    sw.sa_handler = on_winch;        /* no SA_RESTART: interrupts read() */
    sigaction(SIGWINCH, &sw, NULL);
    signal(SIGPIPE, SIG_IGN);

    enter_tui();
    return 0;
}
static void get_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_rows = ws.ws_row; g_cols = ws.ws_col;
    }
    if (g_rows < 5)  g_rows = 5;
    if (g_cols < 20) g_cols = 20;
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                           */
/* ------------------------------------------------------------------ */
enum {
    K_RESIZE = -2, K_EOF = -1,
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_DEL,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10,
    K_MOUSE,
    K_SUP, K_SDOWN, K_SLEFT, K_SRIGHT,           /* Shift + arrows */
    K_ESC = 27, K_ENTER = 13, K_BS = 127, K_TAB = 9
};
static int g_mx, g_my, g_mbtn, g_mrelease;   /* last mouse event */
static int read_key(void) {
    unsigned char c;
    int r;
    while ((r = read(STDIN_FILENO, &c, 1)) < 0) {
        if (errno == EINTR) { if (g_resized) return K_RESIZE; continue; }
        return K_EOF;
    }
    if (r == 0) return K_EOF;
    if (c != 0x1b) return c;

    struct pollfd pf = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pf, 1, 50) <= 0) return K_ESC;
    unsigned char a;
    if (read(STDIN_FILENO, &a, 1) != 1) return K_ESC;

    if (a == 'O') {
        if (poll(&pf, 1, 50) <= 0) return K_ESC;
        unsigned char b;
        if (read(STDIN_FILENO, &b, 1) != 1) return K_ESC;
        switch (b) {
            case 'P': return K_F1; case 'Q': return K_F2;
            case 'R': return K_F3; case 'S': return K_F4;
            case 'H': return K_HOME; case 'F': return K_END;
            case 'A': return K_UP; case 'B': return K_DOWN;
            case 'C': return K_RIGHT; case 'D': return K_LEFT;
        }
        return K_ESC;
    }
    if (a != '[') return K_ESC;
    if (poll(&pf, 1, 50) <= 0) return K_ESC;
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) != 1) return K_ESC;

    if (b == '<') {                 /* SGR mouse: ESC [ < btn ; x ; y (M|m) */
        int v[3] = {0, 0, 0}, idx = 0;
        for (;;) {
            if (poll(&pf, 1, 50) <= 0) return K_ESC;
            unsigned char t;
            if (read(STDIN_FILENO, &t, 1) != 1) return K_ESC;
            if (t >= '0' && t <= '9') { if (idx < 3) v[idx] = v[idx] * 10 + (t - '0'); }
            else if (t == ';') { if (++idx > 2) return K_ESC; }
            else if (t == 'M' || t == 'm') { g_mrelease = (t == 'm'); break; }
            else return K_ESC;
        }
        g_mbtn = v[0]; g_mx = v[1]; g_my = v[2];
        return K_MOUSE;
    }

    if (b >= '0' && b <= '9') {
        int par[3] = {0, 0, 0}, np = 0;
        par[0] = b - '0';
        unsigned char fin;
        for (;;) {
            if (poll(&pf, 1, 50) <= 0) return K_ESC;
            unsigned char t;
            if (read(STDIN_FILENO, &t, 1) != 1) return K_ESC;
            if (t >= '0' && t <= '9') { if (par[np] < 9999) par[np] = par[np] * 10 + (t - '0'); }
            else if (t == ';') { if (np < 2) np++; }
            else { fin = t; break; }
        }
        int mod = (np >= 1) ? par[1] : 0;       /* 2 = Shift, 3 = Alt, 5 = Ctrl, ... */
        if (fin == '~') {
            switch (par[0]) {
                case 1: case 7: return K_HOME;
                case 4: case 8: return K_END;
                case 3:  return K_DEL;
                case 5:  return K_PGUP;
                case 6:  return K_PGDN;
                case 11: return K_F1;  case 12: return K_F2;
                case 13: return K_F3;  case 14: return K_F4;
                case 15: return K_F5;  case 17: return K_F6;
                case 18: return K_F7;  case 19: return K_F8;
                case 20: return K_F9;  case 21: return K_F10;
            }
            return K_ESC;
        }
        if (mod == 2) {                          /* Shift + arrow (ESC [ 1 ; 2 X) */
            switch (fin) {
                case 'A': return K_SUP;    case 'B': return K_SDOWN;
                case 'C': return K_SRIGHT; case 'D': return K_SLEFT;
            }
        }
        switch (fin) {
            case 'A': return K_UP;   case 'B': return K_DOWN;
            case 'C': return K_RIGHT; case 'D': return K_LEFT;
            case 'H': return K_HOME; case 'F': return K_END;
        }
        return K_ESC;
    }
    switch (b) {
        case 'A': return K_UP;   case 'B': return K_DOWN;
        case 'C': return K_RIGHT; case 'D': return K_LEFT;
        case 'H': return K_HOME; case 'F': return K_END;
    }
    return K_ESC;
}

/* ------------------------------------------------------------------ */
/* Panes                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char   cwd[PATH_MAX];
    Entry *e;
    int    n, cur, off;
    char   filter[256];
    int    search;            /* showing recursive search results */
    char   searchpat[128];
} Pane;

static Pane g_pane[2];
static int  g_active = 0;
static int  g_hidden = 0;
static char g_msg[256] = "";

static Pane *active(void)  { return &g_pane[g_active]; }
static Pane *other(void)   { return &g_pane[g_active ^ 1]; }
static int  body_half(void) { int b = (g_rows - 3) / 2; return b < 1 ? 1 : b; }
static void swap_panes(void) { Pane t = g_pane[0]; g_pane[0] = g_pane[1]; g_pane[1] = t; }
static Entry *cur_entry(Pane *p) { return (p->n > 0) ? &p->e[p->cur] : NULL; }

static void pane_clamp(Pane *p) {
    if (p->cur >= p->n) p->cur = p->n - 1;
    if (p->cur < 0) p->cur = 0;
}
static void pane_load(Pane *p) {
    if (p->e) { free_entries(p->e, p->n); p->e = NULL; p->n = 0; }
    p->search = 0;                       /* reloading a real dir exits search */
    load_dir(p->cwd, &p->e, &p->n, g_hidden, p->filter);
    pane_clamp(p);
}
static void pane_set(Pane *p, const char *path, const char *select) {
    char rp[PATH_MAX];
    if (realpath(path, rp)) snprintf(p->cwd, sizeof p->cwd, "%s", rp);
    else snprintf(p->cwd, sizeof p->cwd, "%s", path);
    p->cur = 0; p->off = 0; p->filter[0] = 0;
    pane_load(p);
    if (select)
        for (int i = 0; i < p->n; i++)
            if (strcmp(p->e[i].name, select) == 0) { p->cur = i; break; }
}
static void pane_enter(Pane *p) {
    Entry *it = cur_entry(p);
    if (!it || !it->is_dir) return;
    char np[PATH_MAX];
    join_path(np, sizeof np, p->cwd, it->name);
    pane_set(p, np, NULL);
}
static void pane_parent(Pane *p) {
    if (strcmp(p->cwd, "/") == 0) return;
    char base[PATH_MAX], parent[PATH_MAX];
    path_base(p->cwd, base, sizeof base);
    path_parent(p->cwd, parent, sizeof parent);
    pane_set(p, parent, base);
}
static int marked_count(Pane *p) {
    int c = 0;
    for (int i = 0; i < p->n; i++) if (p->e[i].marked) c++;
    return c;
}
static char **collect_targets(Pane *p, int *cnt) {
    int c = marked_count(p);
    if (c == 0) {
        Entry *it = cur_entry(p);
        if (!it) { *cnt = 0; return NULL; }
        char **a = xmalloc(sizeof *a);
        char path[PATH_MAX];
        join_path(path, sizeof path, p->cwd, it->name);
        a[0] = xstrdup(path);
        *cnt = 1;
        return a;
    }
    char **a = xmalloc(c * sizeof *a);
    int k = 0;
    for (int i = 0; i < p->n; i++)
        if (p->e[i].marked) {
            char path[PATH_MAX];
            join_path(path, sizeof path, p->cwd, p->e[i].name);
            a[k++] = xstrdup(path);
        }
    *cnt = c;
    return a;
}

/* ------------------------------------------------------------------ */
/* External processes                                                 */
/* ------------------------------------------------------------------ */
static int run_silent(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void run_interactive(const char *prog, const char *arg) {
    leave_tui();
    wr("\x1b[2J\x1b[H");
    pid_t pid = fork();
    if (pid == 0) { execlp(prog, prog, arg, (char *)NULL); _exit(127); }
    if (pid > 0) { int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }
    enter_tui();
    g_resized = 1;
}
static void run_detached(const char *prog, const char *arg) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t p2 = fork();
        if (p2 == 0) {
            setsid();
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
            execlp(prog, prog, arg, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}
static int is_text_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) return 1;            /* empty -> editable */
    int nonprint = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 0) return 0;
        if (buf[i] < 9 || (buf[i] > 13 && buf[i] < 32)) nonprint++;
    }
    return nonprint * 100 / n <= 5;
}
/* True if `prog` is an executable found via $PATH (or, if it contains a
   slash, executable at that exact path). */
static int in_path(const char *prog) {
    if (!prog || !prog[0]) return 0;
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path || !path[0]) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t len = sep ? (size_t)(sep - path) : strlen(path);
        char full[PATH_MAX];
        if (len == 0) { /* empty element means current directory */
            if (snprintf(full, sizeof full, "%s", prog) < (int)sizeof full
                && access(full, X_OK) == 0) return 1;
        } else if (len + 1 + strlen(prog) + 1 <= sizeof full) {
            snprintf(full, sizeof full, "%.*s/%s", (int)len, path, prog);
            if (access(full, X_OK) == 0) return 1;
        }
        if (!sep) break;
        path = sep + 1;
    }
    return 0;
}

/* Choose the editor: an explicit $LITEFM_EDITOR always wins, otherwise prefer
   litefe if it is installed, then fall back to $EDITOR, nano, and finally vi. */
static const char *editor(void) {
    const char *e = getenv("LITEFM_EDITOR");
    if (e && e[0]) return e;
    if (in_path("litefe")) return "litefe";
    e = getenv("EDITOR");
    if (e && e[0]) return e;
    if (in_path("nano")) return "nano";
    return "vi";
}

/* Draw an empty centered, opaque box over the current frame. Writes the box
   into o and returns its top-left screen coords (1-based) in *bx,*by. */
static void overlay_frame(Buf *o, int bw, int bh, int *bx, int *by) {
    if (bw > g_cols) bw = g_cols;
    if (bh > g_rows) bh = g_rows;
    int x = (g_cols - bw) / 2 + 1, y = (g_rows - bh) / 2 + 1;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    const char *tl = g_utf8 ? "╭" : "+", *tr = g_utf8 ? "╮" : "+";
    const char *bl = g_utf8 ? "╰" : "+", *brc = g_utf8 ? "╯" : "+";
    const char *hz = g_utf8 ? "─" : "-", *vt = g_utf8 ? "│" : "|";
    buf_printf(o, "\x1b[%d;%dH\x1b[0m%s", y, x, tl);
    for (int i = 0; i < bw - 2; i++) buf_s(o, hz);
    buf_s(o, tr);
    for (int r = 1; r < bh - 1; r++) {
        buf_printf(o, "\x1b[%d;%dH%s", y + r, x, vt);
        for (int i = 0; i < bw - 2; i++) buf_s(o, " ");
        buf_s(o, vt);
    }
    buf_printf(o, "\x1b[%d;%dH%s", y + bh - 1, x, bl);
    for (int i = 0; i < bw - 2; i++) buf_s(o, hz);
    buf_s(o, brc);
    *bx = x; *by = y;
}

/* Shared button row for every overlay: OK (left, green) and Cancel (right,
   grey). focus: 0 = OK, 1 = Cancel, -1 = neither highlighted. Reports the
   button columns for mouse hit-testing. Widths: OK = 6, Cancel = 10. */
#define BTN_OK_W 6
#define BTN_CANCEL_W 10
static void draw_buttons(Buf *o, int bx, int bw, int btnrow, int focus,
                         int *okcol, int *cancelcol) {
    int oc = bx + 2;
    int cc = bx + bw - 2 - BTN_CANCEL_W;
    int fok = (focus == 0), fcn = (focus == 1);
    /* focus shown by color only (green = focused, grey = not); no brackets */
    buf_printf(o, "\x1b[%d;%dH%s  OK  \x1b[0m", btnrow, oc,
               fok ? "\x1b[1;30;42m" : "\x1b[30;47m");
    buf_printf(o, "\x1b[%d;%dH%s  Cancel  \x1b[0m", btnrow, cc,
               fcn ? "\x1b[1;30;42m" : "\x1b[30;47m");
    if (okcol) *okcol = oc;
    if (cancelcol) *cancelcol = cc;
}

/* Modal yes/no overlay with focusable [Yes]/[No] buttons.
   detail may be NULL. arrows/Tab move focus, Enter/Space activate, y/n are
   shortcuts, Esc = No, and the buttons are mouse-clickable. */
static int confirm_overlay(const char *title, const char *detail, int default_yes) {
    int bw = 52, bh = 8;
    int focus = default_yes ? 0 : 1;       /* 0 = OK (left), 1 = Cancel (right) */
    for (;;) {
        render();
        Buf o = {0};
        int bx, by;
        overlay_frame(&o, bw, bh, &bx, &by);
        int ix = bx + 2, okcol, cancelcol;
        int btnrow = by + 5;
        buf_printf(&o, "\x1b[%d;%dH\x1b[1;36m%s\x1b[0m", by + 1, ix, title);
        if (detail && detail[0]) {
            Buf d = {0};
            buf_add_trunc(&d, detail, bw - 5);
            buf_printf(&o, "\x1b[%d;%dH\x1b[2m%s\x1b[0m", by + 3, ix, d.p ? d.p : "");
            free(d.p);
        }
        draw_buttons(&o, bx, bw, btnrow, focus, &okcol, &cancelcol);
        ssize_t w = write(STDOUT_FILENO, o.p, o.len); (void)w;
        free(o.p);
        int k = read_key();
        if (k == K_RESIZE) { get_size(); continue; }
        if (k == 'y' || k == 'Y' || k == 'j' || k == 'J') { g_resized = 1; return 1; }
        if (k == 'n' || k == 'N' || k == K_ESC || k == K_EOF) { g_resized = 1; return 0; }
        if (k == K_ENTER || k == '\n' || k == ' ') { g_resized = 1; return focus == 0; }
        if (k == 'h' || k == K_LEFT) focus = 0;
        else if (k == 'l' || k == K_RIGHT) focus = 1;
        else if (k == K_TAB) focus ^= 1;
        else if (k == K_MOUSE && !g_mrelease && (g_mbtn & 0x3) == 0 && g_my == btnrow) {
            if (g_mx >= okcol && g_mx < okcol + BTN_OK_W) { g_resized = 1; return 1; }
            if (g_mx >= cancelcol && g_mx < cancelcol + BTN_CANCEL_W) { g_resized = 1; return 0; }
        }
    }
}

/* Modal text-input overlay with an OK/Cancel button row. Returns 1 (OK) with
   the edited text in buf, 0 (Cancel). The window version of prompt(). */
static int prompt_overlay(const char *title, char *buf, size_t bufsz, const char *initial) {
    snprintf(buf, bufsz, "%s", initial ? initial : "");
    size_t len = strlen(buf);
    size_t cpos = len;                 /* byte cursor position in buf */
    int focus = -1;                    /* -1 = text field, 0 = OK, 1 = Cancel */
    int bw = 56, bh = 8;
    for (;;) {
        render();
        Buf o = {0};
        int bx, by, okcol, cancelcol;
        overlay_frame(&o, bw, bh, &bx, &by);
        int ix = bx + 2, fieldw = bw - 4, btnrow = by + 5;
        buf_printf(&o, "\x1b[%d;%dH\x1b[1;36m%s\x1b[0m", by + 1, ix, title);
        buf_printf(&o, "\x1b[%d;%dH", by + 3, ix);
        if (focus == -1) {                          /* show text with a block cursor at cpos */
            char before[1024];
            size_t bl = cpos < sizeof before ? cpos : sizeof before - 1;
            memcpy(before, buf, bl); before[bl] = 0;
            int wbefore = buf_add_tail(&o, before, fieldw - 1);
            const char *after = buf + cpos;
            int cclen = after[0] ? utf8_len((unsigned char)after[0]) : 0;
            buf_s(&o, "\x1b[7m");
            if (cclen) buf_add(&o, after, cclen); else buf_s(&o, " ");
            buf_s(&o, "\x1b[0m");
            int rem = fieldw - wbefore - 1;
            if (rem > 0 && cclen) buf_add_trunc(&o, after + cclen, rem);
        } else {
            buf_add_tail(&o, buf, fieldw);
        }
        draw_buttons(&o, bx, bw, btnrow, focus == -1 ? 0 : focus, &okcol, &cancelcol);
        ssize_t w = write(STDOUT_FILENO, o.p, o.len); (void)w;
        free(o.p);

        int k = read_key();
        if (k == K_RESIZE) { get_size(); continue; }
        if (k == K_ESC || k == K_EOF) { g_resized = 1; return 0; }

        if (focus != -1) {                           /* button focus */
            if (k == K_ENTER || k == '\n' || k == ' ') { g_resized = 1; return focus == 0; }
            else if (k == K_LEFT)  focus = 0;
            else if (k == K_RIGHT) focus = 1;
            else if (k == K_TAB)   focus = (focus == 0) ? 1 : -1;
            else if (k == K_UP)    focus = -1;
            else if (k == K_MOUSE && !g_mrelease && (g_mbtn & 0x3) == 0 && g_my == btnrow) {
                if (g_mx >= okcol && g_mx < okcol + BTN_OK_W) { g_resized = 1; return 1; }
                if (g_mx >= cancelcol && g_mx < cancelcol + BTN_CANCEL_W) { g_resized = 1; return 0; }
            } else if (k >= 32 && k < 256 && len + 1 < bufsz) {   /* typing jumps back to field */
                focus = -1;
                memmove(buf + cpos + 1, buf + cpos, len - cpos + 1);
                buf[cpos] = (char)k; len++; cpos++;
            }
            continue;
        }

        /* editing the text field */
        if (k == K_ENTER || k == '\n') { g_resized = 1; return 1; }
        else if (k == K_DOWN || k == K_TAB) focus = 0;       /* go to the buttons */
        else if (k == K_LEFT)  { if (cpos > 0) { do { cpos--; } while (cpos > 0 && ((unsigned char)buf[cpos] & 0xC0) == 0x80); } }
        else if (k == K_RIGHT) { if (cpos < len) { cpos += utf8_len((unsigned char)buf[cpos]); if (cpos > len) cpos = len; } }
        else if (k == K_HOME)  cpos = 0;
        else if (k == K_END)   cpos = len;
        else if (k == K_BS || k == 8) {
            if (cpos > 0) {
                size_t s0 = cpos;
                do { s0--; } while (s0 > 0 && ((unsigned char)buf[s0] & 0xC0) == 0x80);
                memmove(buf + s0, buf + cpos, len - cpos + 1);
                len -= (cpos - s0); cpos = s0;
            }
        }
        else if (k == K_DEL) {
            if (cpos < len) {
                size_t nl = utf8_len((unsigned char)buf[cpos]);
                if (cpos + nl > len) nl = len - cpos;
                memmove(buf + cpos, buf + cpos + nl, len - cpos - nl + 1);
                len -= nl;
            }
        }
        else if (k == K_MOUSE && !g_mrelease && (g_mbtn & 0x3) == 0 && g_my == btnrow) {
            if (g_mx >= okcol && g_mx < okcol + BTN_OK_W) { g_resized = 1; return 1; }
            if (g_mx >= cancelcol && g_mx < cancelcol + BTN_CANCEL_W) { g_resized = 1; return 0; }
        }
        else if (k >= 32 && k < 256 && len + 1 < bufsz) {
            memmove(buf + cpos + 1, buf + cpos, len - cpos + 1);
            buf[cpos] = (char)k; len++; cpos++;
        }
    }
}

/* Modal two-choice overlay (e.g. Folder / File). Returns 0 = first choice,
   1 = second, -1 = cancel. Focused choice is green; arrows/Tab move, Enter
   picks, Esc cancels, both choices mouse-clickable. */
static int choice_overlay(const char *title, const char *la, const char *lb) {
    char A[40], B[40];
    snprintf(A, sizeof A, "  %s  ", la);
    snprintf(B, sizeof B, "  %s  ", lb);
    int aw = (int)strlen(A), bw2 = (int)strlen(B);
    int bw = 52, bh = 7, focus = 0;
    for (;;) {
        render();
        Buf o = {0};
        int bx, by;
        overlay_frame(&o, bw, bh, &bx, &by);
        int ix = bx + 2, acol = ix, bcol = bx + bw - 2 - bw2, btnrow = by + 4;
        buf_printf(&o, "\x1b[%d;%dH\x1b[1;36m%s\x1b[0m", by + 1, ix, title);
        buf_printf(&o, "\x1b[%d;%dH%s%s\x1b[0m", btnrow, acol, focus == 0 ? "\x1b[1;30;42m" : "\x1b[30;47m", A);
        buf_printf(&o, "\x1b[%d;%dH%s%s\x1b[0m", btnrow, bcol, focus == 1 ? "\x1b[1;30;42m" : "\x1b[30;47m", B);
        ssize_t w = write(STDOUT_FILENO, o.p, o.len); (void)w;
        free(o.p);
        int k = read_key();
        if (k == K_RESIZE) { get_size(); continue; }
        if (k == K_ESC || k == K_EOF) { g_resized = 1; return -1; }
        if (k == K_ENTER || k == '\n' || k == ' ') { g_resized = 1; return focus; }
        else if (k == K_LEFT  || k == 'h') focus = 0;
        else if (k == K_RIGHT || k == 'l') focus = 1;
        else if (k == K_TAB) focus ^= 1;
        else if (k == K_MOUSE && !g_mrelease && (g_mbtn & 0x3) == 0 && g_my == btnrow) {
            if (g_mx >= acol && g_mx < acol + aw)  { g_resized = 1; return 0; }
            if (g_mx >= bcol && g_mx < bcol + bw2) { g_resized = 1; return 1; }
        }
    }
}

static void open_entry(Pane *p) {
    Entry *it = cur_entry(p);
    if (!it) return;
    if (p->search) {                      /* a search hit: jump to its location */
        char abs[PATH_MAX];
        join_path(abs, sizeof abs, p->cwd, it->name);
        if (it->is_dir) {
            pane_set(p, abs, NULL);
        } else {
            char par[PATH_MAX], base[PATH_MAX];
            path_parent(abs, par, sizeof par);
            path_base(abs, base, sizeof base);
            pane_set(p, par, base);
        }
        return;
    }
    if (it->is_dir) { pane_enter(p); return; }
    char full[PATH_MAX];
    join_path(full, sizeof full, p->cwd, it->name);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
        (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        snprintf(g_msg, sizeof g_msg, "executable - not opened (e to edit)");
        return;
    }
    if (is_text_file(full)) {
        run_interactive(editor(), full);
    } else {
        run_detached("xdg-open", full);
        snprintf(g_msg, sizeof g_msg, "opened: %.200s", it->name);
    }
}
static void edit_entry(Pane *p) {
    Entry *it = cur_entry(p);
    if (!it || it->is_dir) return;
    char full[PATH_MAX];
    join_path(full, sizeof full, p->cwd, it->name);
    run_interactive(editor(), full);
}

/* ------------------------------------------------------------------ */
/* File operations                                                    */
/* ------------------------------------------------------------------ */
static void clear_marks(Pane *p) { for (int i = 0; i < p->n; i++) p->e[i].marked = 0; }

static void op_transfer(int move) {
    Pane *s = active(), *d = other();
    if (s->n == 0) return;
    if (strcmp(s->cwd, d->cwd) == 0) { snprintf(g_msg, sizeof g_msg, "Source and target are the same folder"); return; }
    int c; char **t = collect_targets(s, &c);
    if (c == 0) { free(t); return; }
    char dst[64]; abbrev_home(d->cwd, dst, sizeof dst);
    char q[128], detail[128];
    snprintf(q, sizeof q, "%s %d item%s?", move ? "Move" : "Copy", c, c == 1 ? "" : "s");
    snprintf(detail, sizeof detail, "to %.110s", dst);
    if (confirm_overlay(q, detail, 1)) {
        char **argv = xmalloc((c + 5) * sizeof *argv);
        int k = 0;
        if (move) { argv[k++] = "mv"; argv[k++] = "--"; }
        else { argv[k++] = "cp"; argv[k++] = "-r"; argv[k++] = "--"; }
        for (int i = 0; i < c; i++) argv[k++] = t[i];
        argv[k++] = d->cwd;
        argv[k] = NULL;
        int rc = run_silent(argv);
        free(argv);
        if (rc == 0) snprintf(g_msg, sizeof g_msg, "%s %d item%s -> %.60s",
                              move ? "Moved" : "Copied", c, c == 1 ? "" : "s", dst);
        else snprintf(g_msg, sizeof g_msg, "%s failed", move ? "Move" : "Copy");
        clear_marks(s);
        pane_load(d);
        if (move) pane_load(s);
    }
    for (int i = 0; i < c; i++) free(t[i]);
    free(t);
}
static void op_delete(void) {
    Pane *p = active();
    if (p->n == 0) return;
    int c; char **t = collect_targets(p, &c);
    if (c == 0) { free(t); return; }
    char q[128], detail[128];
    snprintf(q, sizeof q, "Delete %d item%s?", c, c == 1 ? "" : "s");
    if (c == 1) path_base(t[0], detail, sizeof detail);
    else snprintf(detail, sizeof detail, "(%d items)", c);
    /* default focus on Cancel for a destructive action */
    if (confirm_overlay(q, detail, 0)) {
        char **argv = xmalloc((c + 4) * sizeof *argv);
        int k = 0;
        argv[k++] = "rm"; argv[k++] = "-rf"; argv[k++] = "--";
        for (int i = 0; i < c; i++) argv[k++] = t[i];
        argv[k] = NULL;
        run_silent(argv);
        free(argv);
        snprintf(g_msg, sizeof g_msg, "Deleted %d item%s", c, c == 1 ? "" : "s");
        pane_load(p);
    }
    for (int i = 0; i < c; i++) free(t[i]);
    free(t);
}
static void reselect(Pane *p, const char *name) {
    pane_load(p);
    for (int i = 0; i < p->n; i++) if (strcmp(p->e[i].name, name) == 0) { p->cur = i; break; }
}
static void op_rename(void) {
    Pane *p = active();
    Entry *it = cur_entry(p);
    if (!it) return;
    char name[256];
    if (!prompt_overlay("Rename", name, sizeof name, it->name)) return;
    if (!name[0] || strcmp(name, it->name) == 0) return;
    if (strchr(name, '/')) { snprintf(g_msg, sizeof g_msg, "Name may not contain '/'"); return; }
    char from[PATH_MAX], to[PATH_MAX];
    join_path(from, sizeof from, p->cwd, it->name);
    join_path(to, sizeof to, p->cwd, name);
    if (rename(from, to) == 0) snprintf(g_msg, sizeof g_msg, "renamed");
    else snprintf(g_msg, sizeof g_msg, "Rename failed: %s", strerror(errno));
    reselect(p, name);
}
/* New... : ask Folder or File, then the name, then create it. */
static void op_new(void) {
    Pane *p = active();
    int kind = choice_overlay("Create new", "Folder", "File");
    if (kind < 0) return;                         /* cancelled */
    char name[256];
    if (!prompt_overlay(kind == 0 ? "New folder" : "New file", name, sizeof name, "")) return;
    if (!name[0] || strchr(name, '/')) return;
    char path[PATH_MAX];
    join_path(path, sizeof path, p->cwd, name);
    if (kind == 0) {
        if (mkdir(path, 0755) == 0) snprintf(g_msg, sizeof g_msg, "Created folder: %.180s", name);
        else snprintf(g_msg, sizeof g_msg, "mkdir failed: %s", strerror(errno));
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { close(fd); snprintf(g_msg, sizeof g_msg, "Created file: %.180s", name); }
        else snprintf(g_msg, sizeof g_msg, "Create failed: %s", strerror(errno));
    }
    reselect(p, name);
}
static void op_newfile(void) {
    Pane *p = active();
    char name[256];
    if (!prompt_overlay("New file", name, sizeof name, "")) return;
    if (!name[0] || strchr(name, '/')) return;
    char path[PATH_MAX];
    join_path(path, sizeof path, p->cwd, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) { close(fd); snprintf(g_msg, sizeof g_msg, "Created file: %.180s", name); }
    else snprintf(g_msg, sizeof g_msg, "Create failed: %s", strerror(errno));
    reselect(p, name);
}
static void op_filter(void) {
    Pane *p = active();
    char f[256];
    if (!prompt_overlay("Filter (substring)", f, sizeof f, p->filter)) return;
    snprintf(p->filter, sizeof p->filter, "%s", f);
    p->cur = 0; p->off = 0;
    pane_load(p);
    if (f[0]) snprintf(g_msg, sizeof g_msg, "Filter: %.180s  (/ to clear)", f);
}

/* Interactive chmod overlay: a 3x3 rwx grid you toggle with the cursor.
   Applies the chosen mode to all targets (marked, else current). */
static void op_chmod(void) {
    Pane *p = active();
    Entry *it = cur_entry(p);
    if (!it) return;
    int c; char **t = collect_targets(p, &c);
    if (c == 0) { free(t); return; }

    static const mode_t BITS[3][3] = {
        {0400, 0200, 0100}, {0040, 0020, 0010}, {0004, 0002, 0001}
    };
    static const char *labels[3] = {"User", "Group", "Other"};
    mode_t m = it->mode & 07777;       /* keep special bits, toggle rwx */
    int cr = 0, cc = 0, btn = 0, applied = 0;   /* cr 0..2 = grid rows, 3 = buttons */
    int bw = 40, bh = 12;

    for (;;) {
        render();
        Buf o = {0};
        int bx, by;
        overlay_frame(&o, bw, bh, &bx, &by);
        int ix = bx + 2, okcol, cancelcol;
        int btnrow = by + 10;
        int fcancel = (cr == 3 && btn == 1);
        buf_printf(&o, "\x1b[%d;%dH\x1b[1;36mEdit permissions\x1b[0m", by + 1, ix);
        Buf nm = {0}; buf_add_trunc(&nm, c == 1 ? it->name : "(multiple)", bw - 5);
        buf_printf(&o, "\x1b[%d;%dH\x1b[2m%s\x1b[0m", by + 2, ix, nm.p ? nm.p : ""); free(nm.p);
        char sym[10];
        for (int i = 0; i < 9; i++) sym[i] = (m & (0400 >> i)) ? "rwxrwxrwx"[i] : '-';
        sym[9] = 0;
        buf_printf(&o, "\x1b[%d;%dHMode: %04o   %s", by + 3, ix, (unsigned)m, sym);
        buf_printf(&o, "\x1b[%d;%dH           r   w   x", by + 5, ix);
        for (int gr = 0; gr < 3; gr++) {
            buf_printf(&o, "\x1b[%d;%dH%-9s", by + 6 + gr, ix, labels[gr]);
            for (int col = 0; col < 3; col++) {
                int on = (m & BITS[gr][col]) != 0;
                buf_printf(&o, "\x1b[%d;%dH[%c]", by + 6 + gr, ix + 10 + col * 4, on ? 'x' : ' ');
            }
        }
        if (cr < 3) {                            /* highlight active grid cell */
            int on = (m & BITS[cr][cc]) != 0;
            buf_printf(&o, "\x1b[%d;%dH\x1b[7m[%c]\x1b[0m", by + 6 + cr, ix + 10 + cc * 4, on ? 'x' : ' ');
        }
        /* shared OK/Cancel buttons; focused only when the cursor is on them */
        draw_buttons(&o, bx, bw, btnrow, cr == 3 ? btn : -1, &okcol, &cancelcol);
        ssize_t w = write(STDOUT_FILENO, o.p, o.len); (void)w;
        free(o.p);

        int k = read_key();
        if (k == K_RESIZE) { get_size(); continue; }
        if (k == K_ESC || k == K_EOF) break;
        if (k == K_ENTER || k == '\n') { if (!fcancel) applied = 1; break; }   /* Enter = OK, unless Cancel focused */
        else if (k == K_MOUSE && !g_mrelease && (g_mbtn & 0x3) == 0 && g_my == btnrow) {
            if (g_mx >= okcol && g_mx < okcol + BTN_OK_W) { applied = 1; break; }
            if (g_mx >= cancelcol && g_mx < cancelcol + BTN_CANCEL_W) break;
        }
        else if (k == 'k' || k == K_UP)    { if (cr > 0) cr--; }
        else if (k == 'j' || k == K_DOWN)  { if (cr < 3) cr++; }
        else if (k == 'h' || k == K_LEFT)  { if (cr < 3) cc = (cc + 2) % 3; else btn = 0; }
        else if (k == 'l' || k == K_RIGHT) { if (cr < 3) cc = (cc + 1) % 3; else btn = 1; }
        else if (k == K_TAB)               { if (cr < 3) { cr = 3; btn = 0; } else if (btn == 0) btn = 1; else cr = 0; }
        else if (k == ' ' || k == 'x') {
            if (cr < 3) m ^= BITS[cr][cc];
            else { if (btn == 0) applied = 1; break; }       /* Space on a button activates it */
        }
        else if (k >= '0' && k <= '7' && cr < 3) {
            int sh = (2 - cr) * 3;
            m = (m & ~((mode_t)7 << sh)) | ((mode_t)(k - '0') << sh);
        }
    }

    if (applied) {
        int ok = 0, fail = 0;
        for (int i = 0; i < c; i++) { if (chmod(t[i], (mode_t)(m & 07777)) == 0) ok++; else fail++; }
        snprintf(g_msg, sizeof g_msg, "chmod %04o: %d ok%s", (unsigned)(m & 07777), ok, fail ? ", errors" : "");
        clear_marks(p); pane_load(p);
    }
    for (int i = 0; i < c; i++) free(t[i]);
    free(t);
    g_resized = 1;
}
/* change owner[:group] of the marked entries (or the current one) */
static void op_chown(void) {
    Pane *p = active();
    if (!cur_entry(p)) return;
    int c; char **t = collect_targets(p, &c);
    if (c == 0) { free(t); return; }
    char in[128];
    if (prompt_overlay("Owner (user[:group])", in, sizeof in, "") && in[0]) {
        uid_t uid = (uid_t)-1; gid_t gid = (gid_t)-1;
        int bad = 0;
        char user[128] = "", group[128] = "";
        char *colon = strchr(in, ':');
        if (colon) { *colon = 0; snprintf(user, sizeof user, "%s", in); snprintf(group, sizeof group, "%s", colon + 1); }
        else snprintf(user, sizeof user, "%s", in);
        if (user[0]) {
            struct passwd *pw = getpwnam(user);
            if (pw) uid = pw->pw_uid;
            else { char *e; long v = strtol(user, &e, 10); if (*e == 0) uid = (uid_t)v; else bad = 1; }
        }
        if (!bad && group[0]) {
            struct group *gr = getgrnam(group);
            if (gr) gid = gr->gr_gid;
            else { char *e; long v = strtol(group, &e, 10); if (*e == 0) gid = (gid_t)v; else bad = 1; }
        }
        if (bad) {
            snprintf(g_msg, sizeof g_msg, "unknown user/group");
        } else {
            int ok = 0, fail = 0;
            for (int i = 0; i < c; i++) { if (chown(t[i], uid, gid) == 0) ok++; else fail++; }
            snprintf(g_msg, sizeof g_msg, "chown: %d ok%s", ok, fail ? " (needs root?)" : "");
            clear_marks(p); pane_load(p);
        }
    }
    for (int i = 0; i < c; i++) free(t[i]);
    free(t);
}

/* recursive find: collect entries whose name matches pat into arr (names are
   paths relative to base). Bounded in depth and count to stay snappy. */
static void find_walk(const char *base, const char *rel, const char *pat,
                      Entry **arr, int *n, int *cap, int depth) {
    if (depth > 64 || *n >= 5000) return;
    char dir[PATH_MAX];
    if (rel[0]) join_path(dir, sizeof dir, base, rel);
    else snprintf(dir, sizeof dir, "%s", base);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && *n < 5000) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        if (!g_hidden && nm[0] == '.') continue;
        char childrel[PATH_MAX];
        if (rel[0]) snprintf(childrel, sizeof childrel, "%s/%s", rel, nm);
        else snprintf(childrel, sizeof childrel, "%s", nm);
        char full[PATH_MAX];
        join_path(full, sizeof full, base, childrel);
        struct stat lst, st;
        int isdir = 0, islink = 0, broken = 0; mode_t mode = 0; off_t size = 0;
        if (lstat(full, &lst) == 0) {
            mode = lst.st_mode; size = lst.st_size;
            if (S_ISLNK(lst.st_mode)) { islink = 1; if (stat(full, &st) == 0) isdir = S_ISDIR(st.st_mode); else broken = 1; }
            else isdir = S_ISDIR(lst.st_mode);
        }
        if (ci_contains(nm, pat)) {
            if (*n >= *cap) { *cap *= 2; *arr = xrealloc(*arr, *cap * sizeof **arr); }
            Entry *it = &(*arr)[*n];
            it->name = xstrdup(childrel);
            it->mode = mode; it->size = size; it->is_dir = isdir;
            it->is_link = islink; it->broken = broken; it->marked = 0;
            (*n)++;
        }
        if (isdir && !islink) find_walk(base, childrel, pat, arr, n, cap, depth + 1);
    }
    closedir(d);
}
static void op_find(void) {
    Pane *p = active();
    char pat[128];
    if (!prompt_overlay("Search (recursive)", pat, sizeof pat, p->search ? p->searchpat : "")) return;
    if (!pat[0]) return;
    int cap = 64, n = 0;
    Entry *arr = xmalloc(cap * sizeof *arr);
    find_walk(p->cwd, "", pat, &arr, &n, &cap, 0);
    qsort(arr, n, sizeof *arr, cmp_entry);
    if (p->e) free_entries(p->e, p->n);
    p->e = arr; p->n = n; p->cur = 0; p->off = 0;
    p->search = 1;
    snprintf(p->searchpat, sizeof p->searchpat, "%s", pat);
    snprintf(g_msg, sizeof g_msg, "%d match%s for \"%.60s\"  (Enter: go, h: back)", n, n == 1 ? "" : "es", pat);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */
static void render_row(Buf *L, Pane *p, int row, int width, int active_pane) {
    int idx = p->off + row;
    if (idx < 0 || idx >= p->n) { buf_pad(L, width); return; }
    Entry *it = &p->e[idx];
    int sel = (idx == p->cur);

    char szs[16] = "";
    if (!it->is_dir && !it->broken) human(it->size, szs, sizeof szs);
    int szw = (int)strlen(szs);
    char sfx = suffix_for(it);

    if (sel) buf_s(L, active_pane ? "\x1b[7m" : "\x1b[7;2m");
    else if (it->marked) buf_s(L, "\x1b[1;33m");
    else { const char *c = color_for(it); if (c[0]) buf_s(L, c); }

    char mk[2] = { it->marked ? '+' : ' ', 0 };
    buf_add(L, mk, 1);
    buf_add(L, " ", 1);
    int used = 2;

    int reserve = (szw > 0 && width >= szw + 6) ? szw + 1 : 0;
    int avail = width - used - reserve - (sfx ? 1 : 0);
    if (avail < 0) avail = 0;
    int nw = buf_add_trunc(L, it->name, avail);
    used += nw;
    if (sfx && width - used - reserve > 0) { char s[2] = {sfx, 0}; buf_add(L, s, 1); used++; }
    int gap = width - used - reserve;
    if (gap < 0) gap = 0;
    buf_pad(L, gap); used += gap;
    if (reserve) { buf_add(L, " ", 1); buf_s(L, szs); used += 1 + szw; }
    if (width - used > 0) buf_pad(L, width - used);
    buf_s(L, C_RESET);
}
static void render_header(Buf *L, Pane *p, int width, int active_pane) {
    char disp[PATH_MAX];
    abbrev_home(p->cwd, disp, sizeof disp);
    buf_s(L, active_pane ? "\x1b[30;46m" : "\x1b[90m");
    buf_add(L, " ", 1);
    int used = 1 + buf_add_trunc(L, disp, width - 1);
    buf_pad(L, width - used);
    buf_s(L, C_RESET);
}
static void sep(Buf *L) { buf_s(L, C_SEP); buf_s(L, g_utf8 ? "│" : "|"); buf_s(L, C_RESET); }

static void render(void) {
    int W = g_cols, H = g_rows;
    int lw = (W - 1) / 2;
    int rw = W - 1 - lw;
    int body = H - 3;                 /* header + status + keybar */
    if (body < 1) body = 1;

    for (int s = 0; s < 2; s++) {
        Pane *p = &g_pane[s];
        pane_clamp(p);
        if (p->cur < p->off) p->off = p->cur;
        if (p->cur >= p->off + body) p->off = p->cur - body + 1;
        if (p->off < 0) p->off = 0;
    }

    Buf out = {0};
    buf_s(&out, "\x1b[H");

    /* header */
    render_header(&out, &g_pane[0], lw, g_active == 0);
    sep(&out);
    render_header(&out, &g_pane[1], rw, g_active == 1);
    buf_s(&out, "\x1b[K\r\n");

    /* body */
    for (int r = 0; r < body; r++) {
        render_row(&out, &g_pane[0], r, lw, g_active == 0);
        sep(&out);
        render_row(&out, &g_pane[1], r, rw, g_active == 1);
        buf_s(&out, "\x1b[K\r\n");
    }

    /* status */
    {
        Pane *p = active();
        Buf S = {0};
        if (g_msg[0]) {
            buf_add(&S, " ", 1);
            buf_s(&S, g_msg);
        } else if (p->n > 0) {
            Entry *it = &p->e[p->cur];
            char perm[11] = "----------";
            mode_t m = it->mode;
            if (S_ISDIR(m)) perm[0] = 'd'; else if (S_ISLNK(m)) perm[0] = 'l';
            const char *rwx = "rwxrwxrwx";
            for (int i = 0; i < 9; i++) if (m & (1 << (8 - i))) perm[i + 1] = rwx[i];
            char szs[16]; human(it->size, szs, sizeof szs);
            char line[512];
            snprintf(line, sizeof line, " %d/%d  %s  %s  %s",
                     p->cur + 1, p->n, perm, szs, it->name);
            buf_s(&S, line);
        } else {
            buf_s(&S, " (empty)");
        }
        char right[160]; int ri = 0; right[0] = 0;
        int mc = marked_count(p);
        if (p->search)    ri += snprintf(right + ri, sizeof right - ri, " [search:%.20s]", p->searchpat);
        if (p->filter[0]) ri += snprintf(right + ri, sizeof right - ri, " /%s", p->filter);
        if (g_hidden)     ri += snprintf(right + ri, sizeof right - ri, " [.]");
        if (mc) {
            off_t tot = 0;
            for (int i = 0; i < p->n; i++) if (p->e[i].marked && !p->e[i].is_dir) tot += p->e[i].size;
            char ts[16]; human(tot, ts, sizeof ts);
            ri += snprintf(right + ri, sizeof right - ri, " *%d (%s)", mc, ts);
        }

        buf_s(&out, "\x1b[7m");
        int lwid = str_width(S.p ? S.p : "");
        int rwid = str_width(right);
        Buf F = {0};
        if (lwid + rwid + 1 <= W) {
            buf_s(&F, S.p ? S.p : "");
            buf_pad(&F, W - lwid - rwid);
            buf_s(&F, right);
        } else {
            buf_add_trunc(&F, S.p ? S.p : "", W);
        }
        buf_add(&out, F.p ? F.p : "", F.len);
        buf_s(&out, "\x1b[0m\x1b[K\r\n");
        free(S.p); free(F.p);
    }

    /* key bar: colored, separated entries; Quit pinned to the right */
    {
        static const struct { const char *key, *lab; } KB[] = {
            {"F2/r","Ren"},{"F4/e","Edit"},{"F5/c","Copy"},{"F6/m","Move"},
            {"F7/n","New"},{"F8/d","Del"},{"F9/p","Perm"}, {0,0}
        };
        const char *bar = "\x1b[1;36m";        /* key color (bold cyan) */
        const char *dim = "\x1b[38;5;250m";    /* label color (light grey) */
        const char *vt  = g_utf8 ? "│" : "|";
        int rightw = 12;                       /* "│ F10/q Quit" visible width */
        int limit = W - rightw - 1;
        Buf K = {0};
        int col = 1;
        buf_s(&K, " ");
        for (int i = 0; KB[i].key; i++) {
            int klen = (int)strlen(KB[i].key), llen = (int)strlen(KB[i].lab);
            int need = (i ? 3 : 0) + klen + 1 + llen;
            if (col + need > limit) break;
            if (i) { buf_s(&K, C_SEP); buf_s(&K, " "); buf_s(&K, vt); buf_s(&K, " "); buf_s(&K, C_RESET); col += 3; }
            buf_s(&K, bar); buf_s(&K, KB[i].key); buf_s(&K, C_RESET);
            buf_s(&K, " ");
            buf_s(&K, dim); buf_s(&K, KB[i].lab); buf_s(&K, C_RESET);
            col += klen + 1 + llen;
        }
        if (W - rightw - col > 0) buf_pad(&K, W - rightw - col), col = W - rightw;
        buf_s(&K, C_SEP); buf_s(&K, vt); buf_s(&K, " "); buf_s(&K, C_RESET);
        buf_s(&K, bar); buf_s(&K, "F10/q"); buf_s(&K, C_RESET);
        buf_s(&K, " "); buf_s(&K, dim); buf_s(&K, "Quit"); buf_s(&K, C_RESET);
        buf_add(&out, K.p ? K.p : "", K.len);
        buf_s(&out, "\x1b[K");
        free(K.p);
    }

    ssize_t w = write(STDOUT_FILENO, out.p, out.len); (void)w;
    free(out.p);
}

/* ------------------------------------------------------------------ */
/* Help                                                               */
/* ------------------------------------------------------------------ */
static void show_help(void) {
    static const char *h[] = {
        "  litefm  -  two-pane file manager",
        "",
        "  Tab                  switch active pane",
        "  j k  arrows          move cursor",
        "  l                    enter folder / open file",
        "  Enter  Right         enter folder (does not open files)",
        "  (executables are never opened; use e to edit)",
        "  h  Left  Bksp        go to parent folder",
        "  g G  Ctrl-d  PgUp/Dn top/bottom, half/full page",
        "",
        "  Space                mark (and move down)",
        "  Shift+Up/Down        toggle mark while moving (mark or unmark)",
        "  right-click          toggle mark with the mouse",
        "  Ctrl/Shift+click     toggle a whole range with the mouse",
        "  * + - u              invert / select / deselect by pattern / none",
        "  F5 / c               copy   active -> other pane (all marked)",
        "  F6 / m               move   active -> other pane",
        "  F8 / d / Del         delete (marked, else current)",
        "  F7 / n               new... (asks Folder or File, then the name)",
        "  a       F2 / r        new file / rename",
        "  F4 / e               edit file ($LITEFM_EDITOR)",
        "  F9 / p               edit permissions (chmod dialog: rwx grid)",
        "  o                    change owner (chown, user[:group])",
        "",
        "  s                    search recursively (Enter jumps to a hit)",
        "  /                    filter    .  hidden files",
        "  =                    set other pane to this folder",
        "  Tab                  switch pane   Ctrl-u  swap panes",
        "  ~  home   R  reload",
        "",
        "  Mouse: click = select, click selected = open, right-click",
        "         = toggle mark, Ctrl+click = range, wheel = scroll.",
        "",
        "  Both panes are independent folders. F5 copies from the",
        "  active (highlighted) pane into the other pane's folder.",
        "",
        "  -- press any key --",
        NULL
    };
    Buf b = {0};
    buf_s(&b, "\x1b[2J\x1b[H");
    for (int i = 0; h[i]; i++) {
        buf_s(&b, i == 0 ? "\x1b[1;36m" : "\x1b[0m");
        buf_s(&b, h[i]);
        buf_s(&b, "\x1b[0m\r\n");
    }
    ssize_t r = write(STDOUT_FILENO, b.p, b.len); (void)r;
    free(b.p);
    read_key();
}

/* ------------------------------------------------------------------ */
/* cd-on-quit                                                         */
/* ------------------------------------------------------------------ */
static void write_cwd_file(void) {
    const char *f = getenv("LITEFM_CWD_FILE");
    if (!f || !f[0]) return;
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    const char *cwd = active()->cwd;
    ssize_t r = write(fd, cwd, strlen(cwd)); (void)r;
    close(fd);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *cs = nl_langinfo(CODESET);
    g_utf8 = cs && (strstr(cs, "UTF") || strstr(cs, "utf"));

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("litefm - minimal two-pane file manager\n"
               "usage: litefm [left-dir] [right-dir]\n"
               "keys:  ? inside the program. q to quit.\n"
               "env:   LITEFM_EDITOR (else litefe, $EDITOR, nano, vi), LITEFM_CWD_FILE (cd-on-quit)\n");
        return 0;
    }

    char start[PATH_MAX];
    if (!getcwd(start, sizeof start)) snprintf(start, sizeof start, "/");
    pane_set(&g_pane[0], argc > 1 ? argv[1] : start, NULL);
    pane_set(&g_pane[1], argc > 2 ? argv[2] : start, NULL);

    if (term_init() < 0) {
        fprintf(stderr, "litefm: not a terminal (needs an interactive tty)\n");
        return 1;
    }

    int running = 1, pending_g = 0;
    while (running) {
        if (g_resized) { get_size(); g_resized = 0; }
        render();
        int k = read_key();
        if (k != 'g') pending_g = 0;
        if (k != K_RESIZE) g_msg[0] = 0;
        Pane *p = active();

        switch (k) {
            case K_RESIZE: g_resized = 1; break;
            case K_EOF: case 'q': case K_F10: running = 0; break;

            case K_TAB: g_active ^= 1; break;

            case K_MOUSE: {
                int lw = (g_cols - 1) / 2;
                if (g_mbtn & 64) {                    /* wheel (bit 6); bit 0 = down */
                    p->cur += (g_mbtn & 1) ? 3 : -3;
                    pane_clamp(p);
                    break;
                }
                if (g_mrelease) break;                /* ignore button releases */
                int base = g_mbtn & 0x3;              /* 0=left 1=middle 2=right */
                int range = (g_mbtn & 4) || (g_mbtn & 16);   /* Shift or Ctrl => range */
                int side = (g_mx <= lw) ? 0 : 1;
                int switched = (side != g_active);
                g_active = side;
                Pane *q = active();
                int row = g_my - 2;                   /* body starts at row 2 */
                if (row < 0 || row >= g_rows - 3) break;
                int idx = q->off + row;
                if (idx < 0 || idx >= q->n) break;
                if (base == 2) {                      /* right-click: toggle this mark */
                    q->e[idx].marked = !q->e[idx].marked;
                    q->cur = idx;
                } else if (base == 0) {               /* left-click */
                    if (range) {                      /* Shift/Ctrl+click: toggle a range */
                        int a = q->cur, b = idx;
                        if (a > b) { int tmp = a; a = b; b = tmp; }
                        int want = !q->e[idx].marked;
                        for (int i = a; i <= b; i++) q->e[i].marked = want;
                        q->cur = idx;
                    } else if (!switched && idx == q->cur) {
                        open_entry(q);                /* click already-selected = open */
                    } else {
                        q->cur = idx;
                    }
                }
                break;
            }

            case 'j': case K_DOWN:  if (p->cur < p->n - 1) p->cur++; break;
            case 'k': case K_UP:    if (p->cur > 0) p->cur--; break;
            case K_SDOWN:           /* Shift+Down: toggle mark, move down */
                if (p->n > 0) { p->e[p->cur].marked = !p->e[p->cur].marked; if (p->cur < p->n - 1) p->cur++; }
                break;
            case K_SUP:             /* Shift+Up: toggle mark, move up */
                if (p->n > 0) { p->e[p->cur].marked = !p->e[p->cur].marked; if (p->cur > 0) p->cur--; }
                break;
            case 'l': open_entry(p); break;     /* l is the only key that opens files */
            case K_ENTER: case '\n': case K_RIGHT:  /* Enter/Right: enter folders only */
                if (p->search) open_entry(p);
                else if (cur_entry(p) && cur_entry(p)->is_dir) pane_enter(p);
                break;
            case 'h': case K_LEFT: case K_BS: case 8:
                if (p->search) { p->search = 0; pane_load(p); } else pane_parent(p);
                break;

            case 'g':
                if (pending_g) { p->cur = 0; pending_g = 0; }
                else pending_g = 1;
                break;
            case 'G': p->cur = p->n - 1; break;
            case K_HOME: p->cur = 0; break;
            case K_END:  p->cur = p->n - 1; break;
            case 4:  p->cur += body_half(); pane_clamp(p); break;   /* Ctrl-d */
            case 21: swap_panes(); break;                           /* Ctrl-u: swap panes */
            case K_PGDN: p->cur += g_rows - 4; pane_clamp(p); break;
            case K_PGUP: p->cur -= g_rows - 4; pane_clamp(p); break;

            case ' ':
                if (p->n > 0) { p->e[p->cur].marked = !p->e[p->cur].marked; if (p->cur < p->n - 1) p->cur++; }
                break;

            case K_F5: case 'c': op_transfer(0); break;
            case K_F6: case 'm': op_transfer(1); break;
            case K_F8: case 'd': case K_DEL: op_delete(); break;
            case K_F7: case 'n': op_new(); break;
            case 'a': op_newfile(); break;
            case 'r': case K_F2: op_rename(); break;
            case K_F4: case 'e': edit_entry(p); break;

            case '*': for (int i = 0; i < p->n; i++) p->e[i].marked = !p->e[i].marked; break;
            case '+': {
                char pat[128];
                if (prompt_overlay("Select (pattern)", pat, sizeof pat, "") && pat[0])
                    for (int i = 0; i < p->n; i++) if (ci_contains(p->e[i].name, pat)) p->e[i].marked = 1;
                break;
            }
            case '-': {
                char pat[128];
                if (prompt_overlay("Deselect (pattern)", pat, sizeof pat, "") && pat[0])
                    for (int i = 0; i < p->n; i++) if (ci_contains(p->e[i].name, pat)) p->e[i].marked = 0;
                break;
            }
            case 'u': clear_marks(p); break;

            case 'p': case K_F9: op_chmod(); break;
            case 'o': op_chown(); break;
            case 's': op_find(); break;

            case '/': op_filter(); break;
            case '.': g_hidden = !g_hidden; pane_load(&g_pane[0]); pane_load(&g_pane[1]); break;
            case '=': pane_set(other(), p->cwd, NULL); break;
            case '~': { const char *home = getenv("HOME"); if (home && home[0]) pane_set(p, home, NULL); break; }
            case 'R': {
                char keep[256];
                snprintf(keep, sizeof keep, "%s", cur_entry(p) ? cur_entry(p)->name : "");
                reselect(p, keep);
                wr("\x1b[2J");
                break;
            }
            case K_F1: case '?': show_help(); break;
            default: break;
        }
    }

    leave_tui();
    write_cwd_file();
    free_entries(g_pane[0].e, g_pane[0].n);
    free_entries(g_pane[1].e, g_pane[1].n);
    return 0;
}
