#include "terminal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    char  ch[5];        /* one UTF-8 glyph, NUL padded */
    Color fg, bg;
} Cell;

static struct termios saved_termios;
static int raw_active = 0;
static volatile sig_atomic_t resized = 0;

static Cell *front = NULL;      /* what the terminal currently shows */
static Cell *back  = NULL;      /* what the next frame should show   */
static int n_rows = 0, n_cols = 0;
static int need_full = 1;
static int mouse_button = 0, mouse_row = 0, mouse_col = 0;

/* ---------- output buffer ---------- */

static char  *out = NULL;
static size_t out_len = 0, out_cap = 0;

static void out_put(const char *s, size_t n)
{
    if (out_len + n > out_cap) {
        size_t cap = out_cap ? out_cap : 65536;
        while (cap < out_len + n)
            cap *= 2;
        char *p = realloc(out, cap);
        if (!p)
            return;
        out = p;
        out_cap = cap;
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void out_fmt(const char *fmt, ...)
{
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        out_put(tmp, (size_t)n);
}

static void out_flush(void)
{
    size_t off = 0;
    while (off < out_len) {
        ssize_t w = write(STDOUT_FILENO, out + off, out_len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
    out_len = 0;
}

/* ---------- terminal mode ---------- */

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

static void on_fatal(int sig)
{
    term_restore();
    _exit(128 + sig);
}

void term_restore(void)
{
    if (!raw_active)
        return;
    raw_active = 0;
    const char *seq = "\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1000l\x1b[?1049l";
    ssize_t r = write(STDOUT_FILENO, seq, strlen(seq));
    (void)r;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
}

int term_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
        return -1;

    struct termios raw = saved_termios;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        return -1;
    raw_active = 1;
    atexit(term_restore);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_fatal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* alternate screen, hide cursor, mouse presses (SGR coordinates) */
    const char *init = "\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h";
    out_put(init, strlen(init));
    out_flush();
    return 0;
}

/* ---------- keyboard ---------- */

static int read_escape(void)
{
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    unsigned char c;

    if (poll(&p, 1, 50) <= 0 || read(STDIN_FILENO, &c, 1) != 1)
        return KEY_ESC;                         /* a lone Esc */
    if (c != '[' && c != 'O')
        return 0;                               /* Alt+key: ignore */

    char params[32];
    int np = 0;
    unsigned char final = 0;
    while (poll(&p, 1, 50) > 0 && read(STDIN_FILENO, &c, 1) == 1) {
        if (c >= 0x40 && c <= 0x7E) {
            final = c;
            break;
        }
        if (np < (int)sizeof params - 1)
            params[np++] = (char)c;
    }
    params[np] = '\0';

    if (params[0] == '<' && (final == 'M' || final == 'm')) {      /* SGR mouse */
        int b, x, y;
        if (final == 'M' && sscanf(params + 1, "%d;%d;%d", &b, &x, &y) == 3 &&
            (b & (32 | 64)) == 0 && (b & 3) != 3) {                /* a press, not motion / wheel */
            mouse_button = b & 3;
            mouse_col = x - 1;
            mouse_row = y - 1;
            return KEY_MOUSE;
        }
        return 0;
    }
    if (final == 'A')
        return KEY_UP;
    if (final == 'B')
        return KEY_DOWN;
    if (final == 'C')
        return KEY_RIGHT;
    if (final == 'D')
        return KEY_LEFT;
    return 0;                                   /* other sequences: ignore */
}

void term_mouse(int *button, int *row, int *col)
{
    *button = mouse_button;
    *row = mouse_row;
    *col = mouse_col;
}

int term_read_key(void)
{
    return term_read_key_fd(-1);
}

int term_read_key_fd(int fd)
{
    for (;;) {
        if (resized) {
            resized = 0;
            return KEY_RESIZE;
        }
        struct pollfd p[2] = { { STDIN_FILENO, POLLIN, 0 }, { fd, POLLIN, 0 } };
        int r = poll(p, fd >= 0 ? 2 : 1, 100);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return KEY_EOF;
        }
        if (r == 0)
            continue;
        if (p[0].revents == 0)                  /* no key, so it is the descriptor */
            return KEY_WAKE;

        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return KEY_EOF;
        }
        if (n == 0)
            return KEY_EOF;
        if (c != 27)
            return c;
        int key = read_escape();
        if (key)
            return key;
    }
}

/* ---------- frame buffer ---------- */

static void update_size(void)
{
    struct winsize ws;
    int r = 24, c = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        r = ws.ws_row;
        c = ws.ws_col;
    }
    if (back && r == n_rows && c == n_cols)
        return;

    free(front);
    free(back);
    n_rows = r;
    n_cols = c;
    size_t bytes = sizeof(Cell) * (size_t)r * (size_t)c;
    front = malloc(bytes);
    back = malloc(bytes);
    if (!front || !back) {
        term_restore();
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memset(front, 0xFF, bytes);     /* never equal to a real cell: forces a full repaint */
    memset(back, 0, bytes);
    need_full = 1;
}

int term_rows(void) { return n_rows; }
int term_cols(void) { return n_cols; }

void term_begin_frame(Color bg)
{
    update_size();
    Cell blank;
    memset(&blank, 0, sizeof blank);
    blank.ch[0] = ' ';
    blank.fg = bg;
    blank.bg = bg;
    for (int i = 0; i < n_rows * n_cols; i++)
        back[i] = blank;
}

void term_put(int row, int col, const char *glyph, Color fg, Color bg)
{
    if (row < 0 || row >= n_rows || col < 0 || col >= n_cols)
        return;
    Cell *c = &back[row * n_cols + col];
    size_t n = strlen(glyph);
    if (n > 4)
        n = 4;
    memset(c->ch, 0, sizeof c->ch);
    memcpy(c->ch, glyph, n);
    c->fg = fg;
    c->bg = bg;
}

/* Reads one character from s into g (NUL terminated) and returns the bytes
 * consumed. Control characters and invalid UTF-8 become '?'. */
static int next_glyph(const char *s, char g[5])
{
    unsigned char c = (unsigned char)s[0];
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;

    memset(g, 0, 5);
    if (len == 0 || c < 0x20 || c == 0x7F) {
        g[0] = '?';
        return 1;
    }
    for (int i = 1; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            g[0] = '?';
            return 1;
        }
    }
    memcpy(g, s, (size_t)len);
    return len;
}

