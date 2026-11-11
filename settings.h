#ifndef SETTINGS_H
#define SETTINGS_H

#include "terminal.h"

#include <limits.h>

typedef struct {
    int swing_strategy; /* classic, classic 2, classic 3, 3green, bb_strat */
    Color up;           /* candle closing at or above its open       */
    Color down;         /* candle closing below its open             */
    Color background;
    Color text;         /* ticker, shortcut keys, menus              */
    Color muted;        /* price scale, dates, shortcut descriptions */
    Color border;       /* the box around the chart                  */
    Color bollinger;    /* bollinger band lines                      */
    Color volume_ma;    /* mean volume line                          */
    Color rsi;          /* rsi, inside the limits                    */
    Color rsi_hit;      /* rsi on or beyond a limit                  */
    Color crosshair;    /* price picked with the mouse               */
    Color market;       /* live mode notes: market closed, after hours */

    char   python[PATH_MAX];    /* the python that runs the helper; empty: python3 */
    char   grabber[PATH_MAX];   /* the helper script; empty: grabber.py beside the program */

    double swing_rsi_weight, swing_volume_weight, swing_bollinger_weight, swing_candle_weight;
    double swing_volume_base, swing_close_tolerance, swing_wick_ratio;

    int    volume_ma_len;   /* mean volume length                    */
    int    bb_len;          /* bollinger length                      */
    double bb_std;          /* bollinger standard deviations         */
    int    rsi_len;         /* rsi length                            */
    double rsi_upper;       /* rsi upper limit                       */
    double rsi_lower;       /* rsi lower limit                       */
} Settings;

/* Loads ~/.config/candles/settings.conf. Missing keys keep their defaults.
 * If the file does not exist it is created with the defaults. */
void settings_load(Settings *s);

#endif
