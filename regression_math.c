#include "regression_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

unsigned regression_model_terms(const RegressionModel *model,int degree)
{
    return model->terms?model->terms:(1u<<(degree+1))-1;
}

int regression_term_indices(unsigned mask,int indices[26])
{
    int count=0;for(int j=0;j<26;j++)if(mask&(1u<<j))indices[count++]=j;
    return count;
}

static int fit_terms(const double *x, const double *y, int n, int degree, int logy,unsigned terms,int standardize,
                      double *pred, double *score, RegressionModel *model)
{
    int indices[26],p=regression_term_indices(terms,indices); if (!p || n <= p) return -1;
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

double regression_evaluate(const RegressionModel *m,int degree,double x,double gradient[27])
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

double regression_model_value(const RegressionGraph *g, double x)
{
    return regression_evaluate(&g->model,g->degree,x,NULL);
}

static double nonlinear_system(const double *x,const double *y,int n,int degree,
                               const RegressionModel *m,double *matrix,double *rhs)
{
    int indices[26],count=regression_term_indices(regression_model_terms(m,degree),indices),p=count+1;
    if(matrix)memset(matrix,0,(size_t)p*p*sizeof *matrix);
    if(rhs)memset(rhs,0,(size_t)p*sizeof *rhs);
    double sse=0;
    for(int i=0;i<n;i++) {
        double gradient[27],fit=regression_evaluate(m,degree,x[i],matrix?gradient:NULL);
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
int regression_fit(const double *x,const double *y,int n,int degree,int logy,unsigned terms,int family,int standardize,
                      double *pred,double *score,RegressionModel *model)
{
    if(!family||degree==0)return fit_terms(x,y,n,degree,logy,terms,standardize,pred,score,model);
    int indices[26],count=regression_term_indices(terms,indices),p=count+1;
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
    double sst=0;for(int i=0;i<n;i++){sst+=(y[i]-ym)*(y[i]-ym);pred[i]=regression_evaluate(model,degree,x[i],NULL);}
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

int regression_overall_f_test(const RegressionGraph *g,double *value,double *probability,int *df1,int *df2)
{
    int indices[26],count=regression_term_indices(regression_model_terms(&g->model,g->degree),indices);
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
int regression_coefficient_tests(const double *x,const double *y,int n,int degree,
                             const RegressionModel *model,RegressionCoefficientTests *out)
{
    int indices[26],count=regression_term_indices(regression_model_terms(model,degree),indices);out->df=n-count;
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
int regression_confidence_prepare(const RegressionGraph *g,double level,RegressionConfidence *out)
{
    memset(out,0,sizeof *out);
    if(!g->model.valid||g->model.scale<=0||!isfinite(g->model.scale))return -1;
    int p=regression_term_indices(regression_model_terms(&g->model,g->degree),out->indices);
    if(g->model.family)out->indices[p++]=26;
    out->count=p;out->df=g->count-p;out->level=level;
    out->critical=confidence_critical(level,out->df);
    if(!isfinite(out->critical))return -1;
    double matrix[27*27]={0},scales[27];long double sse=0;
    for(int i=0;i<g->count;i++) {
        double gradient[27],fit=regression_evaluate(&g->model,g->degree,g->x[i],gradient),response=g->y[i];
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

static int response_interval(const RegressionGraph *g,const RegressionConfidence *ci,double x,int prediction,double *lower,double *upper)
{
    double gradient[27],fit=regression_evaluate(&g->model,g->degree,x,gradient);
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

int regression_confidence_interval(const RegressionGraph *g,const RegressionConfidence *ci,double x,double *lower,double *upper)
{
    return response_interval(g,ci,x,0,lower,upper);
}

int regression_prediction_interval(const RegressionGraph *g,const RegressionConfidence *ci,double x,double *lower,double *upper)
{
    return response_interval(g,ci,x,1,lower,upper);
}
