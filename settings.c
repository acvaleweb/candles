#include "settings.h"
#include "state.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char DEFAULT_FILE[] =
    "# candles settings\n"
    "# colors are written as #rrggbb\n"
    "\n"
    "up         = #26a69a\n"
    "down       = #ef5350\n"
    "background = #131722\n"
    "text       = #d1d4dc\n"
    "muted      = #787b86\n"
    "border     = #363a45\n"
    "bollinger  = #5b8def\n"
    "volume_ma  = #ff9800\n"
    "rsi        = #b39ddb\n"
    "rsi_hit    = #ff4081\n"
    "crosshair  = #ffeb3b\n"
    "market     = #ff9800\n"
    "\n"
    "# live mode (l) gets its prices from the python library yfinance, through\n"
    "# grabber.py, which sits in the same folder as the candles program and is\n"
    "# run with python3 from your PATH. To use another python, or another copy\n"
    "# of the helper, remove the # from these lines and set them:\n"
    "# python  = /usr/bin/python3\n"
    "# grabber = /full/path/to/grabber.py\n"
    "\n"
    "# indicators (toggle in the program: k volume, b bollinger, r rsi)\n"
    "volume_ma_len = 20\n"
    "bb_len        = 20\n"
    "bb_std        = 2\n"
    "rsi_len       = 3\n"
    "rsi_upper     = 97\n"
    "rsi_lower     = 3\n";

static const char SWING_SETTINGS[] =
    "\n# Classic swing strategy (uses RSI, Bollinger and volume periods above)\n"
    "swing_rsi_weight = 0.25\n"
    "swing_volume_weight = 0.25\n"
    "swing_bollinger_weight = 0.25\n"
    "swing_candle_weight = 0.25\n"
    "swing_volume_base = 0.2\n"
    "swing_close_body_tolerance = 0.3\n"
    "swing_wick_ratio = 0.1\n";

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_color(const char *s, Color *out)
{
    if (*s == '#')
        s++;
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = hex_digit((unsigned char)s[i]);
        if (v[i] < 0)
            return 0;
    }
    if (s[6] != '\0')
        return 0;
    out->r = (unsigned char)(v[0] * 16 + v[1]);
    out->g = (unsigned char)(v[2] * 16 + v[3]);
    out->b = (unsigned char)(v[4] * 16 + v[5]);
    return 1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

static void set_defaults(Settings *s)
{
    s->swing_strategy=0;
    parse_color("#26a69a", &s->up);
    parse_color("#ef5350", &s->down);
    parse_color("#131722", &s->background);
    parse_color("#d1d4dc", &s->text);
    parse_color("#787b86", &s->muted);
    parse_color("#363a45", &s->border);
    parse_color("#5b8def", &s->bollinger);
    parse_color("#ff9800", &s->volume_ma);
    parse_color("#b39ddb", &s->rsi);
    parse_color("#ff4081", &s->rsi_hit);
    parse_color("#ffeb3b", &s->crosshair);
    parse_color("#ff9800", &s->market);
    s->python[0] = '\0';
    s->grabber[0] = '\0';
    s->volume_ma_len = 20;
    s->bb_len = 20;
    s->bb_std = 2.0;
    s->rsi_len = 3;
    s->rsi_upper = 97.0;
    s->rsi_lower = 3.0;
    s->swing_rsi_weight=s->swing_volume_weight=s->swing_bollinger_weight=s->swing_candle_weight=.25;
    s->swing_volume_base=.2;
    s->swing_close_tolerance=.3;s->swing_wick_ratio=.1;
}

/* Numeric settings. Returns 1 if key is one of them (valid or not), so the
 * caller does not try to read it as a color. Invalid values are ignored. */
