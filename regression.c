#include "regression.h"

#include "data.h"
#include "scale.h"
#include "state.h"
#include "residuals.h"
#include "terminal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { VAR_DATA = -3, VAR_NONE = -2, VAR_TIME = -1 };
enum { DIR_BACKWARD, DIR_FORWARD };
enum { CHANGE_PERCENT, CHANGE_ABSOLUTE, CHANGE_LOG_RETURN };
enum { SAMPLE_ALL, SAMPLE_DISTANCE, SAMPLE_MONTH, SAMPLE_YEAR };
enum { FAMILY_POLYNOMIAL, FAMILY_EXPONENTIAL, FAMILY_LOGARITHMIC };

typedef struct {
    int y, x;                 /* VAR_TIME, VAR_DATA, or StockList index */
    int standardize;
    int y_change, x_change;
    int y_direction, x_direction, y_change_type, x_change_type;
    int y_log_return, x_log_return;
    int y_instant, x_instant, exclude_zero_x, exclude_zero_y, stock_prior_close;
    int y_days, x_days, time_unit; /* Time axis unit: 1, 7, 30, or 365 days. */
    int sample_mode, sample_days, sampled_changes;
    double significance; /* zero means no trimming */
    int exact_degree;
    int remove_intermediate;
    int family;
    int log_y, log_x, max_degree;
    char x_data_file[PATH_MAX], x_data_name[256];
    char y_data_file[PATH_MAX], y_data_name[256];
} Spec;

static long day_number(const char *s)
{
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + s[3]-'0';
    int m = (s[5]-'0')*10 + s[6]-'0', d = (s[8]-'0')*10 + s[9]-'0';
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400, yo = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
}

static void day_date(long z, char out[11])
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    unsigned d = doy - (153*mp + 2)/5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    snprintf(out, 11, "%04d-%02u-%02u", y, m, d);
}

void regression_graph_free(RegressionGraph *g)
{
    free(g->x); free(g->y); free(g->fit); memset(g, 0, sizeof *g);
}

static int choose_stock(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l,
                        const char *title, int current)
{
    int selected=menu_pick_existing(cfg, draw, ctx, title, l->tickers, l->count, current);
    return selected>=0?selected:VAR_NONE;
}

static int choose_data(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l,
                       char *file, size_t file_n, char *name, size_t name_n);

static int choose_y(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l,
                    Spec *spec)
{
    char *types[] = { "Time", "Stock", "Data" };
    int initial = spec->y == VAR_TIME ? 0 : spec->y == VAR_DATA ? 2 : 1;
    int type = menu_select(cfg, draw, ctx, "Explanatory source", types, 3, initial);
    if (type < 0) return VAR_NONE;
    if (type == 0) return VAR_TIME;
    if (type == 1)
        return choose_stock(cfg, draw, ctx, l, "Explanatory stock", spec->y >= 0 ? spec->y : -1);
    return choose_data(cfg, draw, ctx, l, spec->y_data_file, sizeof spec->y_data_file,
                       spec->y_data_name, sizeof spec->y_data_name);
}

static int choose_data(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l,
                       char *file, size_t file_n, char *name, size_t name_n)
{
    DataList data = {0}; char err[128];
    if (datalist_scan(&data, l->dir, err, sizeof err) || data.count == 0) {
        datalist_free(&data);
        char *notice[] = { "No imported data. Press d to import a CSV." };
        menu_select(cfg, draw, ctx, "Data", notice, 1, 0);
        return VAR_NONE;
    }
    int selected = menu_pick_existing(cfg, draw, ctx, "Data", data.names, data.count, -1);
    if (selected >= 0) {
        snprintf(file, file_n, "%s", data.files[selected]);
        snprintf(name, name_n, "%s", data.names[selected]);
    }
    datalist_free(&data);
    return selected >= 0 ? VAR_DATA : VAR_NONE;
}

static int choose_x(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l, Spec *spec)
{
    char *types[] = { "Time", "Stock", "Data" };
    int initial = spec->x == VAR_TIME ? 0 : spec->x == VAR_DATA ? 2 : 1;
    int type = menu_select(cfg, draw, ctx, "Regressor source", types, 3, initial);
    if (type < 0) return VAR_NONE;
    if (type == 0) return VAR_TIME;
    if (type == 1) return choose_stock(cfg, draw, ctx, l, "Regressor stock", spec->x >= 0 ? spec->x : -1);
    return choose_data(cfg, draw, ctx, l, spec->x_data_file, sizeof spec->x_data_file,
                       spec->x_data_name, sizeof spec->x_data_name);
}

static int ask_int(const Settings *cfg, DrawFn draw, void *ctx, const char *label,
                   int old, int lo, int hi)
{
    char b[32]; snprintf(b, sizeof b, "%d", old);
    if (!menu_prompt(cfg, draw, ctx, label, b, sizeof b)) return old;
    char *end; long v = strtol(b, &end, 10);
    if (*end || v < lo || v > hi) return old;
    return (int)v;
}

/* With prior_close enabled, a date without a trading candle resolves to the
 * most recent preceding close.  It deliberately never looks forward: a
 * weekend or holiday must not learn a later session's price. */
static int find_date(const Stock *s, const char *date, int prior_close)
{
    int lo = 0, hi = s->count - 1, ans = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, c = strcmp(s->candles[m].date, date);
        if (c <= 0) { ans = m; lo = m + 1; } else hi = m - 1;
    }
    if (ans < 0) return -1;
    return prior_close || strcmp(s->candles[ans].date, date) == 0 ? ans : -1;
}

static double change_value(double start,double end,int type)
{
    if(type==CHANGE_ABSOLUTE)return end-start;
    if(type==CHANGE_LOG_RETURN)return start>0&&end>0?100*(log(end)-log(start)):NAN;
    return start==0?NAN:(end/start-1)*100;
}

/* A backward return ends on the selected date; a forward return begins on it.
 * With instant set, the nearest actual observation in that direction is used
 * instead of looking for a calendar-day target. */
static double stock_value(const Stock *s, int i, int change, int days,
                          int direction, int instant, int change_type, long sample_day)
{
    double now = s->candles[i].close;
    if (!change) return now;
    int j;
    if (direction == DIR_BACKWARD) {
        long target = sample_day - days;
        j = i - 1;
        if (!instant) {
            j = i;
            while (j >= 0 && day_number(s->candles[j].date) > target) j--;
        }
        return j<0?NAN:change_value(s->candles[j].close,now,change_type);
    }
    long target = sample_day + days;
    j = i + 1;
    if (!instant)
        while (j < s->count && day_number(s->candles[j].date) < target) j++;
    return j>=s->count?NAN:change_value(now,s->candles[j].close,change_type);
}

static int data_find_date(const DataSeries *data, const char *date)
{
    int lo=0, hi=data->count-1;
    while (lo <= hi) {
        int mid=(lo+hi)/2, cmp=strcmp(data->points[mid].date,date);
        if (!cmp) return mid;
        if (cmp < 0) lo=mid+1; else hi=mid-1;
    }
    return -1;
}

static double sampled_value(const double *values, const long *dates, int count,
                            int i, int days, int direction, int instant, int type)
{
    int j=i+(direction==DIR_FORWARD?1:-1);
    long target=dates[i]+(direction==DIR_FORWARD?days:-days);
    if(!instant) {
        if(direction==DIR_FORWARD) while(j<count && dates[j]<target) j++;
        else while(j>=0 && dates[j]>target) j--;
    }
    if(j<0 || j>=count) return NAN;
    double start=direction==DIR_FORWARD?values[i]:values[j];
    double end=direction==DIR_FORWARD?values[j]:values[i];
    return change_value(start,end,type);
}

static double data_value(const DataSeries *data, int i, int change, int days,
                         int direction, int instant, int change_type)
{
    double now=data->points[i].value;
    if (!change) return now;
    int j;
    if (direction == DIR_BACKWARD) {
        long target=day_number(data->points[i].date)-days;
        j=i-1;
        if (!instant)
            while(j>=0&&day_number(data->points[j].date)>target)j--;
        return j<0?NAN:change_value(data->points[j].value,now,change_type);
    }
    long target=day_number(data->points[i].date)+days;
    j=i+1;
    if (!instant)
        while(j<data->count&&day_number(data->points[j].date)<target)j++;
    return j>=data->count?NAN:change_value(now,data->points[j].value,change_type);
}

static int solve(double *a, double *b, int n, double *out)
{
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++) if (fabs(a[r*n+c]) > fabs(a[p*n+c])) p = r;
        if (fabs(a[p*n+c]) < 1e-12) return -1;
        if (p != c) for (int k = c; k < n; k++) { double t=a[c*n+k]; a[c*n+k]=a[p*n+k]; a[p*n+k]=t; }
        if (p != c) { double t=b[c]; b[c]=b[p]; b[p]=t; }
        double q=a[c*n+c]; for (int k=c;k<n;k++) a[c*n+k]/=q; b[c]/=q;
        for (int r=0;r<n;r++) if (r!=c) { q=a[r*n+c]; for(int k=c;k<n;k++) a[r*n+k]-=q*a[c*n+k]; b[r]-=q*b[c]; }
    }
    memcpy(out,b,(size_t)n*sizeof *out); return 0;
}

static unsigned model_terms(const RegressionModel *model,int degree)
{
    return model->terms?model->terms:(1u<<(degree+1))-1;
}

static int term_indices(unsigned mask,int indices[26])
{
    int count=0;for(int j=0;j<26;j++)if(mask&(1u<<j))indices[count++]=j;
    return count;
}

static int fit_terms(const double *x, const double *y, int n, int degree, int logy,unsigned terms,int standardize,
                      double *pred, double *score, RegressionModel *model)
{
    int indices[26],p=term_indices(terms,indices); if (!p || n <= p) return -1;
    double mean=0, sd=0; for(int i=0;i<n;i++) mean+=x[i]; mean/=n;
    for(int i=0;i<n;i++){double d=x[i]-mean;sd+=d*d;} sd=sqrt(sd/n); if(sd==0){if(degree)return -1;sd=1;}
    if(!standardize){mean=0;sd=1;}
    double *a=calloc((size_t)p*p,sizeof *a), *b=calloc((size_t)p,sizeof *b), *coef=calloc((size_t)p,sizeof *coef);
    if(!a||!b||!coef){free(a);free(b);free(coef);return -1;}
    double ym=0; for(int i=0;i<n;i++){if(logy&&y[i]<=0){free(a);free(b);free(coef);return -1;}ym+=logy?log(y[i]):y[i];} ym/=n;
    int constant_y=1;
    for(int i=1;i<n;i++)if(y[i]!=y[0]){constant_y=0;break;}
    if(constant_y)ym=logy?log(y[0]):y[0];
    double sst=0; for(int i=0;i<n;i++){double z=(x[i]-mean)/sd, powv[51];powv[0]=1;for(int k=1;k<=2*degree;k++)powv[k]=powv[k-1]*z;double yy=logy?log(y[i]):y[i];for(int r=0;r<p;r++){b[r]+=powv[indices[r]]*yy;for(int c=0;c<p;c++)a[r*p+c]+=powv[indices[r]+indices[c]];}double d=yy-ym;sst+=d*d;}
    if(sst==0)coef[0]=ym;
    else if(solve(a,b,p,coef)){free(a);free(b);free(coef);return -1;}
    double sse=0;for(int i=0;i<n;i++){double z=(x[i]-mean)/sd, v=0;for(int k=0;k<p;k++)v+=coef[k]*pow(z,indices[k]); double yy=logy?log(y[i]):y[i];double d=yy-v;sse+=d*d;pred[i]=logy?exp(v):v;}
    *score=sst>0?1.0-(sse/(n-p))/(sst/(n-1)):NAN;
    memset(model,0,sizeof *model);
    model->valid=1;model->log_y=logy;model->mean=mean;model->scale=sd;
    model->terms=terms;
    for(int k=0;k<p;k++)model->coefficients[indices[k]]=coef[k];
    free(a);free(b);free(coef);return 0;
}

static int fit_degree(const double *x,const double *y,int n,int degree,int logy,
                      double *pred,double *score,RegressionModel *model)
{
    return fit_terms(x,y,n,degree,logy,(1u<<(degree+1))-1,1,pred,score,model);
}

static double evaluate_model(const RegressionModel *m,int degree,double x,double gradient[27])
{
    double input=x;
    if(m->family==FAMILY_LOGARITHMIC){if(x<=m->shift)return NAN;input=log(x-m->shift);}
    double z=(input-m->mean)/m->scale,powers[26];powers[0]=1;
    for(int k=1;k<=degree;k++)powers[k]=powers[k-1]*z;
    double value=0,derivative=0;
    for(int k=0;k<=degree;k++) {
        value+=m->coefficients[k]*powers[k];
        if(k)derivative+=k*m->coefficients[k]*powers[k-1];
    }
    double multiplier=1;
    if(m->family==FAMILY_EXPONENTIAL)multiplier=m->exp_sign*exp(value);
    if(gradient) {
        for(int k=0;k<=degree;k++)gradient[k]=multiplier*powers[k];
        gradient[26]=m->family==FAMILY_EXPONENTIAL?1:
            m->family==FAMILY_LOGARITHMIC?-derivative/(m->scale*(x-m->shift)):0;
    }
    if(m->family==FAMILY_EXPONENTIAL)return m->shift+multiplier;
    return m->log_y?exp(value):value;
}

static double model_value(const RegressionGraph *g, double x)
{
    return evaluate_model(&g->model,g->degree,x,NULL);
}

static double nonlinear_system(const double *x,const double *y,int n,int degree,
                               const RegressionModel *m,double *matrix,double *rhs)
{
    int indices[26],count=term_indices(model_terms(m,degree),indices),p=count+1;
    if(matrix)memset(matrix,0,(size_t)p*p*sizeof *matrix);
    if(rhs)memset(rhs,0,(size_t)p*sizeof *rhs);
    double sse=0;
    for(int i=0;i<n;i++) {
        double gradient[27],fit=evaluate_model(m,degree,x[i],matrix?gradient:NULL);
        if(!isfinite(fit))return INFINITY;
        double residual=y[i]-fit;sse+=residual*residual;
        if(matrix)for(int r=0;r<p;r++) {
            double gr=gradient[r<count?indices[r]:26];
            rhs[r]+=gr*residual;
            for(int c=0;c<p;c++)matrix[r*p+c]+=gr*gradient[c<count?indices[c]:26];
        }
    }
    return sse;
}

/* Damped nonlinear least squares, with several shift/sign starting points. */
static int fit_family(const double *x,const double *y,int n,int degree,int logy,unsigned terms,int family,int standardize,
                      double *pred,double *score,RegressionModel *model)
{
    if(!family||degree==0)return fit_terms(x,y,n,degree,logy,terms,standardize,pred,score,model);
    int indices[26],count=term_indices(terms,indices),p=count+1;
    if(n<=p)return -1;
    double xmin=x[0],xmax=x[0],ymin=y[0],ymax=y[0],ym=0;
    for(int i=0;i<n;i++){xmin=fmin(xmin,x[i]);xmax=fmax(xmax,x[i]);ymin=fmin(ymin,y[i]);ymax=fmax(ymax,y[i]);ym+=y[i];}
    if(xmax==xmin)return -1;
    ym/=n;
    double xr=xmax-xmin,yr=fmax(ymax-ymin,1e-6),best=INFINITY;
    double *tx=malloc((size_t)n*sizeof *tx),*ty=malloc((size_t)n*sizeof *ty),*initial=malloc((size_t)n*sizeof *initial);
    if(!tx||!ty||!initial){free(tx);free(ty);free(initial);return -1;}
    for(int start=0;start<(family==FAMILY_EXPONENTIAL?6:3);start++) {
        double factor=start%3==0?.05:start%3==1?.5:2;
        int sign=start<3?1:-1;
        double shift=family==FAMILY_LOGARITHMIC?xmin-factor*xr:sign>0?ymin-factor*yr:ymax+factor*yr;
        for(int i=0;i<n;i++){
            tx[i]=family==FAMILY_LOGARITHMIC?log(x[i]-shift):x[i];
            ty[i]=family==FAMILY_EXPONENTIAL?log(sign*(y[i]-shift)):y[i];
        }
        RegressionModel current;double unused;
        if(fit_terms(tx,ty,n,degree,0,terms,standardize,initial,&unused,&current))continue;
        current.family=family;current.shift=shift;current.exp_sign=sign;
        double error=nonlinear_system(x,y,n,degree,&current,NULL,NULL),lambda=.001;
        for(int iter=0;iter<180;iter++) {
            double matrix[27*27],rhs[27],delta[27];
            nonlinear_system(x,y,n,degree,&current,matrix,rhs);
            int finite=1;for(int j=0;j<p*p;j++)if(!isfinite(matrix[j]))finite=0;
            for(int j=0;j<p;j++)if(!isfinite(rhs[j]))finite=0;
            if(!finite)break;
            for(int j=0;j<p;j++)matrix[j*p+j]+=lambda*fmax(matrix[j*p+j],1e-12);
            if(solve(matrix,rhs,p,delta)){lambda*=10;if(lambda>1e15)break;continue;}
            RegressionModel candidate=current;
            for(int j=0;j<count;j++)candidate.coefficients[indices[j]]+=delta[j];
            candidate.shift+=delta[count];
            double next=family==FAMILY_LOGARITHMIC&&candidate.shift>=xmin?INFINITY:
                nonlinear_system(x,y,n,degree,&candidate,NULL,NULL);
            if(isfinite(next)&&next<error) {
                double improvement=error-next;current=candidate;error=next;lambda=fmax(lambda*.3,1e-12);
                if(improvement<1e-11*fmax(error,1e-20))break;
            } else {lambda*=10;if(lambda>1e15)break;}
        }
        if(isfinite(error)&&error<best){best=error;*model=current;}
    }
    free(tx);free(ty);free(initial);
    if(!isfinite(best))return -1;
    double sst=0;for(int i=0;i<n;i++){sst+=(y[i]-ym)*(y[i]-ym);pred[i]=evaluate_model(model,degree,x[i],NULL);}
    *score=sst>0?1-(best/(n-p))/(sst/(n-1)):NAN;
    return 0;
}

/* Regularized incomplete beta, used for two-sided Student t probabilities. */
static double beta_fraction(double a,double b,double x)
{
    double c=1,d=1-(a+b)*x/(a+1),h;
    if(fabs(d)<1e-300)d=1e-300;
    d=1/d;h=d;
    for(int m=1;m<=10000;m++) {
        double aa=m*(b-m)*x/((a+2*m-1)*(a+2*m));
        d=1+aa*d;if(fabs(d)<1e-300)d=1e-300;
        c=1+aa/c;if(fabs(c)<1e-300)c=1e-300;
        d=1/d;h*=d*c;
        aa=-(a+m)*(a+b+m)*x/((a+2*m)*(a+2*m+1));
        d=1+aa*d;if(fabs(d)<1e-300)d=1e-300;
        c=1+aa/c;if(fabs(c)<1e-300)c=1e-300;
        d=1/d;double delta=d*c;h*=delta;
        if(fabs(delta-1)<3e-14)return h;
    }
    return NAN;
}

static double t_probability(double t,int df)
{
    if(df<=0||isnan(t))return NAN;
    if(isinf(t))return 0;
    if(t==0)return 1;
    double x=df/(df+t*t),a=df/2.0,b=.5;
    if(x<=0)return 0;
    double front=exp(lgamma(a+b)-lgamma(a)-lgamma(b)+a*log(x)+b*log1p(-x));
    double p=x<(a+1)/(a+b+2)?front*beta_fraction(a,b,x)/a:
        1-front*beta_fraction(b,a,1-x)/b;
    return isfinite(p)?fmax(0,fmin(1,p)):NAN;
}

typedef struct { double se[26],t[26],p[26]; int df; } CoefficientTests;

static double f_probability(double value,int numerator_df,int denominator_df)
{
    if(numerator_df<=0||denominator_df<=0||isnan(value)||value<0)return NAN;
    if(isinf(value))return 0;
    if(value==0)return 1;
    double x=1/(1+((double)numerator_df/denominator_df)*value);
    if(x<=0)return 0;
    if(x>=1)return 1;
    double a=denominator_df/2.0,b=numerator_df/2.0;
    double front=exp(lgamma(a+b)-lgamma(a)-lgamma(b)+a*log(x)+b*log1p(-x));
    double probability=x<(a+1)/(a+b+2)?front*beta_fraction(a,b,x)/a:
        1-front*beta_fraction(b,a,1-x)/b;
    return isfinite(probability)?fmax(0,fmin(1,probability)):NAN;
}

