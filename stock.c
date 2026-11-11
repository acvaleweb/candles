#include "stock.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- folder scan ---------- */

static int has_csv_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".csv") == 0;
}

void stocklist_free(StockList *l)
{
    for (int i = 0; i < l->count; i++) {
        free(l->tickers[i]);
        free(l->files[i]);
    }
    free(l->tickers);
    free(l->files);
    memset(l, 0, sizeof *l);
}

/* Sorts tickers and files together (insertion sort: lists are small). */
static void sort_list(StockList *l)
{
    for (int i = 1; i < l->count; i++) {
        char *t = l->tickers[i];
        char *f = l->files[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(l->tickers[j], t) > 0) {
            l->tickers[j + 1] = l->tickers[j];
            l->files[j + 1] = l->files[j];
            j--;
        }
        l->tickers[j + 1] = t;
        l->files[j + 1] = f;
    }
}

int stocklist_scan(StockList *l, const char *path, char *err, size_t errn)
{
    memset(l, 0, sizeof *l);

    const char *p = path;
    while (isspace((unsigned char)*p))
        p++;
    size_t len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1]))
        len--;
    if (len == 0) {
        snprintf(err, errn, "No folder given");
        return -1;
    }

    char raw[PATH_MAX];
    const char *home = getenv("HOME");
    if (p[0] == '~' && (len == 1 || p[1] == '/') && home)
        snprintf(raw, sizeof raw, "%s%.*s", home, (int)(len - 1), p + 1);
    else
        snprintf(raw, sizeof raw, "%.*s", (int)len, p);

    char resolved[PATH_MAX];
    if (!realpath(raw, resolved)) {
        snprintf(err, errn, "Folder not found: %s", raw);
        return -1;
    }

    DIR *d = opendir(resolved);
    if (!d) {
        snprintf(err, errn, "Cannot open %s: %s", resolved, strerror(errno));
        return -1;
    }

    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_csv_ext(e->d_name))
            continue;

        char full[PATH_MAX];
        int fw = snprintf(full, sizeof full, "%s/%s", resolved, e->d_name);
        if (fw < 0 || (size_t)fw >= sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (l->count == cap) {
            cap = cap ? cap * 2 : 16;
            char **t = realloc(l->tickers, (size_t)cap * sizeof *t);
            if (!t)
                break;
            l->tickers = t;
            char **f = realloc(l->files, (size_t)cap * sizeof *f);
            if (!f)
                break;
            l->files = f;
        }
        size_t nl = strlen(e->d_name);
        char *file = strdup(e->d_name);
        char *ticker = strndup(e->d_name, nl - 4);
        if (!file || !ticker) {
            free(file);
            free(ticker);
            break;
        }
        l->files[l->count] = file;
        l->tickers[l->count] = ticker;
        l->count++;
    }
    closedir(d);

    sort_list(l);
    snprintf(l->dir, sizeof l->dir, "%s", resolved);
    return 0;
}

/* ---------- csv ---------- */

/* Trims whitespace and surrounding quotes in place. */
static char *clean(char *s)
{
    while (isspace((unsigned char)*s) || *s == '"')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (isspace((unsigned char)s[n - 1]) || s[n - 1] == '"'))
        s[--n] = '\0';
    return s;
}

static int is_date(const char *s)
{
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return 0;
        } else if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return !isdigit((unsigned char)s[10]);
}

static int parse_num(char *s, int comma_decimal, double *out)
{
    if (comma_decimal) {
        for (char *p = s; *p; p++)
            if (*p == ',')
                *p = '.';
    }
    if (!*s)
        return 0;
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0')
        return 0;
    if (v - v != 0)                 /* NaN or infinity */
        return 0;
    *out = v;
    return 1;
}

/* Parses "yyyy-mm-dd, open, close, max, min[, volume]". Lines that do not start with a
 * date (a header, blank lines) are skipped. The delimiter is ',' or ';' (or a
 * tab); with ';' a decimal comma is accepted. Extra columns are ignored. */
static int parse_line(char *line, Candle *c)
{
    char delim = ',';
    if (strchr(line, ';'))
        delim = ';';
    else if (strchr(line, '\t'))
        delim = '\t';

    char *f[6];
    int nf = 0;
    char *p = line;
    while (nf < 6) {
        f[nf++] = p;
        char *d = strchr(p, delim);
        if (!d)
            break;
        *d = '\0';
        p = d + 1;
    }
    if (nf < 5)
        return 0;

    char *date = clean(f[0]);
    if (!is_date(date))
        return 0;
    memcpy(c->date, date, 10);
    c->date[10] = '\0';

    double v[4];
    for (int i = 0; i < 4; i++)
        if (!parse_num(clean(f[i + 1]), delim != ',', &v[i]))
            return 0;
    c->open = v[0];
    c->close = v[1];
    c->high = v[2];
    c->low = v[3];

    c->volume = 0.0;                        /* optional 6th column */
    if (nf >= 6) {
        double vol;
        if (parse_num(clean(f[5]), delim != ',', &vol) && vol > 0.0)
            c->volume = vol;
    }

    /* make max/min really enclose the body */
    double top = c->open > c->close ? c->open : c->close;
    double bot = c->open > c->close ? c->close : c->open;
    if (c->high < top) c->high = top;
    if (c->low > bot)  c->low = bot;

    c->ref_high = c->high;
    c->ref_low = c->low;
    c->ref_close = c->close;
    c->ref_volume = c->volume;
    return 1;
}

