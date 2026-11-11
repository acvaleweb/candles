#ifndef RESIDUALS_H
#define RESIDUALS_H

typedef struct {
    int count, df;
    double mean, sigma2, minimum, maximum, lag1, durbin_watson;
    double shapiro_w, shapiro_p;
} ResidualStats;

typedef struct {
    int bins;
    double minimum, maximum, width, maximum_density;
    double density[128];
} ResidualHistogram;

/* Returns -1 for an undefined test (too few/constant/nonfinite values).
 * W remains available above n=5000, but its p-value is left undefined. */
int residual_shapiro(const double *values,int count,double *w,double *p);
int residual_statistics(const double *values,int count,int df,ResidualStats *out);
int residual_histogram(const double *values,const ResidualStats *stats,int max_bins,ResidualHistogram *out);
/* Rebin the visible range on a grid anchored to the original histogram.
 * Density uses the full sample count, including points outside the view. */
int residual_histogram_view(const double *values,const ResidualStats *stats,
                            const ResidualHistogram *base,double minimum,double maximum,
                            ResidualHistogram *out);

#endif
