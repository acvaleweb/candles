#include "live.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int live_apply(Stock *s, const LiveReading *r)
{
    if (!r->has_candle)
        return 0;

    Candle *last = s->count > 0 ? &s->candles[s->count - 1] : NULL;
    int cmp = last ? strcmp(r->date, last->date) : 1;
    if (cmp < 0)                                /* older than what the file already has */
        return 0;

    Candle *c;
    if (cmp == 0) {
        c = last;                               /* today is already the last candle */
    } else {
        Candle *n = realloc(s->candles, (size_t)(s->count + 1) * sizeof *n);
        if (!n)
            return 0;
        s->candles = n;
        c = &n[s->count++];
        memset(c, 0, sizeof *c);
        memcpy(c->date, r->date, 10);
        c->date[10] = '\0';
        c->ref_high = c->ref_low = c->ref_close = c->ref_volume = NAN;
    }

    c->open = r->open;
    c->close = r->price;
    c->high = r->high;
    c->low = r->low;
    c->volume = r->volume;

    double top = c->open > c->close ? c->open : c->close;      /* the wicks hold the body */
    double bot = c->open > c->close ? c->close : c->open;
    if (c->high < top) c->high = top;
    if (c->low > bot)  c->low = bot;

    if (r->volume > 0.0)
        s->has_volume = 1;
    return 1;
}

const char *live_state_note(MarketState state)
{
    switch (state) {
    case MARKET_PRE:    return "pre-market";
    case MARKET_POST:   return "after hours";
    case MARKET_CLOSED: return "market closed";
    default:            return "";
    }
}
