/* Exercise the real search and menu result handling without touching user config. */
#define menu_select test_menu_select
#define menu_pick_existing test_menu_pick_existing
#define state_load_graph_form test_load_form
#define state_save_graph_form test_save_form
#define state_load_standardize test_load_standardize
#define state_save_standardize test_save_standardize
#include "../regression.c"
#include <assert.h>

static char form[16384];
static int selection=-1,picker_calls=0;
int test_load_form(char *out,size_t n){size_t len=strlen(form);assert(len<n);memcpy(out,form,len+1);return len!=0;}
void test_save_form(const char *value){snprintf(form,sizeof form,"%s",value);}
int test_load_standardize(void){return 1;}
void test_save_standardize(int enabled){(void)enabled;}
int test_menu_select(const Settings *cfg,DrawFn draw,void *ctx,const char *title,
                     char *const *items,int count,int current)
{
    (void)cfg;(void)draw;(void)ctx;(void)title;(void)items;(void)count;(void)current;
    return 0; /* Graphs -> Generate regression -> Generate. */
}
int test_menu_pick_existing(const Settings *cfg,DrawFn draw,void *ctx,const char *title,
                            char *const *items,int count,int current)
{
    (void)cfg;(void)draw;(void)ctx;(void)items;(void)current;
    assert(!strcmp(title,"Select model")&&count>0);picker_calls++;
    return selection;
}

int main(void)
{
    Spec spec;spec_defaults(&spec);
    double x[40],y[40];
    for(int i=0;i<40;i++){x[i]=i*.1;y[i]=2+3*x[i]+.2*sin(i*1.7);}
    for(int degree=0;degree<=6;degree++) {
        ModelCandidate *models=NULL;int count=0;
        assert(!exhaustive_search(x,y,40,&spec,degree,1,&models,&count));
        assert(count==(degree?1<<(degree-1):1));
        for(int i=0;i<count;i++) {
            assert(models[i].terms&1u);
            assert(models[i].terms&(1u<<degree));
            assert((models[i].terms>>(degree+1))==0);
            for(int j=0;j<i;j++)assert(models[i].terms!=models[j].terms);
            free(models[i].fit);
        }
        free(models);
    }
    char dir[]="/tmp/candles-selection-XXXXXX";assert(mkdtemp(dir));
    char path[PATH_MAX];snprintf(path,sizeof path,"%s/data",dir);assert(!mkdir(path,0700));
    snprintf(path,sizeof path,"%s/data/y.csv",dir);FILE *csv=fopen(path,"w");assert(csv);
    fputs("date,value\n",csv);
    for(int i=0;i<28;i++)fprintf(csv,"2026-01-%02d,%.17g\n",i+1,y[i]);
    assert(!fclose(csv));
    StockList list={0};snprintf(list.dir,sizeof list.dir,"%s",dir);
    spec.x=VAR_TIME;spec.y=VAR_DATA;spec.max_degree=3;
    strcpy(spec.y_data_file,"y.csv");strcpy(spec.y_data_name,"Y");spec_save(&list,&spec);
    Settings cfg={0};RegressionGraph graph={0};char error[512]="";
    for(int i=0;i<3;i++) {
        selection=-1;assert(regression_menu(&cfg,NULL,NULL,&list,error,sizeof error,&graph)==0);
        assert(!graph.count&&!graph.x&&!graph.y&&!graph.fit);
        selection=0;assert(regression_menu(&cfg,NULL,NULL,&list,error,sizeof error,&graph)==1);
        assert(graph.count==28&&graph.model.valid&&graph.x&&graph.y&&graph.fit);
        regression_graph_free(&graph);
    }
    assert(picker_calls==6);
    assert(!unlink(path));snprintf(path,sizeof path,"%s/data",dir);assert(!rmdir(path));assert(!rmdir(dir));
    puts("Unique subsets and repeated cancel/reopen/select: passed");
    return 0;
}