static int overall_f_test(const RegressionGraph *g,double *value,double *probability,int *df1,int *df2)
{
    int indices[26],count=term_indices(model_terms(&g->model,g->degree),indices);
    *value=*probability=NAN;*df1=count-1;*df2=g->count-count;
    if(g->model.family||*df1<=0||*df2<=0)return -1;
    double mean=0;
    for(int i=0;i<g->count;i++) {
        if(!isfinite(g->y[i])||(g->model.log_y&&g->y[i]<=0))return -1;
        mean+=g->model.log_y?log(g->y[i]):g->y[i];
    }
    mean/=g->count;double sse=0,sst=0;
    for(int i=0;i<g->count;i++) {
        double y=g->model.log_y?log(g->y[i]):g->y[i];
        double z=(g->x[i]-g->model.mean)/g->model.scale;
        double fit=g->model.coefficients[g->degree];
        for(int j=g->degree-1;j>=0;j--)fit=fit*z+g->model.coefficients[j];
        sse+=(y-fit)*(y-fit);sst+=(y-mean)*(y-mean);
    }
    if(!isfinite(sse)||!isfinite(sst)||sst<=0)return -1;
    *value=sse==0?INFINITY:(fmax(0,sst-sse)/ *df1)/(sse/ *df2);
    *probability=f_probability(*value,*df1,*df2);
    return isnan(*probability)?-1:0;
}

/* Tests use the same centered/scaled polynomial basis as the fitted model. */
static int coefficient_tests(const double *x,const double *y,int n,int degree,
                             const RegressionModel *model,CoefficientTests *out)
{
    int indices[26],count=term_indices(model_terms(model,degree),indices);out->df=n-count;
    for(int j=0;j<26;j++)out->se[j]=out->t[j]=out->p[j]=NAN;
    if(model->family) {
        int p=count+1;out->df=n-p;if(out->df<=0)return -1;
        double matrix[27*27],rhs[27];double sse=nonlinear_system(x,y,n,degree,model,matrix,rhs);
        if(!isfinite(sse))return -1;
        for(int j=0;j<count;j++) {
            double copy[27*27],unit[27]={0},inverse[27];memcpy(copy,matrix,(size_t)p*p*sizeof *copy);unit[j]=1;
            if(solve(copy,unit,p,inverse)||!isfinite(inverse[j])||inverse[j]<0)return -1;
            int k=indices[j];out->se[k]=sqrt(sse/out->df*inverse[j]);
            out->t[k]=out->se[k]>0?model->coefficients[k]/out->se[k]:model->coefficients[k]==0?NAN:copysign(INFINITY,model->coefficients[k]);
            out->p[k]=t_probability(out->t[k],out->df);
        }
        return 0;
    }
    if(out->df<=0)return -1;
    double matrix[26*26]={0},sse=0;
    for(int i=0;i<n;i++) {
        if(model->log_y&&y[i]<=0)return -1;
        double z=(x[i]-model->mean)/model->scale,powers[51];powers[0]=1;
        for(int j=1;j<=2*degree;j++)powers[j]=powers[j-1]*z;
        double fit=0;
        for(int r=0;r<count;r++) {
            fit+=model->coefficients[indices[r]]*powers[indices[r]];
            for(int c=0;c<count;c++)matrix[r*count+c]+=powers[indices[r]+indices[c]];
        }
        double residual=(model->log_y?log(y[i]):y[i])-fit;sse+=residual*residual;
    }
    for(int j=0;j<count;j++) {
        double copy[26*26],rhs[26]={0},inverse[26];
        memcpy(copy,matrix,(size_t)count*count*sizeof *copy);rhs[j]=1;
        if(solve(copy,rhs,count,inverse)||inverse[j]<0||!isfinite(inverse[j]))return -1;
        int k=indices[j];out->se[k]=sqrt(sse/out->df*inverse[j]);
        double beta=model->coefficients[k];
        if(out->se[k]==0)out->t[k]=beta==0?NAN:copysign(INFINITY,beta);
        else out->t[k]=beta/out->se[k];
        out->p[k]=t_probability(out->t[k],out->df);
    }
    return 0;
}

typedef struct {
    int count,df,indices[27];
    double covariance[27*27],sigma2,critical,level;
} MeanConfidence;

static double confidence_critical(double level,int df)
{
    if(!isfinite(level)||level<=0||level>=100||df<=0)return NAN;
    double tail=1-level/100,lo=0,hi=1;
    while(t_probability(hi,df)>tail&&hi<1e16)hi*=2;
    if(!isfinite(t_probability(hi,df))||t_probability(hi,df)>tail)return NAN;
    for(int i=0;i<80;i++) {
        double mid=(lo+hi)/2,p=t_probability(mid,df);
        if(!isfinite(p))return NAN;
        if(p>tail)lo=mid;else hi=mid;
    }
    return (lo+hi)/2;
}

/* Full covariance, including cross terms and the nonlinear shift. The
 * Jacobian of a log-y polynomial is on the fitted log-response scale. */
static int confidence_prepare(const RegressionGraph *g,double level,MeanConfidence *out)
{
    memset(out,0,sizeof *out);
    if(!g->model.valid||g->model.scale<=0||!isfinite(g->model.scale))return -1;
    int p=term_indices(model_terms(&g->model,g->degree),out->indices);
    if(g->model.family)out->indices[p++]=26;
    out->count=p;out->df=g->count-p;out->level=level;
    out->critical=confidence_critical(level,out->df);
    if(!isfinite(out->critical))return -1;
    double matrix[27*27]={0},scales[27];long double sse=0;
    for(int i=0;i<g->count;i++) {
        double gradient[27],fit=evaluate_model(&g->model,g->degree,g->x[i],gradient),response=g->y[i];
        if(g->model.log_y) {
            if(response<=0)return -1;
            response=log(response);fit=0;
            for(int j=0;j<p;j++)fit+=g->model.coefficients[out->indices[j]]*gradient[out->indices[j]];
        }
        if(!isfinite(fit)||!isfinite(response))return -1;
        long double residual=(long double)response-fit;sse+=residual*residual;
        for(int r=0;r<p;r++)for(int c=0;c<p;c++)
            matrix[r*p+c]+=gradient[out->indices[r]]*gradient[out->indices[c]];
    }
    out->sigma2=(double)(sse/out->df);if(!isfinite(out->sigma2))return -1;
    for(int r=0;r<p;r++) {
        if(!isfinite(matrix[r*p+r])||matrix[r*p+r]<=0)return -1;
        scales[r]=sqrt(matrix[r*p+r]);
    }
    for(int r=0;r<p;r++)for(int c=0;c<p;c++)matrix[r*p+c]/=scales[r]*scales[c];
    for(int c=0;c<p;c++) {
        double copy[27*27],unit[27]={0},inverse[27];
        memcpy(copy,matrix,(size_t)p*p*sizeof *copy);unit[c]=1;
        if(solve(copy,unit,p,inverse)||inverse[c]<0)return -1;
        for(int r=0;r<p;r++) {
            double value=out->sigma2*inverse[r]/scales[r]/scales[c];
            if(!isfinite(value))return -1;
            out->covariance[r*p+c]=value;
        }
    }
    return 0;
}

static int response_interval(const RegressionGraph *g,const MeanConfidence *ci,double x,int prediction,double *lower,double *upper)
{
    double gradient[27],fit=evaluate_model(&g->model,g->degree,x,gradient);
    if(!g->model.log_y&&!isfinite(fit))return -1;
    long double variance=0,magnitude=0;
    for(int r=0;r<ci->count;r++)for(int c=0;c<ci->count;c++) {
        long double value=(long double)gradient[ci->indices[r]]*ci->covariance[r*ci->count+c]*gradient[ci->indices[c]];
        variance+=value;magnitude+=fabsl(value);
    }
    if(!isfinite((double)variance)||variance < -1e-12L*magnitude)return -1;
    if(variance<0)variance=0;
    if(g->model.log_y) {
        /* Predict an individual response on the log scale, then transform
         * both limits. Mean intervals instead use the lognormal mean
         * correction and uncertainty in sigma2 (independent of beta). */
        fit=0;
        for(int j=0;j<ci->count;j++)fit+=g->model.coefficients[ci->indices[j]]*gradient[ci->indices[j]];
        if(prediction)variance+=ci->sigma2;
        else {
            fit+=ci->sigma2/2;
            variance+=(long double)ci->sigma2*ci->sigma2/(2*ci->df);
        }
        double half=ci->critical*sqrt((double)variance);
        *lower=exp(fit-half);*upper=exp(fit+half);
    } else {
        double half=ci->critical*sqrt((double)variance+(prediction?ci->sigma2:0));
        *lower=fit-half;*upper=fit+half;
    }
    return isfinite(*lower)&&isfinite(*upper)?0:-1;
}

static int confidence_interval(const RegressionGraph *g,const MeanConfidence *ci,double x,double *lower,double *upper)
{
    return response_interval(g,ci,x,0,lower,upper);
}

static int prediction_interval(const RegressionGraph *g,const MeanConfidence *ci,double x,double *lower,double *upper)
{
    return response_interval(g,ci,x,1,lower,upper);
}

/* Older files saved only predicted observations. Recover their polynomial,
 * checking both ordinary and log-y fits against those saved predictions. */
static int recover_model(RegressionGraph *g)
{
    double *pred=malloc((size_t)g->count*sizeof *pred);
    if(!pred)return -1;
    double best_error=INFINITY;
    for(int logy=0;logy<=1;logy++) {
        RegressionModel trial; double score;
        if(fit_degree(g->x,g->fit,g->count,g->degree,logy,pred,&score,&trial))continue;
        double error=0;
        for(int i=0;i<g->count;i++) {
            double residual=(pred[i]-g->fit[i])/fmax(1.0,fabs(g->fit[i]));
            error+=residual*residual;
        }
        if(error<best_error){best_error=error;g->model=trial;}
    }
    free(pred);
    return g->model.valid?0:-1;
}

static int suggest_graph_name(const StockList *l,const RegressionGraph *g,char name[256],char *err,size_t errn)
{
    const char *stem=g->file_stem[0]?g->file_stem:"regression";
    for(int k=1;k<1000;k++) {
        char path[PATH_MAX];
        if(snprintf(name,256,"%s_%02d.graph",stem,k)>=256 ||
           snprintf(path,sizeof path,"%s/graphs/%s",l->dir,name)>=(int)sizeof path){snprintf(err,errn,"Graph path too long");return -1;}
        if(access(path,F_OK)!=0){if(errno==ENOENT)return 0;snprintf(err,errn,"Cannot check graph name: %s",strerror(errno));return -1;}
    }
    snprintf(err,errn,"Too many graphs with this name");return -1;
}

static int save_graph(const StockList *l, const RegressionGraph *g, const char *name,char *err, size_t errn)
{
    if(!name[0]||strchr(name,'/')||strchr(name,'\\')||!strcmp(name,".")||!strcmp(name,"..")){
        snprintf(err,errn,"Enter a filename without a directory");return -1;
    }
    size_t len=strlen(name);char filename[256];
    int extension=len>=6&&!strcmp(name+len-6,".graph");
    if(snprintf(filename,sizeof filename,"%s%s",name,extension?"":".graph")>=(int)sizeof filename){snprintf(err,errn,"Filename too long");return -1;}
    char dir[PATH_MAX]; if(snprintf(dir,sizeof dir,"%s/graphs",l->dir)>=(int)sizeof dir){snprintf(err,errn,"Graph path too long");return -1;}
    if(mkdir(dir,0755)&&errno!=EEXIST){snprintf(err,errn,"Cannot create graphs: %s",strerror(errno));return -1;}
    char path[PATH_MAX];
    if(snprintf(path,sizeof path,"%s/%s",dir,filename)>=(int)sizeof path){snprintf(err,errn,"Graph path too long");return -1;}
    FILE *f=fopen(path,"wx");if(!f){snprintf(err,errn,errno==EEXIST?"Name already exists; choose another":"Cannot save graph: %s",strerror(errno));return -1;}
    fprintf(f,"CANDLES_GRAPH 8\n%s\n%s\n%s\n%d %.17g %d %d %d %d %d %ld %ld\n",
            g->title, g->x_name, g->y_name, g->degree, g->adjusted_r2, g->count,
            g->x_is_time, g->y_is_time, g->x_time_unit, g->y_time_unit,
            g->x_epoch, g->y_epoch);
    fprintf(f,"%.17g\n",g->significance);
    fprintf(f,"%u\n",model_terms(&g->model,g->degree));
    fprintf(f,"%d %.17g %d\n",g->model.family,g->model.shift,g->model.exp_sign);
    fprintf(f,"%d %.17g %.17g\n",g->model.log_y,g->model.mean,g->model.scale);
    for(int k=0;k<=g->degree;k++)fprintf(f,"%.17g\n",g->model.coefficients[k]);
    for(int i=0;i<g->count;i++) fprintf(f,"%.17g,%.17g,%.17g\n",g->x[i],g->y[i],g->fit[i]);
    int failed=ferror(f);if(fclose(f))failed=1;
    if(failed){unlink(path);snprintf(err,errn,"Failed writing graph");return -1;}
    return 0;
}

static void trim_newline(char *s)
{
    s[strcspn(s, "\r\n")] = '\0';
}

static int load_graph(const char *path, RegressionGraph *g, char *err, size_t errn)
{
    char magic[32], line[256];
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errn, "Cannot open graph: %s", strerror(errno)); return -1; }
    if (!fgets(magic, sizeof magic, f) ||
        (strcmp(magic, "CANDLES_GRAPH 1\n") != 0 && strcmp(magic, "CANDLES_GRAPH 2\n") != 0 &&
         strcmp(magic, "CANDLES_GRAPH 3\n") != 0 && strcmp(magic, "CANDLES_GRAPH 4\n") != 0 && strcmp(magic, "CANDLES_GRAPH 5\n") != 0 && strcmp(magic, "CANDLES_GRAPH 6\n") != 0 && strcmp(magic,"CANDLES_GRAPH 7\n")!=0 && strcmp(magic,"CANDLES_GRAPH 8\n")!=0) ||
        !fgets(g->title, sizeof g->title, f) || !fgets(g->x_name, sizeof g->x_name, f) ||
        !fgets(g->y_name, sizeof g->y_name, f) || !fgets(line, sizeof line, f) ||
        sscanf(line, "%d %lf %d", &g->degree, &g->adjusted_r2, &g->count) != 3 ||
        g->count < 1 || g->degree < (magic[14]>='5'?0:1) || g->degree > 25) {
        fclose(f); regression_graph_free(g); snprintf(err, errn, "Invalid graph file"); return -1;
    }
    if (magic[14] >= '2' &&
        sscanf(line, "%d %lf %d %d %d %d %d %ld %ld", &g->degree, &g->adjusted_r2,
               &g->count, &g->x_is_time, &g->y_is_time, &g->x_time_unit,
               &g->y_time_unit, &g->x_epoch, &g->y_epoch) != 9) {
        fclose(f); regression_graph_free(g); snprintf(err, errn, "Invalid graph file"); return -1;
    }
    g->significance=0;
    if(magic[14]>='6') {
        if(!fgets(line,sizeof line,f)||sscanf(line,"%lf",&g->significance)!=1||
           !isfinite(g->significance)||g->significance<0||g->significance>=1) {
            fclose(f);regression_graph_free(g);snprintf(err,errn,"Invalid graph significance");return -1;
        }
    } else {
        const char *alpha=strstr(g->title," | significance alpha ");
        double value;
        if(alpha&&sscanf(alpha," | significance alpha %lf",&value)==1&&isfinite(value)&&value>0&&value<1)g->significance=value;
    }
    g->model.terms=(1u<<(g->degree+1))-1;
    if(magic[14]>='7') {
        if(!fgets(line,sizeof line,f)||sscanf(line,"%u",&g->model.terms)!=1||
           !(g->model.terms&1u)||!(g->model.terms&(1u<<g->degree))||
           (g->model.terms>>(g->degree+1))) {
            fclose(f);regression_graph_free(g);snprintf(err,errn,"Invalid graph terms");return -1;
        }
    }
    g->model.family=0;g->model.shift=0;g->model.exp_sign=0;
    if(magic[14]>='8') {
        if(!fgets(line,sizeof line,f)||sscanf(line,"%d %lf %d",&g->model.family,&g->model.shift,&g->model.exp_sign)!=3||
           g->model.family<0||g->model.family>2||!isfinite(g->model.shift)||
           (g->model.family==FAMILY_EXPONENTIAL&&g->model.exp_sign!=1&&g->model.exp_sign!=-1)) {
            fclose(f);regression_graph_free(g);snprintf(err,errn,"Invalid graph family");return -1;
        }
    }
    if(magic[14]>='3') {
        if(!fgets(line,sizeof line,f)||
           sscanf(line,"%d %lf %lf",&g->model.log_y,&g->model.mean,&g->model.scale)!=3||
           !isfinite(g->model.mean)||!isfinite(g->model.scale)||g->model.scale<=0) {
            fclose(f);regression_graph_free(g);snprintf(err,errn,"Invalid graph model");return -1;
        }
        for(int k=0;k<=g->degree;k++) {
            if(!fgets(line,sizeof line,f)||sscanf(line,"%lf",&g->model.coefficients[k])!=1||
               !isfinite(g->model.coefficients[k])) {
                fclose(f);regression_graph_free(g);snprintf(err,errn,"Invalid graph coefficients");return -1;
            }
        }
        g->model.valid=1;
        for(int k=0;k<=g->degree;k++)if(!(g->model.terms&(1u<<k))&&g->model.coefficients[k]!=0){
            fclose(f);regression_graph_free(g);snprintf(err,errn,"Invalid removed coefficient");return -1;
        }
    }
    trim_newline(g->title); trim_newline(g->x_name); trim_newline(g->y_name);
    g->x = malloc((size_t)g->count * sizeof *g->x);
    g->y = malloc((size_t)g->count * sizeof *g->y);
    g->fit = malloc((size_t)g->count * sizeof *g->fit);
    if (!g->x || !g->y || !g->fit) {
        fclose(f); regression_graph_free(g); snprintf(err, errn, "Out of memory"); return -1;
    }
    for (int i = 0; i < g->count; i++)
        if (!fgets(line, sizeof line, f) || sscanf(line, "%lf,%lf,%lf", &g->x[i], &g->y[i], &g->fit[i]) != 3) {
            fclose(f); regression_graph_free(g); snprintf(err, errn, "Invalid graph points"); return -1;
        }
    fclose(f);
    if(!g->model.valid&&recover_model(g)) {
        regression_graph_free(g);snprintf(err,errn,"Cannot recover saved regression curve");return -1;
    }
    const char *base=strrchr(path,'/');base=base?base+1:path;
    size_t stem_len=strlen(base);if(stem_len>=6&&!strcmp(base+stem_len-6,".graph"))stem_len-=6;
    size_t digits=stem_len;while(digits>0&&isdigit((unsigned char)base[digits-1]))digits--;
    if(digits>0&&digits<stem_len&&base[digits-1]=='_')stem_len=digits-1;
    snprintf(g->file_stem,sizeof g->file_stem,"%.*s",(int)stem_len,base);
    return 0;
}

static int open_graph(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l,
                      char *err, size_t errn, RegressionGraph *g)
{
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/graphs", l->dir) >= (int)sizeof dir) {
        snprintf(err, errn, "Graph path too long"); return -1;
    }
    DIR *d = opendir(dir);
    if (!d) { snprintf(err, errn, "No saved graphs"); return -1; }
    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        if (n < 7 || strcmp(entry->d_name + n - 6, ".graph")) continue;
        if (count == cap) {
            int next = cap ? cap * 2 : 8;
            char **tmp = realloc(names, (size_t)next * sizeof *names);
            if (!tmp) { closedir(d); goto oom; }
            names = tmp; cap = next;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) { closedir(d); goto oom; }
        count++;
    }
    closedir(d);
    if (!count) { free(names); snprintf(err, errn, "No saved graphs"); return -1; }
    int selected = menu_pick_existing(cfg, draw, ctx, "Open graph", names, count, 0);
    if (selected < 0) { for (int i=0;i<count;i++) free(names[i]); free(names); return 0; }
    char path[PATH_MAX];
    int w = snprintf(path, sizeof path, "%s/%s", dir, names[selected]);
    for (int i=0;i<count;i++) free(names[i]);
    free(names);
    if (w < 0 || (size_t)w >= sizeof path) { snprintf(err, errn, "Graph path too long"); return -1; }
    regression_graph_free(g);
    return load_graph(path, g, err, errn) ? -1 : 1;

