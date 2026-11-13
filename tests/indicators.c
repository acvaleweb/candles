#include "indicators.h"
#include "swing.h"
#include "data.h"
#include "test_support.h"

static Settings config;

static void moving_averages_and_bands(void)
{
    Candle c[5]={0}; Stock s={.candles=c,.count=5}; double volume[5],upper[5],lower[5];
    for (int i=0;i<5;i++) { c[i].close=i+1; c[i].volume=(i+1)*10; }
    ind_volume_mean(&s,3,volume); ind_bollinger(&s,3,2,upper,lower);
    CHECK(isnan(volume[0]) && isnan(volume[1]) && isnan(upper[1]));
    for (int i=2;i<5;i++) {
        NEAR(volume[i],i*10,1e-14);
        NEAR(upper[i],i+2*sqrt(2.0/3),1e-14);
        NEAR(lower[i],i-2*sqrt(2.0/3),1e-14);
    }
    ind_volume_mean(&s,1,volume); ind_bollinger(&s,1,2,upper,lower);
    for (int i=0;i<5;i++) {
        NEAR(volume[i],c[i].volume,1e-14);
        NEAR(upper[i],c[i].close,1e-14); NEAR(lower[i],c[i].close,1e-14);
    }
    ind_volume_mean(&s,6,volume);
    for (int i=0;i<5;i++) CHECK(isnan(volume[i]));
}

static void wilder_rsi(void)
{
    Candle c[6]={0}; Stock s={.candles=c,.count=6}; double out[6];
    double prices[]={10,11,10,12,11,13};
    for (int i=0;i<6;i++) c[i].close=prices[i];
    ind_rsi(&s,3,out); CHECK(isnan(out[0]) && isnan(out[1]) && isnan(out[2]));
    NEAR(out[3],75,1e-12); NEAR(out[4],600.0/11,1e-12); NEAR(out[5],75,1e-12);
    for (int mode=0;mode<3;mode++) {
        for (int i=0;i<6;i++) c[i].close=mode==0?10:mode==1?10+i:10-i;
        ind_rsi(&s,3,out);
        for (int i=3;i<6;i++) NEAR(out[i],mode==0?50:mode==1?100:0,1e-14);
    }
}

static Stock pair(Candle c[2])
{
    c[0]=(Candle){.open=10,.close=10,.high=11,.low=9,.volume=100};
    c[1]=(Candle){.open=11,.close=12,.high=12,.low=10,.volume=200};
    strcpy(c[0].date,"2026-01-01"); strcpy(c[1].date,"2026-01-02");
    Stock s={.candles=c,.count=2,.has_volume=1}; strcpy(s.ticker,"TEST"); return s;
}

static void classic_shape_and_pressure(void)
{
    Candle c[2]; Stock s=pair(c); double out[2]; Settings cfg=config;
    cfg.swing_rsi_weight=cfg.swing_volume_weight=cfg.swing_bollinger_weight=0;
    cfg.swing_candle_weight=1;
    CHECK(!swing_calculate(&s,&cfg,out)); CHECK(isnan(out[0])); NEAR(out[1],.75,1e-12);
    c[1].high=12.1; CHECK(!swing_calculate(&s,&cfg,out)); NEAR(out[1],.5,1e-12);
    pair(c); c[1].open=12; c[1].close=11; c[1].high=12; c[1].low=9;
    CHECK(!swing_calculate(&s,&cfg,out)); NEAR(out[1],.5,1e-12); /* Red hammer. */
    pair(c);
    for (int i=0;i<2;i++) {
        double high=c[i].high;
        c[i].open=20-c[i].open; c[i].close=20-c[i].close;
        c[i].high=20-c[i].low; c[i].low=20-high;
    }
    CHECK(!swing_calculate(&s,&cfg,out)); NEAR(out[1],-.75,1e-12);
    pair(c); c[1].open=10; c[1].close=11; c[1].high=20;
    cfg.swing_candle_weight=0; cfg.swing_volume_weight=1;
    CHECK(!swing_calculate(&s,&cfg,out));
    NEAR(out[1],-.8*(.2+.8/3),1e-12); /* Close near low gives negative pressure. */
    c[1].volume=50; CHECK(!swing_calculate(&s,&cfg,out)); NEAR(out[1],0,1e-14);
    pair(c); cfg=config; cfg.bb_std=2;
    double classic[2]; CHECK(!swing_calculate(&s,&cfg,classic));
    CHECK(!swing_calculate_bb(&s,&cfg,out)); CHECK(classic[1]!=0); NEAR(out[1],0,1e-14);
    c[1].low=9; CHECK(!swing_calculate(&s,&cfg,classic));
    CHECK(!swing_calculate_bb(&s,&cfg,out)); NEAR(out[1],classic[1],1e-14);
}

