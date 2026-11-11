#ifndef REGRESSION_H
#define REGRESSION_H

#include <stddef.h>

#include "menu.h"
#include "stock.h"

typedef struct {
    int valid, log_y;
    double mean, scale;
    double coefficients[26]; /* powers of (x - mean) / scale */
    unsigned terms; /* bit k means z^k is present; zero is legacy full model */
    int family, exp_sign;
    double shift;
} RegressionModel;

typedef struct {
    char title[1024];
    char file_stem[240]; /* Descriptive filename for a newly generated graph. */
    char x_name[64], y_name[64];
    int degree;
    double adjusted_r2;
    double significance; /* saved alpha; zero means no significance coloring */
    int count;
    int x_is_time, y_is_time;
    int x_time_unit, y_time_unit;
    long x_epoch, y_epoch;
    double *x, *y, *fit;
    RegressionModel model;
} RegressionGraph;

void regression_graph_free(RegressionGraph *graph);

/* Shows the g menu. It can generate a graph from the CSV folder or open a
 * saved graph. On success graph is filled for regression_view(). */
int regression_menu(const Settings *cfg, DrawFn draw, void *ctx, const StockList *list,
                    char *err, size_t errn, RegressionGraph *graph);

/* Full-screen Braille scatter/fit view; s saves to list->dir/graphs after a
 * bottom-line filename prompt. Esc returns to the stock viewer. */
void regression_view(const Settings *cfg, RegressionGraph *graph, const StockList *list);

#endif