static int cmp_candle(const void *a, const void *b)
{
    return strcmp(((const Candle *)a)->date, ((const Candle *)b)->date);
}

int stock_load(Stock *s, const char *dir, const char *file, const char *ticker,
               char *err, size_t errn)
{
    memset(s, 0, sizeof *s);

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errn, "Cannot open %s: %s", file, strerror(errno));
        return -1;
    }

    int cap = 0;
    char *line = NULL;
    size_t lcap = 0;
    ssize_t len;
    int first = 1;

    while ((len = getline(&line, &lcap, f)) >= 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *p = line;
        if (first) {                                /* skip a UTF-8 byte order mark */
            first = 0;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF)
                p += 3;
        }

        Candle c;
        if (!parse_line(p, &c))
            continue;

        if (s->count == cap) {
            cap = cap ? cap * 2 : 512;
            Candle *n = realloc(s->candles, (size_t)cap * sizeof *n);
            if (!n)
                break;
            s->candles = n;
        }
        s->candles[s->count++] = c;
    }
    free(line);
    fclose(f);

    if (s->count == 0) {
        snprintf(err, errn, "%s: no valid rows (expected date,open,close,max,min)", file);
        free(s->candles);
        memset(s, 0, sizeof *s);
        return -1;
    }

    qsort(s->candles, (size_t)s->count, sizeof(Candle), cmp_candle);
    for (int i = 0; i < s->count; i++)
        if (s->candles[i].volume > 0.0) {
            s->has_volume = 1;
            break;
        }
    snprintf(s->ticker, sizeof s->ticker, "%s", ticker);
    return 0;
}

void stock_free(Stock *s)
{
    free(s->candles);
    memset(s, 0, sizeof *s);
}

int stock_append_csv(const char *dir, const char *file, const char *rows,
                     char *err, size_t errn)
{
    char path[PATH_MAX];
    int w = snprintf(path, sizeof path, "%s/%s", dir, file);
    if (w < 0 || (size_t)w >= sizeof path) {
        snprintf(err, errn, "Path too long: %s/%s", dir, file);
        return -1;
    }

    struct stat st;
    int has_rows = stat(path, &st) == 0 && st.st_size > 0;

    FILE *f = fopen(path, "a+");
    if (!f) {
        snprintf(err, errn, "Cannot write %s: %s", file, strerror(errno));
        return -1;
    }

    if (has_rows) {
        /* a file whose last line has no newline must not swallow our first row */
        if (fseek(f, -1, SEEK_END) == 0) {
            int last = fgetc(f);
            fseek(f, 0, SEEK_END);              /* required between reading and writing */
            if (last != '\n' && last != EOF)
                fputc('\n', f);
        }
    } else {
        fputs("date,open,close,max,min,volume\n", f);
    }
    fputs(rows, f);

    int bad = ferror(f);
    if (fclose(f) != 0 || bad) {
        snprintf(err, errn, "Cannot write %s: %s", file, strerror(errno));
        return -1;
    }
    return 0;
}

int stock_replace_csv(const char *dir, const char *file, const char *rows,
                      char *err, size_t errn)
{
    char path[PATH_MAX], temporary[PATH_MAX];
    int w = snprintf(path, sizeof path, "%s/%s", dir, file);
    if (w < 0 || (size_t)w >= sizeof path) {
        snprintf(err, errn, "Path too long: %s/%s", dir, file);
        return -1;
    }
    w = snprintf(temporary, sizeof temporary, "%s/.candles-history-XXXXXX", dir);
    if (w < 0 || (size_t)w >= sizeof temporary) {
        snprintf(err, errn, "Folder path is too long: %s", dir);
        return -1;
    }

    int fd = mkstemp(temporary);
    if (fd < 0) {
        snprintf(err, errn, "Cannot create replacement for %s: %s", file, strerror(errno));
        return -1;
    }

    struct stat old;
    if (stat(path, &old) == 0)
        fchmod(fd, old.st_mode & 0777);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        snprintf(err, errn, "Cannot write replacement for %s: %s", file, strerror(errno));
        close(fd);
        unlink(temporary);
        return -1;
    }

    fputs("date,open,close,max,min,volume\n", f);
    fputs(rows, f);
    int bad = ferror(f) || fflush(f) != 0 || fsync(fd) != 0;
    if (fclose(f) != 0)
        bad = 1;
    if (bad) {
        snprintf(err, errn, "Cannot write replacement for %s: %s", file, strerror(errno));
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        snprintf(err, errn, "Cannot replace %s: %s", file, strerror(errno));
        unlink(temporary);
        return -1;
    }
    return 0;
}

