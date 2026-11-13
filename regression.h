#ifndef REGRESSION_H
#define REGRESSION_H

#include <stddef.h>

#include "menu.h"
#include "stock.h"

#include "regression_types.h"

void regression_graph_free(RegressionGraph *graph);

/* Shows the g menu. It can generate a graph from the CSV folder or open a
 * saved graph. Returns 1 with a graph ready for regression_view(), 0 on
 * cancellation, or -1 on error. */
int regression_menu(const Settings *cfg, DrawFn draw, void *ctx, const StockList *list,
                    char *err, size_t errn, RegressionGraph *graph);

/* Full-screen Braille scatter/fit view; s saves to list->dir/graphs after a
 * bottom-line filename prompt. Esc returns to the stock viewer. */
void regression_view(const Settings *cfg, RegressionGraph *graph, const StockList *list);

#endif
