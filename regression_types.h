#ifndef REGRESSION_TYPES_H
#define REGRESSION_TYPES_H

/* Plain model and saved-graph data; no terminal or configuration dependencies. */
enum { FAMILY_POLYNOMIAL, FAMILY_EXPONENTIAL, FAMILY_LOGARITHMIC };

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

#endif
