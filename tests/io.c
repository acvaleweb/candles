#include "stock.h"
#include "data.h"
#include "test_support.h"

static void stock_csv_formats(void)
{
    test_write("quotes.csv","\xef\xbb\xbf" "date,open,close,max,min,volume\r\n"
        "\"2026-01-05\",10,12,11,11,100\r\n"
        "2026-01-02;8,5;9,5;10,5;7,5;50\n"
        "2026-01-03\t9\t8\t10\t7\n"
        "not a row\n2026-01-04,nan,2,3,1\n"
        "2026-01-04,1,inf,3,1\n2026-01-04,1junk,2,3,1\n");
    Stock stock={0}; char error[512];
    CHECK(!stock_load(&stock,test_root,"quotes.csv","TEST",error,sizeof error));
    CHECK(stock.count==3 && stock.has_volume);
    CHECK(!strcmp(stock.candles[0].date,"2026-01-02"));
    NEAR(stock.candles[0].open,8.5,1e-14);
    NEAR(stock.candles[0].volume,50,1e-14);
    NEAR(stock.candles[1].volume,0,1e-14);
    NEAR(stock.candles[2].high,12,1e-14); /* Enclose the body. */
    NEAR(stock.candles[2].low,10,1e-14);
    NEAR(stock.candles[2].ref_close,12,1e-14);
    stock_free(&stock);
    test_write("bad.csv","date,open,close,max,min\n2026-01-01,NA,2,3,1\n");
    CHECK(stock_load(&stock,test_root,"bad.csv","BAD",error,sizeof error)==-1);
    CHECK(!stock.count && !stock.candles && error[0]);
    CHECK(stock_load(&stock,test_root,"missing.csv","BAD",error,sizeof error)==-1);
}

static void append_replace_and_scan(void)
{
    char error[512]; Stock stock={0};
    test_write("APPEND.csv","date,open,close,max,min,volume\n2026-01-01,1,2,3,0,10");
    CHECK(!stock_append_csv(test_root,"APPEND.csv","2026-01-02,2,3,4,1,20\n",error,sizeof error));
    CHECK(!stock_load(&stock,test_root,"APPEND.csv","APPEND",error,sizeof error));
    CHECK(stock.count==2); NEAR(stock.candles[1].volume,20,1e-14); stock_free(&stock);
    CHECK(!stock_replace_csv(test_root,"APPEND.csv","2026-01-01,.5,1,1.5,0,10\n",error,sizeof error));
    CHECK(!stock_load(&stock,test_root,"APPEND.csv","APPEND",error,sizeof error));
    CHECK(stock.count==1); NEAR(stock.candles[0].close,1,1e-14); stock_free(&stock);
    char directory[8192]; test_path(directory,sizeof directory,"cannot-replace.csv");
    CHECK(!mkdir(directory,0700));
    CHECK(stock_replace_csv(test_root,"cannot-replace.csv","anything\n",error,sizeof error)==-1);
    struct stat st; CHECK(!stat(directory,&st) && S_ISDIR(st.st_mode));
    DIR *dir=opendir(test_root); CHECK(dir!=NULL); struct dirent *entry;
    while ((entry=readdir(dir))) CHECK(strncmp(entry->d_name,".candles-history-",17)!=0);
    closedir(dir);
    CHECK(!stock_append_csv(test_root,"NEW.csv","2026-01-01,4,5,6,3,0\n",error,sizeof error));
    StockList list={0}; CHECK(!stocklist_scan(&list,test_root,error,sizeof error));
    CHECK(list.count>=4);
    for (int i=0;i<list.count;i++) {
        CHECK(strstr(list.files[i],".csv")!=NULL);
        if (i) CHECK(strcasecmp(list.tickers[i-1],list.tickers[i])<=0);
        CHECK(strcmp(list.files[i],"cannot-replace.csv")!=0);
    }
    stocklist_free(&list);
}

static void calendar_aggregation(void)
{
    test_write("calendar.csv","2025-12-31,10,11,12,9,10\n"
        "2026-01-02,11,12,14,10,20\n2026-01-05,12,13,15,11,30\n"
        "2026-02-02,13,14,16,12,40\n");
    Stock s={0}, out={0}; char error[512];
    CHECK(!stock_load(&s,test_root,"calendar.csv","CAL",error,sizeof error));
    CHECK(!stock_aggregate(&out,&s,PERIOD_WEEK));
    CHECK(out.count==3 && !strcmp(out.candles[0].date,"2025-12-29"));
    NEAR(out.candles[0].open,10,1e-14); NEAR(out.candles[0].close,12,1e-14);
    NEAR(out.candles[0].high,14,1e-14); NEAR(out.candles[0].low,9,1e-14);
    NEAR(out.candles[0].volume,30,1e-14); stock_free(&out);
    CHECK(!stock_aggregate(&out,&s,PERIOD_MONTH));
    CHECK(out.count==3 && !strcmp(out.candles[1].date,"2026-01-01"));
    NEAR(out.candles[1].volume,50,1e-14); stock_free(&out);
    s.candles[3].ref_high=s.candles[3].ref_low=s.candles[3].ref_close=s.candles[3].ref_volume=NAN;
    CHECK(!stock_aggregate(&out,&s,PERIOD_YEAR)); CHECK(out.count==2);
    NEAR(out.candles[1].high,16,1e-14); NEAR(out.candles[1].ref_high,15,1e-14);
    NEAR(out.candles[1].close,14,1e-14); NEAR(out.candles[1].ref_close,13,1e-14);
    NEAR(out.candles[1].volume,90,1e-14); NEAR(out.candles[1].ref_volume,50,1e-14);
    stock_free(&out); stock_free(&s);
}

static void imported_data(void)
{
    test_write("rate data.csv","date,value\n2026-01-03,-2\n2026-01-01,0\n"
        "2026-01-02,1.25\n2026-01-04,nan\n2026-01-05,inf\n"
        "bad,5\n2026-01-06,7,extra\n");
    char source[8192],name[160],error[512]; test_path(source,sizeof source,"rate data.csv");
    CHECK(!data_import(test_root,source,name,sizeof name,error,sizeof error));
    CHECK(!strcmp(name,"rate_data"));
    CHECK(!data_import(test_root,source,name,sizeof name,error,sizeof error));
    CHECK(!strcmp(name,"rate_data_02"));
    DataSeries data={0}; CHECK(!data_load(&data,test_root,"rate_data.csv","Rates",error,sizeof error));
    CHECK(data.count==3 && !strcmp(data.name,"Rates"));
    CHECK(!strcmp(data.points[0].date,"2026-01-01"));
    NEAR(data.points[0].value,0,1e-14); NEAR(data.points[2].value,-2,1e-14); data_free(&data);
    DataList list={0}; CHECK(!datalist_scan(&list,test_root,error,sizeof error));
    CHECK(list.count==2 && !strcmp(list.names[0],"rate_data")); datalist_free(&list);
    test_write("invalid-data.csv","date,value\n2026-01-01,nan\n");
    test_path(source,sizeof source,"invalid-data.csv");
    CHECK(data_import(test_root,source,name,sizeof name,error,sizeof error)==-1);
    CHECK(data_load(&data,test_root,"missing.csv","Missing",error,sizeof error)==-1);
}

int main(void)
{
    test_setup(); RUN(stock_csv_formats); RUN(append_replace_and_scan);
    RUN(calendar_aggregation); RUN(imported_data); return test_done("CSV and calendar IO");
}
