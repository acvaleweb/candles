#ifndef INDICATORS_H
#define INDICATORS_H

#include "stock.h"

/* Every function fills out[0 .. s->count-1], one value per candle, with NAN
 * where the indicator is not defined yet (not enough candles before it). */

/* Simple moving average of the volume over len candles (the candle itself included). */
void ind_volume_mean(const Stock *s, int len, double *out);

/* Bollinger bands: mean of the close over len candles, plus / minus k times
 * the (population) standard deviation. The mean itself is not returned. */
void ind_bollinger(const Stock *s, int len, double k, double *upper, double *lower);

/* RSI (Wilder smoothing) of the close, 0..100. */
void ind_rsi(const Stock *s, int len, double *out);

#endif
