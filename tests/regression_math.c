#include "regression_math.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void near(double actual, double expected, double tolerance)
{
    assert(isfinite(actual) && isfinite(expected));
    assert(fabs(actual - expected) <= tolerance * fmax(1, fabs(expected)));
}

/* Compare the matrix fit and inference with closed-form simple regression. */
static void linear_fit(void)
{
    double x[10], y[10], fit[10], score;
    double xm = 4.5, ym = 0, sxx = 0, sxy = 0, sst = 0;
    for (int i = 0; i < 10; i++) {
        x[i] = i;
        y[i] = 2 + 3*x[i] + (i % 3 - 1)*.4;
        ym += y[i]/10;
    }
    for (int i = 0; i < 10; i++) {
        sxx += (x[i]-xm)*(x[i]-xm);
        sxy += (x[i]-xm)*(y[i]-ym);
        sst += (y[i]-ym)*(y[i]-ym);
    }
    double slope = sxy/sxx, intercept = ym-slope*xm, sse = 0;
    for (int i = 0; i < 10; i++) {
        double residual = y[i] - (intercept+slope*x[i]);
        sse += residual*residual;
    }
    double sigma2 = sse/8;
    for (int standardized = 0; standardized <= 1; standardized++) {
        RegressionModel model;
        assert(!regression_fit(x,y,10,1,0,3,FAMILY_POLYNOMIAL,standardized,
                               fit,&score,&model));
        near(model.coefficients[0], standardized ? ym : intercept, 1e-12);
        near(model.coefficients[1], slope*(standardized?sqrt(sxx/10):1), 1e-12);
        near(score, 1-(sse/8)/(sst/9), 1e-12);
        for (int i = 0; i < 10; i++) near(fit[i],intercept+slope*x[i],1e-12);

        RegressionCoefficientTests tests;
        assert(!regression_coefficient_tests(x,y,10,1,&model,&tests));
        assert(tests.df == 8);
        near(tests.se[1],sqrt(sigma2/(standardized?10:sxx)),1e-12);
        near(tests.se[0],sqrt(sigma2*(.1+(standardized?0:xm*xm/sxx))),1e-12);
        assert(tests.p[1] > 0 && tests.p[1] < .001);

        RegressionGraph graph = {.degree=1,.count=10,.x=x,.y=y,.fit=fit,.model=model};
        double f, probability; int df1, df2;
        assert(!regression_overall_f_test(&graph,&f,&probability,&df1,&df2));
        assert(df1 == 1 && df2 == 8);
        near(f,tests.t[1]*tests.t[1],1e-10);
        near(probability,tests.p[1],1e-12);

        RegressionConfidence confidence;
        assert(!regression_confidence_prepare(&graph,95,&confidence));
        near(confidence.sigma2,sigma2,1e-12);
        near(confidence.critical,2.306004135204166,1e-10);
        double at = 3.25, mean = intercept+slope*at;
        double leverage = .1+(at-xm)*(at-xm)/sxx;
        double lo, hi, half = confidence.critical*sqrt(sigma2*leverage);
        assert(!regression_confidence_interval(&graph,&confidence,at,&lo,&hi));
        near(lo,mean-half,1e-12); near(hi,mean+half,1e-12);
        half = confidence.critical*sqrt(sigma2*(1+leverage));
        assert(!regression_prediction_interval(&graph,&confidence,at,&lo,&hi));
        near(lo,mean-half,1e-12); near(hi,mean+half,1e-12);
        assert(regression_confidence_prepare(&graph,100,&confidence) == -1);
    }
}

