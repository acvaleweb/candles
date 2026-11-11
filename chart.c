#include "chart.h"
#include "scale.h"
#include "terminal.h"

#include "indicators.h"
#include "swing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Geometry
 *
 * Everything sits on the character grid, so nothing can drift out of line:
 *   - a candle occupies a fixed slot of columns; its body is 1, 3, 5, 7 or 9
 *     columns wide and the wick is drawn in the centre column of the body;
 *   - vertically each text row is split in two halves, and every price edge
 *     (body top/bottom, wick top/bottom) snaps to a half row.
 * ------------------------------------------------------------------------ */

#define X_LEVELS        6
#define Y_ZOOM_IN_MAX   12
#define Y_ZOOM_OUT_MAX  6
#define DATE_W          5       /* "mm-dd" on the axis; years sit at its ends */

static const int SLOT[X_LEVELS] = { 1, 2, 4, 6, 8, 10 };    /* columns per candle, gap included */
static const int BODY[X_LEVELS] = { 1, 1, 3, 5, 7, 9 };     /* body width                       */

void view_init(View *v)
{
    memset(v, 0, sizeof *v);
    v->x_level = 1;
}

void view_reset_pan(View *v)
{
    v->x_off = 0;
    v->y_off = 0.0;
    v->cross_on = 0;
    v->cross_candle = 0;
    v->measure_phase = MEASURE_OFF;
}

void view_zoom_x(View *v, int dir)
{
    v->x_level += dir;
    if (v->x_level < 0) v->x_level = 0;
    if (v->x_level > X_LEVELS - 1) v->x_level = X_LEVELS - 1;
}

void view_zoom_y(View *v, int dir)
{
    v->y_level += dir;
    if (v->y_level < -Y_ZOOM_OUT_MAX) v->y_level = -Y_ZOOM_OUT_MAX;
    if (v->y_level > Y_ZOOM_IN_MAX) v->y_level = Y_ZOOM_IN_MAX;
}

static double y_factor(int level)
{
    double f = 1.0;
    for (int i = 0; i < level; i++) f *= 0.8;
    for (int i = 0; i > level; i--) f /= 0.8;
    return f;
}

/* ------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */

typedef struct {
    int margin;                 /* columns on each side of the box, for the price scale */
    int box_l, box_r;           /* columns of the box's left / right border             */
    int box_t, box_b;           /* rows of the main box's top / bottom border           */
    int rows_plot, cols_plot;   /* size inside the main box                             */

    int sub_on;                 /* volume / rsi box below the main one                  */
    int sub_t, sub_b;           /* its top / bottom border rows                         */
    int sub_rows;               /* rows inside it                                       */
    int bottom;                 /* lowest border row (the dates go under it)            */
    int capacity;               /* candles that fit across                              */

    int slot, body;             /* candle slot and body width                           */
    int first, vis;             /* first visible candle index, number visible           */
    int start;                  /* first used column inside the box (candles hug the right) */

    double pmin, pmax;          /* price range shown                                    */
    ScaleTicks ticks;           /* equally spaced text rows for the price labels        */
} Layout;

/* Prices stay compact without losing rounded precision: 999.99, 1000.0,
 * 10.00k, 100.0k, and so on.  M/B only prevent unusually large values from
 * growing beyond the same practical width. */
static void format_price(double value, char *buf, size_t n)
{
    double a = fabs(value);
    if (a >= 999950000.0) {
        double q = value / 1e9;
        snprintf(buf, n, fabs(q) < 100.0 ? "%.2fB" : fabs(q) < 1000.0 ? "%.1fB" : "%.0fB", q);
    } else if (a >= 999950.0) {
        double q = value / 1e6;
        snprintf(buf, n, fabs(q) < 100.0 ? "%.2fM" : fabs(q) < 1000.0 ? "%.1fM" : "%.0fM", q);
    } else if (a >= 10000.0 || round(a * 10.0) / 10.0 >= 10000.0) {
        double q = value / 1e3;
        snprintf(buf, n, fabs(q) < 100.0 ? "%.2fk" : fabs(q) < 1000.0 ? "%.1fk" : "%.0fk", q);
    } else if (a >= 1000.0 || round(a * 100.0) / 100.0 >= 1000.0) {
        snprintf(buf, n, "%.1f", value);
    } else if (a >= 1.0 || round(a * 10000.0) / 10000.0 >= 1.0) {
        snprintf(buf, n, "%.2f", value);
    } else {
        snprintf(buf, n, "%.4f", value);
    }
}

static void format_volume(double v, char *buf, size_t n);

