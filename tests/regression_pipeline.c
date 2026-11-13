/* Integration coverage for private preparation/serialization paths. Numerical
 * functions are separately tested through their public, UI-free interface. */
#include "../regression.c"
#include "test_support.h"

static StockList sources;
static Settings config;

static Spec data_spec(void)
{
    Spec s; spec_defaults(&s);
    s.x=s.y=VAR_DATA; s.exact_degree=1; s.max_degree=0;
    strcpy(s.x_data_file,"x.csv"); strcpy(s.x_data_name,"X");
    strcpy(s.y_data_file,"y.csv"); strcpy(s.y_data_name,"Y");
    return s;
}

static RegressionGraph graph_for(Spec *spec)
{
    RegressionGraph g={0}; char error[512]="";
    int result=generate(&config,NULL,NULL,&sources,spec,error,sizeof error,&g);
    if (result) fprintf(stderr,"Generation failed: %s\n",error);
    CHECK(result==0 && g.count>=4 && g.model.valid);
    return g;
}

static void daily_data(int count,int plateau)
{
    char x_path[8192],y_path[8192];
    test_path(x_path,sizeof x_path,"data/x.csv"); test_path(y_path,sizeof y_path,"data/y.csv");
    FILE *x=fopen(x_path,"w"),*y=fopen(y_path,"w"); CHECK(x && y);
    fputs("date,value\n2025-12-31,0\n",x); fputs("date,value\n",y);
    for (int day=1;day<=count;day++) {
        fprintf(x,"2026-01-%02d,%d\n",day,plateau?(day+1)/2:day);
        fprintf(y,"2026-01-%02d,%d\n",day,10*day);
    }
    CHECK(!fclose(x)); CHECK(!fclose(y));
}

static void change_directions_and_periods(void)
{
    daily_data(12,0); Spec s=data_spec(); RegressionGraph g=graph_for(&s);
    CHECK(g.count==12); NEAR(g.x[0],1,1e-14); NEAR(g.y[11],120,1e-14); regression_graph_free(&g);
    s.x_change=s.y_change=1; s.x_direction=DIR_BACKWARD; s.y_direction=DIR_FORWARD;
    s.x_instant=s.y_instant=0; s.x_days=2; s.y_days=3;
    s.x_change_type=s.y_change_type=CHANGE_ABSOLUTE;
    g=graph_for(&s); CHECK(g.count==8);
    for (int i=0;i<g.count;i++) { NEAR(g.x[i],2,1e-14); NEAR(g.y[i],30,1e-14); }
    regression_graph_free(&g);
    s.x_change_type=s.y_change_type=CHANGE_PERCENT;
    g=graph_for(&s); CHECK(g.count==7); /* Zero initial percentage denominator is excluded. */
    NEAR(g.x[0],200,1e-12); NEAR(g.y[0],100,1e-12);
    NEAR(g.x[6],100*(9.0/7-1),1e-12); NEAR(g.y[6],100*(120.0/90-1),1e-12);
    regression_graph_free(&g);
    s.x_instant=1; s.x_days=999; s.x_change_type=CHANGE_ABSOLUTE;
    g=graph_for(&s); CHECK(g.count==9);
    for (int i=0;i<g.count;i++) NEAR(g.x[i],1,1e-14);
    regression_graph_free(&g);
    CHECK(isnan(change_value(0,1,CHANGE_PERCENT)));
    NEAR(change_value(0,1,CHANGE_ABSOLUTE),1,1e-14);
    CHECK(isnan(change_value(-1,1,CHANGE_LOG_RETURN)));
    NEAR(change_value(2,4,CHANGE_LOG_RETURN),100*log(2),1e-12);
    s.x_log_return=1; s.y_change=0;
    g=graph_for(&s); CHECK(g.count==11); NEAR(g.x[0],100*log(2),1e-12);
    regression_graph_free(&g);
}

