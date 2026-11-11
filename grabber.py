#!/usr/bin/env python3
"""The yfinance side of candles: the only part of the program that talks to Yahoo.

    grabber.py history TICKER [LAST_DATE]
        Prints every day after LAST_DATE (yyyy-mm-dd), or the whole history when
        there is no date, up to yesterday in the exchange's own calendar. Today
        is never included. Prices use Yahoo's auto_adjust=False data: dividends
        are not adjusted, but splits are. If a split occurred after LAST_DATE,
        it first prints REPLACE and then prints the complete history, so candles
        can replace its now-incompatible CSV instead of appending to it.

            ROW yyyy-mm-dd,open,close,max,min,volume
            ...
            END <number of rows>

    grabber.py live TICKER
        Runs until its standard input is closed (which is how candles tells it
        to stop). Every 60 seconds, counted from the start of each request, it
        prints one line about today:

            LIVE <state> <date> <has> <open> <max> <min> <price> <volume>

        state: open, pre, post or closed (the exchange's state right now).
        has:   0 when there are no trades today yet; the numbers are then 0.
        Extended hours (pre / post) are included in max, min, price and volume:
        they show up live, but history never has them.

Whatever goes wrong is a single line, and the program carries on or stops:

        ERR <what happened>
"""

import datetime as dt
import logging
import os
import select
import sys
import time
import warnings

INTERVAL = 60       # seconds between two live requests
TIMEOUT = 15        # seconds one request may take
STALE_BAR = 600     # without a trading window: bars older than this mean closed


# ---------------------------------------------------------------- plumbing

def out(text):
    """One message to candles. If candles is gone there is nobody to tell."""
    try:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()
    except (BrokenPipeError, OSError):
        os._exit(0)


def one_line(text, limit=200):
    return " ".join(str(text).split())[:limit]


def num(x):
    """A price as short text: 38.53, never 38.529998779296875 or 3.8e+01."""
    x = float(x)
    r = round(x, 4) if abs(x) >= 1 else round(x, 8)
    s = ("%.8f" % r).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def isnan(x):
    return x != x


def now():
    return dt.datetime.now(dt.timezone.utc)


def tz_of(name):
    try:
        from zoneinfo import ZoneInfo
        return ZoneInfo(name)
    except Exception:
        return dt.timezone.utc


def to_epoch(t):
    """Yahoo's times arrive as epoch seconds or as pandas timestamps."""
    if t is None:
        return None
    if hasattr(t, "timestamp"):
        return float(t.timestamp())
    try:
        return float(t)
    except (TypeError, ValueError):
        return None


def describe(exc, ticker):
    """An exception as one line a person can act on."""
    names = [c.__name__ for c in type(exc).__mro__]
    if any(n in ("YFTickerMissingError", "YFTzMissingError", "YFPricesMissingError")
           for n in names):
        return "no data for %s (unknown or delisted ticker?)" % ticker
    if "YFRateLimitError" in names:
        return "Yahoo is limiting requests, try again in a while"
    if any(w in n for n in names for w in ("Connection", "Timeout", "Curl", "Proxy", "SSL")):
        return "network error: " + one_line(exc, 120)
    return "%s: %s" % (type(exc).__name__, one_line(exc, 150))


class NoData(Exception):
    pass


# ---------------------------------------------------------------- yfinance

def load_yfinance():
    """(module, extra history() arguments that make it raise instead of log)."""
    try:
        import yfinance as yf
    except ImportError:
        out("ERR yfinance is not installed (pip install yfinance)")
        sys.exit(2)
    except Exception as e:
        out("ERR cannot load yfinance: " + one_line(e, 150))
        sys.exit(2)

    logging.getLogger("yfinance").setLevel(logging.CRITICAL)
    try:                                   # newer versions
        yf.config.debug.hide_exceptions = False
        return yf, {}
    except AttributeError:                 # older ones
        return yf, {"raise_errors": True}


def exchange_tz_name(tk):
    """The exchange's time zone name, or None."""
    try:
        name = tk.fast_info["timezone"]
        if name:
            return name
    except Exception:
        pass
    try:
        return tk.history_metadata.get("exchangeTimezoneName")
    except Exception:
        return None


# ---------------------------------------------------------------- history

def day_rows(df, after, today):
    """The CSV text of every day of df that is after `after` and before today."""
    rows = []
    for ts, r in df.iterrows():
        day = ts.date()
        if day >= today or (after is not None and day <= after):
            continue
        try:
            o, h, l, c = (float(r["Open"]), float(r["High"]),
                          float(r["Low"]), float(r["Close"]))
        except (KeyError, TypeError, ValueError):
            continue
        if isnan(o) or isnan(h) or isnan(l) or isnan(c):
            continue
        v = r["Volume"] if "Volume" in r else 0
        v = 0 if v is None or isnan(float(v)) else int(float(v))
        rows.append("%s,%s,%s,%s,%s,%d" % (day.isoformat(), num(o), num(c), num(h), num(l), v))
    return rows


def has_split(df):
    """Whether Yahoo reported a non-zero split event in this history window."""
    if "Stock Splits" not in df:
        return False
    for value in df["Stock Splits"]:
        try:
            if value is not None and not isnan(float(value)) and float(value) != 0.0:
                return True
        except (TypeError, ValueError):
            pass
    return False