static void compute_view(const Stock *s, View *v, Layout *L)
{
    L->slot = SLOT[v->x_level];
    L->body = BODY[v->x_level];

    int capacity = L->slot == 1 ? L->cols_plot : (L->cols_plot - 1) / L->slot;
    if (capacity < 1)
        capacity = 1;
    L->capacity = capacity;
    L->vis = s->count < capacity ? s->count : capacity;

    /* scrolled back by x_off candles, but never past the oldest one */
    int max_off = s->count - L->vis;
    if (v->x_off > max_off) v->x_off = max_off;
    if (v->x_off < 0)       v->x_off = 0;
    L->first = s->count - v->x_off - L->vis;
    int width = L->slot == 1 ? L->vis : L->vis * L->slot + 1;
    L->start = L->cols_plot - width;

    /* The scale is built from the file's part of each candle (ref_*), so the
     * moving live candle never changes it; only the keys do. If nothing in view
     * comes from the file, it falls back to what is shown. */
    double lo = NAN, hi = NAN, ref_close = NAN;
    for (int i = L->first; i < L->first + L->vis; i++) {
        const Candle *c = &s->candles[i];
        if (isnan(c->ref_low) || isnan(c->ref_high))
            continue;
        if (isnan(lo) || c->ref_low < lo)  lo = c->ref_low;
        if (isnan(hi) || c->ref_high > hi) hi = c->ref_high;
        ref_close = c->ref_close;
    }
    if (isnan(lo) || isnan(hi)) {
        lo = s->candles[L->first].low;
        hi = s->candles[L->first].high;
        for (int i = L->first + 1; i < L->first + L->vis; i++) {
            if (s->candles[i].low < lo)  lo = s->candles[i].low;
            if (s->candles[i].high > hi) hi = s->candles[i].high;
        }
        ref_close = s->candles[L->first + L->vis - 1].close;
    }

    double mid = (lo + hi) / 2.0;
    double range = hi - lo;
    if (range <= 0.0) {
        double a = hi < 0 ? -hi : hi;
        range = a > 0.0 ? a * 0.02 : 1.0;
    }
    range *= 1.1;                               /* 5% breathing room top and bottom */

    /* Zooming in slides the view towards the latest close so the latest
     * candle stays on screen. */
    double f = y_factor(v->y_level);
    double center = mid;
    if (f < 1.0)
        center = mid + (ref_close - mid) * (1.0 - f);
    double r = range * f;

    /* vertical scrolling, kept within a couple of screens of the data */
    double lim = 2.0 * range;
    if (v->y_off > lim)  v->y_off = lim;
    if (v->y_off < -lim) v->y_off = -lim;
    center += v->y_off;
    L->pmin = center - r / 2.0;
    L->pmax = center + r / 2.0;

    L->ticks=scale_ticks(L->rows_plot,r);

}

static int label_width(const Layout *L)
{
    int maxw = 0;
    char buf[64];
    for(int i=0;i<L->ticks.count;i++) {
        int row=L->ticks.first+i*L->ticks.gap;
        double value=scale_row_value(L->pmin,L->pmax,L->rows_plot,row,0);
        format_price(value,buf,sizeof buf);
        int w = (int)strlen(buf);
        if (w > maxw)
            maxw = w;
    }
    return maxw;
}

/* The visible raw maximum bounds a simple moving average too, so it is enough
 * to reserve the width of this one volume label during layout. */
static int volume_label_width(const Stock *s, const Layout *L)
{
    double max = 0.0;
    for (int i = L->first; i < L->first + L->vis; i++)
        if (s->candles[i].volume > max)
            max = s->candles[i].volume;
    char buf[32];
    format_volume(max, buf, sizeof buf);
    return (int)strlen(buf);
}

/* The width of the price labels decides the box width, which decides how many
 * candles fit, which decides the price range, which decides the labels...
 * so iterate a few times until it settles. */
static int compute_layout(const Stock *s, View *v, Layout *L)
{
    int rows = term_rows(), cols = term_cols();
    memset(L, 0, sizeof *L);
    L->box_t = 1;
    L->bottom = rows - 3;
    L->box_b = L->bottom;

    /* the volume / rsi box takes about a quarter of the height, if the main
     * chart keeps a sensible size */
    if (v->show_volume || v->show_rsi) {
        int avail = L->bottom - L->box_t + 1;
        int sp = avail / 4 - 2;
        if (sp < 3)
            sp = 3;
        if (L->bottom - sp - 4 >= 4) {
            L->sub_on = 1;
            L->sub_rows = sp;
            L->box_b = L->bottom - (sp + 2);
            L->sub_t = L->box_b + 1;
            L->sub_b = L->bottom;
        }
    }
    L->rows_plot = L->box_b - L->box_t - 1;

    int margin = 5;                            /* room for the year labels below */
    for (int it = 0; it < 4; it++) {
        L->margin = margin;
        L->box_l = margin;
        L->box_r = cols - 1 - margin;
        L->cols_plot = L->box_r - L->box_l - 1;
        if (L->cols_plot < 12)
            return 0;
        compute_view(s, v, L);

        int need = label_width(L) + 1;          /* label plus its adjacent box border */
        if (v->show_volume) {
            int volume_need = volume_label_width(s, L) + 1;
            if (volume_need > need)
                need = volume_need;
        }
        if (need < 5)
            need = 5;                           /* both endpoint years fit outside the box */
        if (need == margin)
            break;
        margin = need;
    }
    return 1;
}

static void empty_layout(Layout *L)
{
    int rows = term_rows(), cols = term_cols();
    memset(L, 0, sizeof *L);
    L->margin = 2;
    L->box_l = 2;
    L->box_r = cols - 3;
    L->box_t = 1;
    L->box_b = rows - 3;
    L->bottom = L->box_b;
    L->rows_plot = L->box_b - L->box_t - 1;
    L->cols_plot = L->box_r - L->box_l - 1;
}

/* ------------------------------------------------------------------------
 * Candle shapes
 * ------------------------------------------------------------------------ */

enum { S_EMPTY = 0, S_WICK = 1, S_BODY = 2 };

