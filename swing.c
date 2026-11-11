#include "swing.h"
#include "indicators.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static double unit(double x){return fmax(0,fmin(1,x));}

int swing_calculate(const Stock *s,const Settings *cfg,double *out)
{
    if(s->count<1)return 0;
    double *work=malloc((size_t)s->count*4*sizeof *work);
    if(!work)return -1;
    double *rsi=work,*volume=work+s->count,*upper=work+2*s->count,*lower=work+3*s->count;
    ind_rsi(s,cfg->rsi_len,rsi);ind_volume_mean(s,cfg->volume_ma_len,volume);
    ind_bollinger(s,cfg->bb_len,cfg->bb_std,upper,lower);
    double weights=cfg->swing_rsi_weight+cfg->swing_volume_weight+cfg->swing_bollinger_weight+cfg->swing_candle_weight;
    for(int i=0;i<s->count;i++) {
        out[i]=NAN;
        if(!isfinite(rsi[i])||!isfinite(volume[i])||!isfinite(upper[i])||!isfinite(lower[i])||weights<=0)continue;
        const Candle *c=&s->candles[i];
        if(!isfinite(c->open)||!isfinite(c->close)||!isfinite(c->high)||!isfinite(c->low)||!isfinite(c->volume)||c->high<c->low||c->volume<0)continue;
        double direction=c->close>c->open?1:c->close<c->open?-1:0;
        double range=c->high-c->low;
        double r=rsi[i]<=cfg->rsi_lower?1:rsi[i]>=cfg->rsi_upper?-1:0;
        double v=0;
        if(range>0&&volume[i]>0&&c->volume>=volume[i]) {
            double pressure=2*unit((c->close-c->low)/range)-1;
            v=pressure*(cfg->swing_volume_base+(1-cfg->swing_volume_base)*unit(c->volume/volume[i]-1));
        }
        double middle=(upper[i]+lower[i])/2,half=(upper[i]-lower[i])/2;
        double b=half>0?fmax(-1,fmin(1,(middle-c->open)/half)):0;
        double k=0;
        if(range>0&&direction!=0) {
            double distance=direction>0?c->high-c->close:c->close-c->low;
            double wick=direction>0?c->open-c->low:c->high-c->open;
            double body=fabs(c->close-c->open);
            double wick_ratio=fmax(0,wick)/body;
            double wick_factor=1-.5/(1+wick_ratio);
            k=direction*unit(1-distance/(cfg->swing_close_tolerance*body))*wick_factor;
            double reversal_wick=direction<0?c->close-c->low:c->high-c->close;
            double opposite_wick=direction<0?c->high-c->open:c->open-c->low;
            if(body>0&&opposite_wick<=0&&reversal_wick>=body) {
                double ratio=reversal_wick/body;
                double strength=.75*(1-1/(1+ratio));
                k=-direction*strength;
            }
        }
        out[i]=(cfg->swing_rsi_weight*r+cfg->swing_volume_weight*v+cfg->swing_bollinger_weight*b+cfg->swing_candle_weight*k)/weights;
    }
    free(work);return 0;
}

static int swing_binary(const Stock *s,const Settings *cfg,double *out,int minimum)
{
    if(s->count<1)return 0;
    double *work=malloc((size_t)s->count*4*sizeof *work);if(!work)return -1;
    double *r=work,*v=work+s->count,*u=work+2*s->count,*l=work+3*s->count;
    ind_rsi(s,cfg->rsi_len,r);ind_volume_mean(s,cfg->volume_ma_len,v);ind_bollinger(s,cfg->bb_len,cfg->bb_std,u,l);
    for(int i=0;i<s->count;i++) {
        const Candle *c=&s->candles[i];double range=c->high-c->low;
        out[i]=NAN;
        if(!isfinite(r[i])||!isfinite(v[i])||!isfinite(l[i])||!isfinite(c->open)||!isfinite(c->close)||!isfinite(c->high)||!isfinite(c->low)||!isfinite(c->volume)||range<0||c->volume<0)continue;
        int candle=range>0&&c->close>c->open&&(c->high-c->close)<.1*range&&(c->open-c->low)>=.3*range;
        int satisfied=(c->volume>=v[i])+candle+(c->low<=l[i])+(r[i]<=5);
        out[i]=satisfied>=minimum?1:0;
    }
    free(work);return 0;
}

int swing_calculate2(const Stock *s,const Settings *cfg,double *out){return swing_binary(s,cfg,out,4);}
int swing_calculate3(const Stock *s,const Settings *cfg,double *out){return swing_binary(s,cfg,out,3);}

