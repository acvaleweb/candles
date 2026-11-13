#include "state.h"
#include "test_support.h"

static void settings_validation(void)
{
    Settings s; settings_load(&s);
    CHECK(s.volume_ma_len==20 && s.bb_len==20 && s.rsi_len==3);
    NEAR(s.bb_std,2,1e-14);
    test_write(".config/candles/settings.conf","# keep this comment\nvolume_ma_len = 7\n"
        "bb_len = -1\nbb_std = nan\nrsi_len = 0\nrsi_upper = 101\n"
        "python = /path with spaces/python3\nup = #1234AB\n"
        "swing_volume_base = inf\nswing_rsi_weight = .5\n");
    settings_load(&s);
    CHECK(s.volume_ma_len==7 && s.bb_len==20 && s.rsi_len==3);
    NEAR(s.bb_std,2,1e-14); NEAR(s.rsi_upper,97,1e-14);
    NEAR(s.swing_volume_base,.2,1e-14); NEAR(s.swing_rsi_weight,.5,1e-14);
    CHECK(!strcmp(s.python,"/path with spaces/python3"));
    CHECK(s.up.r==0x12 && s.up.g==0x34 && s.up.b==0xab);
    settings_load(&s);
    char *text=test_read(".config/candles/settings.conf");
    char *first=strstr(text,"swing_close_body_tolerance = 0.3"); CHECK(first!=NULL);
    CHECK(strstr(first+1,"swing_close_body_tolerance = 0.3")==NULL); free(text);
}

static void persistence_is_independent(void)
{
    char value[8192]; state_save_folder("/a folder/with spaces"); state_save_stock("BRK-B");
    CHECK(state_load_folder(value,sizeof value)); CHECK(!strcmp(value,"/a folder/with spaces"));
    CHECK(state_load_stock(value,sizeof value)); CHECK(!strcmp(value,"BRK-B"));
    state_save_stock("NVDA");
    char *text=test_read(".config/candles/settings.conf");
    CHECK(strstr(text,"# keep this comment") && strstr(text,"volume_ma_len = 7"));
    CHECK(!strstr(text,"stock = BRK-B")); free(text);
    View a,b,loaded; view_init(&a); view_init(&b);
    a.x_level=5; a.y_level=-4; a.x_off=17; a.y_off=12.3456789012345;
    a.show_volume=0; a.show_bb=1; a.show_rsi=1; a.period=PERIOD_MONTH;
    b.x_level=1; b.show_bb=0;
    state_save_stock_view("SPY",&a); state_save_stock_view("NVDA",&b);
    view_init(&loaded); state_load_stock_view("SPY",&loaded);
    CHECK(loaded.x_level==5 && loaded.y_level==-4 && loaded.x_off==17);
    CHECK(loaded.show_bb && loaded.show_rsi && !loaded.show_volume && loaded.period==PERIOD_MONTH);
    NEAR(loaded.y_off,a.y_off,1e-15);
    state_load_stock_view("NVDA",&loaded); CHECK(loaded.x_level==1 && !loaded.show_bb);
    View defaults; view_init(&defaults); loaded=defaults;
    state_load_stock_view("MISSING",&loaded); CHECK(!memcmp(&loaded,&defaults,sizeof loaded));
    test_write(".config/candles/views.conf","BAD = 999,0,0,0,1,0,0,0\nNAN = 1,0,0,nan,1,0,0,0\n");
    state_load_stock_view("BAD",&loaded); CHECK(!memcmp(&loaded,&defaults,sizeof loaded));
    state_load_stock_view("NAN",&loaded); CHECK(!memcmp(&loaded,&defaults,sizeof loaded));
    state_save_graph_form("15|opaque|form"); CHECK(state_load_standardize()==1);
    state_save_standardize(0); CHECK(!state_load_standardize());
    CHECK(state_load_graph_form(value,sizeof value)); CHECK(!strcmp(value,"15|opaque|form"));
    state_save_standardize(1); CHECK(state_load_standardize());
    for (int strategy=0;strategy<5;strategy++) {
        state_save_click_strategy(strategy); CHECK(state_load_click_strategy()==strategy);
    }
    state_save_click_strategy(99); CHECK(state_load_click_strategy()==4);
}

int main(void)
{
    test_setup(); RUN(settings_validation); RUN(persistence_is_independent);
    return test_done("Settings and per-stock persistence");
}