oom:
    for (int i=0;i<count;i++) free(names[i]);
    free(names); snprintf(err, errn, "Out of memory"); return -1;
}

static void describe_axis(const StockList *list, const Spec *s, int y_axis,
                          char *label, size_t n, char token[64])
{
    int variable=y_axis?s->y:s->x;
    const char *name=variable==VAR_TIME?"Time":variable==VAR_DATA?
        (y_axis?s->y_data_name:s->x_data_name):list->tickers[variable];
    if(variable==VAR_TIME) {
        snprintf(label,n,"%c: Time",y_axis?'y':'x');
        snprintf(token,64,"time");
        return;
    }
    int change=y_axis?s->y_change:s->x_change;
    int log_value=!s->family&&(y_axis?s->log_y:s->log_x);
    const char *kind=variable==VAR_DATA?"data":"close";
    if(!change) {
        snprintf(label,n,"%c: %s (%s%s)",y_axis?'y':'x',name,log_value?"log ":"",kind);
        snprintf(token,64,"%s-%slevel",kind,log_value?"log-":"");
        return;
    }
    int forward=(y_axis?s->y_direction:s->x_direction)==DIR_FORWARD;
    int absolute=(y_axis?s->y_change_type:s->x_change_type)==CHANGE_ABSOLUTE;
    int log_return=y_axis?s->y_log_return:s->x_log_return;
    int instant=y_axis?s->y_instant:s->x_instant;
    int days=y_axis?s->y_days:s->x_days;
    char period[64], short_period[24];
    if(instant) {
        snprintf(period,sizeof period,"closest %s",forward?"next":"previous");
        snprintf(short_period,sizeof short_period,"closest");
    } else {
        snprintf(period,sizeof period,"%dd",days);
        snprintf(short_period,sizeof short_period,"%dd",days);
    }
    snprintf(label,n,"%c: %s (%s; %s, %s, %s)",y_axis?'y':'x',name,kind,
             log_return?"log return (%)":absolute?"absolute change":"% change",forward?"forward":"backward",period);
    snprintf(token,64,"%s-%s-%s-%s",kind,log_return?"logret":absolute?"abs":"pct",forward?"fwd":"back",
             short_period);
}

static void filename_part(const char *name, char out[64])
{
    int j=0;
    for(int i=0;name[i]&&j<63;i++) {
        unsigned char ch=(unsigned char)name[i];
        out[j++]=(isalnum(ch)||ch=='_'||ch=='-'||ch=='.'||ch=='^'||ch=='=')?(char)ch:'_';
    }
    if(!j)out[j++]='_';
    out[j]='\0';
}

static const char *score_label(double score, char out[32])
{
    if(isfinite(score))snprintf(out,32,"%.4g",score);
    else snprintf(out,32,"n/a");
    return out;
}

static void describe_graph(const StockList *list, const Spec *s, RegressionGraph *g)
{
    char y[384],x[384],yt[64],xt[64],sampling[64]="",sampling_token[32]="";
    describe_axis(list,s,1,y,sizeof y,yt);
    describe_axis(list,s,0,x,sizeof x,xt);
    if(s->sample_mode==SAMPLE_DISTANCE) {
        snprintf(sampling,sizeof sampling," | sample: >= %d days",s->sample_days);
        snprintf(sampling_token,sizeof sampling_token,"__sample-%dd",s->sample_days);
    } else if(s->sample_mode==SAMPLE_MONTH) {
        snprintf(sampling,sizeof sampling," | sample: first eligible/month");
        snprintf(sampling_token,sizeof sampling_token,"__sample-month");
    } else if(s->sample_mode==SAMPLE_YEAR) {
        snprintf(sampling,sizeof sampling," | sample: first eligible/year");
        snprintf(sampling_token,sizeof sampling_token,"__sample-year");
    }
    int zero=s->x_change&&s->exclude_zero_x;
    int prior=s->stock_prior_close && (s->x>=0 || s->y>=0) &&
              s->x!=VAR_TIME && s->y!=VAR_TIME;
    char score[32],alpha[96]="";
    if(s->significance>0)snprintf(alpha,sizeof alpha," | significance alpha %.6g (%s)",s->significance,s->exact_degree?(s->remove_intermediate?"lower terms; highest retained":"display only"):s->remove_intermediate?"all slope terms":"trailing terms");
    snprintf(g->title,sizeof g->title,"%s | %s%s%s%s%s%s%s | degree %d (max %d), adj R2 %s",
             y,x,zero?" | zero x changes excluded":"",
             prior?" | stocks use prior close on missing dates":"",sampling,
             s->sampled_changes==3?" | selected dates for x and y changes":s->sampled_changes==1?" | selected dates for x changes":s->sampled_changes==2?" | selected dates for y changes":"",
             alpha,s->exact_degree?" | exact degree":"",g->degree,s->max_degree,score_label(g->adjusted_r2,score));
    if(s->family) {
        size_t used=strlen(g->title);
        snprintf(g->title+used,sizeof g->title-used," | family: %s, shift %.12g",s->family==FAMILY_EXPONENTIAL?"shifted exponential":"shifted logarithmic",g->model.shift);
    }
    if(s->y_change&&s->exclude_zero_y) {
        size_t used=strlen(g->title);
        snprintf(g->title+used,sizeof g->title-used," | zero y changes excluded");
    }
    {
        size_t used=strlen(g->title);
        snprintf(g->title+used,sizeof g->title-used," | polynomial input: %s",s->standardize?"standardized":"unstandardized");
    }
    char suffix[216],yn[64],xn[64],alpha_token[32]="";
    int zero_y=s->y_change&&s->exclude_zero_y;
    const char *zero_token=zero?(zero_y?"__nonzero-xy":"__nonzero-x"):(zero_y?"__nonzero-y":"");
    if(s->significance>0)snprintf(alpha_token,sizeof alpha_token,"__alpha-%.6g",s->significance);
    snprintf(suffix,sizeof suffix,"__y-%s__x-%s%s%s%s%s%s%s%s%s__max%d-fit%d",yt,xt,
             zero_token,prior?"__prior-stock-close":"",sampling_token,
             s->sampled_changes==3?"__selected-xy":s->sampled_changes==1?"__selected-x":s->sampled_changes==2?"__selected-y":"",
             alpha_token,s->exact_degree?"__exact":"",s->remove_intermediate&&s->significance>0?"__sparse":"",s->family==FAMILY_EXPONENTIAL?"__exp":s->family==FAMILY_LOGARITHMIC?"__log":"",s->max_degree,g->degree);
    filename_part(g->y_name,yn);
    filename_part(g->x_name,xn);
    /* Leave room for both variable names, all settings, and the counter and
     * extension added by save_graph, within the usual 255-byte filename cap. */
    int available=(int)sizeof g->file_stem-2-(int)strlen(suffix);
    int yl=(int)strlen(yn),xl=(int)strlen(xn);
    while(yl+xl>available) { if(yl>=xl)yl--;else xl--; }
    snprintf(g->file_stem,sizeof g->file_stem,"%.*s_%.*s%s",yl,yn,xl,xn,suffix);
}

/* Failure belongs to this candidate, not to the whole degree search. */
typedef struct {
    RegressionModel model;
    double aic, cp, r2_adj, sse;
    int degree, param_count;
    unsigned terms;
    double *fit;
} ModelCandidate;

/* Calculate AIC for a model. Lower is better. */
static double calculate_aic(int n, int p, double sse)
{
    if(n<=p||sse<=0||!isfinite(sse))return INFINITY;
    return n*log(sse/n)+2*p;
}

/* Calculate Cp of Mallows. Lower is better. */
static double calculate_cp(int n, int p, double sse, double sigma2_full)
{
    if(sigma2_full<=0||!isfinite(sse)||!isfinite(sigma2_full))return INFINITY;
    return sse/sigma2_full-(n-2*p);
}

static int apply_significance(const double *x,const double *y,int n,const Spec *sp,
                              int *degree,double *pred,double *score,RegressionModel *model)
{
    if(sp->significance<=0||(sp->exact_degree&&(!sp->remove_intermediate||*degree<=1)))return 0;
    while(*degree>0) {
        CoefficientTests tests;
        if(coefficient_tests(x,y,n,*degree,model,&tests))return sp->exact_degree?0:-1;
        unsigned terms=model_terms(model,*degree);int drop=-1;double worst=-1;
        int last=sp->exact_degree?*degree-1:*degree;
        for(int j=sp->remove_intermediate?1:*degree;j<=last;j++) {
            if(!(terms&(1u<<j)))continue;
            double probability=tests.p[j];
            if(!isfinite(probability)) {
                if(model->coefficients[j]!=0)return sp->exact_degree?0:-1;
                probability=1;
            }
            if(probability>sp->significance&&probability>worst){worst=probability;drop=j;}
        }
        if(drop<0)break;
        terms&=~(1u<<drop);
        int reduced=*degree;
        while(reduced>0&&!(terms&(1u<<reduced)))reduced--;
        RegressionModel candidate;double candidate_score;
        if(fit_family(x,y,n,reduced,!sp->family&&sp->log_y,terms,sp->family,sp->standardize,pred,&candidate_score,&candidate))return sp->exact_degree?0:-1;
        *degree=reduced;*model=candidate;*score=candidate_score;
    }
    return 0;
}

/* Calculate SSE for model on the appropriate scale (for log_y models, SSE is on log scale) */
static double calculate_model_sse(const double *x, const double *y, int n, 
                                  const RegressionModel *model, int degree, int logy)
{
    double sse=0;
    for(int i=0;i<n;i++) {
        double yval=logy?log(y[i]):y[i];
        if(!isfinite(yval))return INFINITY;
        
        /* Evaluate model prediction on transformed scale */
        double z=(x[i]-model->mean)/model->scale;
        double fit=0;
        for(int j=0;j<=degree;j++) {
            if(model->terms&(1u<<j)) {
                fit+=model->coefficients[j]*pow(z,j);
            }
        }
        
        if(!isfinite(fit))return INFINITY;
        double resid=yval-fit;
        sse+=resid*resid;
    }
    return sse;
}

/* Perform exhaustive model selection for a given degree.
 * Returns array of candidates and count via out parameters. */
static int exhaustive_search(const double *x, const double *y, int n, const Spec *sp,
                             int degree, double sigma2_full, ModelCandidate **out_candidates, int *out_count)
{
    if(degree<0||degree>25)return -1;
    
    /* For degree 0, only one model (intercept only) */
    if(degree==0) {
        ModelCandidate *candidates=malloc(sizeof(ModelCandidate));
        if(!candidates)return -1;
        
        double *trial_fit=malloc((size_t)n*sizeof(double));
        if(!trial_fit){free(candidates);return -1;}
        
        double score;
        RegressionModel trial_model;
        unsigned terms=1u; /* Only intercept */
        
        if(fit_family(x,y,n,0,!sp->family&&sp->log_y,terms,sp->family,sp->standardize,
                     trial_fit,&score,&trial_model)) {
            free(trial_fit);free(candidates);
            *out_candidates=NULL;*out_count=0;
            return 0;
        }
        
        int param_count=1;
        if(sp->family)param_count++;
        double sse=calculate_model_sse(x,y,n,&trial_model,0,!sp->family&&sp->log_y);
        
        if(!isfinite(sse)||n<=param_count){free(trial_fit);free(candidates);*out_candidates=NULL;*out_count=0;return 0;}
        
        candidates[0].model=trial_model;
        candidates[0].aic=calculate_aic(n,param_count,sse);
        candidates[0].cp=calculate_cp(n,param_count,sse,sigma2_full);
        candidates[0].r2_adj=score;
        candidates[0].sse=sse;
        candidates[0].degree=0;
        candidates[0].param_count=param_count;
        candidates[0].terms=terms;
        candidates[0].fit=trial_fit;
        
        *out_candidates=candidates;
        *out_count=1;
        return 0;
    }
    
    /* Generate all possible term combinations for degree > 0 */
    unsigned base_terms=1u<<degree; /* Must include highest degree */
    int max_models=1<<(degree-1); /* Only powers 1 .. degree-1 are optional. */
    
    ModelCandidate *candidates=malloc((size_t)max_models*sizeof(ModelCandidate));
    if(!candidates)return -1;
    
    int count=0;
    
    /* Try all subsets that include intercept (term 0) and highest degree term */
    for(unsigned subset=0;subset<(1u<<(degree-1));subset++) {
        /* Always include intercept (bit 0) and highest degree (bit 'degree') */
        unsigned terms=base_terms|(subset<<1)|1u;
        
        /* Count parameters */
        int param_count=0;
        for(int j=0;j<=degree;j++)if(terms&(1u<<j))param_count++;
        if(sp->family)param_count++; /* Add 1 for shift parameter in nonlinear families */
        
        if(n<=param_count)continue; /* Not enough data */
        
        /* Fit the model */
        double *trial_fit=malloc((size_t)n*sizeof(double));
        if(!trial_fit){free(candidates);return -1;}
        
        double score;
        RegressionModel trial_model;
        if(fit_family(x,y,n,degree,!sp->family&&sp->log_y,terms,sp->family,sp->standardize,
                     trial_fit,&score,&trial_model)) {
            free(trial_fit);
            continue;
        }
        
        /* Calculate SSE on the appropriate scale */
        double sse=calculate_model_sse(x,y,n,&trial_model,degree,!sp->family&&sp->log_y);
        
        if(!isfinite(sse)){free(trial_fit);continue;}
        
        /* Calculate AIC and Cp */
        double aic=calculate_aic(n,param_count,sse);
        double cp=calculate_cp(n,param_count,sse,sigma2_full);
        
        /* Store candidate */
        candidates[count].model=trial_model;
        candidates[count].aic=aic;
        candidates[count].cp=cp;
        candidates[count].r2_adj=score;
        candidates[count].sse=sse;
        candidates[count].degree=degree;
        candidates[count].param_count=param_count;
        candidates[count].terms=terms;
        candidates[count].fit=trial_fit;
        count++;
    }
    
    *out_candidates=candidates;
    *out_count=count;
    return 0;
}

/* Comparison function for sorting candidates by AIC */
static int compare_by_aic(const void *a, const void *b)
{
    const ModelCandidate *ca=(const ModelCandidate*)a;
    const ModelCandidate *cb=(const ModelCandidate*)b;
    if(ca->aic<cb->aic)return -1;
    if(ca->aic>cb->aic)return 1;
    /* Tie-break by parameter count (prefer simpler) */
    if(ca->param_count<cb->param_count)return -1;
    if(ca->param_count>cb->param_count)return 1;
    return 0;
}

/* Present model selection menu and return user's choice */
static int select_model_menu(const Settings *cfg, DrawFn draw, void *ctx,
                            ModelCandidate *candidates, int count)
{
    if(count==0)return -1;
    
    /* Build menu items */
    char **items=malloc((size_t)(count+1)*sizeof(char*));
    if(!items)return -1;
    
    /* Find lowest AIC for delta calculation */
    double min_aic=candidates[0].aic;
    
    for(int i=0;i<count;i++) {
        items[i]=malloc(256);
        if(!items[i]) {
            for(int j=0;j<i;j++)free(items[j]);
            free(items);
            return -1;
        }
        double aic_delta=candidates[i].aic-min_aic;
        snprintf(items[i],256,"AIC %.4g (Δ %.4g) | p=%d | adj-R² %.4g",
                candidates[i].aic,aic_delta,candidates[i].param_count,
                candidates[i].r2_adj);
    }
    
    items[count]=NULL;
    
    int selected=menu_pick_existing(cfg,draw,ctx,"Select model",items,count,0);
    
    for(int i=0;i<count;i++)free(items[i]);
    free(items);
    
    return selected;
}

