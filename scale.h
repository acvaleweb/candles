#ifndef SCALE_H
#define SCALE_H

#include <math.h>

typedef struct {
    int first, gap, count;
} ScaleTicks;

/* Keep the previous 1/2/5 density changes, but choose whole-row gaps first.
 * Values are calculated afterwards; they no longer determine tick positions. */
static inline ScaleTicks scale_ticks(int rows, double span)
{
    ScaleTicks ticks={0,1,0};
    if(rows<1||!isfinite(span)||span<=0)return ticks;
    if(rows==1){ticks.count=1;return ticks;}
    int target=rows/4;
    if(target<2)target=2;
    double raw=span/target;
    double magnitude=pow(10.0,floor(log10(raw)));
    double ratio=raw/magnitude;
    double step=(ratio<=1?1:ratio<=2?2:ratio<=5?5:10)*magnitude;
    ticks.gap=(int)lround(step/span*rows);
    if(ticks.gap<2)ticks.gap=2;
    if(ticks.gap>=rows)ticks.gap=rows-1;
    ticks.first=((rows-1)%ticks.gap)/2;
    ticks.count=1+(rows-1-ticks.first)/ticks.gap;
    return ticks;
}

/* Value at the centre of a character row. Stock coordinates partition the
 * range into row bins; Braille coordinates run from dot 0 through dot 4H-1. */
static inline double scale_row_value(double minimum, double maximum, int rows,
                                      int row, int braille)
{
    double fraction=braille?(row*4+1.5)/(rows*4-1):(row+0.5)/rows;
    return maximum-(maximum-minimum)*fraction;
}

#endif