static int apply_number(Settings *s, const char *key, const char *val)
{
    static const char *const names[] = { "volume_ma_len", "bb_len", "bb_std",
                                         "rsi_len", "rsi_upper", "rsi_lower" };
    double *swing=NULL;double minimum=0,maximum=1;
    if(!strcmp(key,"swing_rsi_weight"))swing=&s->swing_rsi_weight;
    else if(!strcmp(key,"swing_volume_weight"))swing=&s->swing_volume_weight;
    else if(!strcmp(key,"swing_bollinger_weight"))swing=&s->swing_bollinger_weight;
    else if(!strcmp(key,"swing_candle_weight"))swing=&s->swing_candle_weight;
    else if(!strcmp(key,"swing_volume_base"))swing=&s->swing_volume_base;
    else if(!strcmp(key,"swing_close_body_tolerance")){swing=&s->swing_close_tolerance;minimum=1e-6;}
    else if(!strcmp(key,"swing_wick_ratio")){swing=&s->swing_wick_ratio;minimum=1e-6;}
    if(swing){char *end;double v=strtod(val,&end);if(end!=val&&!*end&&v-v==0&&v>=minimum&&v<=maximum)*swing=v;return 1;}
    int known = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(key, names[i]) == 0)
            known = 1;
    if (!known)
        return 0;

    char *end;
    double v = strtod(val, &end);
    if (end == val || *end != '\0' || v - v != 0)
        return 1;

    if      (strcmp(key, "volume_ma_len") == 0) { if (v >= 1 && v <= 5000) s->volume_ma_len = (int)v; }
    else if (strcmp(key, "bb_len") == 0)        { if (v >= 1 && v <= 5000) s->bb_len = (int)v; }
    else if (strcmp(key, "bb_std") == 0)        { if (v > 0 && v <= 100)   s->bb_std = v; }
    else if (strcmp(key, "rsi_len") == 0)       { if (v >= 1 && v <= 5000) s->rsi_len = (int)v; }
    else if (strcmp(key, "rsi_upper") == 0)     { if (v >= 0 && v <= 100)  s->rsi_upper = v; }
    else if (strcmp(key, "rsi_lower") == 0)     { if (v >= 0 && v <= 100)  s->rsi_lower = v; }
    return 1;
}

void settings_load(Settings *s)
{
    set_defaults(s);

    char path[PATH_MAX];
    if (state_config_path("settings.conf", path, sizeof path) != 0)
        return;

    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs(DEFAULT_FILE, f);
            fputs(SWING_SETTINGS, f);
            fclose(f);
        }
        return;
    }

    int has_swing_settings=0,has_close_body_tolerance=0;
    char line[PATH_MAX + 128];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(p);
        if(!strncmp(key,"swing_",6))has_swing_settings=1;
        if(!strcmp(key,"swing_close_body_tolerance"))has_close_body_tolerance=1;
        char *val = trim(eq + 1);

        /* paths: everything after the "=", so they may contain spaces */
        if (strcmp(key, "python") == 0 || strcmp(key, "grabber") == 0) {
            char *dst = key[0] == 'p' ? s->python : s->grabber;
            if (strlen(val) < PATH_MAX)
                strcpy(dst, val);
            continue;
        }

        for (char *q = val; *q; q++) {          /* ignore a trailing comment */
            if (isspace((unsigned char)*q)) {
                *q = '\0';
                break;
            }
        }

        if (apply_number(s, key, val))
            continue;

        Color c;
        if (!parse_color(val, &c))
            continue;
        if      (strcmp(key, "up") == 0)         s->up = c;
        else if (strcmp(key, "down") == 0)       s->down = c;
        else if (strcmp(key, "background") == 0) s->background = c;
        else if (strcmp(key, "text") == 0)       s->text = c;
        else if (strcmp(key, "muted") == 0)      s->muted = c;
        else if (strcmp(key, "border") == 0)     s->border = c;
        else if (strcmp(key, "bollinger") == 0)  s->bollinger = c;
        else if (strcmp(key, "volume_ma") == 0)  s->volume_ma = c;
        else if (strcmp(key, "rsi") == 0)        s->rsi = c;
        else if (strcmp(key, "rsi_hit") == 0)    s->rsi_hit = c;
        else if (strcmp(key, "crosshair") == 0)  s->crosshair = c;
        else if (strcmp(key, "market") == 0)     s->market = c;
    }
    fclose(f);
    if(!has_swing_settings){f=fopen(path,"a");if(f){fputs(SWING_SETTINGS,f);fclose(f);}}
    else if(!has_close_body_tolerance){f=fopen(path,"a");if(f){fputs("\n# Closing-side wick tolerance as a fraction of candle body\nswing_close_body_tolerance = 0.3\n",f);fclose(f);}}
}
