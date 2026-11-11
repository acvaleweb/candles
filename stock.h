#ifndef STOCK_H
#define STOCK_H

#include <limits.h>
#include <stddef.h>

/* The candle size shown on the chart. The files only ever hold days; the
 * other periods are calculated from them (see stock_aggregate). */
typedef enum {
    PERIOD_DAY = 0,
    PERIOD_WEEK,
    PERIOD_MONTH,
    PERIOD_YEAR,
    PERIOD_COUNT
} Period;

/* "day", "week", "month", "year", in enum order. */
extern char *const period_names[PERIOD_COUNT];

/* One row of a stock file: yyyy-mm-dd, open, close, max, min[, volume]. */
typedef struct {
    char   date[11];
    double open, close, high, low;
    double volume;          /* 0 if the file has no volume column */

    /* The part of the candle that comes from the file: the same as the values
     * above, but NAN for a day only live mode has seen. The price and volume
     * scales are built from these, so live updates never move them. */
    double ref_high, ref_low, ref_close, ref_volume;
} Candle;

typedef struct {
    char    ticker[256];
    Candle *candles;        /* sorted by date, oldest first */
    int     count;
    int     has_volume;     /* 1 if at least one row has a volume > 0 */
} Stock;

/* The stock files found in a folder (one TICKER.csv per stock). */
typedef struct {
    char   dir[PATH_MAX];   /* resolved absolute path */
    char **tickers;         /* file names without ".csv", sorted */
    char **files;           /* the matching file names */
    int    count;
} StockList;

/* Scans a folder for .csv files. Returns 0 on success (a folder without any
 * file is fine: count is 0 and dir is set); on failure returns -1 and writes a
 * readable reason into err. A leading "~" is expanded. */
int  stocklist_scan(StockList *l, const char *path, char *err, size_t errn);
void stocklist_free(StockList *l);

/* Adds rows to the end of dir/file, creating the file (with a header line) if
 * it does not exist. rows is text, one "yyyy-mm-dd,open,close,max,min,volume"
 * line each, every one ended by a newline. The folder must exist. Returns 0 on
 * success; on failure returns -1 and writes a readable reason into err. */
int  stock_append_csv(const char *dir, const char *file, const char *rows,
                      char *err, size_t errn);

/* Replaces dir/file with rows from a new full history. The replacement is
 * written beside the old file and renamed into place only after it is complete,
 * so a failed refresh leaves the old CSV intact. */
int  stock_replace_csv(const char *dir, const char *file, const char *rows,
                       char *err, size_t errn);

/* Loads one stock file. Returns 0 on success; on failure returns -1 and
 * writes a readable reason into err. */
int  stock_load(Stock *s, const char *dir, const char *file, const char *ticker,
                char *err, size_t errn);
void stock_free(Stock *s);

/* Builds the candles of a longer period out of the daily ones in src, for the
 * whole history at once. A week runs Monday to Sunday (the date shown is the
 * Monday), a month and a year are calendar ones (the 1st / January 1st).
 * Per period: open of the first day, close of the last, highest high, lowest
 * low, volumes added up. The last period is whatever has happened so far.
 * Returns 0 on success, -1 if out of memory (out is then empty). Free out
 * with stock_free. */
int  stock_aggregate(Stock *out, const Stock *src, Period p);

#endif
