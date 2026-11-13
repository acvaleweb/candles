#include "residuals.h"
#include "scale.h"
#include "test_support.h"

static void residual_summaries(void)
{
    double values[]={-2,-1,0,1,2}; ResidualStats stats;
    CHECK(!residual_statistics(values,5,3,&stats));
    CHECK(stats.count==5 && stats.df==3);
    NEAR(stats.mean,0,1e-14); NEAR(stats.sigma2,10.0/3,1e-14);
    NEAR(stats.lag1,.4,1e-14); NEAR(stats.durbin_watson,.4,1e-14);
    NEAR(stats.minimum,-2,1e-14); NEAR(stats.maximum,2,1e-14);
    CHECK(!residual_statistics(values,5,0,&stats)); CHECK(isnan(stats.sigma2));
    double zero[5]={0}; CHECK(!residual_statistics(zero,5,3,&stats));
    CHECK(stats.sigma2==0 && isnan(stats.lag1) && isnan(stats.durbin_watson));
    CHECK(isnan(stats.shapiro_w) && isnan(stats.shapiro_p));
    values[2]=NAN; CHECK(residual_statistics(values,5,3,&stats)==-1);
    CHECK(residual_statistics(values,0,0,&stats)==-1);
}

static void shapiro_properties(void)
{
    double values[]={-1,0,2}, w,p;
    CHECK(!residual_shapiro(values,3,&w,&p));
    NEAR(w,27.0/28,1e-12);
    NEAR(p,1-6/M_PI*acos(sqrt(27.0/28)),1e-12);
    double a[16],b[16];
    for (int i=0;i<16;i++) a[i]=i==15?1000:i;
    double expected_w,expected_p;
    CHECK(!residual_shapiro(a,16,&expected_w,&expected_p)); CHECK(expected_p<.001);
    for (int i=0;i<16;i++) b[i]=100-3*a[15-i];
    CHECK(!residual_shapiro(b,16,&w,&p));
    NEAR(w,expected_w,1e-12); NEAR(p,expected_p,1e-12);
    CHECK(residual_shapiro(a,2,&w,&p)==-1);
    a[0]=INFINITY; CHECK(residual_shapiro(a,16,&w,&p)==-1);
}

static double histogram_area(const ResidualHistogram *hist)
{
    double area=0;
    for (int i=0;i<hist->bins;i++) { CHECK(hist->density[i]>=0); area+=hist->density[i]*hist->width; }
    return area;
}

static void histogram_normalization_and_zoom(void)
{
    double values[23]; for (int i=0;i<23;i++) values[i]=(i-11)*.25;
    ResidualStats stats; ResidualHistogram base,full,zoom,pan;
    CHECK(!residual_statistics(values,23,21,&stats));
    CHECK(!residual_histogram(values,&stats,12,&base)); CHECK(base.bins>=1 && base.bins<=12);
    NEAR(histogram_area(&base),1,1e-12);
    CHECK(!residual_histogram_view(values,&stats,&base,base.minimum,base.maximum,&full));
    NEAR(histogram_area(&full),1,1e-12);
    CHECK(!residual_histogram_view(values,&stats,&base,-1,1,&zoom));
    double expected=0;
    for (int i=0;i<23;i++) if (values[i]>=zoom.minimum && values[i]<zoom.maximum) expected+=1.0/23;
    NEAR(histogram_area(&zoom),expected,1e-12); CHECK(expected<1);
    CHECK(zoom.width<full.width);
    double min=base.minimum+.1*base.width,max=min+base.width*base.bins;
    CHECK(!residual_histogram_view(values,&stats,&base,min,max,&pan));
    NEAR(pan.minimum,base.minimum,1e-12); /* Same grid until a bin boundary is crossed. */
    CHECK(residual_histogram_view(values,&stats,&base,1,1,&pan)==-1);
    CHECK(residual_histogram_view(values,&stats,&base,NAN,1,&pan)==-1);
    CHECK(residual_histogram(values,&stats,0,&base)==-1);
    for (int i=0;i<23;i++) values[i]=4;
    CHECK(!residual_statistics(values,23,21,&stats));
    CHECK(!residual_histogram(values,&stats,128,&base)); CHECK(base.bins==1);
    NEAR(histogram_area(&base),1,1e-12);
}

static void axis_row_spacing(void)
{
    for (int braille=0;braille<=1;braille++) for (int rows=2;rows<=80;rows++) {
        ScaleTicks ticks=scale_ticks(rows,123.456);
        CHECK(ticks.gap>=1 && ticks.count>=1);
        CHECK(ticks.first+(ticks.count-1)*ticks.gap<rows);
        double step=scale_row_value(-20,103.456,rows,0,braille)-scale_row_value(-20,103.456,rows,1,braille);
        for (int row=1;row<rows;row++)
            NEAR(scale_row_value(-20,103.456,rows,row-1,braille)-scale_row_value(-20,103.456,rows,row,braille),step,1e-12);
    }
    CHECK(scale_ticks(0,10).count==0 && scale_ticks(10,NAN).count==0);
}

int main(void)
{
    test_setup(); RUN(residual_summaries); RUN(shapiro_properties);
    RUN(histogram_normalization_and_zoom); RUN(axis_row_spacing);
    return test_done("Residual diagnostics and scales");
}