int swing_calculate_bb(const Stock *s,const Settings *cfg,double *out)
{
    if(swing_calculate(s,cfg,out))return -1;
    if(s->count<1)return 0;
    double *bands=malloc((size_t)s->count*2*sizeof *bands);if(!bands)return -1;
    double *upper=bands,*lower=bands+s->count;
    ind_bollinger(s,cfg->bb_len,cfg->bb_std,upper,lower);
    for(int i=0;i<s->count;i++)if(isfinite(out[i])) {
        double low=s->candles[i].low,high=s->candles[i].high;
        int touches=(low<=lower[i]&&high>=lower[i])||(low<=upper[i]&&high>=upper[i]);
        if(!touches)out[i]=0;
    }
    free(bands);return 0;
}

int swing_calculate3green(const Stock *s,const Settings *cfg,double *out)
{
    if(s->count<1)return 0;
    double *work=malloc((size_t)s->count*3*sizeof *work);if(!work)return -1;
    double *v=work,*u=work+s->count,*l=work+2*s->count;
    ind_volume_mean(s,cfg->volume_ma_len,v);ind_bollinger(s,cfg->bb_len,cfg->bb_std,u,l);
    for(int i=0;i<s->count;i++) {
        out[i]=NAN;const Candle *c=&s->candles[i];
        if(!isfinite(v[i])||!isfinite(l[i])||!isfinite(c->open)||!isfinite(c->close)||!isfinite(c->high)||!isfinite(c->low)||!isfinite(c->volume)||c->high<c->low||c->volume<0)continue;
        double range=c->high-c->low;
        int band=c->open<=l[i]||(range>0&&(l[i]-c->low)>=.25*range);
        out[i]=range>0&&band&&c->volume>v[i]&&c->close>c->open&&(c->high-c->close)<=.2*range?1:0;
    }
    free(work);return 0;
}

int swing_export(const Stock *s,const Settings *cfg,const char *folder,char *error,size_t error_size)
{
    double *scores=malloc((size_t)s->count*sizeof *scores);
    if(!scores||(cfg->swing_strategy==4?swing_calculate_bb(s,cfg,scores):cfg->swing_strategy==3?swing_calculate3green(s,cfg,scores):cfg->swing_strategy==2?swing_calculate3(s,cfg,scores):cfg->swing_strategy==1?swing_calculate2(s,cfg,scores):swing_calculate(s,cfg,scores))){free(scores);snprintf(error,error_size,"Cannot calculate strategy");return -1;}
    char dir[PATH_MAX],path[PATH_MAX],temporary[PATH_MAX];
    const char *name=cfg->swing_strategy==4?"bb_strat":cfg->swing_strategy==3?"3green":cfg->swing_strategy==2?"classic_swing_strategy3":cfg->swing_strategy==1?"classic_swing_strategy2":"classic_swing_strategy";
    if(!folder[0]||strchr(s->ticker,'/')||strchr(s->ticker,'\\')||snprintf(dir,sizeof dir,"%s/data",folder)>=(int)sizeof dir||snprintf(path,sizeof path,"%s/%s_%s.csv",dir,s->ticker,name)>=(int)sizeof path||snprintf(temporary,sizeof temporary,"%s/.swing-XXXXXX",dir)>=(int)sizeof temporary) {
        free(scores);snprintf(error,error_size,"Invalid or excessively long data path");return -1;
    }
    if(mkdir(dir,0755)&&errno!=EEXIST){free(scores);snprintf(error,error_size,"Cannot create data folder: %s",strerror(errno));return -1;}
    int fd=mkstemp(temporary);FILE *f=fd<0?NULL:fdopen(fd,"w");
    if(!f){if(fd>=0){close(fd);unlink(temporary);}free(scores);snprintf(error,error_size,"Cannot create strategy CSV: %s",strerror(errno));return -1;}
    fputs("date,value\n",f);
    for(int i=0;i<s->count;i++) {
        if(isfinite(scores[i]))fprintf(f,"%s,%.17g\n",s->candles[i].date,scores[i]);
        else fprintf(f,"%s,\n",s->candles[i].date);
    }
    free(scores);int failed=ferror(f);if(fclose(f))failed=1;
    if(!failed&&rename(temporary,path)==0)return 0;
    unlink(temporary);snprintf(error,error_size,"Cannot save strategy CSV");return -1;
}