/* Glyph for a cell given the state of its upper and lower half: [upper][lower]. */
static const char *NARROW[3][3] = {     /* 1-column candle: thin wick, heavy body */
    { " ", "╷", "╻" },
    { "╵", "│", "╽" },
    { "╹", "╿", "┃" },
};
static const char *WIDE_CENTER[3][3] = {    /* centre column of a wide candle */
    { " ", "█", "▄" },
    { "█", "█", "█" },  /* every wide-stem part uses the body's solid block */
    { "▀", "█", "█" },
};
static const char *WIDE_SIDE[3][3] = {      /* other columns of a wide candle */
    { " ", " ", "▄" },
    { " ", " ", "▄" },
    { "▀", "▀", "█" },
};

/* Vertical extents in half rows, [top, bottom). */
typedef struct {
    int wick_t, wick_b;
    int body_t, body_b;
} Shape;

static int to_half(double p, double pmin, double pmax, int h2)
{
    double y = (pmax - p) / (pmax - pmin) * h2;
    if (y < 0) y = 0;
    if (y > h2) y = h2;
    return (int)(y + 0.5);
}

/* Returns 0 if the candle is entirely outside the price range shown. */
static int make_shape(const Candle *c, double pmin, double pmax, int h2, Shape *sh)
{
    if (c->high < pmin || c->low > pmax)
        return 0;

    double bhi = c->open > c->close ? c->open : c->close;
    double blo = c->open > c->close ? c->close : c->open;

    sh->wick_t = to_half(c->high, pmin, pmax, h2);
    sh->wick_b = to_half(c->low, pmin, pmax, h2);

    if (bhi < pmin || blo > pmax) {
        sh->body_t = sh->body_b = 0;            /* body off screen, only the wick shows */
        return 1;
    }
    sh->body_t = to_half(bhi, pmin, pmax, h2);
    sh->body_b = to_half(blo, pmin, pmax, h2);
    if (sh->body_b <= sh->body_t) {             /* never thinner than half a row */
        if (sh->body_t < h2)
            sh->body_b = sh->body_t + 1;
        else
            sh->body_t = sh->body_b - 1;
    }
    if (sh->wick_t > sh->body_t) sh->wick_t = sh->body_t;
    if (sh->wick_b < sh->body_b) sh->wick_b = sh->body_b;
    return 1;
}

static int state_at(const Shape *sh, int h)
{
    if (h >= sh->body_t && h < sh->body_b) return S_BODY;
    if (h >= sh->wick_t && h < sh->wick_b) return S_WICK;
    return S_EMPTY;
}

static void draw_candle(const Layout *L, const Shape *sh, int col, int width,
                        Color color, Color bg)
{
    for (int r = 0; r < L->rows_plot; r++) {
        int up = state_at(sh, 2 * r);
        int lo = state_at(sh, 2 * r + 1);
        if (up == S_EMPTY && lo == S_EMPTY)
            continue;
        for (int j = 0; j < width; j++) {
            const char *g;
            if (width == 1)
                g = NARROW[up][lo];
            else if (j == width / 2)
                g = WIDE_CENTER[up][lo];
            else
                g = WIDE_SIDE[up][lo];
            if (g[0] != ' ')
                term_put(L->box_t + 1 + r, col + j, g, color, bg);
        }
    }
}

/* Absolute column where the body of visible candle j starts. */
static int body_col(const Layout *L, int j)
{
    return L->box_l + 1 + L->start + j * L->slot + (L->slot == 1 ? 0 : 1);
}

/* ------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

static void draw_box(const Layout *L, const Settings *cfg, int top, int bottom)
{
    Color bd = cfg->border, bg = cfg->background;

    term_put(top, L->box_l, "┌", bd, bg);
    term_put(top, L->box_r, "┐", bd, bg);
    term_put(bottom, L->box_l, "└", bd, bg);
    term_put(bottom, L->box_r, "┘", bd, bg);
    for (int c = L->box_l + 1; c < L->box_r; c++) {
        term_put(top, c, "─", bd, bg);
        term_put(bottom, c, "─", bd, bg);
    }
    for (int r = top + 1; r < bottom; r++) {
        term_put(r, L->box_l, "│", bd, bg);
        term_put(r, L->box_r, "│", bd, bg);
    }
}

static void draw_scale(const Layout *L, const Settings *cfg)
{
    char buf[64];

    for(int i=0;i<L->ticks.count;i++) {
        int offset=L->ticks.first+i*L->ticks.gap;
        double p=scale_row_value(L->pmin,L->pmax,L->rows_plot,offset,0);
        int row=L->box_t+1+offset;

        format_price(p, buf, sizeof buf);
        int w = (int)strlen(buf);
        term_text(row, L->box_l - 1 - w, buf, cfg->muted, cfg->background);
        term_text(row, L->box_r + 2, buf, cfg->muted, cfg->background);
        term_put(row, L->box_l, "┤", cfg->border, cfg->background);
        term_put(row, L->box_r, "├", cfg->border, cfg->background);
    }
}

static void draw_candles(const Stock *s, const Layout *L, const Settings *cfg)
{
    int h2 = 2 * L->rows_plot;
    for (int j = 0; j < L->vis; j++) {
        const Candle *c = &s->candles[L->first + j];
        Shape sh;
        if (!make_shape(c, L->pmin, L->pmax, h2, &sh))
            continue;
        Color color = c->close >= c->open ? cfg->up : cfg->down;
        draw_candle(L, &sh, body_col(L, j), L->body, color, cfg->background);
    }
}

/* ------------------------------------------------------------------------
 * Overlays and the volume / rsi box
 * ------------------------------------------------------------------------ */