/* 0: graph produced, 1: cancelled, -1: error. */
static int generate(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l, Spec *sp, char *err, size_t errn, RegressionGraph *g)
{
    (void)cfg;(void)draw;(void)ctx;
    if(sp->x==VAR_NONE||sp->y==VAR_NONE||(sp->x==VAR_TIME&&sp->y==VAR_TIME)){snprintf(err,errn,"Choose one stock and both variables");return -1;}
    Stock xs={0},ys={0}; DataSeries xd={0},yd={0}; char e[256];
    if(sp->x>=0&&stock_load(&xs,l->dir,l->files[sp->x],l->tickers[sp->x],e,sizeof e)){snprintf(err,errn,"%s",e);return -1;}
    if(sp->x==VAR_DATA&&data_load(&xd,l->dir,sp->x_data_file,sp->x_data_name,e,sizeof e)){stock_free(&xs);snprintf(err,errn,"%s",e);return -1;}
    if(sp->y>=0&&stock_load(&ys,l->dir,l->files[sp->y],l->tickers[sp->y],e,sizeof e)){stock_free(&xs);data_free(&xd);snprintf(err,errn,"%s",e);return -1;}
    if(sp->y==VAR_DATA&&data_load(&yd,l->dir,sp->y_data_file,sp->y_data_name,e,sizeof e)){stock_free(&xs);stock_free(&ys);data_free(&xd);snprintf(err,errn,"%s",e);return -1;}
    /* Normally stocks anchor a graph, so only mutual trading dates enter it.
     * With prior-close matching on, a stock/data pair instead visits every
     * imported-data date.  That is what lets a weekend/holiday data point use
     * Friday's (or the preceding session's) stock close. */
    int stock_data_pair=sp->stock_prior_close &&
                        ((sp->x>=0 && sp->y==VAR_DATA) ||
                         (sp->y>=0 && sp->x==VAR_DATA));
    Stock *anchor_stock=stock_data_pair?NULL:(sp->y>=0?&ys:sp->x>=0?&xs:NULL);
    DataSeries *anchor_data=anchor_stock?NULL:sp->y==VAR_DATA?&yd:&xd;
    DataSeries union_dates={0};
    if(sp->stock_prior_close && sp->x>=0 && sp->y>=0) {
        union_dates.points=malloc(((size_t)xs.count+ys.count)*sizeof *union_dates.points);
        if(!union_dates.points){stock_free(&xs);stock_free(&ys);snprintf(err,errn,"Out of memory");return -1;}
        int a=0,b=0;
        while(a<xs.count || b<ys.count) {
            const char *date;
            if(b==ys.count || (a<xs.count && strcmp(xs.candles[a].date,ys.candles[b].date)<=0))date=xs.candles[a++].date;
            else date=ys.candles[b++].date;
            if(!union_dates.count || strcmp(date,union_dates.points[union_dates.count-1].date))
                memcpy(union_dates.points[union_dates.count++].date,date,11);
        }
        anchor_stock=NULL;anchor_data=&union_dates;
    }
    int anchor_count=anchor_stock?anchor_stock->count:anchor_data->count;
    double *x=malloc((size_t)anchor_count*sizeof*x),*y=malloc((size_t)anchor_count*sizeof*y);
    if(!x||!y){free(x);free(y);data_free(&union_dates);stock_free(&xs);stock_free(&ys);data_free(&xd);data_free(&yd);snprintf(err,errn,"Out of memory");return -1;}
    const char *first_date=anchor_stock?anchor_stock->candles[0].date:anchor_data->points[0].date;
    long first=day_number(first_date), last_sample=LONG_MIN, last_bucket=LONG_MIN;int n=0;
    long *selected_dates=NULL; double *selected_x=NULL,*selected_y=NULL; int selected_count=0;
    int active_sampled=sp->sample_mode==SAMPLE_ALL?0:
        sp->sampled_changes & ((sp->x_change?1:0)|(sp->y_change?2:0));
    if(active_sampled) {
        selected_dates=malloc((size_t)anchor_count*sizeof *selected_dates);
        selected_x=malloc((size_t)anchor_count*sizeof *selected_x);
        selected_y=malloc((size_t)anchor_count*sizeof *selected_y);
        if(!selected_dates||!selected_x||!selected_y) {
            free(selected_dates);free(selected_x);free(selected_y);free(x);free(y);
            data_free(&union_dates);stock_free(&xs);stock_free(&ys);data_free(&xd);data_free(&yd);
            snprintf(err,errn,"Out of memory");return -1;
        }
        long last=LONG_MIN,bucket_last=LONG_MIN;
        for(int i=0;i<anchor_count;i++) {
            const char *d=anchor_stock?anchor_stock->candles[i].date:anchor_data->points[i].date;
            long day=day_number(d);
            int ix=sp->x>=0?find_date(&xs,d,sp->stock_prior_close):-1,iy=sp->y>=0?find_date(&ys,d,sp->stock_prior_close):-1;
            int dx=sp->x==VAR_DATA?data_find_date(&xd,d):-1,dy=sp->y==VAR_DATA?data_find_date(&yd,d):-1;
            if((sp->x>=0&&ix<0)||(sp->y>=0&&iy<0)||(sp->x==VAR_DATA&&dx<0)||(sp->y==VAR_DATA&&dy<0))continue;
            int year=(d[0]-'0')*1000+(d[1]-'0')*100+(d[2]-'0')*10+d[3]-'0';
            int month=(d[5]-'0')*10+d[6]-'0';
            long bucket=sp->sample_mode==SAMPLE_MONTH?(long)year*12+month:year;
            if(sp->sample_mode==SAMPLE_DISTANCE&&last!=LONG_MIN&&day-last<sp->sample_days)continue;
            if((sp->sample_mode==SAMPLE_MONTH||sp->sample_mode==SAMPLE_YEAR)&&bucket==bucket_last)continue;
            selected_dates[selected_count]=day;
            selected_x[selected_count]=sp->x>=0?xs.candles[ix].close:sp->x==VAR_DATA?xd.points[dx].value:0;
            selected_y[selected_count]=sp->y>=0?ys.candles[iy].close:sp->y==VAR_DATA?yd.points[dy].value:0;
            selected_count++;last=day;bucket_last=bucket;
        }
    }
    /* Thinning applies after source-date pairing but before a point enters the
     * model.  It is independent of every axis's return horizon. */
    int selected_i=0;
    for(int i=0;i<anchor_count;i++){
        const char *d=anchor_stock?anchor_stock->candles[i].date:anchor_data->points[i].date;
        long day=day_number(d);int ix=sp->x>=0?find_date(&xs,d,sp->stock_prior_close):-1,dx=sp->x==VAR_DATA?data_find_date(&xd,d):-1,iy=sp->y>=0?find_date(&ys,d,sp->stock_prior_close):-1,dy=sp->y==VAR_DATA?data_find_date(&yd,d):-1;
        if((sp->x>=0&&ix<0)||(sp->x==VAR_DATA&&dx<0)||(sp->y>=0&&iy<0)||(sp->y==VAR_DATA&&dy<0))continue;
        if(active_sampled) { if(selected_i>=selected_count||day!=selected_dates[selected_i])continue; }
        int year=(d[0]-'0')*1000+(d[1]-'0')*100+(d[2]-'0')*10+d[3]-'0';
        int month=(d[5]-'0')*10+d[6]-'0';
        long bucket=sp->sample_mode==SAMPLE_MONTH?(long)year*12+month:year;
        if(!active_sampled&&sp->sample_mode==SAMPLE_DISTANCE&&last_sample!=LONG_MIN&&day-last_sample<sp->sample_days)continue;
        if(!active_sampled&&(sp->sample_mode==SAMPLE_MONTH||sp->sample_mode==SAMPLE_YEAR)&&bucket==last_bucket)continue;
        int xtype=sp->x_log_return?CHANGE_LOG_RETURN:sp->x_change_type;
        int ytype=sp->y_log_return?CHANGE_LOG_RETURN:sp->y_change_type;
        double xv=sp->x==VAR_TIME?(day-first)/(double)sp->time_unit:sp->x==VAR_DATA?data_value(&xd,dx,sp->x_change,sp->x_days,sp->x_direction,sp->x_instant,xtype):stock_value(&xs,ix,sp->x_change,sp->x_days,sp->x_direction,sp->x_instant,xtype,day);
        double yv=sp->y==VAR_TIME?(day-first)/(double)sp->time_unit:sp->y==VAR_DATA?data_value(&yd,dy,sp->y_change,sp->y_days,sp->y_direction,sp->y_instant,ytype):stock_value(&ys,iy,sp->y_change,sp->y_days,sp->y_direction,sp->y_instant,ytype,day);
        if(active_sampled&1)xv=sampled_value(selected_x,selected_dates,selected_count,selected_i,sp->x_days,sp->x_direction,sp->x_instant,xtype);
        if(active_sampled&2)yv=sampled_value(selected_y,selected_dates,selected_count,selected_i,sp->y_days,sp->y_direction,sp->y_instant,ytype);
        if(active_sampled)selected_i++;
        if(sp->exclude_zero_x && sp->x_change && xv == 0.0) continue;
        if(sp->exclude_zero_y && sp->y_change && yv == 0.0) continue;
        if(!sp->family&&sp->log_x){if(xv<=0)continue;xv=log(xv);}
        if(!sp->family&&sp->log_y&&yv<=0)continue;
        if(!isfinite(xv)||!isfinite(yv))continue;
        x[n]=xv;y[n++]=yv;last_sample=day;if(sp->sample_mode==SAMPLE_MONTH||sp->sample_mode==SAMPLE_YEAR)last_bucket=bucket;
    }
    free(selected_dates);free(selected_x);free(selected_y);
    data_free(&union_dates);
    stock_free(&xs);stock_free(&ys);data_free(&xd);data_free(&yd); if(n<4){free(x);free(y);snprintf(err,errn,"Not enough matching observations");return -1;}
    
    double *best=NULL;
    RegressionModel best_model={0};
    int bestd=0;
    double bestscore=-INFINITY;
    
    /* Use exhaustive search if exact_degree is not set */
    if(!sp->exact_degree) {
        /* First fit full model at max_degree to get sigma2 for Cp calculation */
        double *full_fit=malloc((size_t)n*sizeof(double));
        if(!full_fit){free(x);free(y);snprintf(err,errn,"Out of memory");return -1;}
        
        RegressionModel full_model;
        double full_score;
        unsigned full_terms=(1u<<(sp->max_degree+1))-1;
        
        if(fit_family(x,y,n,sp->max_degree,!sp->family&&sp->log_y,full_terms,sp->family,
                     sp->standardize,full_fit,&full_score,&full_model)) {
            free(full_fit);free(x);free(y);
            snprintf(err,errn,"Cannot fit full model for exhaustive search");
            return -1;
        }
        
        double full_sse=calculate_model_sse(x,y,n,&full_model,sp->max_degree,!sp->family&&sp->log_y);
        int full_params=sp->max_degree+1; /* All terms from 0 to max_degree */
        if(sp->family)full_params++; /* Add shift parameter for nonlinear families */
        
        if(n<=full_params||!isfinite(full_sse)||full_sse<=0) {
            free(full_fit);free(x);free(y);
            snprintf(err,errn,"Full model has no residual variance for Cp calculation");
            return -1;
        }
        
        double sigma2_full=full_sse/(n-full_params);
        free(full_fit);
        
        /* Collect all candidates from all degrees */
        ModelCandidate *all_candidates=NULL;
        int total_count=0,total_capacity=0;
        
        int min_degree=sp->max_degree==0?0:1;
        for(int d=min_degree;d<=sp->max_degree;d++) {
            ModelCandidate *degree_candidates=NULL;
            int degree_count=0;
            
            if(exhaustive_search(x,y,n,sp,d,sigma2_full,&degree_candidates,&degree_count)) {
                /* Failed to search this degree, skip it */
                continue;
            }
            
            /* Filter by Cp < p */
            for(int i=0;i<degree_count;i++) {
                if(degree_candidates[i].cp>=degree_candidates[i].param_count) {
                    free(degree_candidates[i].fit);
                    continue;
                }
                
                /* Add to collection */
                if(total_count==total_capacity) {
                    int new_cap=total_capacity?total_capacity*2:16;
                    ModelCandidate *tmp=realloc(all_candidates,(size_t)new_cap*sizeof(ModelCandidate));
                    if(!tmp) {
                        for(int j=0;j<total_count;j++)free(all_candidates[j].fit);
                        free(all_candidates);
                        free(degree_candidates);
                        free(x);free(y);
                        snprintf(err,errn,"Out of memory");
                        return -1;
                    }
                    all_candidates=tmp;
                    total_capacity=new_cap;
                }
                all_candidates[total_count++]=degree_candidates[i];
            }
            
            free(degree_candidates);
        }
        
        if(total_count==0) {
            free(all_candidates);free(x);free(y);
            snprintf(err,errn,"No models with Cp < p found");
            return -1;
        }
        
        /* Sort by AIC */
        qsort(all_candidates,(size_t)total_count,sizeof(ModelCandidate),compare_by_aic);
        
        /* Take top 10 */
        int menu_count=total_count<10?total_count:10;
        ModelCandidate *menu_candidates=all_candidates; /* First 10 */
        
        /* Present selection menu */
        int selected=select_model_menu(cfg,draw,ctx,menu_candidates,menu_count);
        
        if(selected<0||selected>=menu_count) {
            /* User cancelled */
            for(int i=0;i<total_count;i++)free(all_candidates[i].fit);
            free(all_candidates);free(x);free(y);
            return 1;
        }
        
        /* Use selected model */
        best_model=all_candidates[selected].model;
        bestd=all_candidates[selected].degree;
        bestscore=all_candidates[selected].r2_adj;
        best=all_candidates[selected].fit;
        all_candidates[selected].fit=NULL; /* Take ownership */
        
        /* Free other candidates */
        for(int i=0;i<total_count;i++)
            if(i!=selected&&all_candidates[i].fit)free(all_candidates[i].fit);
        free(all_candidates);
        
    } else {
        /* exact_degree is set - use old behavior */
        double *trial=malloc((size_t)n*sizeof(double));
        best=malloc((size_t)n*sizeof(double));
        if(!best||!trial){free(x);free(y);free(best);free(trial);snprintf(err,errn,"Out of memory");return -1;}
        
        int found=0,failed_tests=0;
        for(int d=sp->exact_degree||sp->max_degree==0?sp->max_degree:1;d<=sp->max_degree;d++) {
            double score;RegressionModel trial_model;int degree=d;
            if(fit_family(x,y,n,d,!sp->family&&sp->log_y,(1u<<(d+1))-1,sp->family,sp->standardize,trial,&score,&trial_model))continue;
            if(apply_significance(x,y,n,sp,&degree,trial,&score,&trial_model)){failed_tests++;continue;}
            if(!found||(isfinite(score)&&(!isfinite(bestscore)||score>bestscore))) {
                found=1;bestscore=score;bestd=degree;best_model=trial_model;memcpy(best,trial,(size_t)n*sizeof*best);
            }
        }
        free(trial);
        if(!found) {
            free(x);free(y);free(best);
            snprintf(err,errn,sp->exact_degree?"Cannot fit requested degree: insufficient points or singular model":
                     failed_tests?"No candidate has usable significance tests":"No valid regression (insufficient points or singular models)");
            return -1;
        }
    }
    
    if(!best) {
        free(x);free(y);
        snprintf(err,errn,"No model selected");
        return -1;
    }
    
    regression_graph_free(g);g->x=x;g->y=y;g->fit=best;g->count=n;g->degree=bestd;g->adjusted_r2=bestscore;
    g->model=best_model;
    g->significance=sp->significance;
    g->x_is_time=sp->x==VAR_TIME; g->y_is_time=sp->y==VAR_TIME;
    g->x_time_unit=sp->time_unit; g->y_time_unit=sp->time_unit;
    g->x_epoch=first; g->y_epoch=first;
    snprintf(g->x_name,sizeof g->x_name,"%.*s",63,sp->x==VAR_TIME?"TIME":sp->x==VAR_DATA?sp->x_data_name:l->tickers[sp->x]);snprintf(g->y_name,sizeof g->y_name,"%.*s",63,sp->y==VAR_TIME?"TIME":sp->y==VAR_DATA?sp->y_data_name:l->tickers[sp->y]);
    describe_graph(l,sp,g);
    return 0;
}

static int ticker_index(const StockList *list, const char *ticker)
{
    for (int i=0; i<list->count; i++)
        if (!strcmp(list->tickers[i], ticker)) return i;
    return VAR_NONE;
}

static void spec_defaults(Spec *s)
{
    memset(s, 0, sizeof *s);
    s->y=s->x=VAR_NONE;
    s->y_days=s->x_days=1;
    s->time_unit=1; /* Time is always represented in days; labels show dates. */
    s->sample_mode=SAMPLE_ALL;
    s->standardize=1;
    s->sample_days=1;
    s->max_degree=1;
    s->y_direction=DIR_FORWARD;
    s->x_direction=DIR_BACKWARD;
    s->y_change_type=CHANGE_PERCENT;
    s->x_change_type=CHANGE_PERCENT;
    s->y_instant=0; /* Preserve the existing change-period default for y. */
    s->x_instant=1;
    s->exclude_zero_x=0;
}

/* The saved form uses ticker names instead of current list indexes, so adding
 * or removing a CSV cannot silently make a selection point at another stock. */
static void spec_load(const StockList *list, Spec *s)
{
    char value[2 * PATH_MAX + 2048], yname[256], xname[256], yfile[PATH_MAX]="-", ydname[256]="-", xfile[PATH_MAX], xdname[256], tail;
    int version, ykind, xkind, yc, xc, yd, xd, instant, days, unit, ly, lx, degree, exclude_zero=0, ytype=0, xtype=0, xdays=1, ydays=1, yinstant=0, stock_prior=0, sample_mode=0, sample_days=1, sampled_changes=0;
    spec_defaults(s);
    s->standardize=state_load_standardize();
    if (!state_load_graph_form(value, sizeof value)) return;
    double significance=0;int exact_degree=0,remove_intermediate=0,family=0,ylr=0,xlr=0,exclude_y=0;
    int fields=sscanf(value,
        "%d|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%lf|%d|%d|%d|%d|%d|%d%c",
        &version,&ykind,yname,yfile,ydname,&xkind,xname,xfile,xdname,&yc,&xc,&yd,&xd,
        &instant,&days,&unit,&ly,&lx,&degree,&exclude_zero,&ytype,&xtype,&xdays,&ydays,&yinstant,&stock_prior,&sample_mode,&sample_days,&sampled_changes,&significance,&exact_degree,&remove_intermediate,&family,&ylr,&xlr,&exclude_y,&tail);
    int v15=fields==36&&version==15;
    int v14=v15||(fields==35&&version==14);
    int v13=v14 || (fields==33&&version==13);
    int v12=v13 || (fields==32 && version==12);
    int v11=v12 || (fields==31 && version==11);
    int v10=v11 || (fields==30 && version==10);
    int v9=v10 || (fields == 29 && version == 9);
    int v8=v9 || (fields == 28 && version == 8);
    int v7=v8 || (fields == 26 && version == 7);
    int v6=v7 || (fields == 25 && version == 6);
    int v5=v6 || (fields == 24 && version == 5);
    int v4=0;
    int v3=0;
    if (!v5) {
        fields=sscanf(value,
            "%d|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d%c",
            &version,&ykind,yname,yfile,ydname,&xkind,xname,xfile,xdname,&yc,&xc,&yd,&xd,
            &instant,&days,&unit,&ly,&lx,&degree,&exclude_zero,&ytype,&xtype,&tail);
        v4=fields == 22 && version == 4;
    }
    if (!v5 && !v4) {
        fields=sscanf(value,
            "%d|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d%c",
            &version,&ykind,yname,yfile,ydname,&xkind,xname,xfile,xdname,&yc,&xc,&yd,&xd,
            &instant,&days,&unit,&ly,&lx,&degree,&exclude_zero,&tail);
        v3=fields == 20 && version == 3;
    }
    if (!v5 && !v4 && !v3) {
        fields=sscanf(value,
            "%d|%d|%255[^|]|%d|%255[^|]|%4095[^|]|%255[^|]|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d%c",
            &version,&ykind,yname,&xkind,xname,xfile,xdname,&yc,&xc,&yd,&xd,&instant,
            &days,&unit,&ly,&lx,&degree,&exclude_zero,&tail);
        if (!((fields == 17 && version == 1) || (fields == 18 && version == 2))) return;
    }
    if ((ykind != 0 && ykind != 1 && !((v3 || v4 || v5) && ykind == 2)) ||
        (xkind != 0 && xkind != 1 && xkind != 2) ||
        (yc != 0 && yc != 1) || (xc != 0 && xc != 1) ||
        (yd != DIR_BACKWARD && yd != DIR_FORWARD) ||
        (xd != DIR_BACKWARD && xd != DIR_FORWARD) || (instant != 0 && instant != 1) ||
        (v6 && yinstant != 0 && yinstant != 1) ||
        (v7 && stock_prior != 0 && stock_prior != 1) ||
        (v8 && (sample_mode < SAMPLE_ALL || sample_mode > SAMPLE_YEAR || sample_days < 1 || sample_days > 3650)) ||
        (v9 && (sampled_changes < 0 || sampled_changes > 3)) ||
        (v10 && (!isfinite(significance)||significance<0||significance>=1)) ||
        (v11 && exact_degree!=0 && exact_degree!=1) ||
        (v12 && remove_intermediate!=0 && remove_intermediate!=1) ||
        (v13 && (family<0||family>2)) ||
        (v14 && ((ylr!=0&&ylr!=1)||(xlr!=0&&xlr!=1))) ||
        (v15 && exclude_y!=0 && exclude_y!=1) ||
        ((v3 || v4 || v5 || fields == 18) && exclude_zero != 0 && exclude_zero != 1) ||
        ((v4 || v5) && (ytype != CHANGE_PERCENT && ytype != CHANGE_ABSOLUTE)) ||
        ((v4 || v5) && (xtype != CHANGE_PERCENT && xtype != CHANGE_ABSOLUTE)) ||
        (v5 ? (xdays < 1 || xdays > 3650 || ydays < 1 || ydays > 3650) : (days < 1 || days > 3650)) ||
        (unit != 1 && unit != 7 && unit != 30 && unit != 365) ||
        (ly != 0 && ly != 1) || (lx != 0 && lx != 1) || degree < 0 || degree > 25)
        return;
    s->y=ykind == 1 ? VAR_TIME : ykind == 2 ? VAR_DATA : ticker_index(list,yname);
    s->x=xkind == 1 ? VAR_TIME : xkind == 2 ? VAR_DATA : ticker_index(list,xname);
    if (s->y == VAR_DATA && (!strcmp(yfile,"-") || !strcmp(ydname,"-"))) s->y=VAR_NONE;
    if (s->x == VAR_DATA && (!strcmp(xfile,"-") || !strcmp(xdname,"-"))) s->x=VAR_NONE;
    if (s->y == VAR_DATA) {
        snprintf(s->y_data_file,sizeof s->y_data_file,"%s",yfile);
        snprintf(s->y_data_name,sizeof s->y_data_name,"%s",ydname);
    }
    if (s->x == VAR_DATA) {
        snprintf(s->x_data_file,sizeof s->x_data_file,"%s",xfile);
        snprintf(s->x_data_name,sizeof s->x_data_name,"%s",xdname);
    }
    s->y_change=yc; s->x_change=xc;
    s->y_direction=yd; s->x_direction=xd; s->x_instant=instant;
    s->y_instant=v6 ? yinstant : 0;
    s->y_change_type=(v4 || v5) ? ytype : CHANGE_PERCENT;
    s->x_change_type=(v4 || v5) ? xtype : CHANGE_PERCENT;
    s->exclude_zero_x=(v3 || v4 || v5 || fields == 18) ? exclude_zero : 0;
    s->x_days=v5 ? xdays : days; s->y_days=v5 ? ydays : days;
    s->stock_prior_close=v7 ? stock_prior : 0;
    s->time_unit=1; s->log_y=ly; s->log_x=lx; s->max_degree=degree;
    if(v8) { s->sample_mode=sample_mode; s->sample_days=sample_days; }
    else if(s->y==VAR_TIME || s->x==VAR_TIME) { s->sample_mode=SAMPLE_DISTANCE; s->sample_days=unit; }
    s->sampled_changes=v9?sampled_changes:0;
    s->significance=v10?significance:0;
    s->exact_degree=v11?exact_degree:0;
    s->remove_intermediate=v12?remove_intermediate:0;
    s->family=v13?family:0;
    s->y_log_return=v14?ylr:0;s->x_log_return=v14?xlr:0;
    s->exclude_zero_y=v15?exclude_y:0;
    if (s->y == VAR_TIME) s->y_change=s->log_y=0;
    if (s->x == VAR_TIME) s->x_change=s->log_x=0;
}

