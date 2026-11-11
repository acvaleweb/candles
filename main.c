#include "chart.h"
#include "data.h"
#include "grabber.h"
#include "live.h"
#include "menu.h"
#include "regression.h"
#include "settings.h"
#include "state.h"
#include "stock.h"
#include "swing.h"
#include "terminal.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    LIVE_OFF,
    LIVE_HISTORY,           /* fetching the days the file is missing */
    LIVE_RUNNING            /* one reading every 60 seconds */
} LivePhase;

typedef struct {
    LivePhase   phase;
    Grabber     grab;
    int         got_reading;    /* at least one reading arrived in this run */
    int         has_price;      /* today has a price on screen */
    MarketState state;
    double      price, open;
} Live;

typedef struct {
    Settings  cfg;
    StockList list;             /* stocks in the current folder; dir is empty: no folder */
    Stock     stock;            /* the stock as in its file (days); live adds today's candle */
    int       has_stock;
    int       current;          /* index of stock in list       */
    Stock     agg;              /* the stock regrouped by week / month / year */
    int       has_agg;          /* 1 if agg is what is on screen (period is not day) */
    View      view;
    RegressionGraph graph;
    char      message[512];     /* shown at the bottom until the next key */
    int       message_is_live;  /* ...and cleared by the next good live reading */

    char      pending[64];      /* a ticker chosen without a file: waits for live to fetch it */
    Live      live;
} App;

static void set_message(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->message, sizeof a->message, fmt, ap);
    va_end(ap);
    a->message_is_live = 0;
}

/* The candles to draw: the days themselves, or the regrouped ones. */
static const Stock *shown(const App *a)
{
    if (!a->has_stock)
        return NULL;
    return a->has_agg ? &a->agg : &a->stock;
}

/* Tells the drawing what screen this is and how live mode looks. */
static void sync_view(App *a)
{
    View *v = &a->view;
    const Live *l = &a->live;

    if (a->has_stock)               v->screen = SCREEN_CHART;
    else if (a->pending[0])         v->screen = SCREEN_PENDING;
    else if (!a->list.dir[0])       v->screen = SCREEN_NO_FOLDER;
    else if (a->list.count == 0)    v->screen = SCREEN_EMPTY_FOLDER;
    else                            v->screen = SCREEN_NO_STOCK;
    snprintf(v->ticker, sizeof v->ticker, "%s", a->pending);

    memset(&v->live, 0, sizeof v->live);
    v->live.on = l->phase != LIVE_OFF;
    v->live.show_header_price = l->phase == LIVE_RUNNING && l->got_reading &&
                                l->state == MARKET_CLOSED;
    /* Once truly closed, use the regular last-price header and no live scale
     * labels. Pre-market and after-hours keep their normal live presentation. */
    v->live.has_price = l->has_price && !v->live.show_header_price;
    v->live.price = l->price;
    v->live.open = l->open;
    if (l->phase == LIVE_HISTORY) {
        snprintf(v->live.note, sizeof v->live.note, "fetching history...");
    } else if (l->phase == LIVE_RUNNING && !l->got_reading) {
        snprintf(v->live.note, sizeof v->live.note, "waiting for price...");
    } else if (l->phase == LIVE_RUNNING) {
        snprintf(v->live.note, sizeof v->live.note, "%s", live_state_note(l->state));
        v->live.market_note = 1;
    }
}

static void redraw(void *ctx)
{
    App *a = ctx;
    term_begin_frame(a->cfg.background);
    sync_view(a);
    chart_draw(shown(a), &a->view, &a->cfg, a->message);
}

static void free_stock(App *a)
{
    if (a->has_agg)
        stock_free(&a->agg);
    if (a->has_stock)
        stock_free(&a->stock);
    a->has_agg = 0;
    a->has_stock = 0;
}

/* The regrouped candles again, from the days (they changed). */
static void rebuild_agg(App *a)
{
    Stock g;
    if (!a->has_agg || stock_aggregate(&g, &a->stock, a->view.period) != 0)
        return;
    stock_free(&a->agg);
    a->agg = g;
}

