#include "menu.h"
#include "live.h"
#include "test_support.h"

static void editable_prompts(void)
{
    char buf[32]="ab"; size_t cursor=1;
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,'X')); CHECK(!strcmp(buf,"aXb") && cursor==2);
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,KEY_LEFT));
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,127)); CHECK(!strcmp(buf,"Xb") && cursor==0);
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,KEY_RIGHT)); CHECK(cursor==1);
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,1)); CHECK(cursor==0);
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,8)); CHECK(!strcmp(buf,"Xb"));
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,5)); CHECK(cursor==2);
    CHECK(menu_edit_key(buf,sizeof buf,&cursor,KEY_RIGHT)); CHECK(cursor==2);
    CHECK(!menu_edit_key(buf,sizeof buf,&cursor,KEY_ESC));
    strcpy(buf,"aé界z"); cursor=strlen(buf);
    menu_edit_key(buf,sizeof buf,&cursor,KEY_LEFT); CHECK(cursor==6);
    menu_edit_key(buf,sizeof buf,&cursor,KEY_LEFT); CHECK(cursor==3);
    menu_edit_key(buf,sizeof buf,&cursor,127); CHECK(!strcmp(buf,"a界z") && cursor==1);
    menu_edit_key(buf,sizeof buf,&cursor,KEY_RIGHT); CHECK(cursor==4);
    menu_edit_key(buf,sizeof buf,&cursor,127); CHECK(!strcmp(buf,"az") && cursor==1);
    char full[4]="abc"; cursor=1; menu_edit_key(full,sizeof full,&cursor,'X');
    CHECK(!strcmp(full,"abc") && cursor==1);
}

static void live_reference_values(void)
{
    Stock stock={0}; stock.count=1; stock.candles=calloc(1,sizeof *stock.candles); CHECK(stock.candles!=NULL);
    stock.candles[0]=(Candle){.open=10,.close=11,.high=12,.low=9,.volume=100,
        .ref_high=12,.ref_low=9,.ref_close=11,.ref_volume=100};
    strcpy(stock.candles[0].date,"2026-01-02");
    LiveReading reading={.state=MARKET_OPEN,.has_candle=1,.open=10,.price=15,.high=14,.low=11,.volume=200};
    strcpy(reading.date,"2026-01-02"); CHECK(live_apply(&stock,&reading));
    CHECK(stock.count==1); NEAR(stock.candles[0].high,15,1e-14); NEAR(stock.candles[0].low,10,1e-14);
    NEAR(stock.candles[0].ref_high,12,1e-14); NEAR(stock.candles[0].ref_close,11,1e-14);
    NEAR(stock.candles[0].ref_volume,100,1e-14);
    strcpy(reading.date,"2026-01-01"); CHECK(!live_apply(&stock,&reading)); CHECK(stock.count==1);
    strcpy(reading.date,"2026-01-05"); CHECK(live_apply(&stock,&reading)); CHECK(stock.count==2);
    CHECK(isnan(stock.candles[1].ref_high) && isnan(stock.candles[1].ref_volume));
    CHECK(stock.has_volume);
    reading.has_candle=0; strcpy(reading.date,"2026-01-06"); CHECK(!live_apply(&stock,&reading)); CHECK(stock.count==2);
    stock_free(&stock);
    CHECK(!strcmp(live_state_note(MARKET_PRE),"pre-market"));
    CHECK(!strcmp(live_state_note(MARKET_POST),"after hours"));
    CHECK(!strcmp(live_state_note(MARKET_CLOSED),"market closed"));
    CHECK(!strcmp(live_state_note(MARKET_OPEN),""));
}

int main(void)
{
    test_setup(); RUN(editable_prompts); RUN(live_reference_values);
    return test_done("Prompt editing and live updates");
}