static void spec_save(const StockList *list, const Spec *s)
{
    char value[2 * PATH_MAX + 2048];
    int ykind=s->y == VAR_TIME ? 1 : s->y == VAR_DATA ? 2 : 0;
    int xkind=s->x == VAR_TIME ? 1 : s->x == VAR_DATA ? 2 : 0;
    const char *yname=s->y >= 0 ? list->tickers[s->y] : "-";
    const char *xname=s->x >= 0 ? list->tickers[s->x] : "-";
    const char *yfile=s->y == VAR_DATA && s->y_data_file[0] ? s->y_data_file : "-";
    const char *ydname=s->y == VAR_DATA && s->y_data_name[0] ? s->y_data_name : "-";
    const char *xfile=s->x == VAR_DATA && s->x_data_file[0] ? s->x_data_file : "-";
    const char *xdname=s->x == VAR_DATA && s->x_data_name[0] ? s->x_data_name : "-";
    snprintf(value,sizeof value,"15|%d|%s|%s|%s|%d|%s|%s|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%.17g|%d|%d|%d|%d|%d|%d",
             ykind,yname,yfile,ydname,xkind,xname,xfile,xdname,s->y_change,s->x_change,
             s->y_direction,s->x_direction,s->x_instant,s->y_days,s->time_unit,
             s->log_y,s->log_x,s->max_degree,s->exclude_zero_x,
             s->y_change_type,s->x_change_type,s->x_days,s->y_days,s->y_instant,
             s->stock_prior_close,s->sample_mode,s->sample_days,s->sampled_changes,s->significance,s->exact_degree,s->remove_intermediate,s->family,s->y_log_return,s->x_log_return,s->exclude_zero_y);
    state_save_graph_form(value);
    state_save_standardize(s->standardize);
}

static const char *variable_name(const StockList *l, const Spec *spec, int variable, int y_axis)
{
    if (variable == VAR_TIME) return "Time";
    if (variable == VAR_DATA) {
        const char *name=y_axis ? spec->y_data_name : spec->x_data_name;
        return name[0] ? name : "[choose]";
    }
    return variable >= 0 ? l->tickers[variable] : "[choose]";
}

static void significance_prompt(const Settings *cfg,DrawFn draw,void *ctx,Spec *s)
{
    char buf[32]="";int len=0;
    if(s->significance>0){snprintf(buf,sizeof buf,"%.6g",s->significance);len=(int)strlen(buf);}
    int invalid=0;
    for(;;) {
        draw(ctx);int row=term_rows()-1;
        term_fill(row,0,1,term_cols(),cfg->background);
        char text[160];snprintf(text,sizeof text,"%sAlpha (e.g. 0.05; empty = adjusted R2): %s_",invalid?"Invalid value. ":"",buf);
        term_text(row,1,text,cfg->text,cfg->background);term_end_frame();
        int k=term_read_key();
        if(k==KEY_ESC||k==3||k==KEY_EOF)return;
        if(k=='\r'||k=='\n') {
            if(!len){s->significance=0;return;}
            char *end;double value=strtod(buf,&end);
            if(!*end&&isfinite(value)&&value>0&&value<1){s->significance=value;return;}
            invalid=1;
        } else if(k==127||k==8){if(len)buf[--len]=0;}
        else if(k>=32&&k<127&&len<(int)sizeof buf-1){buf[len++]=(char)k;buf[len]=0;}
    }
}

static void choose_change_type(const Settings *cfg,DrawFn draw,void *ctx,Spec *s,int y_axis)
{
    int *type=y_axis?&s->y_change_type:&s->x_change_type;
    int *log_return=y_axis?&s->y_log_return:&s->x_log_return;
    char *options[]={"percentage","absolute","log return (%)"};
    int q=menu_select(cfg,draw,ctx,y_axis?"Y change type":"X change type",options,3,*log_return?2:*type);
    if(q>=0){*log_return=q==2;if(q!=2)*type=q;}
}

static int configure(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l, Spec *s)
{
    spec_load(l,s);
    int selected=0;
    for (;;) {
        spec_save(l,s);
        char generate[64], y[96], x[96], yc[64], yd[64], yi[96], yt[64], yzero[96], yperiod[64], xc[64], xd[64], xi[96], xt[64], zero[96], xperiod[64], prior[112], sampling[96], sample_days[72], ysampled[96], xsampled[96], ly[64], lx[64], degree[64];
        int ready=s->x!=VAR_NONE&&s->y!=VAR_NONE&&!(s->x==VAR_TIME&&s->y==VAR_TIME);
        snprintf(generate,sizeof generate,"%sGenerate regression",ready ? "" : "[disabled] ");
        snprintf(y,sizeof y,"Explanatory / y: %.*s",76,variable_name(l,s,s->y,1));
        snprintf(x,sizeof x,"Regressor / x: %.*s",78,variable_name(l,s,s->x,0));
        snprintf(yc,sizeof yc,"%s[%c] y as change",(s->y>=0||s->y==VAR_DATA)?"":"[disabled] ",s->y_change?'o':' ');
        snprintf(yd,sizeof yd,"%sy change direction: %s",s->y_change?"":"[disabled] ",s->y_direction==DIR_FORWARD?"forward":"backward");
        snprintf(yi,sizeof yi,"%sy change sampling: %s",s->y_change?"":"[disabled] ",s->y_instant?"closest point":"change period");
        snprintf(yt,sizeof yt,"%sy change type: %s",s->y_change?"":"[disabled] ",s->y_log_return?"log return (%)":s->y_change_type==CHANGE_PERCENT?"percentage":"absolute");
        int uses_y_days=s->y_change&&!s->y_instant;
        snprintf(yzero,sizeof yzero,"%s[%c] exclude y changes equal to 0",s->y_change?"":"[disabled] ",s->exclude_zero_y?'o':' ');
        snprintf(yperiod,sizeof yperiod,"%sy change period: %d days",uses_y_days?"":"[disabled] ",s->y_days);
        snprintf(xc,sizeof xc,"%s[%c] x as change",(s->x>=0||s->x==VAR_DATA)?"":"[disabled] ",s->x_change?'o':' ');
        snprintf(xd,sizeof xd,"%sx change direction: %s",s->x_change?"":"[disabled] ",s->x_direction==DIR_FORWARD?"forward":"backward");
        snprintf(xi,sizeof xi,"%sx change sampling: %s",s->x_change?"":"[disabled] ",s->x_instant?"closest point":"change period");
        snprintf(xt,sizeof xt,"%sx change type: %s",s->x_change?"":"[disabled] ",s->x_log_return?"log return (%)":s->x_change_type==CHANGE_PERCENT?"percentage":"absolute");
        snprintf(zero,sizeof zero,"%s[%c] exclude x changes equal to 0",s->x_change?"":"[disabled] ",s->exclude_zero_x?'o':' ');
        int uses_x_days=s->x_change&&!s->x_instant;
        snprintf(xperiod,sizeof xperiod,"%sx change period: %d days",uses_x_days?"":"[disabled] ",s->x_days);
        int uses_prior=(s->x>=0||s->y>=0)&&s->x!=VAR_TIME&&s->y!=VAR_TIME;
        snprintf(prior,sizeof prior,"%s[%c] stocks use previous close on missing dates",uses_prior?"":"[disabled] ",s->stock_prior_close?'o':' ');
        const char *sample_name=s->sample_mode==SAMPLE_ALL?"all eligible dates":s->sample_mode==SAMPLE_DISTANCE?"minimum day distance":s->sample_mode==SAMPLE_MONTH?"first eligible each month":"first eligible each year";
        snprintf(sampling,sizeof sampling,"Point sampling: %s",sample_name);
        snprintf(sample_days,sizeof sample_days,"%sMinimum sample distance: %d days",s->sample_mode==SAMPLE_DISTANCE?"":"[disabled] ",s->sample_days);
        int thinning=s->sample_mode!=SAMPLE_ALL;
        snprintf(ysampled,sizeof ysampled,"%sy calculate changes using: %s",s->y_change&&thinning?"":"[disabled] ",(s->sampled_changes&2)?"sampled points":"all observations");
        snprintf(xsampled,sizeof xsampled,"%sx calculate changes using: %s",s->x_change&&thinning?"":"[disabled] ",(s->sampled_changes&1)?"sampled points":"all observations");
        snprintf(ly,sizeof ly,"%s[%c] log y",!s->family&&(s->y>=0||s->y==VAR_DATA)&&!s->y_change?"":"[disabled] ",s->log_y?'o':' ');
        snprintf(lx,sizeof lx,"%s[%c] log x",!s->family&&(s->x>=0||s->x==VAR_DATA)&&!s->x_change?"":"[disabled] ",s->log_x?'o':' ');
        snprintf(degree,sizeof degree,"%s polynomial degree: %d",s->exact_degree||s->max_degree<=1?"Chosen":"Maximum",s->max_degree);
        char exact[80];snprintf(exact,sizeof exact,"[%c] fit exactly this degree",s->exact_degree?'o':' ');
        char alpha[96];
        if(s->significance>0)snprintf(alpha,sizeof alpha,"Significance alpha: %.6g",s->significance);
        else snprintf(alpha,sizeof alpha,"Significance alpha: [empty]");
        int can_remove=s->significance>0&&s->max_degree>1;
        char remove[112];snprintf(remove,sizeof remove,"%s[%c] remove insignificant intermediate terms",can_remove?"":"[disabled] ",s->remove_intermediate?'o':' ');
        char family_label[80];snprintf(family_label,sizeof family_label,"Model family: %s",s->family==FAMILY_EXPONENTIAL?"shifted exponential":s->family==FAMILY_LOGARITHMIC?"shifted logarithmic":"polynomial");
        char standardize[64];snprintf(standardize,sizeof standardize,"[%c] standardize polynomial input",s->standardize?'o':' ');
        char *items[]={generate,y,x,sampling,sample_days,yc,ysampled,yd,yi,yt,yzero,yperiod,xc,xsampled,xd,xi,xt,zero,xperiod,prior,ly,lx,degree,exact,alpha,remove,family_label,standardize,"Back"};
        int choice=menu_select(cfg,draw,ctx,"Generate regression",items,29,selected);
        if(choice<0||choice==28)return 0;
        selected=choice;
        if(choice==27){s->standardize=!s->standardize;continue;}
        if(choice==10){if(s->y_change)s->exclude_zero_y=!s->exclude_zero_y;continue;}
        if(choice>10)choice--; /* Existing actions retain their original indexes. */
        if(choice==25){char *families[]={"polynomial","shifted exponential","shifted logarithmic"};int q=menu_select(cfg,draw,ctx,"Model family",families,3,s->family);if(q>=0)s->family=q;continue;}
        if(choice==24&&can_remove){s->remove_intermediate=!s->remove_intermediate;continue;}
        if(choice==22){s->exact_degree=!s->exact_degree;continue;}
        if(choice==23){significance_prompt(cfg,draw,ctx,s);continue;}
        if(choice==3){char *u[]={"all eligible dates","minimum day distance","first eligible date each month","first eligible date each year"};int q=menu_select(cfg,draw,ctx,"Point sampling",u,4,s->sample_mode);if(q>=0)s->sample_mode=q;continue;}
        if(choice==4&&s->sample_mode==SAMPLE_DISTANCE){s->sample_days=ask_int(cfg,draw,ctx,"Minimum sample distance (days): ",s->sample_days,1,3650);continue;}
        if(thinning&&((choice==6&&s->y_change)||(choice==12&&s->x_change))) {
            int bit=choice==6?2:1;
            char *options[]={"all observations","sampled points"};
            int q=menu_select(cfg,draw,ctx,choice==6?"Y: calculate changes using":"X: calculate changes using",options,2,!!(s->sampled_changes&bit));
            if(q>=0)s->sampled_changes=(s->sampled_changes&~bit)|(q?bit:0);
            continue;
        }
        static const int actions[]={0,1,2,-1,-1,3,-1,4,5,6,7,8,-1,9,10,11,12,13,14,17,18,19};
        if(choice>=0&&choice<(int)(sizeof actions/sizeof actions[0]))choice=actions[choice];
        if(choice==0){if(ready)return 1;continue;}
        if(choice==1){
            int q=choose_y(cfg,draw,ctx,l,s);
            if(q!=VAR_NONE){s->y=q;if(q==VAR_TIME)s->y_change=s->log_y=0;}
            continue;
        }
        if(choice==2){
            int q=choose_x(cfg,draw,ctx,l,s);
            if(q!=VAR_NONE){s->x=q;if(q==VAR_TIME)s->x_change=s->log_x=0;}
            continue;
        }
        if(choice==3&&(s->y>=0||s->y==VAR_DATA)){s->y_change=!s->y_change; if(s->y_change)s->log_y=0; continue;}
        if(choice==4&&s->y_change){s->y_direction=!s->y_direction;continue;}
        if(choice==5&&s->y_change){s->y_instant=!s->y_instant;continue;}
        if(choice==6&&s->y_change){choose_change_type(cfg,draw,ctx,s,1);continue;}
        if(choice==7&&uses_y_days){s->y_days=ask_int(cfg,draw,ctx,"Y change period (days): ",s->y_days,1,3650);continue;}
        if(choice==8&&(s->x>=0||s->x==VAR_DATA)){s->x_change=!s->x_change; if(s->x_change)s->log_x=0; continue;}
        if(choice==9&&s->x_change){s->x_direction=!s->x_direction;continue;}
        if(choice==10&&s->x_change){s->x_instant=!s->x_instant;continue;}
        if(choice==11&&s->x_change){choose_change_type(cfg,draw,ctx,s,0);continue;}
        if(choice==12&&s->x_change){s->exclude_zero_x=!s->exclude_zero_x;continue;}
        if(choice==13&&uses_x_days){s->x_days=ask_int(cfg,draw,ctx,"X change period (days): ",s->x_days,1,3650);continue;}
        if(choice==14&&uses_prior){s->stock_prior_close=!s->stock_prior_close;continue;}
        if(choice==17&&(s->y>=0||s->y==VAR_DATA)&&!s->y_change){s->log_y=!s->log_y;continue;}
        if(choice==18&&(s->x>=0||s->x==VAR_DATA)&&!s->x_change){s->log_x=!s->log_x;continue;}
        if(choice==19)s->max_degree=ask_int(cfg,draw,ctx,"Polynomial degree (0-25): ",s->max_degree,0,25);
    }
}

int regression_menu(const Settings *cfg, DrawFn draw, void *ctx, const StockList *l,
                    char *err, size_t errn, RegressionGraph *g)
{
    char *actions[]={"Generate regression","Open graph"};int action=menu_select(cfg,draw,ctx,"Graphs",actions,2,0);if(action<0)return 0;
    if(action==1) return open_graph(cfg, draw, ctx, l, err, errn, g);
    Spec s={0};if(!configure(cfg,draw,ctx,l,&s))return 0;
    int result=generate(cfg,draw,ctx,l,&s,err,errn,g);
    return result==0?1:result==1?0:-1;
}

static void add_dot(unsigned char *mask, int width, int height, int px, int py)
{
    static const unsigned char dots[4][2] = {{1,8},{2,16},{4,32},{64,128}};
    if (px < 0 || py < 0 || px >= width * 2 || py >= height * 4) return;
    mask[(py / 4) * width + px / 2] |= dots[py % 4][px % 2];
}

/* Clip in floating point before converting to pixel coordinates. This keeps
 * steep portions connected without iterating over points outside the view. */
static void curve_segment(unsigned char *mask, int width, int height,
                           double x0, double y0, double x1, double y1)
{
    double bottom=height*4-1;
    if(!isfinite(y0)||!isfinite(y1)||(y0<0&&y1<0)||(y0>bottom&&y1>bottom))return;
    if(y0<0||y0>bottom) {
        double clipped=y0<0?0:bottom;
        x0+=(x1-x0)*(clipped-y0)/(y1-y0);y0=clipped;
    }
    if(y1<0||y1>bottom) {
        double clipped=y1<0?0:bottom;
        x1=x0+(x1-x0)*(clipped-y0)/(y1-y0);y1=clipped;
    }
    int steps=(int)ceil(fmax(fabs(x1-x0),fabs(y1-y0)));
    if(!steps){add_dot(mask,width,height,(int)lround(x0),(int)lround(y0));return;}
    for(int i=0;i<=steps;i++) {
        double t=(double)i/steps;
        add_dot(mask,width,height,(int)lround(x0+(x1-x0)*t),(int)lround(y0+(y1-y0)*t));
    }
}

static void draw_curve(const RegressionGraph *g, unsigned char *mask, int width, int height,
                       double xmin, double xmax, double ymin, double ymax,
                       double data_xmin, double data_xmax)
{
    double lo=fmax(xmin,data_xmin), hi=fmin(xmax,data_xmax);
    if(lo>hi||!g->model.valid)return;
    /* Four evaluations per horizontal Braille dot, refreshed at every zoom. */
    int samples=width*8;
    double previous_x=0, previous_y=0;
    int have_previous=0;
    for(int i=0;i<=samples;i++) {
        double x=lo+(hi-lo)*i/samples;
        double y=model_value(g,x);
        double px=(x-xmin)/(xmax-xmin)*(width*2-1);
        double py=(ymax-y)/(ymax-ymin)*(height*4-1);
        if(!isfinite(py)){have_previous=0;continue;}
        if(have_previous)curve_segment(mask,width,height,previous_x,previous_y,px,py);
        else if(py>=0&&py<=height*4-1)add_dot(mask,width,height,(int)lround(px),(int)lround(py));
        previous_x=px;previous_y=py;have_previous=1;
    }
}

static void draw_confidence(const RegressionGraph *g,const MeanConfidence *ci,unsigned char *mask,
                            int width,int height,double xmin,double xmax,double ymin,double ymax,
                            double data_xmin,double data_xmax)
{
    double lo=fmax(xmin,data_xmin),hi=fmin(xmax,data_xmax),previous_x=0,previous_y[2]={0};
    int previous=0,samples=width*8;if(lo>hi)return;
    for(int i=0;i<=samples;i++) {
        double x=lo+(hi-lo)*i/samples,bounds[2];
        if(confidence_interval(g,ci,x,&bounds[0],&bounds[1])){previous=0;continue;}
        double px=(x-xmin)/(xmax-xmin)*(width*2-1);
        for(int j=0;j<2;j++) {
            double py=(ymax-bounds[j])/(ymax-ymin)*(height*4-1);
            if(previous)curve_segment(mask,width,height,previous_x,previous_y[j],px,py);
            else if(isfinite(py)&&py>=0&&py<=height*4-1)add_dot(mask,width,height,(int)lround(px),(int)lround(py));
            previous_y[j]=py;
        }
        previous_x=px;previous=1;
    }
}

static void add_scatter(unsigned char *mask, unsigned short *density, int width, int height,
                        int px, int py)
{
    if (px < 0 || py < 0 || px >= width * 2 || py >= height * 4) return;
    int cell=(py / 4) * width + px / 2;
    add_dot(mask,width,height,px,py);
    if (density[cell] < 1000) density[cell]++;
}

static Color scatter_color(Color blue, Color red, unsigned short density)
{
    double t=log((double)density+1.0)/log(1001.0); /* makes early overlaps visible */
    Color out={
        (unsigned char)lround(blue.r+(red.r-blue.r)*t),
        (unsigned char)lround(blue.g+(red.g-blue.g)*t),
        (unsigned char)lround(blue.b+(red.b-blue.b)*t)
    };
    return out;
}

static void braille(unsigned char mask, char out[4])
{
    unsigned c = 0x2800u + mask;
    out[0] = (char)(0xe0u | (c >> 12));
    out[1] = (char)(0x80u | ((c >> 6) & 0x3fu));
    out[2] = (char)(0x80u | (c & 0x3fu));
    out[3] = '\0';
}

static void axis_value(char out[24], int is_time, long epoch, int unit, double v)
{
    if (is_time && unit > 0) day_date(epoch + lround(v * unit), out);
    else snprintf(out, 24, "%.5g", v);
}