static int pick_stock(App *a, int idx)
{
    Stock s;
    char err[512];
    View next;
    view_init(&next);                         /* the default for a new ticker */
    state_load_stock_view(a->list.tickers[idx], &next);
    if (stock_load(&s, a->list.dir, a->list.files[idx], a->list.tickers[idx],
                   err, sizeof err) != 0) {
        set_message(a, "%s", err);
        return -1;
    }
    Stock g;
    int want_agg = next.period != PERIOD_DAY;
    if (want_agg && stock_aggregate(&g, &s, next.period) != 0) {
        stock_free(&s);
        set_message(a, "Not enough memory for the %s candles", period_names[next.period]);
        return -1;
    }
    if (a->has_stock)
        state_save_stock_view(a->stock.ticker, &a->view);
    free_stock(a);
    a->view = next;
    a->stock = s;
    a->has_stock = 1;
    if (want_agg) {
        a->agg = g;
        a->has_agg = 1;
    }
    a->current = idx;
    a->pending[0] = '\0';
    a->view.cross_on = 0;                    /* its price and candle belong to the old stock */
    a->view.cross_candle = 0;
    state_save_stock(a->stock.ticker);       /* the last selection is immediate, not a q action */
    return 0;
}

/* The current stock again from its file, keeping the view where it is. This is
 * how today's live candle goes away, and how fetched days come in. */
static void reload_csv(App *a)
{
    Stock s, g;
    char err[512];
    if (!a->has_stock)
        return;
    if (stock_load(&s, a->list.dir, a->list.files[a->current], a->list.tickers[a->current],
                   err, sizeof err) != 0) {
        set_message(a, "%s", err);
        return;
    }
    int want_agg = a->view.period != PERIOD_DAY;
    if (want_agg && stock_aggregate(&g, &s, a->view.period) != 0) {
        stock_free(&s);
        set_message(a, "Not enough memory for the %s candles", period_names[a->view.period]);
        return;
    }
    free_stock(a);
    a->stock = s;
    a->has_stock = 1;
    if (want_agg) {
        a->agg = g;
        a->has_agg = 1;
    }
}

/* Switches the candle size. All candles of the stock are calculated here, in
 * one go, so drawing only reads them. The days stay in memory untouched. */
static void change_period(App *a, Period p)
{
    if (!a->has_stock || p == a->view.period)
        return;

    Stock g;
    if (p != PERIOD_DAY && stock_aggregate(&g, &a->stock, p) != 0) {
        set_message(a, "Not enough memory for the %s candles", period_names[p]);
        return;                             /* stay as we were */
    }
    if (a->has_agg)
        stock_free(&a->agg);
    a->has_agg = p != PERIOD_DAY;
    if (a->has_agg)
        a->agg = g;
    a->view.period = p;
    view_reset_pan(&a->view);               /* the scroll was counted in the old candles */
}

/* Loads a folder (it may have no stock files) and shows a stock: the one named
 * want (its ticker) if the folder has it, otherwise the first. The folder is
 * remembered. */
static int open_folder(App *a, const char *path, const char *want)
{
    StockList nl;
    char err[512];
    if (stocklist_scan(&nl, path, err, sizeof err) != 0) {
        set_message(a, "%s", err);
        return -1;
    }
    stocklist_free(&a->list);
    a->list = nl;
    if (a->has_stock)
        state_save_stock_view(a->stock.ticker, &a->view);
    free_stock(a);
    a->pending[0] = '\0';
    a->current = 0;
    state_save_folder(a->list.dir);
    if (a->list.count == 0)
        return 0;

    int idx = 0;
    for (int i = 0; want && i < a->list.count; i++)
        if (strcmp(a->list.tickers[i], want) == 0) {
            idx = i;
            break;
        }
    if (pick_stock(a, idx) != 0 && idx != 0)
        pick_stock(a, 0);                   /* the wanted file could not be read */
    return 0;
}

/* ---------- live mode ---------- */

/* Ends live mode (and kills the helper). With reload, the stock goes back to
 * what its file says: today's live candle disappears. */
static void live_stop(App *a, int reload)
{
    int was_on = a->live.phase != LIVE_OFF;
    grab_stop(&a->live.grab);
    a->live.phase = LIVE_OFF;
    a->live.has_price = 0;
    a->live.got_reading = 0;
    if (was_on && reload)
        reload_csv(a);
}