int term_text(int row, int col, const char *s, Color fg, Color bg)
{
    int start = col;
    char g[5];
    while (*s) {
        s += next_glyph(s, g);
        term_put(row, col, g, fg, bg);
        col++;
    }
    return col - start;
}

int term_text_width(const char *s)
{
    int w = 0;
    char g[5];
    while (*s) {
        s += next_glyph(s, g);
        w++;
    }
    return w;
}

void term_fill(int row, int col, int h, int w, Color bg)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            term_put(row + r, col + c, " ", bg, bg);
}

static int same_color(Color a, Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

void term_end_frame(void)
{
    if (need_full) {
        out_put("\x1b[2J", 4);
        need_full = 0;
    }

    int cur_r = -1, cur_c = -1;
    int have_color = 0;
    Color fg = {0, 0, 0}, bg = {0, 0, 0};

    for (int r = 0; r < n_rows; r++) {
        for (int c = 0; c < n_cols; c++) {
            Cell *b = &back[r * n_cols + c];
            Cell *f = &front[r * n_cols + c];
            if (memcmp(b, f, sizeof(Cell)) == 0)
                continue;
            if (r != cur_r || c != cur_c)
                out_fmt("\x1b[%d;%dH", r + 1, c + 1);
            if (!have_color || !same_color(b->fg, fg))
                out_fmt("\x1b[38;2;%d;%d;%dm", b->fg.r, b->fg.g, b->fg.b);
            if (!have_color || !same_color(b->bg, bg))
                out_fmt("\x1b[48;2;%d;%d;%dm", b->bg.r, b->bg.g, b->bg.b);
            have_color = 1;
            fg = b->fg;
            bg = b->bg;
            out_put(b->ch, strlen(b->ch));
            *f = *b;
            cur_r = r;
            cur_c = c + 1;
        }
    }
    out_flush();
}