static void sparse_and_constant(void)
{
    double x[20], y[20], fit[20], score;
    RegressionModel model;
    for (int i = 0; i < 20; i++) {
        x[i] = -2+i*.2;
        y[i] = 2+.5*x[i]+.25*x[i]*x[i]*x[i];
    }
    unsigned mask = (1u<<0)|(1u<<1)|(1u<<3);
    assert(!regression_fit(x,y,20,3,0,mask,FAMILY_POLYNOMIAL,0,fit,&score,&model));
    assert(regression_model_terms(&model,3) == mask);
    int indices[26];
    assert(regression_term_indices(mask,indices) == 3);
    assert(indices[0] == 0 && indices[1] == 1 && indices[2] == 3);
    near(model.coefficients[0],2,1e-12);
    near(model.coefficients[1],.5,1e-12);
    near(model.coefficients[2],0,1e-12);
    near(model.coefficients[3],.25,1e-12);
    near(regression_evaluate(&model,3,1.25,NULL),2+.5*1.25+.25*pow(1.25,3),1e-12);
    RegressionCoefficientTests tests;
    assert(!regression_coefficient_tests(x,y,20,3,&model,&tests));
    assert(tests.df == 17 && isnan(tests.p[2]));
    model.terms = 0;
    assert(regression_model_terms(&model,3) == 15); /* Older saved graphs. */

    for (int i = 0; i < 20; i++) y[i] = 7;
    assert(!regression_fit(x,y,20,0,0,1,FAMILY_POLYNOMIAL,1,fit,&score,&model));
    assert(isnan(score));
    for (int i = 0; i < 20; i++) near(fit[i],7,1e-12);
    assert(regression_fit(x,y,2,1,0,3,FAMILY_POLYNOMIAL,1,fit,&score,&model) == -1);
    for (int i = 0; i < 20; i++) x[i] = 1;
    assert(regression_fit(x,y,20,1,0,3,FAMILY_POLYNOMIAL,1,fit,&score,&model) == -1);
}

static void log_response(void)
{
    double x[20], y[20], log_y[20], fit[20], log_fit[20], score, log_score;
    RegressionModel model, log_model;
    for (int i = 0; i < 20; i++) {
        x[i] = i*.1;
        log_y[i] = 2+.3*x[i]+.02*sin(i);
        y[i] = exp(log_y[i]);
    }
    assert(!regression_fit(x,y,20,1,1,3,FAMILY_POLYNOMIAL,1,fit,&score,&model));
    assert(!regression_fit(x,log_y,20,1,0,3,FAMILY_POLYNOMIAL,1,log_fit,&log_score,&log_model));
    near(score,log_score,1e-12);
    for (int i = 0; i < 20; i++) near(fit[i],exp(log_fit[i]),1e-12);
    RegressionGraph graph = {.degree=1,.count=20,.x=x,.y=y,.fit=fit,.model=model};
    RegressionGraph log_graph = {.degree=1,.count=20,.x=x,.y=log_y,.fit=log_fit,.model=log_model};
    RegressionConfidence ci, log_ci;
    assert(!regression_confidence_prepare(&graph,95,&ci));
    assert(!regression_confidence_prepare(&log_graph,95,&log_ci));
    double lo, hi, log_lo, log_hi;
    assert(!regression_prediction_interval(&graph,&ci,.75,&lo,&hi));
    assert(!regression_prediction_interval(&log_graph,&log_ci,.75,&log_lo,&log_hi));
    near(lo,exp(log_lo),1e-12); near(hi,exp(log_hi),1e-12);
    y[0] = 0;
    assert(regression_fit(x,y,20,1,1,3,FAMILY_POLYNOMIAL,1,fit,&score,&model) == -1);
}

static void shifted_families(void)
{
    double x[32], y[32], fit[32], score;
    for (int family = FAMILY_EXPONENTIAL; family <= FAMILY_LOGARITHMIC; family++) {
        for (int standardized = 0; standardized <= 1; standardized++) {
            for (int i = 0; i < 32; i++) {
                x[i] = .25+i*.125;
                y[i] = family==FAMILY_EXPONENTIAL ? 2+exp(.3*x[i]) : 3+2*log(x[i]+1);
            }
            RegressionModel model;
            assert(!regression_fit(x,y,32,1,0,3,family,standardized,fit,&score,&model));
            assert(model.family == family);
            for (int i = 0; i < 32; i++) near(fit[i],y[i],1e-6);
            if (family == FAMILY_LOGARITHMIC)
                assert(isnan(regression_evaluate(&model,1,model.shift,NULL)));
        }
    }
}

int main(void)
{
    linear_fit();
    sparse_and_constant();
    log_response();
    shifted_families();
    puts("Regression math: closed-form OLS, inference, sparse/log/shifted fits passed");
    return 0;
}
