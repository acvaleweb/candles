#include "state.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int state_config_path(const char *file, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return -1;

    char dir[PATH_MAX];
    int w = snprintf(dir, sizeof dir, "%s/.config", home);
    if (w < 0 || (size_t)w >= sizeof dir)
        return -1;
    mkdir(dir, 0755);                       /* may already exist */

    w = snprintf(dir, sizeof dir, "%s/.config/candles", home);
    if (w < 0 || (size_t)w >= sizeof dir)
        return -1;
    mkdir(dir, 0755);

    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;

    w = snprintf(out, n, "%s/%s", dir, file);
    if (w < 0 || (size_t)w >= n)
        return -1;
    return 0;
}

/* ---------- settings.conf keys ---------- */

/* The whole file, NUL-terminated, or NULL if it cannot be read. */
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    while (b) {
        n += fread(b + n, 1, cap - n, f);
        if (n < cap)
            break;
        cap *= 2;
        char *nb = realloc(b, cap + 1);
        if (!nb) {
            free(b);
            b = NULL;
        } else {
            b = nb;
        }
    }
    fclose(f);
    if (!b)
        return NULL;
    b[n] = '\0';
    *len = n;
    return b;
}

/* Is this line "key = something" (not a comment)? */
static int line_is_key(const char *line, const char *key)
{
    while (*line == ' ' || *line == '\t')
        line++;
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0)
        return 0;
    line += kl;
    while (*line == ' ' || *line == '\t')
        line++;
    return *line == '=';
}

/* The value of the last "key = value" line: everything after the "=", without
 * the blanks around it (so a path may contain spaces or '#'). */
static int conf_get(const char *file, const char *key, char *out, size_t n)
{
    char path[PATH_MAX];
    if (state_config_path(file, path, sizeof path) != 0)
        return 0;
    size_t len;
    char *buf = read_all(path, &len);
    if (!buf)
        return 0;

    int found = 0;
    for (const char *p = buf; *p; ) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *next = nl ? nl + 1 : end;

        if (line_is_key(p, key)) {
            const char *v = strchr(p, '=') + 1;
            while (v < end && isspace((unsigned char)*v))
                v++;
            while (end > v && isspace((unsigned char)end[-1]))
                end--;
            size_t vl = (size_t)(end - v);
            found = vl > 0 && vl < n;
            if (found) {
                memcpy(out, v, vl);
                out[vl] = '\0';
            }
        }
        p = next;
    }
    free(buf);
    return found;
}

/* Writes "key = value" into settings.conf: in place of the existing line (a
 * repeated line is dropped), otherwise at the end. Every other line stays as
 * it is. */
static void conf_set(const char *file, const char *key, const char *value)
{
    char path[PATH_MAX];
    if (state_config_path(file, path, sizeof path) != 0)
        return;

    size_t len = 0;
    char *old = read_all(path, &len);
    if (!old) {
        struct stat st;
        if (stat(path, &st) == 0)
            return;                         /* it exists but cannot be read: leave it alone */
        old = strdup("");
        if (!old)
            return;
    }

    char *out = malloc(len + strlen(key) + strlen(value) + 8);
    if (!out) {
        free(old);
        return;
    }

    size_t o = 0;
    int done = 0;
    for (const char *p = old; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) + 1 : strlen(p);

        if (line_is_key(p, key)) {
            if (!done) {
                o += (size_t)sprintf(out + o, "%s = %s\n", key, value);
                done = 1;
            }
        } else {
            memcpy(out + o, p, ll);
            o += ll;
        }
        p += ll;
    }
    if (!done) {
        if (o > 0 && out[o - 1] != '\n')
            out[o++] = '\n';
        o += (size_t)sprintf(out + o, "%s = %s\n", key, value);
    }

    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(out, 1, o, f);
        fclose(f);
    }
    free(out);
    free(old);
}

int state_load_folder(char *out, size_t n)
{
    return conf_get("settings.conf", "folder", out, n);
}

void state_save_folder(const char *dir)
{
    conf_set("settings.conf", "folder", dir);
}

int state_load_stock(char *out, size_t n)
{
    return conf_get("settings.conf", "stock", out, n);
}

void state_save_stock(const char *ticker)
{
    conf_set("settings.conf", "stock", ticker);
}

void state_load_stock_view(const char *ticker, View *view)
{
    char value[256], tail;
    int x_level, y_level, x_off, volume, bb, rsi, period;
    double y_off;

    if (!ticker || !ticker[0] || !conf_get("views.conf", ticker, value, sizeof value))
        return;
    if (sscanf(value, "%d,%d,%d,%lf,%d,%d,%d,%d%c", &x_level, &y_level, &x_off, &y_off,
               &volume, &bb, &rsi, &period, &tail) != 8)
        return;
    if (x_level < 0 || x_level > 5 || y_level < -6 || y_level > 12 || x_off < 0 ||
        !isfinite(y_off) || (volume != 0 && volume != 1) || (bb != 0 && bb != 1) ||
        (rsi != 0 && rsi != 1) || period < PERIOD_DAY || period >= PERIOD_COUNT)
        return;

    view->x_level = x_level;
    view->y_level = y_level;
    view->x_off = x_off;
    view->y_off = y_off;
    view->show_volume = volume;
    view->show_bb = bb;
    view->show_rsi = rsi;
    view->period = (Period)period;
}

void state_save_stock_view(const char *ticker, const View *view)
{
    char value[256];
    if (!ticker || !ticker[0])
        return;
    snprintf(value, sizeof value, "%d,%d,%d,%.17g,%d,%d,%d,%d",
             view->x_level, view->y_level, view->x_off, view->y_off,
             view->show_volume, view->show_bb, view->show_rsi, (int)view->period);
    conf_set("views.conf", ticker, value);
}

int state_load_graph_form(char *out, size_t n)
{
    return conf_get("graphs.conf", "form", out, n);
}

void state_save_graph_form(const char *value)
{
    if (value && *value)
        conf_set("graphs.conf", "form", value);
}

int state_load_standardize(void)
{
    char value[16];
    if(!conf_get("graphs.conf","standardize",value,sizeof value))return 1;
    return strcmp(value,"0")!=0;
}

void state_save_standardize(int enabled)
{
    conf_set("graphs.conf","standardize",enabled?"1":"0");
}

int state_load_click_strategy(void)
{
    char value[64];
    if(!conf_get("strategy.conf","click_strategy",value,sizeof value))return 0;
    if(!strcmp(value,"classic2"))return 1;
    if(!strcmp(value,"classic3"))return 2;
    if(!strcmp(value,"3green"))return 3;
    if(!strcmp(value,"bb_strat"))return 4;
    return 0;
}

void state_save_click_strategy(int strategy)
{
    static const char *names[]={"classic","classic2","classic3","3green","bb_strat"};
    if(strategy>=0&&strategy<5)conf_set("strategy.conf","click_strategy",names[strategy]);
}
