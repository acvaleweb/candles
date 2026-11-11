#ifndef TERMINAL_H
#define TERMINAL_H

/* Raw terminal, keyboard input and an in-memory frame buffer.
 * Everything is drawn into the buffer with term_put / term_text / term_fill
 * and only the cells that changed are sent to the terminal by term_end_frame. */

typedef struct {
    unsigned char r, g, b;
} Color;

enum {
    KEY_ESC    = 27,
    KEY_RESIZE = 1000,
    KEY_UP     = 1001,
    KEY_DOWN   = 1002,
    KEY_EOF    = 1003,
    KEY_LEFT   = 1004,
    KEY_RIGHT  = 1005,
    KEY_MOUSE  = 1006,      /* a mouse press; read it with term_mouse() */
    KEY_WAKE   = 1007       /* the extra descriptor given to term_read_key_fd has input */
};

/* Enter raw mode and the alternate screen. Returns 0 on success, -1 if
 * stdin/stdout are not a terminal. The terminal is restored automatically
 * at exit and on SIGTERM / SIGHUP. */
int  term_init(void);
void term_restore(void);

/* Blocks until a key arrives. Returns a byte (0-255), or one of the KEY_*
 * codes above. */
int  term_read_key(void);

/* Like term_read_key, but also watches fd (if it is >= 0): when fd has input
 * and no key is waiting, returns KEY_WAKE without reading fd. The caller reads
 * it, and must stop passing it once it is closed or at its end, otherwise this
 * keeps waking up. */
int  term_read_key_fd(int fd);

/* The last mouse press (valid after term_read_key returned KEY_MOUSE).
 * button: 0 left, 1 middle, 2 right. row / col are 0-based. */
void term_mouse(int *button, int *row, int *col);

int  term_rows(void);
int  term_cols(void);

/* Frame: begin (clears the buffer to bg and picks up a new window size),
 * draw, end (flushes to the terminal). */
void term_begin_frame(Color bg);
void term_end_frame(void);

/* Drawing. Coordinates are 0-based (row, col); anything off screen is
 * ignored. glyph must be one UTF-8 character. */
void term_put(int row, int col, const char *glyph, Color fg, Color bg);
int  term_text(int row, int col, const char *s, Color fg, Color bg);   /* returns columns used */
void term_fill(int row, int col, int h, int w, Color bg);
int  term_text_width(const char *s);                                  /* columns of a UTF-8 string */

#endif
