#include "residuals.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Shapiro-Wilk coefficients and Royston's AS R94 approximation follow the
 * SciPy reference implementation. See THIRD_PARTY_NOTICES.md for its license.
 * Normal quantiles here are inverted using the standard C erfc function.
 * Reference: Royston (1995), Applied Statistics 44, doi:10.2307/2986146. */
static double polynomial(const double *c,int count,double x)
{
    double value=c[count-1];
    for(int i=count-2;i>=0;i--)value=value*x+c[i];
    return value;
}

static double normal_quantile(double probability)
{
    double lo=-10,hi=10;
    for(int i=0;i<64;i++) {
        double middle=(lo+hi)/2;
        if(.5*erfc(-middle/sqrt(2.0))<probability)lo=middle;else hi=middle;
    }
    return (lo+hi)/2;
}

static int compare_double(const void *left,const void *right)
{
    double a=*(const double *)left,b=*(const double *)right;
    return (a>b)-(a<b);
}

int residual_shapiro(const double *values,int n,double *w,double *p)
{
    *w=*p=NAN;if(n<3)return -1;
    double *sorted=malloc((size_t)n*sizeof *sorted),*a=malloc((size_t)(n/2)*sizeof *a);
    if(!sorted||!a){free(sorted);free(a);return -1;}
    for(int i=0;i<n;i++){if(!isfinite(values[i])){free(sorted);free(a);return -1;}sorted[i]=values[i];}
    qsort(sorted,(size_t)n,sizeof *sorted,compare_double);
    double range=sorted[n-1]-sorted[0];
    if(!isfinite(range)||range<=0){free(sorted);free(a);return -1;}
    if(n==3)a[0]=sqrt(.5);
    else {
        static const double c1[]={0,.221157,-.147981,-2.07119,4.434685,-2.706056};
        static const double c2[]={0,.042981,-.293762,-1.752461,5.682633,-3.582633};
        double sum=0;
        for(int i=0;i<n/2;i++){a[i]=normal_quantile((i+1-.375)/(n+.25));sum+=2*a[i]*a[i];}
        double root=sqrt(sum),rsn=1/sqrt((double)n),first=polynomial(c1,6,rsn)-a[0]/root;
        double factor;int start;
        if(n>5) {
            double second=polynomial(c2,6,rsn)-a[1]/root;
            factor=sqrt((sum-2*a[0]*a[0]-2*a[1]*a[1])/(1-2*first*first-2*second*second));
            a[1]=second;start=2;
        } else {factor=sqrt((sum-2*a[0]*a[0])/(1-2*first*first));start=1;}
        a[0]=first;
        for(int i=start;i<n/2;i++)a[i]/=-factor;
    }
    double mean=0,center=sorted[n/2];
    for(int i=0;i<n;i++)mean+=(sorted[i]-center)/range;
    mean/=n;
    double ssa=0,ssx=0,sax=0;
    for(int i=0;i<n;i++) {
        double weight=i<n/2?-a[i]:i>=(n+1)/2?a[n-1-i]:0;
        double x=(sorted[i]-center)/range-mean;
        ssa+=weight*weight;ssx+=x*x;sax+=weight*x;
    }
    free(sorted);free(a);
    if(ssa<=0||ssx<=0)return -1;
    double cross=sqrt(ssa*ssx);
    double complement=fmax(0,fmin(1,(cross-sax)*(cross+sax)/(ssa*ssx)));
    *w=fmax(0,fmin(1,1-complement));
    if(n>5000)return 0;
    if(n==3){*w=fmax(.75,*w);*p=fmax(0,1-6/M_PI*acos(sqrt(*w)));return 0;}
    if(complement<=0){*p=1;return 0;}
    double y=log(complement),m,s;
    if(n<=11) {
        static const double c3[]={.544,-.39978,.025054,-.0006714};
        static const double c4[]={1.3822,-.77857,.062767,-.0020322};
        double gamma=-2.273+.459*n;
        if(y>=gamma){*p=1e-19;return 0;}
        y=-log(gamma-y);m=polynomial(c3,4,n);s=exp(polynomial(c4,4,n));
    } else {
        static const double c5[]={-1.5861,-.31082,-.083751,.0038915};
        static const double c6[]={-.4803,-.082676,.0030302};
        double logn=log((double)n);m=polynomial(c5,4,logn);s=exp(polynomial(c6,3,logn));
    }
    *p=fmax(0,fmin(1,.5*erfc((y-m)/(s*sqrt(2.0)))));
    return 0;
}

