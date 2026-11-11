#ifndef STATE_H
#define STATE_H

#include <stddef.h>

#include "chart.h"

/* Builds ~/.config/candles/<file> into out, creating the directory if needed.
 * Returns 0 on success, -1 on failure. */
int  state_config_path(const char *file, char *out, size_t n);

/* The remembered folder and stock live in settings.conf, as the keys "folder"
 * and "stock" (the stock is its ticker). The load functions return 1 if a
 * value was found. Saving changes just that line of the file and leaves the
 * rest as it is; the line is added at the end if it is not there yet. */
int  state_load_folder(char *out, size_t n);
void state_save_folder(const char *dir);

int  state_load_stock(char *out, size_t n);
void state_save_stock(const char *ticker);

/* Ticker-keyed chart profiles live in views.conf, separate from settings.conf.
 * They hold zoom, scroll, selected period, and the three indicator toggles.
 * A missing or invalid profile leaves the view's defaults untouched. */
void state_load_stock_view(const char *ticker, View *view);
void state_save_stock_view(const char *ticker, const View *view);

/* The regression form is deliberately separate from the ordinary settings:
 * its one opaque value is owned and interpreted by regression.c. */
int  state_load_graph_form(char *out, size_t n);
void state_save_graph_form(const char *value);
int state_load_standardize(void);
void state_save_standardize(int enabled);
int state_load_click_strategy(void);
void state_save_click_strategy(int strategy);

#endif