static void sampled_endpoints_and_zero_filters(void)
{
    daily_data(18,0); Spec s=data_spec();
    s.sample_mode=SAMPLE_DISTANCE; s.sample_days=3;
    s.x_change=1; s.x_instant=1; s.x_direction=DIR_BACKWARD; s.x_change_type=CHANGE_ABSOLUTE;
    RegressionGraph g=graph_for(&s); CHECK(g.count==6);
    for (int i=0;i<g.count;i++) NEAR(g.x[i],1,1e-14);
    regression_graph_free(&g);
    s.sampled_changes=1; g=graph_for(&s); CHECK(g.count==5);
    for (int i=0;i<g.count;i++) NEAR(g.x[i],3,1e-14);
    NEAR(g.y[0],40,1e-14); regression_graph_free(&g);
    s.sample_mode=SAMPLE_ALL; g=graph_for(&s); CHECK(g.count==18);
    for (int i=0;i<g.count;i++) NEAR(g.x[i],1,1e-14);
    regression_graph_free(&g);
    s=data_spec(); s.sample_mode=SAMPLE_DISTANCE; s.sample_days=3;
    s.y_change=1; s.y_instant=1; s.y_direction=DIR_FORWARD; s.y_change_type=CHANGE_ABSOLUTE;
    g=graph_for(&s); CHECK(g.count==6);
    for (int i=0;i<g.count;i++) NEAR(g.y[i],10,1e-14);
    regression_graph_free(&g);
    s.sampled_changes=2; g=graph_for(&s); CHECK(g.count==5);
    for (int i=0;i<g.count;i++) NEAR(g.y[i],30,1e-14);
    regression_graph_free(&g);
    s.sampled_changes=3; s.x_change=1; s.x_instant=1;
    s.x_direction=DIR_BACKWARD; s.x_change_type=CHANGE_ABSOLUTE;
    g=graph_for(&s); CHECK(g.count==4);
    for (int i=0;i<g.count;i++) { NEAR(g.x[i],3,1e-14); NEAR(g.y[i],30,1e-14); }
    regression_graph_free(&g);
    daily_data(12,1); s=data_spec(); s.x_change=1; s.x_change_type=CHANGE_ABSOLUTE;
    g=graph_for(&s); CHECK(g.count==12); regression_graph_free(&g);
    s.exclude_zero_x=1; g=graph_for(&s); CHECK(g.count==6);
    for (int i=0;i<g.count;i++) NEAR(g.x[i],1,1e-14);
    regression_graph_free(&g);
    s=data_spec(); strcpy(s.y_data_file,"x.csv"); s.y_change=1; s.y_instant=1;
    s.y_direction=DIR_BACKWARD; s.y_change_type=CHANGE_ABSOLUTE;
    s.exclude_zero_y=1; g=graph_for(&s); CHECK(g.count==6); regression_graph_free(&g);
}

static void monthly_and_yearly_time_sampling(void)
{
    char path[8192]; test_path(path,sizeof path,"data/y.csv"); FILE *file=fopen(path,"w"); CHECK(file);
    for (int year=2019;year<=2023;year++) for (int month=1;month<=2;month++)
        for (int day=2;day<=3;day++) fprintf(file,"%04d-%02d-%02d,%d\n",year,month,day,year+month+day);
    CHECK(!fclose(file)); Spec s=data_spec(); s.x=VAR_TIME; s.sample_mode=SAMPLE_YEAR;
    RegressionGraph g=graph_for(&s); CHECK(g.count==5 && g.x_is_time);
    NEAR(g.x[0],0,1e-14); NEAR(g.x[1],365,1e-14); NEAR(g.x[2],731,1e-14);
    regression_graph_free(&g);
    s.sample_mode=SAMPLE_MONTH; g=graph_for(&s); CHECK(g.count==10);
    NEAR(g.x[1],31,1e-14); regression_graph_free(&g);
}

