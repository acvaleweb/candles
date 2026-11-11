#ifndef GRABBER_H
#define GRABBER_H

#include <stddef.h>
#include <sys/types.h>

#include "settings.h"

/* Runs grabber.py (the yfinance helper) as a separate process and reads what
 * it prints, without ever blocking: the program watches grab_fd() together
 * with the keyboard and calls grab_poll() when it has input.
 *
 * Two jobs: the history (one run, ends by itself) and live (runs until
 * stopped, one reading every 60 seconds). Only one job runs at a time. */

typedef enum { MARKET_OPEN, MARKET_PRE, MARKET_POST, MARKET_CLOSED } MarketState;

/* What the helper knows about today. */
typedef struct {
    MarketState state;
    char   date[11];        /* the exchange's date */
    int    has_candle;      /* 0: no trades today yet (the numbers are then 0) */
    double open, high, low, price, volume;
} LiveReading;

typedef enum {
    GRAB_IDLE,              /* nothing new */
    GRAB_HISTORY,           /* the history is complete: days rows are in csv */
    GRAB_READING,           /* a live reading is in reading */
    GRAB_ERROR,             /* something failed: the text is in err */
    GRAB_ENDED              /* the helper is gone; err says why */
} GrabEvent;

typedef struct {
    pid_t  pid;
    int    fd;              /* the helper's output; -1 when nothing runs */
    int    wfd;             /* the helper's input; closing it tells it to stop */
    int    history;         /* 1 while the job is the history */
    int    eof;

    char  *buf;             /* what was read and not yet taken apart */
    size_t len, off, cap;

    char  *csv;             /* history: the rows, ready to go into the file */
    size_t csv_len, csv_cap;
    int    days;
    int    replace_csv;     /* history found a split: csv is the whole fresh history */

    LiveReading reading;    /* valid after GRAB_READING */
    char   err[512];        /* the last error the helper reported */
    char   noise[256];      /* the last line that was not a message (its own complaints) */
} Grabber;

void grab_init(Grabber *g);

/* Start the history of ticker. after is the last date the file already has
 * (yyyy-mm-dd), or NULL for the whole history. Returns 0, or -1 with a reason
 * in err. */
int  grab_start_history(Grabber *g, const Settings *cfg, const char *ticker,
                        const char *after, char *err, size_t errn);

/* Start live readings of ticker. */
int  grab_start_live(Grabber *g, const Settings *cfg, const char *ticker,
                     char *err, size_t errn);

/* The descriptor to watch for input, -1 if no job runs. */
int  grab_fd(const Grabber *g);

/* Takes in what the helper has printed and returns the next thing that
 * happened (call again until GRAB_IDLE). */
GrabEvent grab_poll(Grabber *g);

/* Stops the job (if any) and frees what it held. Safe to call at any time. */
void grab_stop(Grabber *g);

/* Where grabber.py is when settings do not say: beside the running program. */
void grab_default_script(char *out, size_t n, const char *argv0);

#endif