/* A vertical scale has room for at most six terminal cells per label. */
static void axis_y_value(char out[24], int is_time, long epoch, int unit, double value)
{
    if (is_time && unit > 0) {
        char date[11];
        day_date(epoch + lround(value * unit), date);
        snprintf(out, 24, "%s", date + 5);       /* MM-DD */
        return;
    }

    static const char suffix[] = "kMBT";
    double scaled=value, absolute=fabs(value);
    int suffix_index=-1;
    while (absolute >= 9999.5 && suffix_index < 3) {
        scaled/=1000.0;
        absolute/=1000.0;
        suffix_index++;
    }
    char unit_text[2] = { suffix_index >= 0 ? suffix[suffix_index] : '\0', '\0' };
    for (int decimals=2; decimals>=0; decimals--) {
        char candidate[24];
        snprintf(candidate,sizeof candidate,"%.*f%s",decimals,scaled,unit_text);
        if (term_text_width(candidate) <= 6) {
            snprintf(out,24,"%s",candidate);
            return;
        }
    }
    int exponent=(int)floor(log10(fabs(value)));
    snprintf(out,24,"%.0fe%d",value/pow(10.0,exponent),exponent);
}

/* The same 1/2/5 progression used by the stock price scale. */
static double nice_step(double raw)
{
    double magnitude=1.0;
    while(magnitude*10.0<=raw)magnitude*=10.0;
    while(magnitude>raw)magnitude/=10.0;
    double ratio=raw/magnitude;
    return (ratio<=1.0?1.0:ratio<=2.0?2.0:ratio<=5.0?5.0:10.0)*magnitude;
}

static long long first_tick(double minimum, double step)
{
    long long tick=(long long)(minimum/step);
    if((double)tick<minimum/step)tick++;
    return tick;
}

static int scale_margin(const RegressionGraph *g, double ymin, double ymax,
                         int height, ScaleTicks ticks, int has_click, double click_y)
{
    int max_width=1;
    char label[24];
    for(int i=0;i<ticks.count;i++) {
        int row=ticks.first+i*ticks.gap;
        double value=scale_row_value(ymin,ymax,height,row,1);
        axis_y_value(label,g->y_is_time,g->y_epoch,g->y_time_unit,value);
        int width=term_text_width(label);
        if(width>max_width)max_width=width;
    }
    if(has_click&&click_y>=ymin&&click_y<=ymax) {
        axis_y_value(label,g->y_is_time,g->y_epoch,g->y_time_unit,click_y);
        int width=term_text_width(label);
        if(width>max_width)max_width=width;
    }
    /* Match the stock chart's label-width margin and minimum of five. */
    int margin=max_width+1;
    return margin<5?5:margin;
}

/* Measure and optionally draw the complete settings header, wrapping at word
 * boundaries without cutting a UTF-8 character. Keep it aligned to the box. */
static int text_header(const Settings *cfg,const char *header,int left,int width)
{
    char line[1152];
    const char *p=header;
    int rows=0;
    if(width<1)width=1;
    while(*p) {
        size_t len=0,space=0;
        while(p[len]) {
            size_t next=len+1;
            while(((unsigned char)p[next]&0xc0)==0x80)next++;
            memcpy(line,p,next);line[next]='\0';
            if(term_text_width(line)>width&&len)break;
            if(p[len]==' ')space=len;
            len=next;
            if(term_text_width(line)>width)break;
        }
        if(p[len]&&space)len=space;
        memcpy(line,p,len);line[len]='\0';
        if(cfg)term_text(rows,left,line,cfg->text,cfg->background);
        rows++;
        p+=len;
        while(*p==' ')p++;
    }
    return rows;
}

static int graph_header(const Settings *cfg,const RegressionGraph *g,int left,int width)
{
    char header[1152],score[32];
    snprintf(header,sizeof header,"y: %s | x: %s | adjusted R2: %s | %d points | %s degree %d%s",g->y_name,g->x_name,score_label(g->adjusted_r2,score),g->count,g->degree==0?"constant":g->model.family==FAMILY_EXPONENTIAL?"shifted exponential":g->model.family==FAMILY_LOGARITHMIC?"shifted logarithmic":"polynomial",g->degree,g->model.log_y?" (log y)":"");
    return text_header(cfg,header,left,width);
}

static void panel_text(int row,int col,const char *text,int skip,int width,Color fg,Color bg)
{
    int position=0;
    while(*text) {
        int bytes=1;while(((unsigned char)text[bytes]&0xc0)==0x80)bytes++;
        char glyph[8];if(bytes>7)return;memcpy(glyph,text,(size_t)bytes);glyph[bytes]=0;
        int columns=term_text_width(glyph);
        if(position>=skip && position+columns<=skip+width)term_text(row,col+position-skip,glyph,fg,bg);
        position+=columns;text+=bytes;if(position>=skip+width)break;
    }
}

/* Read the graph's saved axis description, never the current generator form. */
static int graph_axis_is_change(const RegressionGraph *g,int y_axis)
{
    const char *axis=y_axis?(!strncmp(g->title,"y: ",3)?g->title:NULL):strstr(g->title," | x: ");
    if(!axis)return 0;
    if(!y_axis)axis+=3;
    const char *end=strstr(axis," | ");
    const char *change=strstr(axis," change,");
    if(!change|| (end&&change>=end))change=strstr(axis,"log return (%)");
    return change&&(!end||change<end);
}

typedef struct {int left,top,right,bottom,visible;} GraphPanel;

static void graph_panel(const Settings *cfg,const char *title,char lines[][512],
                        const int *p_column,const int *p_color,int n,
                        int left,int top,int right,int bot,int *offset,int *pan,GraphPanel *box)
{
    const char *footer="j/k scroll  H/L horizontal scroll";
    int longest=term_text_width(footer);box->visible=0;
    for(int i=0;i<n;i++){int w=term_text_width(lines[i]);if(w>longest)longest=w;}
    int width=longest+2;if(width>right-left-1)width=right-left-1;
    int visible=n;if(visible>bot-top-3)visible=bot-top-3;
    if(width<1||visible<1)return;
    if(*offset>n-visible)*offset=n-visible;
    if(*offset<0)*offset=0;
    int content=width-2;if(content<1)content=1;
    int max_pan=longest-content;if(max_pan<0)max_pan=0;
    if(*pan>max_pan)*pan=max_pan;
    if(*pan<0)*pan=0;
    int x0=right-width-1,y1=top+visible+3;
    *box=(GraphPanel){x0,top,right,y1,1};
    term_fill(top,x0,y1-top+1,width+2,cfg->background);
    for(int c=x0+1;c<right;c++){term_put(top,c,"─",cfg->border,cfg->background);term_put(y1,c,"─",cfg->border,cfg->background);}
    for(int r=top+1;r<y1;r++){term_put(r,x0,"│",cfg->border,cfg->background);term_put(r,right,"│",cfg->border,cfg->background);}
    term_put(top,x0,"┌",cfg->border,cfg->background);term_put(top,right,"┐",cfg->border,cfg->background);
    term_put(y1,x0,"└",cfg->border,cfg->background);term_put(y1,right,"┘",cfg->border,cfg->background);
    panel_text(top,x0+2,title,0,content,cfg->text,cfg->background);
    for(int i=0;i<visible;i++) {
        int row=i+*offset;
        panel_text(top+1+i,x0+2,lines[row],*pan,content,cfg->text,cfg->background);
        if(p_color&&p_color[row]) {
            int start=p_column[row],skip=*pan-start,col=x0+2+start-*pan,available=content;
            if(skip<0){available+=skip;skip=0;}else col=x0+2;
            if(available>0)panel_text(top+1+i,col,lines[row]+start,skip,available,p_color[row]>0?cfg->up:cfg->down,cfg->background);
        }
    }
    panel_text(y1-1,x0+2,footer,0,content,cfg->muted,cfg->background);
}

static void graph_tests(const Settings *cfg,const RegressionGraph *g,
                        int left,int top,int right,int bot,int *offset,int *pan,GraphPanel *box)
{
    CoefficientTests tests;int valid=!coefficient_tests(g->x,g->y,g->count,g->degree,&g->model,&tests);
    char lines[40][512];int p_column[40]={0},p_color[40]={0};int n=0;
    snprintf(lines[n++],256,"%s, two-sided H0: beta = 0",g->model.family?"nonlinear least squares (approximate t-tests)":"ordinary OLS");
    int indices[26];unsigned terms=model_terms(&g->model,g->degree);
    int count=term_indices(terms,indices);
    snprintf(lines[n++],256,"%d observations, %d residual degrees of freedom",g->count,g->count-count-(g->model.family?1:0));
    if(g->model.mean==0&&g->model.scale==1) {
        if(g->model.family==FAMILY_LOGARITHMIC)snprintf(lines[n++],256,"z = ln(x - (%.12g)) (unstandardized basis)",g->model.shift);
        else snprintf(lines[n++],256,"z = x (unstandardized basis)");
    } else if(g->model.family==FAMILY_LOGARITHMIC)snprintf(lines[n++],256,"z = (ln(x - (%.12g)) - %.12g) / %.12g",g->model.shift,g->model.mean,g->model.scale);
    else snprintf(lines[n++],256,"z = (x - %.12g) / %.12g",g->model.mean,g->model.scale);
    snprintf(lines[n++],256,"x = %s%s; y = %s%s",graph_axis_is_change(g,0)?"change of ":"",g->x_name,
             graph_axis_is_change(g,1)?"change of ":"",g->y_name);
    size_t used;
    if(g->model.family==FAMILY_EXPONENTIAL)used=(size_t)snprintf(lines[n],sizeof lines[n],"y fitted = %.12g %s exp(b0",g->model.shift,g->model.exp_sign>0?"+":"-");
    else used=(size_t)snprintf(lines[n],sizeof lines[n],"y fitted = %sb0",g->model.log_y?"exp(":"");
    int penultimate=0,omitted=0,ellipsis=0;
    for(int j=1;j<g->degree;j++)if(terms&(1u<<j))penultimate=j;
    for(int j=3;j<penultimate;j++)if(terms&(1u<<j))omitted++;
    for(int j=1;j<=g->degree;j++) {
        if(!(terms&(1u<<j)))continue;
        if(omitted>3&&j>=3&&j<penultimate) {
            if(!ellipsis) {
                int added=snprintf(lines[n]+used,sizeof lines[n]-used," + ...");
                if(added<0||(size_t)added>=sizeof lines[n]-used)break;
                used+=(size_t)added;ellipsis=1;
            }
            continue;
        }
        size_t remaining=sizeof lines[n]-used;
        int added=j==1?snprintf(lines[n]+used,remaining," + b1*z"):
            snprintf(lines[n]+used,remaining," + b%d*z^%d",j,j);
        if(added<0||(size_t)added>=remaining)break;
        used+=(size_t)added;
    }
    if(g->model.log_y||g->model.family==FAMILY_EXPONENTIAL)snprintf(lines[n]+used,sizeof lines[n]-used,")");
    n++;
    snprintf(lines[n++],256,"Coefficients below define the equation in this z basis.");
    if(g->significance>0)snprintf(lines[n++],256,"Alpha %.6g: green p <= alpha; red p > alpha; n/a uncolored",g->significance);
    snprintf(lines[n++],256,"beta       estimate       std.error        t              p");
    for(int j=0;j<=g->degree;j++) {
        if(!(terms&(1u<<j))){snprintf(lines[n++],256,"b%d removed",j);continue;}
        char se[32],t[32],p[32];
        if(valid) {
            snprintf(se,sizeof se,"%.7g",tests.se[j]);
            if(isnan(tests.t[j]))snprintf(t,sizeof t,"n/a");else snprintf(t,sizeof t,"%.7g",tests.t[j]);
            if(isnan(tests.p[j]))snprintf(p,sizeof p,"n/a");else snprintf(p,sizeof p,"%.7g",tests.p[j]);
        } else {strcpy(se,"n/a");strcpy(t,"n/a");strcpy(p,"n/a");}
        int prefix=snprintf(lines[n],256,"b%-2d %16.9g %14s %14s ",j,g->model.coefficients[j],se,t);
        p_column[n]=prefix;
        snprintf(lines[n]+prefix,256-(size_t)prefix,"%14s",p);
        if(valid&&g->significance>0&&isfinite(tests.p[j]))p_color[n]=tests.p[j]<=g->significance?1:-1;
        n++;
    }
    if(!valid)snprintf(lines[n++],256,"Coefficient covariance could not be calculated for this model.");
    if(g->model.family==FAMILY_POLYNOMIAL) {
        snprintf(lines[n++],256,"Overall F-test: all retained slopes = 0%s",g->model.log_y?" (log-y space)":"");
        double f,p;int df1,df2;
        if(valid&&!overall_f_test(g,&f,&p,&df1,&df2)) {
            int prefix=snprintf(lines[n],256,"F(%d, %d) = %.7g; p = ",df1,df2,f);
            p_column[n]=prefix;snprintf(lines[n]+prefix,256-(size_t)prefix,"%.7g",p);
            if(g->significance>0)p_color[n]=p<=g->significance?1:-1;
            n++;
        } else snprintf(lines[n++],256,"F = n/a; p = n/a");
    }
    graph_panel(cfg," Coefficient tests ",lines,p_column,p_color,n,left,top,right,bot,offset,pan,box);
}

static void graph_info(const Settings *cfg,const RegressionGraph *g,
                       int left,int top,int right,int bot,int *offset,int *pan,GraphPanel *box)
{
    char lines[40][512];int n=0;
    const char *p=g->title;
    while(*p&&n<40) {
        const char *end=strstr(p," | ");size_t len=end?(size_t)(end-p):strlen(p);
        size_t take=len<sizeof lines[n]-1?len:sizeof lines[n]-1;
        memcpy(lines[n],p,take);lines[n][take]=0;n++;
        p+=take;
        if(take==len&&end)p=end+3;
    }
    if(!n)snprintf(lines[n++],512,"No creation information available");
    graph_panel(cfg," Graph creation settings ",lines,NULL,NULL,n,left,top,right,bot,offset,pan,box);
}

static void graph_save_prompt(const Settings *cfg,const char *name,const char *error,size_t cursor)
{
    char label[200];snprintf(label,sizeof label,"%s%sSave graph as: ",error[0]?error:"",error[0]?". ":"");
    menu_draw_prompt(cfg,label,name,cursor);
}

static void diagnostic_axes(const Settings *cfg,int left,int top,int right,int bottom,
                             double xmin,double xmax,double ymin,double ymax,int observations,const RegressionGraph *x_axis)
{
    int width=right-left-1,height=bottom-top-1;
    for(int c=left+1;c<right;c++){term_put(top,c,"─",cfg->border,cfg->background);term_put(bottom,c,"─",cfg->border,cfg->background);}
    for(int r=top+1;r<bottom;r++){term_put(r,left,"│",cfg->border,cfg->background);term_put(r,right,"│",cfg->border,cfg->background);}
    term_put(top,left,"┌",cfg->border,cfg->background);term_put(top,right,"┐",cfg->border,cfg->background);
    term_put(bottom,left,"└",cfg->border,cfg->background);term_put(bottom,right,"┘",cfg->border,cfg->background);
    ScaleTicks ticks=scale_ticks(height,ymax-ymin);
    for(int i=0;i<ticks.count;i++) {
        int offset=ticks.first+i*ticks.gap,row=top+1+offset;char label[24];
        axis_y_value(label,0,0,1,scale_row_value(ymin,ymax,height,offset,1));
        term_text(row,left-1-term_text_width(label),label,cfg->muted,cfg->background);
        term_text(row,right+2,label,cfg->muted,cfg->background);
        term_put(row,left,"┤",cfg->muted,cfg->background);term_put(row,right,"├",cfg->muted,cfg->background);
    }
    double step=nice_step((xmax-xmin)/fmax(2,width/12));if(observations&&step<1)step=1;
    long long tick=first_tick(xmin,step);
    for(int i=0;i<100&&tick*step<=xmax;i++,tick++) {
        double x=tick*step;int col=left+1+(int)((x-xmin)/(xmax-xmin)*(width*2-1))/2;
        char label[24];if(observations)snprintf(label,sizeof label,"%.0f",x);else axis_value(label,x_axis&&x_axis->x_is_time,x_axis?x_axis->x_epoch:0,x_axis?x_axis->x_time_unit:1,x);
        int start=col-term_text_width(label)/2;
        if(col>left&&col<right&&start>left&&start+term_text_width(label)<right) {
            term_text(bottom+1,start,label,cfg->muted,cfg->background);term_put(bottom,col,"┬",cfg->border,cfg->background);
        }
    }
}

static void diagnostic_mask(const Settings *cfg,const unsigned char *mask,int left,int top,int width,int height,Color color)
{
    for(int row=0;row<height;row++)for(int col=0;col<width;col++)if(mask[row*width+col]) {
        char glyph[4];braille(mask[row*width+col],glyph);term_put(top+1+row,left+1+col,glyph,color,cfg->background);
    }
}

/* Shared block raster for both density histograms. Bin densities already
 * use the appropriate complete sample count, including off-screen values. */
static void histogram_bars(const ResidualHistogram *hist,unsigned char *bars,int width,int height,
                            double xmin,double xmax,double ymin,double ymax)
{
    double xr=xmax-xmin,yr=ymax-ymin,units=height*8;
    for(int col=0;col<width;col++) {
        double x=xmin+(col+.5)/width*xr;
        if(x<hist->minimum||x>=hist->maximum)continue;
        int bin=(int)((x-hist->minimum)/hist->width);
        if(bin>=hist->bins)bin=hist->bins-1;
        double edge=hist->minimum+(bin+1)*hist->width;
        if(hist->width/xr*width>=3&&edge-x<xr/width)continue;
        int bar_top=(int)ceil(fmax(0,fmin(units,(ymax-hist->density[bin])/yr*units)));
        int bar_bottom=(int)floor(fmax(0,fmin(units,ymax/yr*units)));
        for(int u=bar_top;u<bar_bottom;u++)bars[(u/8)*width+col]|=(unsigned char)(1u<<(u%8));
    }
}

/* Bars use block glyphs only. Omit fragments with no matching block rather
 * than approximating them with Braille. Curve cells always draw on top. */
static void histogram_cells(const Settings *cfg,const unsigned char *bars,const unsigned char *normal,
                             int left,int top,int width,int height)
{
    static const char *blocks[]={" ","▁","▂","▃","▄","▅","▆","▇","█"};
    for(int row=0;row<height;row++)for(int col=0;col<width;col++) {
        int cell=row*width+col;unsigned bar=bars[cell],curve=normal?normal[cell]:0;
        if(!bar&&!curve)continue;
        if(curve) {
            char glyph[4];braille((unsigned char)curve,glyph);
            term_put(top+1+row,left+1+col,glyph,cfg->up,cfg->background);
        } else {
            int start=0,end=8;while(start<8&&!(bar&(1u<<start)))start++;
            while(end>start&&!(bar&(1u<<(end-1))))end--;
            if(end==8)term_put(top+1+row,left+1+col,blocks[8-start],cfg->bollinger,cfg->background);
            else if(start==0)term_put(top+1+row,left+1+col,blocks[8-end],cfg->background,cfg->bollinger);
        }
    }
}

static int make_residuals(const RegressionGraph *g,double *values,ResidualStats *stats)
{
    int indices[26],parameters=term_indices(model_terms(&g->model,g->degree),indices)+(g->model.family?1:0);
    for(int i=0;i<g->count;i++) {
        if(g->model.log_y) {
            if(g->y[i]<=0)return -1;
            double z=(g->x[i]-g->model.mean)/g->model.scale,fit=g->model.coefficients[g->degree];
            for(int j=g->degree-1;j>=0;j--)fit=fit*z+g->model.coefficients[j];
            values[i]=log(g->y[i])-fit;
        } else values[i]=g->y[i]-g->fit[i];
    }
    return residual_statistics(values,g->count,g->count-parameters,stats);
}