static void binary_rules_and_exports(void)
{
    Candle c[2]; Stock s=pair(c); Settings cfg=config; double a[2],b[2];
    c[1].open=1; c[1].close=2; c[1].high=2.1; c[1].low=0;
    CHECK(!swing_calculate2(&s,&cfg,a)); CHECK(!swing_calculate3(&s,&cfg,b));
    CHECK(isnan(a[0]) && a[1]==1 && b[1]==1);
    c[1].volume=50;
    CHECK(!swing_calculate2(&s,&cfg,a)); CHECK(!swing_calculate3(&s,&cfg,b));
    CHECK(a[1]==0 && b[1]==1); /* Exactly three satisfied conditions. */
    c[1].high=8;
    CHECK(!swing_calculate3(&s,&cfg,b)); CHECK(b[1]==0);
    const char *files[]={"TEST_classic_swing_strategy2.csv","TEST_classic_swing_strategy3.csv","TEST_3green.csv"};
    for (int strategy=1;strategy<=3;strategy++) {
        cfg.swing_strategy=strategy; char error[512],file[256];
        CHECK(!swing_export(&s,&cfg,test_root,error,sizeof error));
        snprintf(file,sizeof file,"data/%s",files[strategy-1]);
        char *contents=test_read(file);
        CHECK(strstr(contents,"2026-01-01,\n")!=NULL); /* Warmup stays blank. */
        CHECK(strstr(contents,"2026-01-02,0\n")!=NULL); /* Binary zero must be saved. */
        free(contents);
        DataSeries data={0}; CHECK(!data_load(&data,test_root,files[strategy-1],"Strategy",error,sizeof error));
        CHECK(data.count==1 && data.points[0].value==0); data_free(&data);
    }
}

static void green_boundaries(void)
{
    Candle c[2]; Stock s=pair(c); double out[2];
    c[1].open=10; c[1].low=10; c[1].high=12.5;
    CHECK(!swing_calculate3green(&s,&config,out)); CHECK(out[1]==1); /* Upper wick exactly 20%. */
    c[1].high=12.6; CHECK(!swing_calculate3green(&s,&config,out)); CHECK(out[1]==0);
    c[1].high=12.5; c[1].volume=100;
    CHECK(!swing_calculate3green(&s,&config,out)); CHECK(out[1]==0); /* Strictly above mean. */
    c[1].volume=200; c[1].open=10.5; c[1].low=10.4;
    CHECK(!swing_calculate3green(&s,&config,out)); CHECK(out[1]==0);
    c[1].low=8; CHECK(!swing_calculate3green(&s,&config,out)); CHECK(out[1]==1); /* >=25% below band. */
}

int main(void)
{
    test_setup(); settings_load(&config);
    config.rsi_len=1; config.bb_len=2; config.bb_std=1; config.volume_ma_len=2;
    RUN(moving_averages_and_bands); RUN(wilder_rsi); RUN(classic_shape_and_pressure);
    RUN(binary_rules_and_exports); RUN(green_boundaries); return test_done("Indicators and strategies");
}
