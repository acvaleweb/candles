#include "indicators.h"

#include <math.h>

void ind_volume_mean(const Stock *s, int len, double *out)
{
    double sum = 0.0;
    for (int i = 0; i < s->count; i++) {
        sum += s->candles[i].volume;
        if (i >= len)
            sum -= s->candles[i - len].volume;
        out[i] = i >= len - 1 ? sum / len : NAN;
    }
}

void ind_bollinger(const Stock *s, int len, double k, double *upper, double *lower)
{
    for (int i = 0; i < s->count; i++) {
        upper[i] = lower[i] = NAN;
        if (i < len - 1)
            continue;

        /* recomputed per window (not a running sum) so rounding never builds up */
        double mean = 0.0;
        for (int j = i - len + 1; j <= i; j++)
            mean += s->candles[j].close;
        mean /= len;

        double var = 0.0;
        for (int j = i - len + 1; j <= i; j++) {
            double d = s->candles[j].close - mean;
            var += d * d;
        }
        double sd = sqrt(var / len);
        upper[i] = mean + k * sd;
        lower[i] = mean - k * sd;
    }
}

void ind_rsi(const Stock *s, int len, double *out)
{
    double gain = 0.0, loss = 0.0;

    for (int i = 0; i < s->count; i++) {
        out[i] = NAN;
        if (i == 0)
            continue;

        double d = s->candles[i].close - s->candles[i - 1].close;
        double g = d > 0 ? d : 0.0;
        double l = d < 0 ? -d : 0.0;

        if (i <= len) {                         /* first average: plain mean of the first len changes */
            gain += g;
            loss += l;
            if (i < len)
                continue;
            gain /= len;
            loss /= len;
        } else {
            gain = (gain * (len - 1) + g) / len;
            loss = (loss * (len - 1) + l) / len;
        }

        if (loss == 0.0)
            out[i] = gain == 0.0 ? 50.0 : 100.0;
        else
            out[i] = 100.0 - 100.0 / (1.0 + gain / loss);
    }
}