static int live_run_helper(App *a, const char *ticker)
{
    char err[512];
    if (grab_start_live(&a->live.grab, &a->cfg, ticker, err, sizeof err) != 0) {
        set_message(a, "%s", err);
        return -1;
    }
    a->live.phase = LIVE_RUNNING;
    a->live.got_reading = 0;
    a->live.has_price = 0;
    return 0;
}

/* l: first the days the file is missing, then the readings. */
static void live_start(App *a)
{
    char err[512], last[11];
    const char *ticker = a->has_stock ? a->stock.ticker : a->pending;

    if (!ticker[0]) {
        set_message(a, "Pick a stock first (press s)");
        return;
    }
    const char *after = NULL;
    if (a->has_stock && a->stock.count > 0) {
        memcpy(last, a->stock.candles[a->stock.count - 1].date, sizeof last);
        after = last;
    }
    if (grab_start_history(&a->live.grab, &a->cfg, ticker, after, err, sizeof err) != 0) {
        set_message(a, "%s", err);
        return;
    }
    a->live.phase = LIVE_HISTORY;
    a->live.got_reading = 0;
    a->live.has_price = 0;
}

/* The history arrived: write it into the file, load the file, start the readings. */
static void history_done(App *a)
{
    Grabber *g = &a->live.grab;
    char err[512], dir[PATH_MAX], file[300], ticker[256];
    int existing = a->has_stock;

    snprintf(ticker, sizeof ticker, "%s", existing ? a->stock.ticker : a->pending);
    if (existing) {
        snprintf(dir, sizeof dir, "%s", a->list.dir);
        snprintf(file, sizeof file, "%s", a->list.files[a->current]);
    } else {
        if (a->list.dir[0]) {
            snprintf(dir, sizeof dir, "%s", a->list.dir);
        } else {                            /* no folder chosen: ./stocks, made now */
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof cwd)) {
                live_stop(a, 0);
                set_message(a, "Cannot find the working directory: %s", strerror(errno));
                return;
            }
            if (snprintf(dir, sizeof dir, "%s/stocks", cwd) >= (int)sizeof dir) {
                live_stop(a, 0);
                set_message(a, "The working directory path is too long");
                return;
            }
            if (g->days > 0 && mkdir(dir, 0755) != 0 && errno != EEXIST) {
                live_stop(a, 0);
                set_message(a, "Cannot create %s: %s", dir, strerror(errno));
                return;
            }
        }
        snprintf(file, sizeof file, "%s.csv", ticker);
    }

    if (g->days > 0) {
        int write_failed;
        if (g->replace_csv)
            write_failed = stock_replace_csv(dir, file, g->csv, err, sizeof err);
        else
            write_failed = stock_append_csv(dir, file, g->csv, err, sizeof err);
        if (write_failed != 0) {
            live_stop(a, 0);
            set_message(a, "%s", err);
            return;
        }
    } else if (!existing) {                 /* nothing fetched, nothing on disk: no file is made */
        live_stop(a, 0);
        set_message(a, "No data found for %s", ticker);
        return;
    }
    grab_stop(g);                           /* the history job is over */

    if (existing) {
        reload_csv(a);
    } else if (open_folder(a, dir, ticker) != 0 || !a->has_stock) {
        a->live.phase = LIVE_OFF;
        return;
    }
    if (live_run_helper(a, ticker) != 0)
        a->live.phase = LIVE_OFF;
}

/* A live reading: today's candle is updated in memory (never in the file). */
static void apply_reading(App *a)
{
    const LiveReading *r = &a->live.grab.reading;
    Live *l = &a->live;

    l->got_reading = 1;
    l->state = r->state;
    l->has_price = r->has_candle && a->has_stock;
    l->price = r->price;
    l->open = r->open;
    if (a->has_stock && live_apply(&a->stock, r))
        rebuild_agg(a);
    if (a->message_is_live) {               /* the problem is over */
        a->message[0] = '\0';
        a->message_is_live = 0;
    }
}

