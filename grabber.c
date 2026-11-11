#include "grabber.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PENDING (4u << 20)      /* a line this long without a newline is garbage */

void grab_init(Grabber *g)
{
    memset(g, 0, sizeof *g);
    g->fd = -1;
    g->wfd = -1;
}

/* ---------- starting the helper ---------- */

/* argv is the whole command line, argv[0] being the program to run. */
static int spawn(Grabber *g, char *const *argv, char *err, size_t errn)
{
    int to_us[2], to_it[2];

    if (pipe(to_us) < 0) {
        snprintf(err, errn, "Cannot start the data helper: %s", strerror(errno));
        return -1;
    }
    if (pipe(to_it) < 0) {
        snprintf(err, errn, "Cannot start the data helper: %s", strerror(errno));
        close(to_us[0]);
        close(to_us[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errn, "Cannot start the data helper: %s", strerror(errno));
        close(to_us[0]); close(to_us[1]); close(to_it[0]); close(to_it[1]);
        return -1;
    }
    if (pid == 0) {
        /* The helper must never write on the screen: its output and its
         * complaints both go to the pipe, and its input is a pipe we hold open
         * (it stops by itself when that closes, even if we are killed). */
        dup2(to_it[0], STDIN_FILENO);
        dup2(to_us[1], STDOUT_FILENO);
        dup2(to_us[1], STDERR_FILENO);
        close(to_us[0]); close(to_us[1]); close(to_it[0]); close(to_it[1]);
        execvp(argv[0], argv);
        dprintf(STDOUT_FILENO, "ERR cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(to_us[1]);
    close(to_it[0]);
    fcntl(to_us[0], F_SETFL, fcntl(to_us[0], F_GETFL) | O_NONBLOCK);
    fcntl(to_us[0], F_SETFD, FD_CLOEXEC);
    fcntl(to_it[1], F_SETFD, FD_CLOEXEC);

    g->pid = pid;
    g->fd = to_us[0];
    g->wfd = to_it[1];
    return 0;
}

static int start(Grabber *g, const Settings *cfg, int history, const char *ticker,
                 const char *after, char *err, size_t errn)
{
    if (g->fd >= 0)
        grab_stop(g);
    grab_init(g);

    if (!ticker || !ticker[0]) {
        snprintf(err, errn, "No ticker to fetch");
        return -1;
    }
    if (!cfg->grabber[0]) {
        snprintf(err, errn, "No data helper configured (grabber.py)");
        return -1;
    }

    const char *py = cfg->python[0] ? cfg->python : "python3";
    char *argv[8];
    int n = 0;
    argv[n++] = (char *)py;
    argv[n++] = "-u";
    argv[n++] = (char *)cfg->grabber;
    argv[n++] = history ? "history" : "live";
    argv[n++] = (char *)ticker;
    if (history && after && after[0])
        argv[n++] = (char *)after;
    argv[n] = NULL;

    if (spawn(g, argv, err, errn) != 0)
        return -1;
    g->history = history;
    return 0;
}

int grab_start_history(Grabber *g, const Settings *cfg, const char *ticker,
                       const char *after, char *err, size_t errn)
{
    return start(g, cfg, 1, ticker, after, err, errn);
}

int grab_start_live(Grabber *g, const Settings *cfg, const char *ticker,
                    char *err, size_t errn)
{
    return start(g, cfg, 0, ticker, NULL, err, errn);
}

int grab_fd(const Grabber *g)
{
    return g->fd;
}

/* ---------- stopping ---------- */

void grab_stop(Grabber *g)
{
    if (g->wfd >= 0)
        close(g->wfd);                          /* its input ends: it stops by itself */
    if (g->pid > 0) {
        kill(g->pid, SIGTERM);
        int done = 0;
        for (int i = 0; i < 50 && !done; i++) {
            pid_t r = waitpid(g->pid, NULL, WNOHANG);
            if (r == g->pid || (r < 0 && errno != EINTR))
                done = 1;
            else
                poll(NULL, 0, 10);
        }
        if (!done) {
            kill(g->pid, SIGKILL);
            while (waitpid(g->pid, NULL, 0) < 0 && errno == EINTR)
                ;
        }
    }
    if (g->fd >= 0)
        close(g->fd);
    free(g->buf);
    free(g->csv);
    grab_init(g);
}

/* ---------- reading what it says ---------- */

static int looks_like_date(const char *s)
{
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return 0;
        } else if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

static int add_row(Grabber *g, const char *row)
{
    size_t rl = strlen(row);
    if (g->csv_len + rl + 2 > g->csv_cap) {
        size_t cap = g->csv_cap ? g->csv_cap : 65536;
        while (cap < g->csv_len + rl + 2)
            cap *= 2;
        char *p = realloc(g->csv, cap);
        if (!p)
            return -1;
        g->csv = p;
        g->csv_cap = cap;
    }
    memcpy(g->csv + g->csv_len, row, rl);
    g->csv_len += rl;
    g->csv[g->csv_len++] = '\n';
    g->csv[g->csv_len] = '\0';
    g->days++;
    return 0;
}

static int parse_live(Grabber *g, const char *text)
{
    char state[16], date[16];
    int has;
    double o, h, l, p, v;

    if (sscanf(text, "%15s %15s %d %lf %lf %lf %lf %lf", state, date, &has, &o, &h, &l, &p, &v) != 8)
        return 0;
    if (!looks_like_date(date))
        return 0;

    LiveReading *r = &g->reading;
    memset(r, 0, sizeof *r);
    if (strcmp(state, "open") == 0)        r->state = MARKET_OPEN;
    else if (strcmp(state, "pre") == 0)    r->state = MARKET_PRE;
    else if (strcmp(state, "post") == 0)   r->state = MARKET_POST;
    else if (strcmp(state, "closed") == 0) r->state = MARKET_CLOSED;
    else                                   return 0;
    memcpy(r->date, date, 10);
    r->has_candle = has != 0;
    r->open = o;
    r->high = h;
    r->low = l;
    r->price = p;
    r->volume = v;
    return 1;
}

/* One line of what the helper printed. Returns 1 if it is an event. */
static int handle_line(Grabber *g, const char *line, GrabEvent *ev)
{
    if (strcmp(line, "REPLACE") == 0 && g->history) {
        g->replace_csv = 1;
        return 0;
    }
    if (strncmp(line, "ROW ", 4) == 0 && g->history) {
        if (looks_like_date(line + 4))
            add_row(g, line + 4);
        return 0;
    }
    if (strncmp(line, "END ", 4) == 0 && g->history) {
        *ev = GRAB_HISTORY;
        return 1;
    }
    if (strncmp(line, "LIVE ", 5) == 0 && !g->history) {
        if (parse_live(g, line + 5)) {
            *ev = GRAB_READING;
            return 1;
        }
        return 0;
    }
    if (strncmp(line, "ERR ", 4) == 0) {
        snprintf(g->err, sizeof g->err, "%s", line + 4);
        *ev = GRAB_ERROR;
        return 1;
    }
    if (line[0])
        snprintf(g->noise, sizeof g->noise, "%s", line);
    return 0;
}

/* Takes apart the complete lines in the buffer until one is an event. */
static int take_line_events(Grabber *g, GrabEvent *ev)
{
    int found = 0;
    while (!found && g->off < g->len) {
        char *nl = memchr(g->buf + g->off, '\n', g->len - g->off);
        if (!nl)
            break;
        *nl = '\0';
        char *line = g->buf + g->off;
        g->off = (size_t)(nl - g->buf) + 1;
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r')
            line[ll - 1] = '\0';
        found = handle_line(g, line, ev);
    }

    if (g->off > 0) {                           /* keep only the unfinished line */
        memmove(g->buf, g->buf + g->off, g->len - g->off);
        g->len -= g->off;
        g->off = 0;
    }
    if (g->len > MAX_PENDING)
        g->len = 0;
    return found;
}

/* The helper's output ended: say why, in words. */
static void explain_end(Grabber *g)
{
    int status = 0;
    pid_t r = g->pid > 0 ? waitpid(g->pid, &status, WNOHANG) : 0;

    if (g->err[0])
        return;
    if (g->noise[0]) {                          /* e.g. python: can't open file ... */
        snprintf(g->err, sizeof g->err, "%s", g->noise);
    } else if (r == g->pid && WIFEXITED(status)) {
        snprintf(g->err, sizeof g->err, "The data helper stopped (exit status %d)", WEXITSTATUS(status));
    } else if (r == g->pid && WIFSIGNALED(status)) {
        snprintf(g->err, sizeof g->err, "The data helper was killed (signal %d)", WTERMSIG(status));
    } else {
        snprintf(g->err, sizeof g->err, "The data helper stopped");
    }
}

GrabEvent grab_poll(Grabber *g)
{
    if (g->fd < 0)
        return GRAB_IDLE;

    for (;;) {
        GrabEvent ev = GRAB_IDLE;
        if (take_line_events(g, &ev))
            return ev;
        if (g->eof) {
            explain_end(g);
            return GRAB_ENDED;
        }

        char tmp[8192];
        ssize_t n = read(g->fd, tmp, sizeof tmp);
        if (n > 0) {
            if (g->len + (size_t)n + 1 > g->cap) {
                size_t cap = g->cap ? g->cap : 16384;
                while (cap < g->len + (size_t)n + 1)
                    cap *= 2;
                char *p = realloc(g->buf, cap);
                if (!p) {
                    snprintf(g->err, sizeof g->err, "Out of memory");
                    g->eof = 1;
                    return GRAB_ERROR;
                }
                g->buf = p;
                g->cap = cap;
            }
            memcpy(g->buf + g->len, tmp, (size_t)n);
            g->len += (size_t)n;
            continue;
        }
        if (n == 0) {
            g->eof = 1;
            continue;                           /* take any last complete lines first */
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return GRAB_IDLE;
        g->eof = 1;
    }
}

/* ---------- where the helper script is ---------- */

void grab_default_script(char *out, size_t n, const char *argv0)
{
    char exe[PATH_MAX];
    exe[0] = '\0';

    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (r > 0) {
        exe[r] = '\0';
    } else if (argv0 && strchr(argv0, '/')) {
        if (!realpath(argv0, exe))
            exe[0] = '\0';
    } else if (argv0 && argv0[0]) {             /* started through PATH */
        const char *path = getenv("PATH");
        while (path && *path) {
            const char *end = strchr(path, ':');
            size_t dl = end ? (size_t)(end - path) : strlen(path);
            char cand[PATH_MAX];
            snprintf(cand, sizeof cand, "%.*s/%s", (int)dl, dl ? path : ".", argv0);
            if (access(cand, X_OK) == 0 && realpath(cand, exe))
                break;
            exe[0] = '\0';
            path = end ? end + 1 : NULL;
        }
    }

    char *slash = strrchr(exe, '/');
    if (!slash) {
        snprintf(out, n, "grabber.py");         /* best guess: the current folder */
        return;
    }
    *slash = '\0';
    snprintf(out, n, "%s/grabber.py", exe);
}