/* Column of the centre of visible candle j (j may be -1, one off the left). */
static int center_col(const Layout *L, int j)
{
    return body_col(L, j) + L->body / 2;
}

/* One marker per candle, at the candle's own mean volume. The five scan-line
 * glyphs retain fractional-row precision without drawing an interpolation to
 * either neighbour. */
static void draw_volume_mean(const Layout *L, const Settings *cfg, const double *mean,
                             double vmax)
{
    static const char *const SCAN[5] = { "⎺", "⎻", "─", "⎼", "⎽" };
    int rows = L->sub_rows, row0 = L->sub_t + 1;

    for (int j = 0; j < L->vis; j++) {
        double v = mean[L->first + j];
        if (isnan(v))
            continue;
        double y = (vmax - v) / vmax * rows;             /* rows from the top */
        int r = (int)floor(y);
        if (r < 0 || r >= rows)
            continue;
        int sub = (int)((y - r) * 5.0);
        if (sub < 0) sub = 0;
        if (sub > 4) sub = 4;
        term_put(row0 + r, center_col(L, j), SCAN[sub], cfg->volume_ma, cfg->background);
    }
}

/* Braille cells have two columns and four rows of dots. This gives bands four
 * vertical positions per terminal row (rather than the five scan-line glyphs)
 * and lets both bands share a cell instead of overwriting one another. */
static const unsigned char BRAILLE_LEFT[4]  = { 1, 2, 4, 64 };
static const unsigned char BRAILLE_RIGHT[4] = { 8, 16, 32, 128 };

/* Adds one flat piece per candle to a 2 x 4 dot raster. The gap column before
 * a wide body remains part of the piece, as it was in the scan-line renderer. */
static void raster_band(const Layout *L, const double *vals, unsigned char *dots)
{
    int cl = L->box_l + 1, cr = L->box_r - 1;
    double span = L->pmax - L->pmin;

    for (int j = 0; j < L->vis; j++) {
        double v = vals[L->first + j];
        if (isnan(v))
            continue;

        double y = (L->pmax - v) / span * L->rows_plot;
        int dot_y = (int)floor(y * 4.0 + 0.5);
        if (dot_y < 0 || dot_y >= L->rows_plot * 4)
            continue;

        int c0 = L->slot == 1 ? body_col(L, j) : body_col(L, j) - 1;
        int c1 = body_col(L, j) + L->body - 1;
        unsigned char line = BRAILLE_LEFT[dot_y % 4] | BRAILLE_RIGHT[dot_y % 4];
        int r = dot_y / 4;
        for (int c = c0; c <= c1; c++)
            if (c >= cl && c <= cr)
                dots[(size_t)r * (size_t)L->cols_plot + (size_t)(c - cl)] |= line;
    }
}

static void draw_braille_bands(const Layout *L, const unsigned char *dots,
                               Color color, Color bg)
{
    int cl = L->box_l + 1;
    for (int r = 0; r < L->rows_plot; r++) {
        for (int c = 0; c < L->cols_plot; c++) {
            unsigned char mask = dots[(size_t)r * (size_t)L->cols_plot + (size_t)c];
            if (!mask)
                continue;
            char glyph[4] = { (char)0xe2, (char)(0xa0 | (mask >> 6)),
                              (char)(0x80 | (mask & 0x3f)), '\0' };
            term_put(L->box_t + 1 + r, cl + c, glyph, color, bg);
        }
    }
}

static void draw_bollinger(const Stock *s, const Layout *L, const Settings *cfg)
{
    double *up = malloc(sizeof(double) * (size_t)s->count);
    double *lo = malloc(sizeof(double) * (size_t)s->count);
    size_t cells = (size_t)L->rows_plot * (size_t)L->cols_plot;
    unsigned char *dots = calloc(cells, 1);
    if (up && lo && dots) {
        ind_bollinger(s, cfg->bb_len, cfg->bb_std, up, lo);
        raster_band(L, up, dots);
        raster_band(L, lo, dots);
        draw_braille_bands(L, dots, cfg->bollinger, cfg->background);
    }
    free(dots);
    free(up);
    free(lo);
}

static Color mix(Color a, Color b, double t)      /* t = share of a */
{
    Color c;
    c.r = (unsigned char)(a.r * t + b.r * (1.0 - t) + 0.5);
    c.g = (unsigned char)(a.g * t + b.g * (1.0 - t) + 0.5);
    c.b = (unsigned char)(a.b * t + b.b * (1.0 - t) + 0.5);
    return c;
}

/* Where an rsi value sits in the box: a cell row and one of 4 heights in it. */
static void rsi_pos(double v, int rows, int *row, int *sub)
{
    int levels = rows * 4;
    int lv = (int)((100.0 - v) / 100.0 * (levels - 1) + 0.5);
    if (lv < 0) lv = 0;
    if (lv > levels - 1) lv = levels - 1;
    *row = lv / 4;
    *sub = lv % 4;
}

/* A suffix leaves room for at most six characters: 12.34K or 123.4K. */
static void format_volume_suffix(double q, char suffix, char *buf, size_t n)
{
    if (round(q * 100.0) / 100.0 < 100.0)
        snprintf(buf, n, "%.2f%c", q, suffix);
    else if (round(q * 10.0) / 10.0 < 1000.0)
        snprintf(buf, n, "%.1f%c", q, suffix);
    else
        snprintf(buf, n, "%.0f%c", q, suffix);
}

