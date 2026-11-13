"""Drive the actual executable through a PTY; no terminal emulator package."""
import codecs
import datetime as dt
import errno
import fcntl
import math
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
ESC = b"\x1b"
UP, DOWN, LEFT, RIGHT = (ESC + value for value in (b"[A", b"[B", b"[D", b"[C"))


class Screen:
    """Interpret the cursor/erase sequences Candles emits, ignoring colors."""
    def __init__(self, rows=36, columns=160):
        self.rows, self.columns = rows, columns
        self.cells = [[" "]*columns for _ in range(rows)]
        self.row = self.column = 0
        self.escape = ""

    def feed(self, text):
        for char in text:
            if self.escape:
                self.escape += char
                if self.escape == "\x1b[":
                    continue
                if self.escape.startswith("\x1b[") and "@" <= char <= "~":
                    params = self.escape[2:-1]
                    if char in "Hf":
                        numbers = params.split(";")
                        self.row = int(numbers[0] or 1)-1
                        self.column = int(numbers[1] or 1)-1 if len(numbers)>1 else 0
                    elif char == "J" and params in ("2", "3"):
                        self.cells = [[" "]*self.columns for _ in range(self.rows)]
                    elif char == "K" and 0 <= self.row < self.rows:
                        start = 0 if params == "2" else self.column
                        self.cells[self.row][start:] = [" "]*(self.columns-start)
                    self.escape = ""
                elif len(self.escape)>64 or not self.escape.startswith("\x1b["):
                    self.escape = ""
            elif char == "\x1b":
                self.escape = char
            elif char == "\r":
                self.column = 0
            elif char == "\n":
                self.row += 1
            elif char >= " ":
                if 0 <= self.row < self.rows and 0 <= self.column < self.columns:
                    self.cells[self.row][self.column] = char
                self.column += 1

    @property
    def text(self):
        return "\n".join("".join(row) for row in self.cells)

    @property
    def header(self):
        return "".join(self.cells[0])


class TerminalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="candles-pty-")
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)/"home"
        self.folder = Path(self.temporary.name)/"stock data"
        self.folder.mkdir()
        config = self.home/".config/candles"
        config.mkdir(parents=True)
        (config/"settings.conf").write_text(f"folder = {self.folder}\nstock = AAA\n")
        rows = []
        for i in range(80):
            date = dt.date(2025, 1, 1)+dt.timedelta(days=i)
            close = 100+i*.3+math.sin(i)
            rows.append(f"{date},{close-1},{close},{close+2},{close-2},{1000+i*10}\n")
        for name in ("AAA", "ZZZ"):
            (self.folder/f"{name}.csv").write_text("date,open,close,max,min,volume\n"+"".join(rows))
        (self.folder/"graphs").mkdir()
        points = "".join(f"{i},{1+2*i+.1*math.sin(i)},{1+2*i}\n" for i in range(20))
        (self.folder/"graphs/fixture.graph").write_text(
            "CANDLES_GRAPH 8\ny: AAA | x: ZZZ | test fixture\nZZZ\nAAA\n"
            "1 .99 20 0 0 1 1 0 0\n.05\n3\n0 0 0\n0 0 1\n1\n2\n"+points)
        self.screen = Screen()
        self.master, self.slave = pty.openpty()
        self.original_modes = termios.tcgetattr(self.slave)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 36, 160, 0, 0))
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.process = subprocess.Popen([str(ROOT/"candles")], stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, cwd=self.temporary.name,
                                        env={**os.environ, "HOME": str(self.home), "TERM": "xterm-256color"},
                                        start_new_session=True)
        self.addCleanup(self.close)
        self.wait(lambda: "AAA" in self.screen.header)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=3)
        os.close(self.master)
        os.close(self.slave)

    def pump(self, duration=.05):
        end = time.monotonic()+duration
        while time.monotonic()<end:
            readable, _, _ = select.select([self.master], [], [], max(0, end-time.monotonic()))
            if not readable:
                return
            try:
                chunk = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    return
                raise
            if not chunk:
                return
            self.screen.feed(self.decoder.decode(chunk))

    def wait(self, condition, timeout=4):
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.pump()
            if condition():
                return
            if self.process.poll() is not None:
                break
        self.fail("Terminal condition was not reached:\n"+self.screen.text)

    def send(self, keys):
        if isinstance(keys, str):
            keys = keys.encode()
        os.write(self.master, keys)

    def click(self, column, row, button=0):
        self.send(f"\x1b[<{button};{column};{row}M")

    def quit(self):
        self.send("q")
        self.pump(.1)
        self.assertEqual(self.process.wait(timeout=3), 0)
        self.assertEqual(termios.tcgetattr(self.slave), self.original_modes)

    def open_graph(self):
        self.send("g")
        self.wait(lambda: "Graphs" in self.screen.text)
        self.send(DOWN+b"\r")
        self.wait(lambda: "fixture.graph" in self.screen.text)
        self.send(b"\r")
        self.wait(lambda: "adjusted R2" in self.screen.text)

    def test_stock_mouse_measurement_and_resize(self):
        self.assertFalse(termios.tcgetattr(self.slave)[3] & termios.ICANON)
        self.click(60, 10)
        self.wait(lambda: " O " in self.screen.header and " C " in self.screen.header)
        self.click(60, 10, 2)
        self.wait(lambda: " O " not in self.screen.header)
        self.send("h")
        self.wait(lambda: "measure: click first price" in self.screen.header)
        self.click(60, 10)
        self.wait(lambda: "measure: click second price" in self.screen.header)
        self.send(ESC)
        self.wait(lambda: "measure:" not in self.screen.header)
        self.send("h")
        self.wait(lambda: "measure: click first price" in self.screen.header)
        self.click(60, 10)
        self.wait(lambda: "measure: click second price" in self.screen.header)
        self.click(60, 15)
        self.wait(lambda: "%" in self.screen.header)
        self.send("z")
        self.wait(lambda: "%" not in self.screen.header)
        self.screen = Screen(40, 180)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 180, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)
        self.wait(lambda: "AAA" in self.screen.header and "quit" in self.screen.text)
        self.quit()

    def test_search_and_wrapping_strategy_menu(self):
        self.send("s")
        self.wait(lambda: "ZZZ" in self.screen.text)
        self.send("ZZ")
        self.pump(.1)
        self.send(b"\r")
        self.wait(lambda: "ZZZ" in self.screen.header)
        self.assertIn("stock = ZZZ", (self.home/".config/candles/settings.conf").read_text())
        self.send("j")
        self.wait(lambda: "Strategy shown on click" in self.screen.text)
        self.send(UP+b"\r")  # First entry wraps to the final strategy.
        self.wait(lambda: "Strategy shown on click" not in self.screen.text)
        self.assertIn("click_strategy = bb_strat", (self.home/".config/candles/strategy.conf").read_text())
        self.quit()

    def test_candidate_cancel_reopen_and_generate(self):
        # Current version-15 form: y=AAA, x=Time, maximum degree 3,
        # exhaustive selection, levels, all dates, no significance trimming.
        fields = [15, 0, "AAA", "-", "-", 1, "-", "-", "-", 0, 0, 1, 0,
                  1, 1, 1, 0, 0, 3, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0,
                  0, 0, 0, 0, 0, 0]
        self.assertEqual(len(fields), 36)
        (self.home/".config/candles/graphs.conf").write_text(
            "form = "+"|".join(map(str, fields))+"\nstandardize = 1\n")
        for cancel in (True, True, False):
            self.send("g")
            self.wait(lambda: "Graphs" in self.screen.text)
            self.send(b"\r")
            self.wait(lambda: "Maximum polynomial degree: 3" in self.screen.text)
            self.send(b"\r")
            self.wait(lambda: "Select model" in self.screen.text)
            self.send(ESC if cancel else b"\r")
            if not cancel:
                self.wait(lambda: "adjusted R2" in self.screen.text)
                self.send(ESC)
            self.wait(lambda: "Select model" not in self.screen.text and
                      "adjusted R2" not in self.screen.text and "AAA" in self.screen.header)
        self.quit()

    def test_saved_regression_panels_diagnostics_and_filename_editing(self):
        self.open_graph()
        self.send("p")
        self.wait(lambda: "Coefficient tests" in self.screen.text)
        before = self.screen.text
        self.send(RIGHT)
        self.wait(lambda: self.screen.text != before)
        self.assertIn("Coefficient tests", self.screen.text)
        self.send("i")
        self.wait(lambda: "Graph creation settings" in self.screen.text)
        self.assertNotIn("Coefficient tests", self.screen.text)
        self.send("i")
        self.wait(lambda: "Graph creation settings" not in self.screen.text)
        self.send("h")
        self.wait(lambda: "Residuals" in self.screen.text and "focus: residues" in self.screen.text)
        self.send("j")
        self.wait(lambda: "focus: histogram" in self.screen.text)
        self.send("t")
        self.wait(lambda: "x: ZZZ" in self.screen.text)
        self.send(ESC)
        self.wait(lambda: "adjusted R2" in self.screen.text)
        self.send("b")
        self.wait(lambda: "Confidence level" in self.screen.text)
        self.send(b"\r")
        self.wait(lambda: "Confidence level" not in self.screen.text)
        self.send("b")
        self.pump(.1)
        self.send("e")
        self.wait(lambda: "X value" in self.screen.text)
        self.send(b"\r")
        self.wait(lambda: "nearby observations" in self.screen.text)
        self.send("p")
        self.wait(lambda: "Prediction interval level" in self.screen.text)
        self.send(b"\r")
        self.wait(lambda: "95% prediction" in self.screen.text and "interval: [" in self.screen.text)
        self.send(ESC)
        self.wait(lambda: "adjusted R2" in self.screen.text)
        self.send("s")
        self.wait(lambda: "Save graph as:" in self.screen.text)
        self.send(LEFT*6+b"_edited\r")
        self.wait(lambda: "Saved graph:" in self.screen.text)
        self.assertTrue((self.folder/"graphs/fixture_01_edited.graph").is_file())
        self.send(ESC)
        self.wait(lambda: "AAA" in self.screen.header and "adjusted R2" not in self.screen.text)
        self.quit()