static void weekend_prior_close_both_axes(void)
{
    test_write("A.csv","2026-01-02,100,100,100,100\n2026-01-05,110,110,110,110\n"
        "2026-01-06,120,120,120,120\n2026-01-08,130,130,130,130\n"
        "2026-01-09,140,140,140,140\n2026-01-12,150,150,150,150\n");
    test_write("B.csv","2026-01-02,1,1,1,1\n2026-01-06,2,2,2,2\n2026-01-07,3,3,3,3\n"
        "2026-01-08,4,4,4,4\n2026-01-09,5,5,5,5\n2026-01-12,6,6,6,6\n");
    daily_data(12,0); char error[512];
    CHECK(!stocklist_scan(&sources,test_root,error,sizeof error)); CHECK(sources.count==2);
    Spec s=data_spec(); s.y=0;
    RegressionGraph g=graph_for(&s); CHECK(g.count==6); regression_graph_free(&g);
    s.stock_prior_close=1; g=graph_for(&s); CHECK(g.count==11);
    NEAR(g.x[0],2,1e-14); NEAR(g.x[1],3,1e-14); NEAR(g.y[1],100,1e-14);
    NEAR(g.y[2],100,1e-14); NEAR(g.y[3],110,1e-14); regression_graph_free(&g);
    s.y=VAR_DATA; s.x=0; g=graph_for(&s); CHECK(g.count==11);
    NEAR(g.x[1],100,1e-14); NEAR(g.y[1],30,1e-14); regression_graph_free(&g);
    s.y=1; s.stock_prior_close=0; g=graph_for(&s); CHECK(g.count==5); regression_graph_free(&g);
    s.stock_prior_close=1; g=graph_for(&s); CHECK(g.count==7); regression_graph_free(&g);
    Stock stock={0}; CHECK(!stock_load(&stock,test_root,"A.csv","A",error,sizeof error));
    CHECK(find_date(&stock,"2026-01-01",1)==-1);
    CHECK(find_date(&stock,"2026-01-03",0)==-1);
    CHECK(find_date(&stock,"2026-01-03",1)==0);
    NEAR(stock_value(&stock,0,1,1,DIR_FORWARD,0,CHANGE_PERCENT,day_number("2026-01-02")),10,1e-12);
    stock_free(&stock);
}

static void form_roundtrip(void)
{
    Spec s=data_spec(); s.x=0; s.y=1; s.x_change=s.y_change=1;
    s.x_days=17; s.y_days=360; s.sample_mode=SAMPLE_MONTH; s.sampled_changes=3;
    s.x_log_return=1; s.y_change_type=CHANGE_ABSOLUTE; s.exclude_zero_x=s.exclude_zero_y=1;
    s.stock_prior_close=1; s.max_degree=5; s.significance=.01; s.remove_intermediate=1;
    s.standardize=0; spec_save(&sources,&s); Spec loaded; spec_load(&sources,&loaded);
    CHECK(loaded.x==s.x && loaded.y==s.y && loaded.x_days==17 && loaded.y_days==360);
    CHECK(loaded.x_log_return && loaded.y_change_type==CHANGE_ABSOLUTE);
    CHECK(loaded.exclude_zero_x && loaded.exclude_zero_y && loaded.sampled_changes==3);
    CHECK(loaded.max_degree==5 && loaded.exact_degree && loaded.remove_intermediate);
    CHECK(!loaded.standardize && loaded.stock_prior_close && loaded.sample_mode==SAMPLE_MONTH);
    NEAR(loaded.significance,.01,1e-14);
    state_save_graph_form("bad|record"); spec_load(&sources,&loaded);
    CHECK(loaded.x==VAR_NONE && loaded.y==VAR_NONE && loaded.max_degree==1);
}

