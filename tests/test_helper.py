"""Offline tests of the real Yahoo helper, with tiny DataFrame/Ticker doubles."""
import contextlib
import datetime as dt
import importlib.util
import io
import math
from pathlib import Path
import types
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("candles_helper", Path(__file__).resolve().parents[1] / "grabber.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
UTC = dt.timezone.utc
NOW = dt.datetime(2026, 11, 13, 17, tzinfo=UTC)


def row(day, **values):
    result = dict(Open=10, High=12, Low=9, Close=11, Volume=100)
    result.update(values)
    return dt.datetime.fromisoformat(day).replace(tzinfo=UTC), result


class Frame:
    def __init__(self, rows=()):
        self.rows = list(rows)

    def iterrows(self):
        return iter(self.rows)

    def __contains__(self, column):
        return any(column in values for _, values in self.rows)

    def __getitem__(self, column):
        return [values.get(column) for _, values in self.rows]

    def dropna(self, subset):
        return Frame((stamp, values) for stamp, values in self.rows
                     if all(values.get(key) is not None and not math.isnan(float(values[key])) for key in subset))


class HelperTests(unittest.TestCase):
    def ticker(self, frames):
        return types.SimpleNamespace(fast_info={"timezone": "UTC"}, history_metadata={}, history=Mock(side_effect=frames))

    def history(self, ticker, last):
        yf = types.SimpleNamespace(Ticker=Mock(return_value=ticker))
        output = io.StringIO()
        with patch.object(helper, "now", return_value=NOW), contextlib.redirect_stdout(output):
            helper.cmd_history(yf, {}, "TEST", last)
        yf.Ticker.assert_called_once_with("TEST")
        return output.getvalue()

    def test_day_rows_order_and_date_boundaries(self):
        frame = Frame([row("2026-11-10"), row("2026-11-11", Volume=None),
                       row("2026-11-12", Close=float("nan")), row("2026-11-13"), row("2026-11-14")])
        self.assertEqual(helper.day_rows(frame, dt.date(2026, 11, 10), NOW.date()),
                         ["2026-11-11,10,11,12,9,0"])

    def test_no_split_appends_only_missing_days(self):
        ticker = self.ticker([Frame([row("2026-11-11", **{"Stock Splits": 0}), row("2026-11-12")])])
        output = self.history(ticker, "2026-11-10")
        self.assertNotIn("REPLACE", output)
        self.assertIn("ROW 2026-11-11,10,11,12,9,100", output)
        self.assertTrue(output.endswith("END 2\n"))
        ticker.history.assert_called_once_with(start="2026-11-11", end="2026-11-13", interval="1d",
                                               auto_adjust=False, actions=True, timeout=helper.TIMEOUT)

    def test_split_refetches_complete_adjusted_history(self):
        ticker = self.ticker([Frame([row("2026-11-11", **{"Stock Splits": 4})]),
                              Frame([row("2020-01-02", Close=2.75), row("2026-11-12"), row("2026-11-13")])])
        output = self.history(ticker, "2026-11-10")
        self.assertTrue(output.startswith("REPLACE\n"))
        self.assertIn("ROW 2020-01-02,10,2.75,12,9,100", output)
        self.assertNotIn("ROW 2026-11-13", output)
        self.assertTrue(output.endswith("END 2\n"))
        self.assertEqual(ticker.history.call_count, 2)
        self.assertEqual(ticker.history.call_args.kwargs["period"], "max")
        self.assertFalse(ticker.history.call_args.kwargs["auto_adjust"])

    def test_reverse_split_is_also_detected(self):
        self.assertTrue(helper.has_split(Frame([row("2026-11-12", **{"Stock Splits": .1})])))

    def test_absent_zero_and_invalid_split_values_are_ignored(self):
        self.assertFalse(helper.has_split(Frame([row("2026-11-12")])))
        self.assertFalse(helper.has_split({"Stock Splits": [None, 0, "0", float("nan"), "invalid"]}))

    def test_initial_fetch_and_empty_history(self):
        ticker = self.ticker([Frame([row("2020-01-02"), row("2026-11-12")])])
        self.assertTrue(self.history(ticker, None).endswith("END 2\n"))
        self.assertEqual(ticker.history.call_args.kwargs["period"], "max")
        with self.assertRaises(helper.NoData):
            self.history(self.ticker([Frame()]), None)

    def test_already_current_never_fetches(self):
        ticker = self.ticker([])
        self.assertEqual(self.history(ticker, "2026-11-12"), "END 0\n")
        ticker.history.assert_not_called()

    def test_missing_window_without_trades_is_not_an_error(self):
        self.assertEqual(self.history(self.ticker([Frame()]), "2026-11-10"), "END 0\n")

    def test_market_session_boundaries(self):
        windows = {"pre": (100, 200), "regular": (200, 300), "post": (300, 400)}
        for now, expected in [(99, "closed"), (100, "pre"), (199, "pre"), (200, "open"),
                              (299, "open"), (300, "post"), (399, "post"), (400, "closed")]:
            with self.subTest(now=now):
                self.assertEqual(helper.market_state(now, windows, 0), expected)
        self.assertEqual(helper.market_state(1000, None, 999), "open")
        self.assertEqual(helper.market_state(1000, None, 1000-helper.STALE_BAR), "closed")

    def metadata(self):
        def epoch(hour):
            return NOW.replace(hour=hour, minute=0).timestamp()
        return {"exchangeTimezoneName": "UTC", "currentTradingPeriod": {
            "pre": {"start": epoch(13), "end": epoch(15)},
            "regular": {"start": epoch(15), "end": epoch(16)},
            "post": {"start": epoch(16), "end": epoch(18)}}}

    def test_old_session_metadata_is_not_reused(self):
        metadata = self.metadata()
        self.assertIsNotNone(helper.trading_windows(metadata, UTC, NOW.date()))
        self.assertIsNone(helper.trading_windows(metadata, UTC, NOW.date()+dt.timedelta(days=1)))

    def test_after_hours_uses_regular_open_and_all_bar_extremes(self):
        ticker = self.ticker([Frame([row("2026-11-13T14:00", Open=200, High=205, Low=190, Close=201),
                                    row("2026-11-13T15:00", Open=110, High=115, Low=105, Close=112),
                                    row("2026-11-13T17:00", Open=125, High=140, Low=90, Close=130)])])
        ticker.history_metadata = self.metadata()
        with patch.object(helper, "now", return_value=NOW):
            line, zone = helper.live_line(None, {}, ticker, None)
        self.assertEqual(line, "LIVE post 2026-11-13 1 110 205 90 130 300")
        self.assertEqual(zone, "UTC")
        self.assertTrue(ticker.history.call_args.kwargs["prepost"])

    def test_premarket_only_and_no_trades_today(self):
        ticker = self.ticker([Frame([row("2026-11-13T14:00")])])
        ticker.history_metadata = self.metadata()
        with patch.object(helper, "now", return_value=NOW.replace(hour=14, minute=30)):
            line, _ = helper.live_line(None, {}, ticker, None)
        self.assertEqual(line, "LIVE pre 2026-11-13 1 10 12 9 11 100")
        ticker = self.ticker([Frame([row("2026-11-12T14:00")])])
        with patch.object(helper, "now", return_value=NOW):
            line, _ = helper.live_line(None, {}, ticker, "UTC")
        self.assertEqual(line, "LIVE closed 2026-11-13 0 0 0 0 0 0")

    def test_formatting_and_error_messages(self):
        self.assertEqual(helper.num(-0.000000001), "0")
        self.assertEqual(helper.num(.123456789), "0.12345679")
        self.assertEqual(helper.one_line("some\n multiline\tmessage"), "some multiline message")
        self.assertIn("network error", helper.describe(TimeoutError("timed out\nagain"), "TEST"))

    def test_invalid_command_is_rejected_before_loading_yfinance(self):
        with patch.object(helper, "load_yfinance") as load, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(helper.main(["grabber.py", "bad", "TEST"]), 2)
            self.assertEqual(helper.main(["grabber.py", "history", "TEST", "not-a-date"]), 2)
        load.assert_not_called()