/* ---------- periods ---------- */

char *const period_names[PERIOD_COUNT] = { "day", "week", "month", "year" };

/* Days since 1970-01-01 of a calendar date (proleptic Gregorian). */
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The inverse of days_from_civil. */
static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* Splits "yyyy-mm-dd" (already checked by is_date); a nonsense month or day
 * in the file is pulled into range instead of being trusted. */
static void split_date(const char *s, int *y, int *m, int *d)
{
    *y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    *m = (s[5] - '0') * 10 + (s[6] - '0');
    *d = (s[8] - '0') * 10 + (s[9] - '0');
    if (*m < 1)  *m = 1;
    if (*m > 12) *m = 12;
    if (*d < 1)  *d = 1;
    if (*d > 31) *d = 31;
}

/* A number that is the same for every date of one period and different for
 * the next one; for a week it is the day number of its Monday. */
static long period_key(const char *date, Period p)
{
    int y, m, d;
    split_date(date, &y, &m, &d);
    switch (p) {
    case PERIOD_WEEK: {
        long days = days_from_civil(y, m, d);
        long wd = ((days + 3) % 7 + 7) % 7;         /* 1970-01-01 was a Thursday; Monday = 0 */
        return days - wd;
    }
    case PERIOD_MONTH: return (long)y * 12 + (m - 1);
    case PERIOD_YEAR:  return y;
    default:           return days_from_civil(y, m, d);
    }
}

/* The date printed for a period: its first calendar day. */
static void period_label(long key, Period p, char *out)
{
    int y, m, d;
    switch (p) {
    case PERIOD_MONTH:
        y = (int)(key / 12);
        m = (int)(key % 12) + 1;
        d = 1;
        break;
    case PERIOD_YEAR:
        y = (int)key;
        m = d = 1;
        break;
    default:                                /* a week (or a day): key is a day number */
        civil_from_days(key, &y, &m, &d);
        break;
    }
    char tmp[40];                           /* roomy, so nothing can be cut short */
    snprintf(tmp, sizeof tmp, "%04d-%02d-%02d", y, m, d);
    memcpy(out, tmp, 10);
    out[10] = '\0';
}

int stock_aggregate(Stock *out, const Stock *src, Period p)
{
    memset(out, 0, sizeof *out);
    if (src->count <= 0)
        return -1;

    Candle *c = malloc((size_t)src->count * sizeof *c);     /* at most one per day */
    if (!c)
        return -1;

    int n = 0;
    long cur = 0;
    for (int i = 0; i < src->count; i++) {
        const Candle *d = &src->candles[i];
        long key = period_key(d->date, p);

        if (n == 0 || key != cur) {
            cur = key;
            Candle *a = &c[n++];
            period_label(key, p, a->date);
            a->open = d->open;
            a->close = d->close;
            a->high = d->high;
            a->low = d->low;
            a->volume = d->volume;
            a->ref_high = d->ref_high;
            a->ref_low = d->ref_low;
            a->ref_close = d->ref_close;
            a->ref_volume = d->ref_volume;
        } else {
            Candle *a = &c[n - 1];
            if (d->high > a->high) a->high = d->high;
            if (d->low < a->low)   a->low = d->low;
            a->close = d->close;
            a->volume += d->volume;

            /* the file's part only grows from days that have one */
            if (!isnan(d->ref_high) && (isnan(a->ref_high) || d->ref_high > a->ref_high))
                a->ref_high = d->ref_high;
            if (!isnan(d->ref_low) && (isnan(a->ref_low) || d->ref_low < a->ref_low))
                a->ref_low = d->ref_low;
            if (!isnan(d->ref_close))
                a->ref_close = d->ref_close;
            if (!isnan(d->ref_volume))
                a->ref_volume = (isnan(a->ref_volume) ? 0.0 : a->ref_volume) + d->ref_volume;
        }
    }

    Candle *shrunk = realloc(c, (size_t)n * sizeof *c);     /* a year is far fewer than the days */
    out->candles = shrunk ? shrunk : c;
    out->count = n;
    snprintf(out->ticker, sizeof out->ticker, "%s", src->ticker);
    for (int i = 0; i < n; i++)
        if (out->candles[i].volume > 0.0) {
            out->has_volume = 1;
            break;
        }
    return 0;
}