static void saved_graph_versions(void)
{
    char path[8192],error[512]; test_path(path,sizeof path,"legacy.graph");
    for (int version=1;version<=8;version++) {
        FILE *file=fopen(path,"w"); CHECK(file);
        fprintf(file,"CANDLES_GRAPH %d\ny: Y | x: X | significance alpha 0.05\nX\nY\n1 .9 4",version);
        if (version>=2) fputs(" 0 0 1 1 0 0",file);
        fputc('\n',file);
        if (version>=6) fputs("0.05\n",file);
        if (version>=7) fputs("3\n",file);
        if (version>=8) fputs("0 0 0\n",file);
        if (version>=3) fputs("0 0 1\n2\n3\n",file);
        for (int i=0;i<4;i++) fprintf(file,"%d,%d,%d\n",i,2+3*i,2+3*i);
        CHECK(!fclose(file)); RegressionGraph g={0};
        CHECK(!load_graph(path,&g,error,sizeof error)); CHECK(g.count==4 && g.model.valid);
        NEAR(regression_model_value(&g,1.25),5.75,1e-12); NEAR(g.significance,.05,1e-12);
        regression_graph_free(&g);
    }
    FILE *file=fopen(path,"w"); CHECK(file);
    fputs("CANDLES_GRAPH 1\nlegacy log-y\nX\nY\n1 .9 4\n",file);
    for (int i=0;i<4;i++) fprintf(file,"%d,%.17g,%.17g\n",i,exp(2+.3*i),exp(2+.3*i));
    CHECK(!fclose(file)); RegressionGraph g={0}; CHECK(!load_graph(path,&g,error,sizeof error));
    CHECK(g.model.log_y); NEAR(regression_model_value(&g,1.25),exp(2+.3*1.25),1e-12);
    regression_graph_free(&g);
}

static void save_reopen_sparse_and_corrupt(void)
{
    daily_data(12,0); Spec s=data_spec(); s.max_degree=3; s.significance=.05;
    RegressionGraph g=graph_for(&s); char error[512],path[8192],name[256];
    CHECK(!regression_fit(g.x,g.y,g.count,3,0,11,FAMILY_POLYNOMIAL,0,g.fit,&g.adjusted_r2,&g.model));
    CHECK(!save_graph(&sources,&g,"roundtrip",error,sizeof error));
    CHECK(save_graph(&sources,&g,"roundtrip",error,sizeof error)==-1);
    CHECK(save_graph(&sources,&g,"../escape",error,sizeof error)==-1);
    test_path(path,sizeof path,"graphs/roundtrip.graph"); RegressionGraph loaded={0};
    CHECK(!load_graph(path,&loaded,error,sizeof error));
    CHECK(loaded.count==g.count && loaded.degree==g.degree && loaded.model.terms==11);
    for (int i=0;i<g.count;i++) {
        NEAR(loaded.x[i],g.x[i],1e-14); NEAR(loaded.y[i],g.y[i],1e-14);
        NEAR(loaded.fit[i],g.fit[i],1e-14);
    }
    regression_graph_free(&loaded); regression_graph_free(&g);
    g=(RegressionGraph){0}; strcpy(g.file_stem,"roundtrip");
    CHECK(!suggest_graph_name(&sources,&g,name,error,sizeof error)); CHECK(!strcmp(name,"roundtrip_01.graph"));
    const char *bad[]={"not a graph\n", "CANDLES_GRAPH 8\ntitle\nx\ny\n26 .9 4\n",
        "CANDLES_GRAPH 8\ntitle\nx\ny\n1 .9 4 0 0 1 1 0 0\n.05\n2\n",
        "CANDLES_GRAPH 8\ntitle\nx\ny\n1 .9 4 0 0 1 1 0 0\n.05\n3\n0 0 0\n0 0 1\nnan\n3\n",
        "CANDLES_GRAPH 8\ntitle\nx\ny\n1 .9 4 0 0 1 1 0 0\n.05\n3\n0 0 0\n0 0 1\n2\n3\n0,2,2\n"};
    test_path(path,sizeof path,"bad.graph");
    for (size_t i=0;i<sizeof bad/sizeof bad[0];i++) {
        test_write("bad.graph",bad[i]); CHECK(load_graph(path,&loaded,error,sizeof error)==-1);
        CHECK(!loaded.x && !loaded.y && !loaded.fit && !loaded.count);
    }
}

int main(void)
{
    test_setup(); char path[8192]; test_path(path,sizeof path,"data"); CHECK(!mkdir(path,0700));
    snprintf(sources.dir,sizeof sources.dir,"%s",test_root);
    RUN(change_directions_and_periods); RUN(sampled_endpoints_and_zero_filters);
    RUN(monthly_and_yearly_time_sampling); RUN(weekend_prior_close_both_axes);
    RUN(form_roundtrip); RUN(saved_graph_versions); RUN(save_reopen_sparse_and_corrupt);
    stocklist_free(&sources); return test_done("Regression preparation and persistence");
}