static void format_volume(double v, char *buf, size_t n)
{
    if (v >= 999950000.0)      format_volume_suffix(v / 1e9, 'B', buf, n);
    else if (v >= 999950.0)    format_volume_suffix(v / 1e6, 'M', buf, n);
    else if (v >= 1e3)         format_volume_suffix(v / 1e3, 'K', buf, n);
    else                       snprintf(buf, n, "%.0f", v);
}

/* Largest volume bar or mean in view; also what the volume scale is set to. */
static double volume_max(const Stock *s, const Layout *L, const double *mean)
{
    /* like the price scale, from the file's part only (a live volume that is
     * still growing must not move it); what is shown is the fallback */
    double m = 0.0, shown = 0.0;
    for (int i = L->first; i < L->first + L->vis; i++) {
        const Candle *c = &s->candles[i];
        if (c->volume > shown) shown = c->volume;
        if (isnan(c->ref_volume))
            continue;
        if (c->ref_volume > m) m = c->ref_volume;
        if (mean && !isnan(mean[i]) && mean[i] > m) m = mean[i];
    }
    return m > 0.0 ? m : shown;
}

static void draw_volume(const Stock *s, const Layout *L, const Settings *cfg,
                        const double *mean, double vmax)
{
    static const char *const EIGHTH[7] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇" };
    int rows = L->sub_rows, row0 = L->sub_t + 1;
    Color bg = cfg->background;

    if (vmax <= 0.0)
        return;

    for (int j = 0; j < L->vis; j++) {
        const Candle *c = &s->candles[L->first + j];
        if (c->volume <= 0.0)
            continue;
        int h8 = (int)(c->volume / vmax * rows * 8 + 0.5);
        if (h8 < 1) h8 = 1;
        if (h8 > rows * 8) h8 = rows * 8;

        Color color = mix(c->close >= c->open ? cfg->up : cfg->down, bg, 0.7);
        int col = body_col(L, j);
        for (int r = 0; r < rows && r * 8 < h8; r++) {
            int lv = h8 - r * 8;
            const char *g = lv >= 8 ? "█" : EIGHTH[lv - 1];
            for (int k = 0; k < L->body; k++)
                term_put(row0 + rows - 1 - r, col + k, g, color, bg);
        }
    }

    if (mean)
        draw_volume_mean(L, cfg, mean, vmax);
}

/* The two limits as faint dotted rows; drawn first so volume covers them. */
static void draw_rsi_limits(const Layout *L, const Settings *cfg)
{
    double lim[2] = { cfg->rsi_upper, cfg->rsi_lower };
    for (int k = 0; k < 2; k++) {
        int r, sub;
        rsi_pos(lim[k], L->sub_rows, &r, &sub);
        for (int c = L->box_l + 1; c < L->box_r; c++)
            term_put(L->sub_t + 1 + r, c, "┄", cfg->border, cfg->background);
    }
}

static void draw_rsi(const Layout *L, const Settings *cfg, const double *rsi)
{
    /* Dashes at four heights per row (braille, so the value reads precisely):
     * one dash per candle, as wide as its body. They replace the volume. */
    static const char *const DASH[4] = { "⠉", "⠒", "⠤", "⣀" };
    int rows = L->sub_rows, row0 = L->sub_t + 1;

    for (int j = 0; j < L->vis; j++) {
        double v = rsi[L->first + j];
        if (isnan(v))
            continue;
        int r, sub;
        rsi_pos(v, rows, &r, &sub);
        int hit = v >= cfg->rsi_upper || v <= cfg->rsi_lower;
        Color color = hit ? cfg->rsi_hit : cfg->rsi;
        int col = body_col(L, j);
        for (int k = 0; k < L->body; k++)
            term_put(row0 + r, col + k, DASH[sub], color, cfg->background);
    }
}

/* Labels on both sides of the volume / rsi box: the rsi limits when rsi is
 * shown, otherwise the largest volume in view. */
static void draw_sub_scale(const Layout *L, const View *v, const Settings *cfg, double vmax)
{
    Color bg = cfg->background;
    char buf[64];
    struct { int row; char text[32]; } lab[2];
    int n = 0;

    if (v->show_rsi) {
        double lim[2] = { cfg->rsi_upper, cfg->rsi_lower };
        for (int k = 0; k < 2; k++) {
            int r, sub;
            rsi_pos(lim[k], L->sub_rows, &r, &sub);
            lab[n].row = L->sub_t + 1 + r;
            snprintf(lab[n].text, sizeof lab[n].text, "%g", lim[k]);
            n++;
        }
    } else if (vmax > 0.0) {
        lab[0].row = L->sub_t + 1;
        format_volume(vmax, lab[0].text, sizeof lab[0].text);
        n = 1;
    }

    for (int k = 0; k < n; k++) {
        snprintf(buf, sizeof buf, "%s", lab[k].text);
        int w = (int)strlen(buf);
        term_text(lab[k].row, L->box_l - 1 - w, buf, cfg->muted, bg);
        term_text(lab[k].row, L->box_r + 2, buf, cfg->muted, bg);
        term_put(lab[k].row, L->box_l, "┤", cfg->border, bg);
        term_put(lab[k].row, L->box_r, "├", cfg->border, bg);
    }
}

