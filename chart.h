#ifndef CHART_H
#define CHART_H

#include "settings.h"
#include "stock.h"

/* What the screen shows when there is no chart. */
typedef enum {
    SCREEN_CHART,           /* a stock with data */
    SCREEN_NO_FOLDER,       /* nothing loaded yet */
    SCREEN_EMPTY_FOLDER,    /* a folder without any stock file */
    SCREEN_NO_STOCK,        /* a folder with files, but none is loaded */
    SCREEN_PENDING          /* a ticker that has no data yet: it can be fetched live */
} Screen;

typedef enum {
    MEASURE_OFF,
    MEASURE_WAIT_FIRST,
    MEASURE_WAIT_SECOND,
    MEASURE_RESULT
} MeasurePhase;

/* Live mode, as far as drawing is concerned. */
typedef struct {
    int    on;              /* live mode is running */
    int    show_header_price; /* market is actually closed: use the normal last-price header */
    int    has_price;       /* a price of today is known */
    double price;           /* exactly as received, not rounded to the scale */
    double open;            /* today's open: green at or above it, red below */
    char   note[40];        /* after the ticker: "market closed", "fetching history..." */
    int    market_note;     /* 1: the note is about the market (its own color), 0: progress */
} LiveView;

/* Zoom, scroll and overlay state. x_level selects how wide each candle is
 * (more zoom = wider candles = fewer shown); y_level > 0 zooms the price axis
 * in, < 0 out. */
typedef struct {
    int    x_level;
    int    y_level;

    int    x_off;           /* candles the view is scrolled back from the latest (0 = latest) */
    double y_off;           /* price the view is shifted up (+) or down (-)                   */

    int    show_volume;     /* volume bars + mean volume under the chart */
    int    show_bb;         /* bollinger bands over the candles          */
    int    show_rsi;        /* rsi in the volume box                     */

    int    cross_on;        /* a price and candle picked with the mouse  */
    double cross_price;
    int    cross_candle;    /* index in the stock passed to chart_draw   */

    MeasurePhase measure_phase;
    double measure_first;   /* first clicked chart price */
    double measure_percent; /* signed change from first click to second */

    Period period;          /* candle size, shown in the footer (the caller supplies matching candles) */

    Screen   screen;        /* what is shown; decides the message and the footer keys */
    char     ticker[64];    /* SCREEN_PENDING: the ticker waiting for data */
    LiveView live;
} View;

void view_init(View *v);
void view_zoom_x(View *v, int dir);     /* +1 zoom in, -1 zoom out */
void view_zoom_y(View *v, int dir);

/* Moves the viewer: dx +1 right (newer) / -1 left (older), dy +1 up / -1 down. */
void chart_pan(View *v, const Stock *stock, int dx, int dy);

/* A mouse press (button 0 left, 2 right; row / col 0-based). A left click in
 * the main chart picks a price from its row and a candle from its column; a
 * right click clears both and cancels a pending measurement. */
void chart_click(View *v, const Stock *stock, int button, int row, int col);

/* Forgets scroll and picked price (used when changing the candle period). */
void view_reset_pan(View *v);

/* Draws the whole screen into the frame buffer: ticker and last price on top,
 * the boxed candlestick chart with price scales on both sides, the optional
 * volume / rsi box, dates below, and the shortcuts (or message, if not
 * NULL/empty) at the bottom. stock may be NULL. The view is clamped to what
 * the data and the terminal allow. The caller starts and ends the frame. */
void chart_draw(const Stock *stock, View *view, const Settings *cfg,
                const char *message);

#endif