static void diagnostic_click(const Settings *cfg,int left,int top,int right,int bottom,
                              double xmin,double xmax,double ymin,double ymax,double x,double y,const RegressionGraph *x_axis)
{
    int width=right-left-1,height=bottom-top-1;char label[24];
    if(y>=ymin&&y<=ymax) {
        int row=top+1+(int)lround((ymax-y)/(ymax-ymin)*(height*4-1))/4;
        axis_y_value(label,0,0,1,y);
        term_fill(row,0,1,left,cfg->background);term_fill(row,right+1,1,term_cols()-right-1,cfg->background);
        term_text(row,left-1-term_text_width(label),label,cfg->crosshair,cfg->background);term_text(row,right+2,label,cfg->crosshair,cfg->background);
    }
    if(x>=xmin&&x<=xmax) {
        axis_value(label,x_axis&&x_axis->x_is_time,x_axis?x_axis->x_epoch:0,x_axis?x_axis->x_time_unit:1,x);int col=left+1+(int)lround((x-xmin)/(xmax-xmin)*(width*2-1))/2-term_text_width(label)/2;
        if(col<0)col=0;
        if(col+term_text_width(label)>term_cols())col=term_cols()-term_text_width(label);
        term_text(bottom+1,col,label,cfg->crosshair,cfg->background);
    }
}

