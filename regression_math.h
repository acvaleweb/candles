#ifndef REGRESSION_MATH_H
#define REGRESSION_MATH_H

#include "regression_types.h"

typedef struct {
    double se[26], t[26], p[26];
    int df;
} RegressionCoefficientTests;

typedef struct {
    int count, df, indices[27];
    double covariance[27*27], sigma2, critical, level;
} RegressionConfidence;

/* Term masks use bit k for power k. Zero denotes a legacy full model. */
unsigned regression_model_terms(const RegressionModel *model, int degree);
int regression_term_indices(unsigned mask, int indices[26]);

/* Fit finite paired arrays. Degree is 0..25; terms includes the intercept
 * and highest power. n must exceed the fitted parameter count. pred has n
 * elements. score is adjusted R² on the fitted response scale. Returns
 * 0 on success, -1 if fitting fails. The caller owns all arrays. */
int regression_fit(const double *x, const double *y, int n, int degree, int logy,
                   unsigned terms, int family, int standardize,
                   double *pred, double *score, RegressionModel *model);

/* Evaluate on the plotted response scale. Optional gradient has 27 slots;
 * log-y polynomial gradients refer to the fitted log-response scale. */
double regression_evaluate(const RegressionModel *model, int degree, double x,
                           double gradient[27]);
double regression_model_value(const RegressionGraph *graph, double x);

/* Tests and interval preparation return 0 on success, -1 if unavailable.
 * Nonlinear families retain their existing Jacobian approximations. */
int regression_coefficient_tests(const double *x, const double *y, int n, int degree,
                                 const RegressionModel *model,
                                 RegressionCoefficientTests *out);
int regression_overall_f_test(const RegressionGraph *graph, double *value,
                             double *probability, int *df1, int *df2);
int regression_confidence_prepare(const RegressionGraph *graph, double level,
                                  RegressionConfidence *out);
int regression_confidence_interval(const RegressionGraph *graph,
                                   const RegressionConfidence *confidence,
                                   double x, double *lower, double *upper);
int regression_prediction_interval(const RegressionGraph *graph,
                                   const RegressionConfidence *confidence,
                                   double x, double *lower, double *upper);

#endif