def cmd_history(yf, strict, ticker, last):
    tk = yf.Ticker(ticker)
    tz = tz_of(exchange_tz_name(tk))
    today = now().astimezone(tz).date()

    after = None
    if last:
        after = dt.date.fromisoformat(last)
        first = after + dt.timedelta(days=1)
        if first >= today:                          # nothing can be missing
            out("END 0")
            return
        # Request actions only for the small missing window.  If Yahoo reports a
        # split there, all older split-adjusted prices have changed basis, so the
        # caller must replace its entire file with a fresh full-history fetch.
        df = tk.history(start=first.isoformat(), end=today.isoformat(), interval="1d",
                        auto_adjust=False, actions=True, timeout=TIMEOUT, **strict)
        if has_split(df):
            out("REPLACE")
            after = None
            df = tk.history(period="max", interval="1d",
                            auto_adjust=False, actions=False, timeout=TIMEOUT, **strict)
    else:
        df = tk.history(period="max", interval="1d",
                        auto_adjust=False, actions=False, timeout=TIMEOUT, **strict)

    rows = day_rows(df, after, today)
    if not rows and after is None:
        raise NoData("no data for %s (unknown or delisted ticker?)" % ticker)
    if rows:
        sys.stdout.write("".join("ROW " + r + "\n" for r in rows))
        sys.stdout.flush()
    out("END %d" % len(rows))


# ---------------------------------------------------------------- live

def trading_windows(meta, tz, today):
    """{'pre': (start, end), 'regular': ..., 'post': ...} as epoch seconds for
    today, or None if Yahoo's current trading period is not today's."""
    ctp = meta.get("currentTradingPeriod") or {}
    w = {}
    for k in ("pre", "regular", "post"):
        p = ctp.get(k) or {}
        s, e = to_epoch(p.get("start")), to_epoch(p.get("end"))
        if s is not None and e is not None:
            w[k] = (s, e)
    reg = w.get("regular")
    if not reg or dt.datetime.fromtimestamp(reg[0], tz).date() != today:
        return None
    return w


def market_state(now_ts, windows, last_bar_ts):
    if windows:
        rs, re_ = windows["regular"]
        if rs <= now_ts < re_:
            return "open"
        if now_ts < rs:
            pre = windows.get("pre")
            return "pre" if pre and pre[0] <= now_ts else "closed"
        post = windows.get("post")
        return "post" if post and now_ts < post[1] else "closed"
    # no trading window known (some markets): judge by how fresh the last bar is
    return "open" if now_ts - last_bar_ts < STALE_BAR else "closed"


def live_line(yf, strict, tk, tz_name):
    """One LIVE line for today. Returns (line, tz_name)."""
    df = tk.history(period="1d", interval="1m", prepost=True,
                    auto_adjust=False, actions=False, timeout=TIMEOUT, **strict)
    meta = tk.history_metadata
    tz_name = meta.get("exchangeTimezoneName") or tz_name
    tz = tz_of(tz_name)
    now_ts = now().timestamp()
    today = now().astimezone(tz).date()

    df = df.dropna(subset=["Close"])
    bars = [(ts, r) for ts, r in df.iterrows() if ts.date() == today]
    if not bars:
        return "LIVE closed %s 0 0 0 0 0 0" % today.isoformat(), tz_name

    windows = trading_windows(meta, tz, today)
    regular = bars
    if windows:
        rs, re_ = windows["regular"]
        regular = [(ts, r) for ts, r in bars if rs <= to_epoch(ts) < re_] or bars

    def col(name):
        return [float(r[name]) for _, r in bars if not isnan(float(r[name]))]

    opens = [float(r["Open"]) for _, r in regular if not isnan(float(r["Open"]))]
    highs, lows = col("High"), col("Low")
    closes = col("Close")
    volume = sum(0 if isnan(float(r["Volume"])) else float(r["Volume"]) for _, r in bars)
    if not (opens and highs and lows and closes):
        return "LIVE closed %s 0 0 0 0 0 0" % today.isoformat(), tz_name

    state = market_state(now_ts, windows, to_epoch(bars[-1][0]))
    return ("LIVE %s %s 1 %s %s %s %s %d" % (
        state, today.isoformat(), num(opens[0]), num(max(highs)), num(min(lows)),
        num(closes[-1]), int(volume))), tz_name


def parent_gone(fd, deadline):
    """Sleeps until deadline. True if candles closed our input meanwhile."""
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return False
        try:
            ready, _, _ = select.select([fd], [], [], left)
        except (OSError, ValueError):
            return True
        if ready:
            try:
                if not os.read(fd, 4096):
                    return True
            except OSError:
                return True


def cmd_live(yf, strict, ticker):
    fd = sys.stdin.fileno()
    tz_name = None
    while True:
        started = time.monotonic()
        try:
            line, tz_name = live_line(yf, strict, yf.Ticker(ticker), tz_name)
            out(line)
        except Exception as e:
            out("ERR " + describe(e, ticker))
        if parent_gone(fd, started + INTERVAL):
            return 0


# ---------------------------------------------------------------- main

def main(argv):
    warnings.simplefilter("ignore")
    usage = "ERR usage: grabber.py history TICKER [yyyy-mm-dd] | live TICKER"
    if len(argv) < 3 or argv[1] not in ("history", "live"):
        out(usage)
        return 2
    mode, ticker = argv[1], argv[2]
    last = argv[3] if len(argv) > 3 else None
    if last:
        try:
            dt.date.fromisoformat(last)
        except ValueError:
            out(usage)
            return 2

    yf, strict = load_yfinance()
    try:
        if mode == "history":
            cmd_history(yf, strict, ticker, last)
            return 0
        return cmd_live(yf, strict, ticker)
    except NoData as e:
        out("ERR " + str(e))
    except Exception as e:
        out("ERR " + describe(e, ticker))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