static void residual_view(const Settings *cfg,const RegressionGraph *g)
{
    double *values=malloc((size_t)g->count*sizeof *values);ResidualStats stats;
    if(!values)return;
    if(make_residuals(g,values,&stats)) {
        term_begin_frame(cfg->background);term_text(1,2,"Cannot calculate finite residuals for this graph. Esc returns.",cfg->down,cfg->background);term_end_frame();
        while(1){int k=term_read_key();if(k==KEY_ESC||k=='q'||k==KEY_EOF||k==3)break;}
        free(values);return;
    }
    char sw[128],acf[40],variance[40],header[512];
    if(isfinite(stats.shapiro_w)) {
        if(isfinite(stats.shapiro_p))snprintf(sw,sizeof sw,"Shapiro-Wilk W = %.6g; p = %.6g",stats.shapiro_w,stats.shapiro_p);
        else snprintf(sw,sizeof sw,"Shapiro-Wilk W = %.6g; p = n/a (n > 5000)",stats.shapiro_w);
    } else snprintf(sw,sizeof sw,"Shapiro-Wilk W = n/a; p = n/a");
    if(isfinite(stats.lag1))snprintf(acf,sizeof acf,"%.6g",stats.lag1);else strcpy(acf,"n/a");
    if(isfinite(stats.sigma2))snprintf(variance,sizeof variance,"%.6g",stats.sigma2);else strcpy(variance,"n/a");
    double low=fmin(0,stats.minimum),high=fmax(0,stats.maximum),span=high-low;
    if(span<=0){low=-.5;high=.5;span=1;}else {low-=span*.05;high+=span*.05;span=high-low;}
    double reg_min=g->x[0],reg_max=g->x[0];
    for(int i=1;i<g->count;i++){reg_min=fmin(reg_min,g->x[i]);reg_max=fmax(reg_max,g->x[i]);}
    double centers[2]={(g->count+1)/2.0,reg_min+(reg_max-reg_min)/2},zooms[2]={1,1};
    double spans[2]={fmax(1,g->count-1)*1.1,(reg_max>reg_min?(reg_max-reg_min):1)*1.1};
    double yc=(low+high)/2,zy=1;int x_mode=0;
    double hxc=0,hyc=0,hzx=1,hzy=1;
    int histogram=1,focus_histogram=0,h_initialized=0,click=0,show_zero=1,show_normal=1;double click_x=0,click_y=0;
    for(;;) {
        term_begin_frame(cfg->background);
        snprintf(header,sizeof header,"Residuals%s | x: %s | %d observations | sigma2 = %s | lag-1 autocorrelation = %s | %s",g->model.log_y?" (log-y space)":"",x_mode?g->x_name:"observation order",g->count,variance,acf,sw);
        int rows=term_rows(),cols=term_cols(),left=7,right=cols-8,bottom=rows-3;
        int top=text_header(NULL,header,left,cols-left-7),h_top=0,plot_bottom=bottom;
        int h_height=rows/4;if(h_height<4)h_height=4;
        if(histogram){h_top=bottom-h_height-1;plot_bottom=h_top-2;}
        int width=right-left-1,height=plot_bottom-top-1;
        if(width<8||height<3||(histogram&&h_top<=top+4)) {
            term_text(0,1,"Terminal too small for residual diagnostics (n hides histogram)",cfg->market,cfg->background);term_end_frame();
            int k=term_read_key();if(k==KEY_ESC||k=='q'||k==3||k==KEY_EOF)break;
            if(k=='n'){histogram=!histogram;if(!histogram)focus_histogram=0;}
            continue;
        }
        double xr=spans[x_mode]/zooms[x_mode],yr=span/zy;
        if(!isfinite(xr)||!isfinite(yr)||xr<fmin(spans[x_mode]*1e-12,1e-12)||xr<=0||yr<span*1e-12||yr<=0){zooms[x_mode]=zy=1;continue;}
        double xmin=centers[x_mode]-xr/2,xmax=centers[x_mode]+xr/2,ymin=yc-yr/2,ymax=yc+yr/2;
        diagnostic_axes(cfg,left,top,right,plot_bottom,xmin,xmax,ymin,ymax,!x_mode,x_mode?g:NULL);
        unsigned char *points=calloc((size_t)width*height,1),*line=calloc((size_t)width*height,1);
        unsigned short *density=calloc((size_t)width*height,sizeof *density);
        if(!points||!line||!density){free(points);free(line);free(density);break;}
        for(int i=0;i<g->count;i++) {
            double px=((x_mode?g->x[i]:i+1)-xmin)/(xmax-xmin)*(width*2-1),py=(ymax-values[i])/(ymax-ymin)*(height*4-1);
            if(px>=0&&px<width*2&&py>=0&&py<height*4)add_scatter(points,density,width,height,(int)lround(px),(int)lround(py));
        }
        for(int row=0;row<height;row++)for(int col=0;col<width;col++) {
            int p=row*width+col;
            if(points[p]) {
                char glyph[4];braille(points[p],glyph);
                term_put(top+1+row,left+1+col,glyph,scatter_color(cfg->bollinger,cfg->down,density[p]),cfg->background);
            }
        }
        free(density);
        if(show_zero&&ymin<=0&&ymax>=0) {
            int py=(int)lround(ymax/(ymax-ymin)*(height*4-1));
            for(int px=0;px<width*2;px++)add_dot(line,width,height,px,py);
        }
        diagnostic_mask(cfg,line,left,top,width,height,cfg->up);free(points);free(line);
        ResidualHistogram hist={0};double hxmin=0,hxmax=1,hymin=0,hymax=1,hxr=1,hyr=1;
        if(histogram&&!residual_histogram(values,&stats,width/3,&hist)) {
            double density_max=hist.maximum_density*1.1;if(density_max<=0)density_max=1;
            if(!h_initialized){hxc=(hist.minimum+hist.maximum)/2;hyc=density_max/2;h_initialized=1;}
            hxr=(hist.maximum-hist.minimum)/hzx;hyr=density_max/hzy;
            if(!isfinite(hxr)||hxr<=0||hxr<(hist.maximum-hist.minimum)*1e-12){hzx=1;hxr=hist.maximum-hist.minimum;}
            if(!isfinite(hyr)||hyr<=0||hyr<density_max*1e-12){hzy=1;hyr=density_max;}
            hxmin=hxc-hxr/2;hxmax=hxc+hxr/2;hymin=hyc-hyr/2;hymax=hyc+hyr/2;
            ResidualHistogram visible;
            if(!residual_histogram_view(values,&stats,&hist,hxmin,hxmax,&visible))hist=visible;
            diagnostic_axes(cfg,left,h_top,right,bottom,hxmin,hxmax,hymin,hymax,0,NULL);
            unsigned char *bars=calloc((size_t)width*h_height,1);
            if(!bars)break;
            histogram_bars(&hist,bars,width,h_height,hxmin,hxmax,hymin,hymax);
            unsigned char *normal=calloc((size_t)width*h_height,1);
            if(show_normal&&normal&&isfinite(stats.sigma2)&&stats.sigma2>0) {
                double last_y=0,last_x=0;
                for(int i=0;i<=width*8;i++) {
                    double x=hxmin+hxr*i/(width*8.0);
                    double density=exp(-.5*(x-stats.mean)*(x-stats.mean)/stats.sigma2)/sqrt(2*M_PI*stats.sigma2);
                    double px=(double)i/(width*8)*(width*2-1),py=(hymax-density)/hyr*(h_height*4-1);
                    if(i)curve_segment(normal,width,h_height,last_x,last_y,px,py);
                    last_x=px;last_y=py;
                }
            }
            histogram_cells(cfg,bars,normal,left,h_top,width,h_height);
            free(bars);free(normal);
        }
        text_header(cfg,header,left,cols-left-7);
        if(click==1)diagnostic_click(cfg,left,top,right,plot_bottom,xmin,xmax,ymin,ymax,click_x,click_y,x_mode?g:NULL);
        if(click==2&&histogram&&hist.bins)diagnostic_click(cfg,left,h_top,right,bottom,hxmin,hxmax,hymin,hymax,click_x,click_y,NULL);
        int row=rows-1,col=left;
        col+=term_text(row,col,"j",cfg->text,cfg->background);
        col+=term_text(row,col,focus_histogram?" focus: histogram  ":" focus: residues  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"t",x_mode?cfg->bollinger:cfg->text,cfg->background);
        col+=term_text(row,col,x_mode?" x: regressor  ":" x: observation order  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"k",(focus_histogram?show_normal:show_zero)?cfg->bollinger:cfg->text,cfg->background);
        col+=term_text(row,col,focus_histogram?" normal line  ":" zero line  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"n",histogram?cfg->bollinger:cfg->text,cfg->background);
        col+=term_text(row,col," histogram  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"←↑↓→",cfg->text,cfg->background);col+=term_text(row,col," pan  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"z/x",cfg->text,cfg->background);col+=term_text(row,col," zoom x  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"c/v",cfg->text,cfg->background);col+=term_text(row,col," zoom y  ",cfg->muted,cfg->background);
        col+=term_text(row,col,"Esc",cfg->text,cfg->background);term_text(row,col," back",cfg->muted,cfg->background);
        term_end_frame();int k=term_read_key();
        if(k==KEY_ESC||k=='q'||k==KEY_EOF||k==3)break;
        if(k=='t'){x_mode=!x_mode;if(click==1)click=0;continue;}
        if(k=='n'){histogram=!histogram;if(!histogram){focus_histogram=0;if(click==2)click=0;}}
        if(k=='j'&&histogram)focus_histogram=!focus_histogram;
        if(k=='k'){if(focus_histogram)show_normal=!show_normal;else show_zero=!show_zero;}
        double *active_x=focus_histogram?&hxc:&centers[x_mode],*active_y=focus_histogram?&hyc:&yc;
        double *active_zx=focus_histogram?&hzx:&zooms[x_mode],*active_zy=focus_histogram?&hzy:&zy;
        double active_xr=focus_histogram?hxr:xr,active_yr=focus_histogram?hyr:yr;
        if(k==KEY_LEFT)*active_x-=active_xr*.1;
        if(k==KEY_RIGHT)*active_x+=active_xr*.1;
        if(k==KEY_UP)*active_y+=active_yr*.1;
        if(k==KEY_DOWN)*active_y-=active_yr*.1;
        if(k=='z')*active_zx*=1.25;
        if(k=='x')*active_zx/=1.25;
        if(k=='c')*active_zy*=1.25;
        if(k=='v')*active_zy/=1.25;
        if(k==KEY_MOUSE) {
            int button,mrow,mcol;term_mouse(&button,&mrow,&mcol);
            if(button==2)click=0;
            else if(button==0&&mcol>left&&mcol<right) {
                if(mrow>top&&mrow<plot_bottom){click=1;click_x=xmin+(mcol-left-.5)/width*(xmax-xmin);click_y=ymax-(mrow-top-.5)/height*(ymax-ymin);}
                else if(histogram&&hist.bins&&mrow>h_top&&mrow<bottom){click=2;click_x=hxmin+(mcol-left-.5)/width*hxr;click_y=hymax-(mrow-h_top-.5)/h_height*hyr;}
            }
        }
    }
    free(values);
}

typedef struct { double x,mean,variance,sigma; } ConditionalDistribution;

static int conditional_distribution(const RegressionGraph *g,double x,ConditionalDistribution *out)
{
    if(!isfinite(x))return -1;
    double *residuals=malloc((size_t)g->count*sizeof *residuals);ResidualStats stats;
    if(!residuals)return -1;
    int failed=make_residuals(g,residuals,&stats);free(residuals);
    if(failed||!isfinite(stats.sigma2)||stats.sigma2<0)return -1;
    double mean=model_value(g,x),variance=stats.sigma2;
    if(g->model.log_y) {
        double z=(x-g->model.mean)/g->model.scale,eta=g->model.coefficients[g->degree];
        for(int j=g->degree-1;j>=0;j--)eta=eta*z+g->model.coefficients[j];
        mean=exp(eta+stats.sigma2/2);
        variance=stats.sigma2==0?0:exp(2*eta+stats.sigma2)*expm1(stats.sigma2);
    }
    if(!isfinite(mean)||!isfinite(variance)||variance<0)return -1;
    *out=(ConditionalDistribution){x,mean,variance,sqrt(variance)};return 0;
}

static double conditional_density(const ConditionalDistribution *d,double y)
{
    if(d->sigma<=0)return 0;
    double z=(y-d->mean)/d->sigma;
    return exp(-.5*z*z)/(sqrt(2*M_PI)*d->sigma);
}

static double conditional_radius(const RegressionGraph *g,double x,double percent)
{
    double reference=fabs(x);
    if(reference==0) {
        double lo=g->x[0],hi=lo;
        for(int i=1;i<g->count;i++){lo=fmin(lo,g->x[i]);hi=fmax(hi,g->x[i]);}
        reference=hi-lo;
    }
    return reference*(percent/100);
}

static int parse_finite_input(const char *input,int percent,double *value)
{
    char *end;errno=0;*value=strtod(input,&end);
    if(end==input||errno||!isfinite(*value))return -1;
    while(isspace((unsigned char)*end))end++;
    if(percent&&*end=='%'){end++;while(isspace((unsigned char)*end))end++;}
    return *end?-1:0;
}

static int parse_distribution_x(const RegressionGraph *g,const char *input,double *x)
{
    if(g->x_is_time&&g->x_time_unit>0&&strlen(input)==10&&input[4]=='-'&&input[7]=='-') {
        for(int i=0;i<10;i++)if(i!=4&&i!=7&&!isdigit((unsigned char)input[i]))return -1;
        long day=day_number(input);char check[11];day_date(day,check);
        if(strcmp(check,input))return -1;
        *x=(double)(day-g->x_epoch)/g->x_time_unit;return 0;
    }
    return parse_finite_input(input,0,x);
}

static void clicked_distribution_x(const RegressionGraph *g,double x,char input[64])
{
    if(g->x_is_time)axis_value(input,1,g->x_epoch,g->x_time_unit,x);
    else snprintf(input,64,"%.17g",x);
}

static int conditional_sample(const RegressionGraph *g,double x,double radius,double *values)
{
    int count=0;
    for(int i=0;i<g->count;i++)if(fabs(g->x[i]-x)<=radius)values[count++]=g->y[i];
    return count;
}

static int conditional_histogram(const double *values,int count,int max_bins,double xmin,double xmax,double initial_range,ResidualHistogram *out)
{
    if(count<1)return -1;
    ResidualStats sample={.count=count,.minimum=values[0],.maximum=values[0],.sigma2=NAN};
    long double sum=0;
    for(int i=0;i<count;i++) {
        if(!isfinite(values[i]))return -1;
        sample.minimum=fmin(sample.minimum,values[i]);sample.maximum=fmax(sample.maximum,values[i]);sum+=values[i];
    }
    sample.mean=(double)(sum/count);ResidualHistogram base;
    if(residual_histogram(values,&sample,max_bins,&base))return -1;
    /* Use about 50% more bins for the conditional distribution histogram. */
    if(base.bins>1) {
        base.bins=(base.bins*3+1)/2;
        /* The initial plot can be much wider than the selected observations.
         * Preserve the intended bin count across their range in that case. */
        double sample_range=base.maximum-base.minimum;
        double target=ceil(base.bins*fmax(1,initial_range/sample_range));
        base.bins=!isfinite(target)||target>max_bins?max_bins:(int)target;
        if(base.bins>max_bins)base.bins=max_bins;
        if(base.bins>126)base.bins=126;
    }
    return residual_histogram_view(values,&sample,&base,xmin,xmax,out);
}

static void conditional_view(const Settings *cfg,const RegressionGraph *g,const ConditionalDistribution *d)
{
    static double last_margin=1;
    double margin=last_margin,radius=conditional_radius(g,d->x,margin);
    double half=d->sigma>0?4*d->sigma:fmax(1,fabs(d->mean)*.05);
    half=fmax(half,fabs(d->mean)*1e-12);
    double peak=d->sigma>0?conditional_density(d,d->mean):1;
    if(!isfinite(half)||!isfinite(peak)||peak<=0)return;
    double *values=malloc((size_t)g->count*sizeof *values);if(!values)return;
    double extent=peak,xc=d->mean,yc=peak*.525,zx=1,zy=1;
    int initial_scale=1;
    int asking_margin=0,has_click=0;char input[64]="",error[128]="";size_t cursor=0;
    int asking_prediction=0,show_prediction=0;
    char last_prediction[64]="95";double prediction_low=0,prediction_high=0,prediction_level=95;
    double click_x=0,click_y=0;
    for(;;) {
        term_begin_frame(cfg->background);
        int selected=conditional_sample(g,d->x,radius,values);
        int show_histogram=selected>=3;
        char header[768],x_label[24];axis_value(x_label,g->x_is_time,g->x_epoch,g->x_time_unit,d->x);
        snprintf(header,sizeof header,"Y | X=x: %s | x: %s (%s) | E[Y|X=x] = %.8g | sigma = %.8g | %d/%d nearby observations, +/- %.6g%% (%.6g x units)",
                 g->y_name,x_label,g->x_name,d->mean,d->sigma,
                 selected,g->count,margin,radius);
        if(show_prediction) {
            size_t used=strlen(header);
            snprintf(header+used,sizeof header-used," | %.6g%% prediction interval%s: [%.10g, %.10g]",prediction_level,
                     g->model.log_y?" (lognormal)":g->model.family?" (approx)":"",prediction_low,prediction_high);
        }
        int rows=term_rows(),cols=term_cols(),left=7,right=cols-8,bottom=rows-3;
        int top=text_header(NULL,header,left,cols-left-7),width=right-left-1,height=bottom-top-1;
        if(width<8||height<3) {
            term_text(0,1,"Terminal too small for conditional distribution",cfg->market,cfg->background);term_end_frame();
            int k=term_read_key();if(k==KEY_ESC||k=='q'||k==KEY_EOF||k==3)break;continue;
        }
        double xr=2*half/zx,yr=extent*1.15/zy;
        if(!isfinite(xr)||!isfinite(yr)||xr<half*1e-12||yr<extent*1e-12||xr<=0||yr<=0){zx=zy=1;continue;}
        double xmin=xc-xr/2,xmax=xc+xr/2;
        ResidualHistogram hist={0};
        int have_histogram=show_histogram&&!conditional_histogram(values,selected,width/2,xmin,xmax,2*half,&hist);
        if(initial_scale) {
            if(have_histogram){extent=fmax(peak,hist.maximum_density);yc=extent*.525;yr=extent*1.15/zy;}
            initial_scale=0;
        }
        double ymin=yc-yr/2,ymax=yc+yr/2;
        diagnostic_axes(cfg,left,top,right,bottom,xmin,xmax,ymin,ymax,0,NULL);
        unsigned char *bars=calloc((size_t)width*height,1),*curve=calloc((size_t)width*height,1),*mean=calloc((size_t)width*height,1);
        if(!bars||!curve||!mean){free(bars);free(curve);free(mean);break;}
        if(have_histogram)histogram_bars(&hist,bars,width,height,xmin,xmax,ymin,ymax);
        if(d->mean>=xmin&&d->mean<=xmax) {
            int px=(int)lround((d->mean-xmin)/xr*(width*2-1));
            for(int py=0;py<height*4;py++)add_dot(mean,width,height,px,py);
        }
        histogram_cells(cfg,bars,NULL,left,top,width,height);
        if(show_prediction) {
            memset(curve,0,(size_t)width*height);
            double limits[]={prediction_low,prediction_high};
            for(int i=0;i<2;i++)if(limits[i]>=xmin&&limits[i]<=xmax) {
                int px=(int)lround((limits[i]-xmin)/xr*(width*2-1));
                for(int py=0;py<height*4;py++)add_dot(curve,width,height,px,py);
            }
            diagnostic_mask(cfg,curve,left,top,width,height,cfg->crosshair);
        }
        diagnostic_mask(cfg,mean,left,top,width,height,cfg->down);
        free(bars);free(curve);free(mean);
        text_header(cfg,header,left,cols-left-7);
        if(has_click)diagnostic_click(cfg,left,top,right,bottom,xmin,xmax,ymin,ymax,click_x,click_y,NULL);
        const struct {const char *key,*description;} keys[]={
            {"p","prediction interval"},{"m","observation window"},{"\u2190\u2191\u2193\u2192","pan"},{"z/x","zoom Y"},{"c/v","zoom density"},{"Esc","back"}};
        int col=left;for(size_t i=0;i<sizeof keys/sizeof keys[0];i++) {
            col+=term_text(rows-1,col,keys[i].key,(i==0&&show_prediction)?cfg->bollinger:cfg->text,cfg->background);
            col+=term_text(rows-1,col," ",cfg->muted,cfg->background);
            col+=term_text(rows-1,col,keys[i].description,cfg->muted,cfg->background);
            col+=term_text(rows-1,col,"  ",cfg->muted,cfg->background);
        }
        if(asking_margin||asking_prediction) {
            char label[256];snprintf(label,sizeof label,"%s%s%s",error,error[0]?". ":"",
                                    asking_prediction?"Prediction interval level (%, 0 < value < 100): ":"Nearby X window (+/- %, histogram only): ");
            menu_draw_prompt(cfg,label,input,cursor);
        }
        term_end_frame();int k=term_read_key();
        if(asking_prediction) {
            if(k==3||k==KEY_EOF)break;
            if(k==KEY_ESC){asking_prediction=0;continue;}
            if(k=='\r'||k=='\n') {
                double level,low,high;MeanConfidence ci;
                if(parse_finite_input(input,1,&level)||level<=0||level>=100)
                    snprintf(error,sizeof error,"Enter a percentage greater than 0 and less than 100");
                else if(confidence_prepare(g,level,&ci)||prediction_interval(g,&ci,d->x,&low,&high))
                    snprintf(error,sizeof error,"Cannot calculate a reliable prediction interval for this model");
                else {
                    prediction_low=low;prediction_high=high;prediction_level=level;
                    snprintf(last_prediction,sizeof last_prediction,"%s",input);
                    asking_prediction=0;show_prediction=1;
                }
            } else if(menu_edit_key(input,sizeof input,&cursor,k))error[0]=0;
            continue;
        }
        if(asking_margin) {
            if(k==3||k==KEY_EOF)break;
            if(k==KEY_ESC){asking_margin=0;continue;}
            if(k=='\r'||k=='\n') {
                double value;
                if(parse_finite_input(input,1,&value)||value<0||!isfinite(conditional_radius(g,d->x,value)))
                    snprintf(error,sizeof error,"Enter a finite percentage of 0 or more");
                else {margin=last_margin=value;radius=conditional_radius(g,d->x,margin);asking_margin=0;}
            } else if(menu_edit_key(input,sizeof input,&cursor,k))error[0]=0;
            continue;
        }
        if(k==KEY_ESC||k=='q'||k==3||k==KEY_EOF)break;
        if(k=='p') {
            if(show_prediction)show_prediction=0;
            else {snprintf(input,sizeof input,"%s",last_prediction);cursor=strlen(input);error[0]=0;asking_prediction=1;}
        }
        if(k=='m'){snprintf(input,sizeof input,"%.12g",margin);cursor=strlen(input);error[0]=0;asking_margin=1;}
        if(k==KEY_LEFT)xc-=xr*.1;
        if(k==KEY_RIGHT)xc+=xr*.1;
        if(k==KEY_UP)yc+=yr*.1;
        if(k==KEY_DOWN)yc-=yr*.1;
        if(k=='z')zx*=1.25;
        if(k=='x')zx/=1.25;
        if(k=='c')zy*=1.25;
        if(k=='v')zy/=1.25;
        if(k==KEY_MOUSE) {
            int button,row,column;term_mouse(&button,&row,&column);
            if(button==2)has_click=0;
            else if(button==0&&column>left&&column<right&&row>top&&row<bottom) {
                has_click=1;click_x=xmin+(column-left-.5)/width*xr;click_y=ymax-(row-top-.5)/height*yr;
            }
        }
    }
    free(values);
}

static void graph_footer(const Settings *cfg,int left,int show_fit,int panel,int bands,const char *band_label,const char *status)
{
    int row=term_rows()-1,col=left;
    if(status[0]){term_text(row,col,status,cfg->muted,cfg->background);return;}
    const struct {const char *key,*description;int active;} items[]={
        {"s","save",0},{"p","tests",panel=='p'},{"i","info",panel=='i'},
        {"r","regression line",show_fit},{"b",band_label,bands},{"e","Y given x",0},{"h","residuals",0},{"Esc","back",0},
        {"←↑↓→","pan",0},{"z/x","zoom x",0},{"c/v","zoom y",0}
    };
    for(size_t i=0;i<sizeof items/sizeof items[0];i++) {
        int key_width=term_text_width(items[i].key);
        int width=key_width+1+term_text_width(items[i].description);
        if(col+width>term_cols())break;
        term_text(row,col,items[i].key,items[i].active?cfg->bollinger:cfg->text,cfg->background);
        term_text(row,col+key_width+1,items[i].description,cfg->muted,cfg->background);
        col+=width+2;
    }
}

void regression_view(const Settings *cfg, RegressionGraph *g, const StockList *list)
{
    double xc=0, yc=0, zx=1, zy=1, data_xmin=g->x[0], data_xmax=g->x[0];
    double data_ymin=g->y[0], data_ymax=g->y[0];
    int has_click=0,show_fit=1,panel=0,panel_offset=0,panel_pan=0;
    int saving=0;char save_name[256]="",save_error[128]="",save_status[320]="";
    static char last_confidence[32]="95";
    int show_bands=0,asking_confidence=0;MeanConfidence confidence={0};
    char confidence_input[32]="",confidence_error[128]="";size_t confidence_cursor=0;
    int asking_x=0;char x_input[64]="",x_error[128]="";size_t x_cursor=0;
    size_t save_cursor=0;
    double click_x=0, click_y=0;
    for (int i=0;i<g->count;i++) {
        if(g->x[i]<data_xmin)data_xmin=g->x[i];
        if(g->x[i]>data_xmax)data_xmax=g->x[i];
        if(g->y[i]<data_ymin)data_ymin=g->y[i];
        if(g->y[i]>data_ymax)data_ymax=g->y[i];
        if(g->fit[i]<data_ymin)data_ymin=g->fit[i];
        if(g->fit[i]>data_ymax)data_ymax=g->fit[i];
    }
    /* Include between-observation curvature in the initial vertical range. */
    for(int i=0;i<=2048;i++) {
        double x=data_xmin+(data_xmax-data_xmin)*i/2048;
        double y=model_value(g,x);
        if(!isfinite(y))continue;
        if(y<data_ymin)data_ymin=y;
        if(y>data_ymax)data_ymax=y;
    }
    /* Center the full bounds so the initial view includes every observation
     * and fitted value, even when the distribution is strongly skewed. */
    xc=data_xmin+(data_xmax-data_xmin)/2;
    yc=data_ymin+(data_ymax-data_ymin)/2;
    for (;;) {
        term_begin_frame(cfg->background);
        int rows=term_rows(), cols=term_cols(), bot=rows-3;
        /* Six-character y labels require at most seven columns per side. */
        int header_width=cols-14;
        int top=graph_header(NULL,g,0,header_width);
        int height=bot-top-1;
        double xr=(data_xmax-data_xmin)*1.1/zx, yr=(data_ymax-data_ymin)*1.1/zy;
        if(xr<=0)xr=1;
        if(yr<=0)yr=1;
        double xmin=xc-xr/2, xmax=xc+xr/2, ymin=yc-yr/2, ymax=yc+yr/2;
        ScaleTicks yticks=scale_ticks(height,ymax-ymin);
        int margin=scale_margin(g,ymin,ymax,height,yticks,has_click,click_y);
        int left=margin, right=cols-1-margin;
        int width=right-left-1;
        if (width < 2 || height < 2) {
            term_text(0, 1, "Terminal is too small for a graph", cfg->market, cfg->background);
            term_end_frame(); if (term_read_key() == KEY_ESC) return; continue;
        }
        unsigned char *fit=calloc((size_t)width*height,1), *points=calloc((size_t)width*height,1);
        unsigned char *bands=show_bands?calloc((size_t)width*height,1):NULL;
        unsigned short *density=calloc((size_t)width*height,sizeof *density);
        if (!fit || !points || !density || (show_bands&&!bands)) { free(fit); free(points); free(density);free(bands); return; }
        for(int c=left;c<=right;c++){term_put(top,c,"─",cfg->border,cfg->background);term_put(bot,c,"─",cfg->border,cfg->background);}
        for(int r=top;r<=bot;r++){term_put(r,left,"│",cfg->border,cfg->background);term_put(r,right,"│",cfg->border,cfg->background);}
        term_put(top,left,"┌",cfg->border,cfg->background); term_put(top,right,"┐",cfg->border,cfg->background);
        term_put(bot,left,"└",cfg->border,cfg->background); term_put(bot,right,"┘",cfg->border,cfg->background);
        for(int i=0;i<g->count;i++) {
            int px=(int)lround((g->x[i]-xmin)/(xmax-xmin)*(width*2-1));
            int py=(int)lround((ymax-g->y[i])/(ymax-ymin)*(height*4-1)); add_scatter(points,density,width,height,px,py);
        }
        if (show_fit)
            draw_curve(g,fit,width,height,xmin,xmax,ymin,ymax,data_xmin,data_xmax);
        if(show_bands)draw_confidence(g,&confidence,bands,width,height,xmin,xmax,ymin,ymax,data_xmin,data_xmax);
        for(int r=0;r<height;r++) for(int c=0;c<width;c++) {
            int p=r*width+c; char ch[4];
            if(points[p]) { braille(points[p],ch); term_put(top+1+r,left+1+c,ch,scatter_color(cfg->bollinger,cfg->down,density[p]),cfg->background); }
            if(bands&&bands[p]) { braille(bands[p],ch);term_put(top+1+r,left+1+c,ch,cfg->down,cfg->background); }
            if(fit[p]) { braille(fit[p],ch); term_put(top+1+r,left+1+c,ch,cfg->up,cfg->background); }
        }
        for(int i=0;i<yticks.count;i++) {
            int offset=yticks.first+i*yticks.gap;
            double value=scale_row_value(ymin,ymax,height,offset,1);
            int row=top+1+offset;
            char label[24]; axis_y_value(label,g->y_is_time,g->y_epoch,g->y_time_unit,value);
            term_text(row,left-1-term_text_width(label),label,cfg->muted,cfg->background);
            term_text(row,right+2,label,cfg->muted,cfg->background);
            term_put(row,left,"┤",cfg->muted,cfg->background);
            term_put(row,right,"├",cfg->muted,cfg->background);
        }
        int target=width/12;
        if(target<2)target=2;
        double xstep=nice_step((xmax-xmin)/target);
        long long tick=first_tick(xmin,xstep);
        for(int count=0;(double)tick*xstep<=xmax&&count<100;tick++,count++) {
            double value=(double)tick*xstep;
            double pos=(value-xmin)/(xmax-xmin)*(width*2);
            if(pos<0||pos>=width*2)continue; /* leaves both bottom corners intact */
            char label[24]; axis_value(label,g->x_is_time,g->x_epoch,g->x_time_unit,value);
            int col=left+1+(int)pos/2-term_text_width(label)/2;
            if(col<left+1||col+term_text_width(label)>right)continue;
            term_text(bot+1,col,label,cfg->muted,cfg->background);
            term_put(bot,left+1+(int)pos/2,"┬",cfg->border,cfg->background);
        }
        graph_header(cfg,g,left,header_width);
        if (has_click) {
            char sx[24], sy[24];
            axis_value(sx,g->x_is_time,g->x_epoch,g->x_time_unit,click_x);
            axis_y_value(sy,g->y_is_time,g->y_epoch,g->y_time_unit,click_y);
            int py=(int)lround((ymax-click_y)/(ymax-ymin)*(height*4-1));
            int row=top+1+py/4;
            if (row > top && row < bot) {
                term_fill(row,0,1,left,cfg->background);
                term_fill(row,right+1,1,cols-right-1,cfg->background);
                term_text(row,left-1-term_text_width(sy),sy,cfg->crosshair,cfg->background);
                term_text(row,right+2,sy,cfg->crosshair,cfg->background);
                term_put(row,left,"┤",cfg->crosshair,cfg->background);
                term_put(row,right,"├",cfg->crosshair,cfg->background);
            }
            int px=(int)lround((click_x-xmin)/(xmax-xmin)*(width*2-1));
            int col=left+1+px/2-term_text_width(sx)/2;
            if(col<0)col=0;
            if(col+term_text_width(sx)>cols)col=cols-term_text_width(sx);
            if(px>=0&&px<width*2)term_text(bot+1,col,sx,cfg->crosshair,cfg->background);
        }
        char band_label[80]="mean CI";
        if(show_bands)snprintf(band_label,sizeof band_label,"%.6g%% mean CI%s",confidence.level,
                              g->model.log_y?" (approx lognormal)":g->model.family?" (approx)":"");
        graph_footer(cfg,left,show_fit,panel,show_bands,band_label,save_status);
        GraphPanel box={0};
        if(panel=='p')graph_tests(cfg,g,left,top,right,bot,&panel_offset,&panel_pan,&box);
        if(panel=='i')graph_info(cfg,g,left,top,right,bot,&panel_offset,&panel_pan,&box);
        if(saving)graph_save_prompt(cfg,save_name,save_error,save_cursor);
        if(asking_confidence) {
            char label[256];snprintf(label,sizeof label,"%s%sConfidence level (%%, 0 < value < 100): ",confidence_error,
                                    confidence_error[0]?". ":"");
            menu_draw_prompt(cfg,label,confidence_input,confidence_cursor);
        }
        if(asking_x) {
            char label[256];snprintf(label,sizeof label,"%s%s%s",x_error,x_error[0]?". ":"",
                                    g->x_is_time?"X value (YYYY-MM-DD or graph coordinate): ":"X value (graph coordinate): ");
            menu_draw_prompt(cfg,label,x_input,x_cursor);
        }
        free(fit); free(points); free(density);free(bands); term_end_frame();
        int k=term_read_key();
        if(asking_x) {
            if(k==3||k==KEY_EOF)return;
            if(k==KEY_ESC){asking_x=0;continue;}
            if(k==KEY_MOUSE) {
                int button,row,col;term_mouse(&button,&row,&col);
                if(box.visible&&row>=box.top&&row<=box.bottom&&col>=box.left&&col<=box.right)continue;
                if(button==2)has_click=0;
                else if(button==0&&row>top&&row<bot&&col>left&&col<right) {
                    has_click=1;click_x=xmin+(col-left-.5)/width*(xmax-xmin);
                    click_y=ymax-(row-top-.5)/height*(ymax-ymin);
                    clicked_distribution_x(g,click_x,x_input);
                    x_cursor=strlen(x_input);x_error[0]=0;
                }
                continue;
            }
            if(k=='\r'||k=='\n') {
                double x;ConditionalDistribution distribution;
                if(parse_distribution_x(g,x_input,&x))snprintf(x_error,sizeof x_error,"Enter a finite X value%s",g->x_is_time?" or valid date":"");
                else if(conditional_distribution(g,x,&distribution))
                    snprintf(x_error,sizeof x_error,"Cannot estimate distribution at this X (domain, variance or residual degrees of freedom)");
                else {asking_x=0;conditional_view(cfg,g,&distribution);}
            } else if(menu_edit_key(x_input,sizeof x_input,&x_cursor,k))x_error[0]=0;
            continue;
        }
        if(asking_confidence) {
            if(k==3||k==KEY_EOF)return;
            if(k==KEY_ESC){asking_confidence=0;continue;}
            if(k=='\r'||k=='\n') {
                char *end;errno=0;double level=strtod(confidence_input,&end);
                while(isspace((unsigned char)*end))end++;
                if(*end=='%'){end++;while(isspace((unsigned char)*end))end++;}
                if(end==confidence_input||*end||errno||!isfinite(level)||level<=0||level>=100)
                    snprintf(confidence_error,sizeof confidence_error,"Enter a percentage greater than 0 and less than 100");
                else if(confidence_prepare(g,level,&confidence))
                    snprintf(confidence_error,sizeof confidence_error,"Cannot calculate reliable mean confidence intervals for this model");
                else {
                    snprintf(last_confidence,sizeof last_confidence,"%s",confidence_input);
                    asking_confidence=0;show_bands=1;
                }
            } else if(menu_edit_key(confidence_input,sizeof confidence_input,&confidence_cursor,k))confidence_error[0]=0;
            continue;
        }
        if(saving) {
            size_t len=strlen(save_name);
            if(k==3||k==KEY_EOF)return;
            if(k==KEY_ESC){saving=0;save_error[0]=0;continue;}
            if(k=='\r'||k=='\n') {
                if(!save_graph(list,g,save_name,save_error,sizeof save_error)) {
                    snprintf(save_status,sizeof save_status,"Saved graph: %s%s",save_name,
                             len>=6&&!strcmp(save_name+len-6,".graph")?"":".graph");saving=0;
                }
            } else if(menu_edit_key(save_name,sizeof save_name,&save_cursor,k))save_error[0]=0;
            continue;
        }
        if(k!=KEY_RESIZE)save_status[0]=0;
        if(k=='e') {
            if(has_click)clicked_distribution_x(g,click_x,x_input);
            else if(!x_input[0])axis_value(x_input,g->x_is_time,g->x_epoch,g->x_time_unit,xc);
            x_cursor=strlen(x_input);x_error[0]=0;asking_x=1;continue;
        }
        if(k=='b') {
            if(show_bands)show_bands=0;
            else {
                snprintf(confidence_input,sizeof confidence_input,"%s",last_confidence);
                confidence_cursor=strlen(confidence_input);confidence_error[0]=0;asking_confidence=1;
            }
            continue;
        }
        if(k=='s') {
            if(!suggest_graph_name(list,g,save_name,save_error,sizeof save_error)){saving=1;save_error[0]=0;save_cursor=strlen(save_name);}
            else snprintf(save_status,sizeof save_status,"%s",save_error);
            continue;
        }
        if(k==KEY_ESC||k=='q'||k==3||k==KEY_EOF)return;
        if(k=='h'){residual_view(cfg,g);continue;}
        if(k=='i'||k=='p'){panel=panel==k?0:k;panel_offset=panel_pan=0;continue;}
        if(panel) {
            if(k=='j'){panel_offset++;continue;}
            if(k=='k'){if(panel_offset>0)panel_offset--;continue;}
            if(k=='L'){panel_pan+=4;continue;}
            if(k=='H'){panel_pan-=4;if(panel_pan<0)panel_pan=0;continue;}
        }
        if(k==KEY_MOUSE){
            int button,row,col; term_mouse(&button,&row,&col);
            if(box.visible&&row>=box.top&&row<=box.bottom&&col>=box.left&&col<=box.right)continue;
            if(button==2) has_click=0;
            else if(button==0&&row>top&&row<bot&&col>left&&col<right){
                click_x=xmin+(col-left-1+0.5)/width*(xmax-xmin);
                click_y=ymax-(row-top-1+0.5)/height*(ymax-ymin);
                has_click=1;
            }
        }
        if(k==KEY_LEFT)xc-=xr*.1;
        if(k==KEY_RIGHT)xc+=xr*.1;
        if(k==KEY_UP)yc+=yr*.1;
        if(k==KEY_DOWN)yc-=yr*.1;
        if(k=='z')zx*=1.25;
        if(k=='x')zx/=1.25;
        if(k=='c')zy*=1.25;
        if(k=='v')zy/=1.25;
        if(k=='r')show_fit=!show_fit;
    }
}