/* The price picked with the mouse, on both scales, if it is in view. */
static void draw_crosshair(const Layout *L, const View *v, const Settings *cfg)
{
    if (!v->cross_on || v->cross_price < L->pmin || v->cross_price > L->pmax)
        return;
    double y = (L->pmax - v->cross_price) / (L->pmax - L->pmin) * L->rows_plot;
    int r = (int)y;
    if (r >= L->rows_plot)
        r = L->rows_plot - 1;
    int row = L->box_t + 1 + r;

    char buf[64];
    format_price(v->cross_price, buf, sizeof buf);
    int w = (int)strlen(buf);

    /* clear what the scale had on this row, so no stray digits stay next to the label */
    term_fill(row, L->box_l - L->margin + 1, 1, L->margin - 1, cfg->background);
    term_fill(row, L->box_r + 1, 1, L->margin - 1, cfg->background);
    term_text(row, L->box_l - 1 - w, buf, cfg->crosshair, cfg->background);
    term_text(row, L->box_r + 2, buf, cfg->crosshair, cfg->background);
    term_put(row, L->box_l, "┤", cfg->crosshair, cfg->background);
    term_put(row, L->box_r, "├", cfg->crosshair, cfg->background);
}

static void draw_dates(const Stock *s, const Layout *L, const View *v, const Settings *cfg)
{
    int row = L->bottom + 1;
    int per = (DATE_W + 2 + L->slot - 1) / L->slot;     /* candles between labels */
    const Candle *first = &s->candles[L->first];
    const Candle *last = &s->candles[L->first + L->vis - 1];
    char left_year[5], right_year[5];
    memcpy(left_year, first->date, 4);
    memcpy(right_year, last->date, 4);
    left_year[4] = right_year[4] = '\0';

    term_text(row, L->box_l - 5, left_year, cfg->muted, cfg->background);
    term_text(row, L->box_r + 2, right_year, cfg->muted, cfg->background);

    for (int j = L->vis - 1; j >= 0; j -= per) {
        int center = body_col(L, j) + L->body / 2;
        int x = center - DATE_W / 2;
        if (x < L->box_l || x + DATE_W - 1 > L->box_r)
            continue;
        Color color = (per == 1 && v->cross_on && v->cross_candle == L->first + j)
                      ? cfg->crosshair : cfg->muted;
        term_text(row, x, s->candles[L->first + j].date + 5, color, cfg->background);
        term_put(L->bottom, center, "┬", cfg->border, cfg->background);
    }
}

/* Ticker at the top left, then the last close whenever the market is not
 * actively live, then the live note. s is NULL for a ticker with no data yet. */
static void draw_header(const Stock *s, const Layout *L, const View *v, const Settings *cfg)
{
    Color bg = cfg->background;
    int col = L->box_l;

    col += term_text(0, col, s ? s->ticker : v->ticker, cfg->text, bg) + 2;

    if (s && (!v->live.on || v->live.show_header_price)) {
        const Candle *last = &s->candles[s->count - 1];
        double prev = s->count > 1 ? s->candles[s->count - 2].close : last->open;
        Color color = last->close >= prev ? cfg->up : cfg->down;

        char buf[64];
        format_price(last->close, buf, sizeof buf);
        col += term_text(0, col, buf, color, bg) + 2;
    }
    if (s && v->cross_on && v->cross_candle >= 0 && v->cross_candle < s->count) {
        const Candle *c = &s->candles[v->cross_candle];
        Color close_color = c->close >= c->open ? cfg->up : cfg->down;
        char buf[64];

        col += term_text(0, col, c->date, cfg->muted, bg) + 1;
        col += term_text(0, col, "O ", cfg->muted, bg);
        format_price(c->open, buf, sizeof buf);
        col += term_text(0, col, buf, cfg->text, bg) + 1;
        col += term_text(0, col, "C ", cfg->muted, bg);
        format_price(c->close, buf, sizeof buf);
        col += term_text(0, col, buf, close_color, bg) + 1;
        col += term_text(0, col, "H ", cfg->muted, bg);
        format_price(c->high, buf, sizeof buf);
        col += term_text(0, col, buf, cfg->up, bg) + 1;
        col += term_text(0, col, "L ", cfg->muted, bg);
        format_price(c->low, buf, sizeof buf);
        col += term_text(0, col, buf, cfg->down, bg) + 2;
        Stock prefix=*s;prefix.count=v->cross_candle+1;
        double *scores=malloc((size_t)prefix.count*sizeof *scores);
        double score=NAN;
        if(scores){if(!(cfg->swing_strategy==4?swing_calculate_bb(&prefix,cfg,scores):cfg->swing_strategy==3?swing_calculate3green(&prefix,cfg,scores):cfg->swing_strategy==2?swing_calculate3(&prefix,cfg,scores):cfg->swing_strategy==1?swing_calculate2(&prefix,cfg,scores):swing_calculate(&prefix,cfg,scores)))score=scores[prefix.count-1];free(scores);}
        const char *suffix=cfg->swing_strategy==2?" 3":cfg->swing_strategy==1?" 2":"";
        const char *name=cfg->swing_strategy==4?"bb_strat":cfg->swing_strategy==3?"3green":"Classic swing strategy";
        if(isfinite(score))snprintf(buf,sizeof buf,"%s%s %+.3f",name,suffix,score);
        else snprintf(buf,sizeof buf,"%s%s n/a",name,suffix);
        col += term_text(0,col,buf,isfinite(score)?(score>=0?cfg->up:cfg->down):cfg->muted,bg)+2;
    }
    if (v->measure_phase == MEASURE_WAIT_FIRST) {
        col += term_text(0, col, "measure: click first price", cfg->crosshair, bg) + 2;
    } else if (v->measure_phase == MEASURE_WAIT_SECOND) {
        col += term_text(0, col, "measure: click second price", cfg->crosshair, bg) + 2;
    } else if (v->measure_phase == MEASURE_RESULT) {
        char buf[64];
        Color color = cfg->muted;
        if (v->measure_percent > 0.0) {
            snprintf(buf, sizeof buf, "+%.2f%%", v->measure_percent);
            color = cfg->up;
        } else if (v->measure_percent < 0.0) {
            snprintf(buf, sizeof buf, "-%.2f%%", -v->measure_percent);
            color = cfg->down;
        } else {
            snprintf(buf, sizeof buf, "0.00%%");
        }
        col += term_text(0, col, buf, color, bg) + 2;
    }
    if (v->live.note[0])
        term_text(0, col, v->live.note, v->live.market_note ? cfg->market : cfg->muted, bg);
}

