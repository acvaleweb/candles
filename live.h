#ifndef LIVE_H
#define LIVE_H

#include "grabber.h"
#include "stock.h"

/* Puts a live reading into the days of a stock held in memory: today's candle
 * is updated in place, or added after the last day. Nothing is ever written to
 * a file. The candle's file part (ref_*) is left alone, so the price and volume
 * scales do not move. Returns 1 if the days changed. */
int live_apply(Stock *days, const LiveReading *r);

/* The note shown after the ticker for a market state: "market closed",
 * "after hours", "pre-market", or "" while the market is open. */
const char *live_state_note(MarketState state);

#endif