int residual_statistics(const double *values,int n,int df,ResidualStats *out)
{
    memset(out,0,sizeof *out);out->count=n;out->df=df;
    out->sigma2=out->lag1=out->durbin_watson=out->shapiro_w=out->shapiro_p=NAN;
    if(n<1)return -1;
    out->minimum=out->maximum=values[0];
    long double sum=0,sse=0,differences=0;
    for(int i=0;i<n;i++) {
        if(!isfinite(values[i]))return -1;
        sum+=values[i];sse+=(long double)values[i]*values[i];
        out->minimum=fmin(out->minimum,values[i]);out->maximum=fmax(out->maximum,values[i]);
        if(i){long double delta=(long double)values[i]-values[i-1];differences+=delta*delta;}
    }
    out->mean=(double)(sum/n);
    if(df>0)out->sigma2=(double)(sse/df);
    if(sse>0)out->durbin_watson=(double)(differences/sse);
    long double variance=0,lag=0;
    for(int i=0;i<n;i++) {
        long double v=(long double)values[i]-out->mean;variance+=v*v;
        if(i)lag+=v*((long double)values[i-1]-out->mean);
    }
    if(variance>0&&n>1)out->lag1=(double)(lag/variance);
    residual_shapiro(values,n,&out->shapiro_w,&out->shapiro_p);
    return 0;
}

int residual_histogram(const double *values,const ResidualStats *stats,int max_bins,ResidualHistogram *out)
{
    memset(out,0,sizeof *out);int n=stats->count;
    if(n<1||max_bins<1)return -1;
    if(max_bins>128)max_bins=128;
    double *sorted=malloc((size_t)n*sizeof *sorted);if(!sorted)return -1;
    memcpy(sorted,values,(size_t)n*sizeof *sorted);qsort(sorted,(size_t)n,sizeof *sorted,compare_double);
    double spread=stats->maximum-stats->minimum,pad=spread*.05;
    if(pad<=0)pad=fmax(1,fabs(stats->mean)*.01);
    out->minimum=stats->minimum-pad;out->maximum=stats->maximum+pad;
    double sigma=isfinite(stats->sigma2)&&stats->sigma2>0?sqrt(stats->sigma2):0;
    if(sigma>0){out->minimum=fmin(out->minimum,stats->mean-3.5*sigma);out->maximum=fmax(out->maximum,stats->mean+3.5*sigma);}
    double bin_width=2*(sorted[(3*(n-1))/4]-sorted[(n-1)/4])/cbrt((double)n);
    free(sorted);
    double desired=bin_width>0?ceil((out->maximum-out->minimum)/bin_width):ceil(sqrt((double)n));
    if(spread==0)desired=1;
    if(!isfinite(desired)||desired>max_bins)desired=max_bins;
    out->bins=(int)fmax(1,desired);out->width=(out->maximum-out->minimum)/out->bins;
    if(!isfinite(out->width)||out->width<=0)return -1;
    for(int i=0;i<n;i++) {
        int bin=(int)((values[i]-out->minimum)/out->width);
        if(bin<0)bin=0;
        if(bin>=out->bins)bin=out->bins-1;
        out->density[bin]+=1/(n*out->width);
    }
    for(int i=0;i<out->bins;i++)out->maximum_density=fmax(out->maximum_density,out->density[i]);
    if(sigma>0)out->maximum_density=fmax(out->maximum_density,1/(sqrt(2*M_PI)*sigma));
    return 0;
}

int residual_histogram_view(const double *values,const ResidualStats *stats,
                            const ResidualHistogram *base,double minimum,double maximum,
                            ResidualHistogram *out)
{
    memset(out,0,sizeof *out);
    if(stats->count<1||base->bins<1||!isfinite(minimum)||!isfinite(maximum)||maximum<=minimum)return -1;
    int target=base->bins>126?126:base->bins;
    out->width=(maximum-minimum)/target;
    if(!isfinite(out->width)||out->width<=0)return -1;
    /* Keep bin boundaries stationary when panning at a fixed zoom. */
    out->minimum=base->minimum+floor((minimum-base->minimum)/out->width)*out->width;
    double bins=ceil((maximum-out->minimum)/out->width);
    if(!isfinite(bins)||bins<1||bins>128)return -1;
    out->bins=(int)bins;out->maximum=out->minimum+out->bins*out->width;
    for(int i=0;i<stats->count;i++) {
        double position=(values[i]-out->minimum)/out->width;
        if(position<0||position>=out->bins)continue;
        out->density[(int)position]+=1/(stats->count*out->width);
    }
    for(int i=0;i<out->bins;i++)out->maximum_density=fmax(out->maximum_density,out->density[i]);
    return 0;
}