/* The live price on both scales, exactly as received, at the height where it
 * falls: green at or above today's open, red below. Nothing if it is outside
 * the range in view (live updates never change the range). */
static void draw_live_price(const Layout *L, const View *v, const Settings *cfg)
{
    const LiveView *lv = &v->live;
    if (!lv->on || !lv->has_price || lv->price < L->pmin || lv->price > L->pmax)
        return;

    Color color = lv->price >= lv->open ? cfg->up : cfg->down;
    double y = (L->pmax - lv->price) / (L->pmax - L->pmin) * L->rows_plot;
    int r = (int)y;
    if (r >= L->rows_plot)
        r = L->rows_plot - 1;
    int row = L->box_t + 1 + r;

    char buf[64];
    format_price(lv->price, buf, sizeof buf);
    int w = (int)strlen(buf);

    term_fill(row, L->box_l - L->margin + 1, 1, L->margin - 1, cfg->background);
    term_fill(row, L->box_r + 1, 1, L->margin - 1, cfg->background);
    term_text(row, L->box_l - 1 - w, buf, color, cfg->background);
    term_text(row, L->box_r + 2, buf, color, cfg->background);
    term_put(row, L->box_l, "┤", color, cfg->background);
    term_put(row, L->box_r, "├", color, cfg->background);
}

/* The middle of the box when there is no chart. */
static void draw_empty(const Layout *L, const View *v, const Settings *cfg)
{
    char buf[160];
    const char *m;

    switch (v->screen) {
    case SCREEN_NO_FOLDER:
        m = "No folder loaded. Press f to choose one, or s to look up a stock.";
        break;
    case SCREEN_EMPTY_FOLDER:
        m = "This folder has no stocks yet. Press s to add one, or f for another folder.";
        break;
    case SCREEN_PENDING:
        if (v->live.on)
            snprintf(buf, sizeof buf, "Fetching %s...", v->ticker);
        else
            snprintf(buf, sizeof buf, "No data for %s yet. Press l (live) to fetch it.", v->ticker);
        m = buf;
        break;
    default:
        m = "No stock loaded. Press s to pick one.";
        break;
    }
    int x = L->box_l + 1 + (L->cols_plot - term_text_width(m)) / 2;
    if (x < L->box_l + 1)
        x = L->box_l + 1;
    term_text(L->box_t + 1 + L->rows_plot / 2, x, m, cfg->muted, cfg->background);
}

#define ON(sc) (1u << (sc))
#define ALL_SCREENS (ON(SCREEN_CHART) | ON(SCREEN_NO_FOLDER) | ON(SCREEN_EMPTY_FOLDER) | \
                     ON(SCREEN_NO_STOCK) | ON(SCREEN_PENDING))

static void draw_footer(const Layout *L, const View *v, const Settings *cfg, const char *message)
{
    /* each shortcut shows only on the screens where it does something */
    const struct { const char *key, *desc; int on; unsigned screens; } items[] = {
        { "f",    "folder", 0, ALL_SCREENS },
        { "j",    "strategy", 0, ALL_SCREENS },
        { "s",    "stock", 0, ALL_SCREENS },
        { "l",    "live", v->live.on, ON(SCREEN_CHART) | ON(SCREEN_PENDING) },
        { "p",    period_names[v->period], 0, ON(SCREEN_CHART) },
        { "z/x",  "zoom date", 0, ON(SCREEN_CHART) },
        { "c/v",  "zoom price", 0, ON(SCREEN_CHART) },
        { "←↑↓→", "move", 0, ON(SCREEN_CHART) },
        { "k",    "volume", v->show_volume, ON(SCREEN_CHART) },
        { "b",    "bollinger", v->show_bb, ON(SCREEN_CHART) },
        { "r",    "rsi", v->show_rsi, ON(SCREEN_CHART) },
        { "g",    "graph", 0, ON(SCREEN_CHART) },
        { "d",    "data", 0, ON(SCREEN_CHART) },
        { "h",    "measure", v->measure_phase != MEASURE_OFF, ON(SCREEN_CHART) },
        { "q",    "quit", 0, ALL_SCREENS },
        { "click", "price", v->cross_on, ON(SCREEN_CHART) },
    };
    const size_t n_items = sizeof items / sizeof items[0];

    int row = term_rows() - 1;
    int col = L->box_l;

    if (message && message[0]) {
        term_text(row, col, message, cfg->down, cfg->background);
        return;
    }
    int total = -2;
    for (size_t i = 0; i < n_items; i++) {
        if (!(items[i].screens & ON(v->screen)))
            continue;
        total += term_text_width(items[i].key) + 1 + term_text_width(items[i].desc) + 2;
    }
    if (col + total > term_cols())              /* too narrow to line up with the box */
        col = 1;

    for (size_t i = 0; i < n_items; i++) {
        if (!(items[i].screens & ON(v->screen)))
            continue;
        int kw = term_text_width(items[i].key);
        int w = kw + 1 + term_text_width(items[i].desc);
        if (col + w > term_cols())
            break;
        term_text(row, col, items[i].key, items[i].on ? cfg->up : cfg->text, cfg->background);
        term_text(row, col + kw + 1, items[i].desc, cfg->muted, cfg->background);
        col += w + 2;
    }
}

