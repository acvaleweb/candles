#include "grabber.h"
#include "test_support.h"
#include <fcntl.h>
#include <poll.h>

static int stream(Grabber *g,int history)
{
    int descriptors[2]; CHECK(!pipe(descriptors)); grab_init(g);
    g->fd=descriptors[0]; g->history=history;
    CHECK(fcntl(g->fd,F_SETFL,O_NONBLOCK)>=0); return descriptors[1];
}

static void send_text(int fd,const char *text)
{
    CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text));
}

static void split_history_protocol(void)
{
    Grabber g; int fd=stream(&g,1);
    send_text(fd,"RE"); CHECK(grab_poll(&g)==GRAB_IDLE);
    send_text(fd,"PLACE\r\nnoise\nROW 2026-01-02,10,11,12,9,100\r\nEND ");
    CHECK(grab_poll(&g)==GRAB_IDLE); CHECK(g.days==1 && g.replace_csv);
    send_text(fd,"1\n"); CHECK(grab_poll(&g)==GRAB_HISTORY);
    CHECK(!strcmp(g.csv,"2026-01-02,10,11,12,9,100\n"));
    CHECK(g.csv_len==strlen(g.csv));
    close(fd); CHECK(grab_poll(&g)==GRAB_ENDED); grab_stop(&g);
    CHECK(g.fd==-1 && !g.csv && g.pid==0); grab_stop(&g);
}

static void live_messages_and_errors(void)
{
    Grabber g; int fd=stream(&g,0);
    const char *states[]={"open","pre","post","closed"};
    for (int state=0;state<4;state++) {
        char line[256]; snprintf(line,sizeof line,"LIVE %s 2026-01-02 1 10 12 9 11 100\n",states[state]);
        send_text(fd,line); CHECK(grab_poll(&g)==GRAB_READING);
        CHECK(g.reading.state==(MarketState)state && g.reading.has_candle);
        CHECK(!strcmp(g.reading.date,"2026-01-02")); NEAR(g.reading.price,11,1e-14);
    }
    send_text(fd,"LIVE invalid 2026-01-02 1 10 12 9 11 100\nLIVE broken\n");
    CHECK(grab_poll(&g)==GRAB_IDLE);
    send_text(fd,"LIVE closed 2026-01-03 0 0 0 0 0 0\n");
    CHECK(grab_poll(&g)==GRAB_READING && !g.reading.has_candle);
    send_text(fd,"ERR simulated network error\n"); CHECK(grab_poll(&g)==GRAB_ERROR);
    CHECK(!strcmp(g.err,"simulated network error")); close(fd); grab_stop(&g);
}

static GrabEvent await_event(Grabber *g)
{
    for (int attempt=0;attempt<100;attempt++) {
        GrabEvent event=grab_poll(g); if (event!=GRAB_IDLE) return event;
        struct pollfd fd={.fd=grab_fd(g),.events=POLLIN}; poll(&fd,1,20);
    }
    CHECK(!"helper event timed out"); return GRAB_IDLE;
}

static void actual_child_lifecycle(void)
{
    test_write("helper with spaces.py","import sys\n"
        "assert sys.argv[2] == 'TEST WITH SPACES'\n"
        "if sys.argv[1] == 'history':\n"
        "    assert sys.argv[3] == '2026-01-01'\n"
        "    print('END 0', flush=True)\n"
        "else:\n"
        "    print('LIVE pre 2026-01-02 0 0 0 0 0 0', flush=True)\n"
        "sys.stdin.read()\n");
    Settings config={0}; char error[512];
    test_path(config.grabber,sizeof config.grabber,"helper with spaces.py");
    const char *python=getenv("CANDLES_TEST_PYTHON");
    if (python) snprintf(config.python,sizeof config.python,"%s",python);
    Grabber g; grab_init(&g);
    CHECK(!grab_start_history(&g,&config,"TEST WITH SPACES","2026-01-01",error,sizeof error));
    CHECK(await_event(&g)==GRAB_HISTORY && g.days==0 && !g.replace_csv); grab_stop(&g);
    CHECK(!grab_start_live(&g,&config,"TEST WITH SPACES",error,sizeof error));
    CHECK(await_event(&g)==GRAB_READING && g.reading.state==MARKET_PRE); grab_stop(&g);
    strcpy(config.python,"/no/such/candles-python");
    CHECK(!grab_start_live(&g,&config,"TEST",error,sizeof error));
    CHECK(await_event(&g)==GRAB_ERROR && strstr(g.err,"cannot run")); grab_stop(&g);
    CHECK(grab_start_live(&g,&config,"",error,sizeof error)==-1);
    config.grabber[0]=0; CHECK(grab_start_live(&g,&config,"TEST",error,sizeof error)==-1);
}

int main(void)
{
    test_setup(); RUN(split_history_protocol); RUN(live_messages_and_errors);
    RUN(actual_child_lifecycle); return test_done("Helper protocol and process lifecycle");
}