/* Takes in everything the helper has said. */
static void live_service(App *a)
{
    for (;;) {
        Grabber *g = &a->live.grab;
        GrabEvent ev = grab_poll(g);
        char text[512];

        if (ev == GRAB_IDLE)
            return;
        snprintf(text, sizeof text, "%s", g->err);

        switch (ev) {
        case GRAB_HISTORY:
            if (a->live.phase == LIVE_HISTORY)
                history_done(a);
            break;
        case GRAB_READING:
            if (a->live.phase == LIVE_RUNNING)
                apply_reading(a);
            break;
        case GRAB_ERROR:
            if (a->live.phase == LIVE_HISTORY)  /* nothing to retry: live is off */
                live_stop(a, 0);
            set_message(a, "%s", text);
            a->message_is_live = 1;             /* (live keeps trying every 60 s) */
            break;
        case GRAB_ENDED:
            live_stop(a, 1);
            set_message(a, "%s", text);
            break;
        default:
            break;
        }
    }
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof app);
    grab_init(&app.live.grab);
    settings_load(&app.cfg);
    app.cfg.swing_strategy=state_load_click_strategy();
    if (!app.cfg.grabber[0])
        grab_default_script(app.cfg.grabber, sizeof app.cfg.grabber, argc > 0 ? argv[0] : NULL);
    view_init(&app.view);

    if (term_init() != 0) {
        fprintf(stderr, "candles: needs an interactive terminal\n");
        return 1;
    }

    char saved[PATH_MAX], saved_stock[256];
    if (state_load_folder(saved, sizeof saved)) {
        if (!state_load_stock(saved_stock, sizeof saved_stock))
            saved_stock[0] = '\0';
        open_folder(&app, saved, saved_stock);
    }

    for (;;) {
        redraw(&app);
        term_end_frame();

        int k = term_read_key_fd(grab_fd(&app.live.grab));
        if (k == KEY_WAKE) {                /* the helper has something to say */
            live_service(&app);
            continue;
        }
        app.message[0] = '\0';
        app.message_is_live = 0;
        if (app.view.measure_phase == MEASURE_RESULT)
            app.view.measure_phase = MEASURE_OFF;  /* every user action dismisses the result */

        if (k == 'q') {
            break;
        }
        if (k == 3 || k == KEY_EOF)
            break;

        switch (k) {
        case 'f': {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s", app.list.dir);
            if (menu_prompt(&app.cfg, redraw, &app, "Folder: ", path, sizeof path) &&
                open_folder(&app, path, NULL) == 0)
                live_stop(&app, 0);         /* live belonged to the old folder's stock */
            break;
        }
        case 'j': {
            char *strategies[]={"Classic swing strategy","Classic swing strategy 2","Classic swing strategy 3","3green","bb_strat"};
            int q=menu_select(&app.cfg,redraw,&app,"Strategy shown on click",strategies,5,app.cfg.swing_strategy);
            if(q>=0){app.cfg.swing_strategy=q;state_save_click_strategy(q);}
            break;
        }
        case 's': {
            char typed[64];
            int sel = menu_pick_stock(&app.cfg, redraw, &app, app.list.tickers, app.list.count,
                                      app.has_stock ? app.current : -1, typed, sizeof typed);
            if (sel >= 0) {
                if (!(app.has_stock && sel == app.current) && pick_stock(&app, sel) == 0)
                    live_stop(&app, 0);
            } else if (sel == PICK_NEW) {   /* a ticker with no file: live will fetch it */
                live_stop(&app, 0);
                if (app.has_stock)
                    state_save_stock_view(app.stock.ticker, &app.view);
                free_stock(&app);
                snprintf(app.pending, sizeof app.pending, "%s", typed);
                app.view.cross_on = 0;
                app.view.cross_candle = 0;
                app.view.measure_phase = MEASURE_OFF;
            }
            break;
        }
        case 'd': {
            char *actions[]={"Import","Generate"};
            int action=menu_select(&app.cfg,redraw,&app,"Data",actions,2,0);
            if(action<0)break;
            if(action==1) {
                char *indicators[]={"Classic swing strategy","Classic swing strategy 2","Classic swing strategy 3","3green","bb_strat"};
                int selected=menu_select(&app.cfg,redraw,&app,"Generate data",indicators,5,0);
                if(selected<0)break;
                if(!app.has_stock){set_message(&app,"Choose a stock before generating data");break;}
                char err[512];
                Settings export_cfg=app.cfg;export_cfg.swing_strategy=selected;
                if(swing_export(&app.stock,&export_cfg,app.list.dir,err,sizeof err))set_message(&app,"%s",err);
                else set_message(&app,"Generated %s_%s.csv",app.stock.ticker,selected==4?"bb_strat":selected==3?"3green":selected==2?"classic_swing_strategy3":selected==1?"classic_swing_strategy2":"classic_swing_strategy");
                break;
            }
            char path[PATH_MAX], imported[256], err[512];
            if (!app.list.dir[0]) {
                set_message(&app, "Choose a folder before importing data");
                break;
            }
            path[0] = '\0';
            if (!menu_prompt(&app.cfg, redraw, &app, "Data CSV path: ", path, sizeof path))
                break;
            if (data_import(app.list.dir, path, imported, sizeof imported, err, sizeof err) == 0)
                set_message(&app, "Imported data: %s", imported);
            else
                set_message(&app, "%s", err);
            break;
        }
        case 'l':
            if (app.live.phase != LIVE_OFF)
                live_stop(&app, 1);
            else
                live_start(&app);
            break;
        case 'g': {
            char err[512];
            if (!app.list.dir[0] || app.list.count == 0) {
                set_message(&app, "Choose a folder with stock CSV files first");
                break;
            }
            int result = regression_menu(&app.cfg, redraw, &app, &app.list, err, sizeof err,
                                         &app.graph);
            if (result > 0) {
                regression_view(&app.cfg, &app.graph, &app.list);
                regression_graph_free(&app.graph);
            } else if (result < 0) {
                set_message(&app, "%s", err);
            }
            break;
        }
        default:
            break;
        }

        if (!app.has_stock)                 /* nothing to chart: the other keys are locked */
            continue;

        int view_changed = 0;
        switch (k) {
        case 'p': {
            int sel = menu_select(&app.cfg, redraw, &app, "Period",
                                  period_names, PERIOD_COUNT, (int)app.view.period);
            if (sel >= 0) {
                change_period(&app, (Period)sel);
                view_changed = 1;
            }
            break;
        }
        case KEY_LEFT:  chart_pan(&app.view, shown(&app), -1, 0); view_changed = 1; break;
        case KEY_RIGHT: chart_pan(&app.view, shown(&app), +1, 0); view_changed = 1; break;
        case KEY_UP:    chart_pan(&app.view, shown(&app), 0, +1); view_changed = 1; break;
        case KEY_DOWN:  chart_pan(&app.view, shown(&app), 0, -1); view_changed = 1; break;
        case KEY_MOUSE: {
            int button, row, col;
            term_mouse(&button, &row, &col);
            chart_click(&app.view, shown(&app), button, row, col);
            break;
        }
        case 'k':
            app.view.show_volume = !app.view.show_volume;
            if (app.view.show_volume && !app.stock.has_volume)
                set_message(&app, "No volume in this file (expected as the 6th column)");
            view_changed = 1;
            break;
        case 'b': app.view.show_bb = !app.view.show_bb; view_changed = 1; break;
        case 'r': app.view.show_rsi = !app.view.show_rsi; view_changed = 1; break;
        case 'h': app.view.measure_phase = MEASURE_WAIT_FIRST; break;
        case KEY_ESC: app.view.measure_phase = MEASURE_OFF; break;
        case 'z': view_zoom_x(&app.view, +1); view_changed = 1; break;
        case 'x': view_zoom_x(&app.view, -1); view_changed = 1; break;
        case 'c': view_zoom_y(&app.view, +1); view_changed = 1; break;
        case 'v': view_zoom_y(&app.view, -1); view_changed = 1; break;
        default:  break;
        }
        if (view_changed)
            state_save_stock_view(app.stock.ticker, &app.view);
    }

    live_stop(&app, 0);                     /* the helper must not outlive us */
    regression_graph_free(&app.graph);
    free_stock(&app);
    stocklist_free(&app.list);
    return 0;
}