void chart_draw(const Stock *s, View *v, const Settings *cfg, const char *message)
{
    int rows = term_rows(), cols = term_cols();
    int has = s && s->count > 0;
    Layout L;

    if (rows < 10 || cols < 60 || (has && !compute_layout(s, v, &L))) {
        const char *m = "Terminal too small";
        term_text(rows / 2, (cols - (int)strlen(m)) / 2, m, cfg->muted, cfg->background);
        return;
    }
    if (!has)
        empty_layout(&L);

    draw_box(&L, cfg, L.box_t, L.box_b);
    if (has) {
        draw_header(s, &L, v, cfg);
        draw_scale(&L, cfg);
        draw_candles(s, &L, cfg);
        if (v->show_bb)
            draw_bollinger(s, &L, cfg);
        draw_crosshair(&L, v, cfg);
        draw_live_price(&L, v, cfg);

        if (L.sub_on) {
            draw_box(&L, cfg, L.sub_t, L.sub_b);

            double *mean = NULL, *rsi = NULL;
            if (v->show_volume) {
                mean = malloc(sizeof(double) * (size_t)s->count);
                if (mean)
                    ind_volume_mean(s, cfg->volume_ma_len, mean);
            }
            if (v->show_rsi) {
                rsi = malloc(sizeof(double) * (size_t)s->count);
                if (rsi)
                    ind_rsi(s, cfg->rsi_len, rsi);
            }

            double vmax = v->show_volume ? volume_max(s, &L, mean) : 0.0;
            draw_sub_scale(&L, v, cfg, vmax);
            if (rsi)
                draw_rsi_limits(&L, cfg);
            if (v->show_volume) {
                draw_volume(s, &L, cfg, mean, vmax);
                if (vmax <= 0.0 && !rsi) {
                    const char *m = "no volume in this file (expected as the 6th column)";
                    int x = L.box_l + 1 + (L.cols_plot - (int)strlen(m)) / 2;
                    term_text(L.sub_t + 1 + L.sub_rows / 2, x, m, cfg->muted, cfg->background);
                }
            }
            if (rsi)
                draw_rsi(&L, cfg, rsi);
            free(mean);
            free(rsi);
        }
        draw_dates(s, &L, v, cfg);
    } else {
        if (v->screen == SCREEN_PENDING)
            draw_header(NULL, &L, v, cfg);
        draw_empty(&L, v, cfg);
    }
    draw_footer(&L, v, cfg, message);
}

void chart_pan(View *v, const Stock *s, int dx, int dy)
{
    Layout L;
    if (!s || s->count == 0 || term_rows() < 10 || term_cols() < 60 || !compute_layout(s, v, &L))
        return;

    if (dx) {
        int step = L.vis / 10;                  /* a tenth of the screen, at least one candle */
        if (step < 1)
            step = 1;
        v->x_off -= dx * step;
    }
    if (dy)
        v->y_off += dy * 0.1 * (L.pmax - L.pmin);
    compute_layout(s, v, &L);                   /* clamps */
}

void chart_click(View *v, const Stock *s, int button, int row, int col)
{
    if (button == 2) {
        v->cross_on = 0;
        v->cross_candle = 0;
        if (v->measure_phase != MEASURE_RESULT)
            v->measure_phase = MEASURE_OFF;
        return;
    }
    Layout L;
    if (button != 0 || !s || s->count == 0 || term_rows() < 10 || term_cols() < 60 ||
        !compute_layout(s, v, &L))
        return;
    if (row <= L.box_t || row >= L.box_b || col <= L.box_l || col >= L.box_r)
        return;                                 /* the main chart only */

    int slot_start = L.box_l + 1 + L.start;
    int j = (col - slot_start) / L.slot;
    if (col < slot_start || j < 0 || j >= L.vis)
        return;

    /* The measurement uses this price without disturbing an existing picked
     * candle or crosshair. Normal clicks still update both. */
    double price = L.pmax - ((row - L.box_t - 1) + 0.5) / L.rows_plot * (L.pmax - L.pmin);
    if (v->measure_phase == MEASURE_WAIT_FIRST) {
        v->measure_first = price;
        v->measure_phase = MEASURE_WAIT_SECOND;
        return;
    } else if (v->measure_phase == MEASURE_WAIT_SECOND) {
        v->measure_percent = v->measure_first != 0.0
                           ? (price - v->measure_first) / v->measure_first * 100.0 : 0.0;
        v->measure_phase = MEASURE_RESULT;
        return;
    }
    v->cross_price = price;
    v->cross_candle = L.first + j;             /* deliberately independent of click height */
    v->cross_on = 1;
}
