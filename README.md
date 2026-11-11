# Candles — User Manual

Candles is a keyboard-driven terminal application for viewing stock candlesticks, fetching price data, displaying indicators, and exploring relationships with polynomial regression graphs.

This manual describes **version 0.3.27**.

## Contents

- [Requirements](#requirements)
- [Build and launch](#build-and-launch)
- [Quick start](#quick-start)
- [Files and folders](#files-and-folders)
- [Stock CSV format](#stock-csv-format)
- [Stock viewer](#stock-viewer)
- [Menus and search](#menus-and-search)
- [Live mode](#live-mode)
- [Importing external data](#importing-external-data)
- [Regression graphs](#regression-graphs)
- [Regression examples](#regression-examples)
- [Graph viewer](#graph-viewer)
- [Configuration and persistence](#configuration-and-persistence)
- [Troubleshooting](#troubleshooting)
- [Current limitations](#current-limitations)
- [Source layout](#source-layout)

## Requirements

For local CSV viewing and regression:

- A POSIX environment, such as Linux, with a C compiler supporting GNU C11 and `make`.
- The standard C/POSIX facilities and system math library. The C application does not require ncurses or a third-party charting library.
- An interactive terminal supporting ANSI colors, Unicode block/box/Braille characters, and terminal mouse reporting.
- A monospace font containing those glyphs. Rendering quality depends on the terminal and font.

The stock viewer requires at least **60 columns and 10 rows**. Use a larger window for indicators, menus, and detailed regression headers.

Fetching history and live prices additionally requires Python 3, `yfinance`, and network access. Python is not needed to view existing CSVs or fit regressions from them.

The code uses POSIX terminal and process APIs; there is no native Windows or graphical-window backend in this version.

## Build and launch

Run from this version's directory:

```sh
make
./candles
```

To rebuild all C objects:

```sh
make clean
make
```

Run the program in a terminal, not through an output pipe. Choose data folders inside the application with `f`; there is no command-line CSV/folder selection interface.

### Optional Python environment

For history/live fetching, create an environment and install the helper's dependency:

```sh
python3 -m venv /path/to/candles-venv
/path/to/candles-venv/bin/python3 -m pip install yfinance
```

After the first application launch, edit `~/.config/candles/settings.conf`:

```ini
python = /path/to/candles-venv/bin/python3
```

Use a real absolute path in place of `/path/to`. Activation is unnecessary when the interpreter path is configured directly. If `python` is unset, Candles runs `python3` from `PATH`.

Keep `grabber.py` beside the executable. To use a helper elsewhere, configure its absolute path:

```ini
grabber = /path/to/grabber.py
```

Restart Candles after manually changing configuration settings.

## Quick start

### View an existing stock

1. Put a stock CSV, such as `SPY.csv`, in a folder using the [stock CSV format](#stock-csv-format).
2. Start `./candles`.
3. Press `f`, enter that folder's path, and press `Enter`.
4. Press `s` to choose another stock from the folder.
5. Use arrows to pan, `z`/`x` to change candle width, and `c`/`v` to zoom the price scale.
6. Press `q` to quit.

The last folder and existing stock selection are restored on the next launch. If the remembered stock is absent, the application attempts to show the first stock in the folder.

### Download a new stock

1. Configure Python as described above.
2. Optionally press `f` and select an existing destination folder.
3. Press `s` and type the ticker, for example `SPY` or `PETR4.SA`.
4. Select the new ticker entry and press `Enter`.
5. Press lowercase `l` to fetch history and enter live mode.

If no folder has been selected, a successful initial download creates `stocks/` under the process's working directory. Selecting a new ticker alone does not create a CSV; downloading must succeed first.

## Files and folders

A typical data folder looks like this:

```text
stock-data/
├── SPY.csv
├── PETR4.SA.csv
├── data/
│   ├── fed_rates.csv
│   └── petrol_prices.csv
└── graphs/
    └── ...generated .graph files...
```

| Location | Purpose |
| --- | --- |
| `<chosen folder>/*.csv` | Stock files; each filename without `.csv` is a ticker. |
| `<chosen folder>/data/*.csv` | Imported two-column series for regression. |
| `<chosen folder>/graphs/*.graph` | Saved regression points, model, and title. |
| `~/.config/candles/settings.conf` | Colors, helper paths, indicator parameters, last folder and stock. |
| `~/.config/candles/views.conf` | Per-ticker chart position, scales, period, and indicator visibility. |
| `~/.config/candles/graphs.conf` | Last regression form selections and options. |

**Version directories share the same configuration and selected data folder.** Running an older executable does not isolate its settings or CSVs. Keep separate copies if you need fully independent test environments. Newer graph/configuration formats may not be understood by older executables.

## Stock CSV format

Each file represents one stock. Columns are interpreted by **position**, not by header names:

```csv
date,open,close,max,min,volume
2025-01-02,100.00,102.00,103.00,99.00,1200000
2025-01-03,102.00,101.00,104.00,100.00,1500000
```

| Column | Meaning | Required |
| --- | --- | --- |
| 1 | Date, `YYYY-MM-DD` | Yes |
| 2 | Open | Yes |
| 3 | Close | Yes |
| 4 | High (`max`) | Yes |
| 5 | Low (`min`) | Yes |
| 6 | Volume | No |

> **Column-order warning:** the format is `date,open,close,high,low`, not the common `date,open,high,low,close` order. A different header does not change how columns are parsed.

The stock reader supports comma, semicolon, or tab delimiters. With semicolons it also accepts decimal commas. A header is optional, extra columns are ignored, and valid rows are sorted by date. High and low are expanded if needed to enclose the open/close body.

Use real calendar dates, finite numeric values, and one row per date. Invalid rows can be skipped silently, and duplicate dates are not resolved automatically. Missing/nonpositive volume is treated as zero.

## Stock viewer

### Keyboard reference

Keys are lowercase and depend on the current screen.

| Key | Action in the stock viewer |
| --- | --- |
| `f` | Choose a stock-data folder. The folder prompt accepts a leading `~`. |
| `j` | Choose the strategy shown on candle clicks. |
| `s` | Search/select a stock or enter a new ticker for downloading. |
| `l` | Start or stop history synchronization and live prices. |
| `p` | Select daily, weekly, monthly, or yearly candles. |
| Left / Right | Pan toward older/newer candles. |
| Up / Down | Move the visible price range up/down. |
| `z` / `x` | Increase/decrease candle width. |
| `c` / `v` | Zoom the price scale in/out. |
| `k` | Toggle volume and its moving mean. |
| `b` | Toggle Bollinger upper/lower bands. |
| `r` | Toggle RSI. |
| `h` | Start a two-click percentage measurement. |
| `Esc` | Cancel a pending measurement. |
| `d` | Open Data: Import or Generate an indicator CSV. |
| `g` | Generate or open a regression graph. |
| `q` | Quit the application. |
| `Ctrl+C` | Exit from the main viewer; inside menus it cancels the menu. |

Available footer hints depend on the screen and window width; some may not fit in a narrow terminal.

### Reading the chart

- Candle bodies show open-to-close movement; stems show high and low. Default up/down colors are teal/red.
- Price scales appear on both sides. Label density changes with the visible scale.
- Date labels use month/day, with years at the ends of the date axis.
- Prices and volumes are rounded and abbreviated to keep labels compact. For example, `10.00k`, `12.34K`, and `46.50B` denote thousands/billions; volume labels aim for at most six characters.
- The header normally shows the latest close. This is the latest loaded candle, not necessarily the candle at the edge of your panned view.

### Mouse inspection

Left-click inside the main price plot over a candle's horizontal slot:

- The horizontal position chooses a candle, independently of click height.
- The header displays its date and open, close, high, and low values.
- The vertical position selects a price coordinate, shown on the price scales. It need not equal any of that candle's OHLC values.
- When each candle has its own date label, the selected candle's date is highlighted.

Right-click clears the inspection. Mouse clicks must be delivered by the terminal; a terminal that captures them for text selection can prevent this feature from working.

### Percentage measurement

1. Press `h`.
2. Left-click the first price level over a visible candle slot.
3. Left-click the second price level.

The header displays:

```text
(second price / first price - 1) × 100
```

Positive changes have a `+` sign and the up color; negative changes have a `-` sign and the down color. The measurement uses click heights, not candle closes. Its two clicks preserve an existing candle inspection. `Esc` cancels either pending step; the next user action after a completed measurement dismisses the result.

### Candle periods and indicators

`p` changes display aggregation; it does not rewrite daily CSVs:

| Period | Aggregation |
| --- | --- |
| Day | One source row per candle. |
| Week | Monday-based calendar weeks. |
| Month | Calendar months. |
| Year | Calendar years. |

Aggregated candles use the first open, last close, highest high, lowest low, and summed volume. Their date is the start of the calendar period. The latest period can be incomplete.

Indicators operate on the **displayed candle period**. A 20-period Bollinger setting means 20 weekly candles when viewing weeks, rather than 20 underlying daily rows.

- **Volume mean:** arithmetic moving mean of volume; one marker per candle slot.
- **Bollinger bands:** rolling mean of close plus/minus the configured number of population standard deviations, rendered with Braille dots.
- **RSI:** Wilder-style smoothed gains/losses with configurable period and upper/lower guides.

Indicators need enough observations before their first value. Volume and RSI use the lower panel; Bollinger bands overlay price.

## Menus and search

In bottom-line folder/file prompts and the graph-save filename prompt, Left/Right moves the text cursor. Typing inserts at the cursor, and Backspace removes the preceding character. Ctrl+A and Ctrl+E move to the beginning and end. Long text scrolls to keep the cursor visible.

### Ordinary menus

- Move with Up/Down or `k`/`j`.
- Press `Enter` to select, toggle, or edit an item.
- Press `Esc` or `q` to return.
- `[o]` means enabled; `[ ]` means disabled.
- Gray options are inactive and skipped by navigation.

### Search pickers

Stock, imported-data, and saved-graph pickers have a search box:

- Type a **case-insensitive prefix** to filter names.
- Use Up/Down and `Enter` to choose.
- Use Backspace to remove search characters and `Esc` to cancel.
- Lists scroll within a bounded window.

In search pickers, letters such as `j`, `k`, and `q` are search input, not ordinary-menu navigation commands. Search currently accepts ticker-style letters/digits and `.`, `-`, `_`, `^`, `=`, with a 31-character limit.

Only the stock picker offers new entries. Imported-data and graph pickers select existing files.

## Live mode

Press `l` to start. Candles first fetches missing **completed daily history**, then requests today's one-minute data approximately every **60 seconds**. This is periodic polling, not a tick-by-tick feed.

### History and split handling

- Existing stock: fetch completed dates after the CSV's last date.
- New stock: fetch the available full daily history.
- If a split is reported in the missing-history window, fetch full history and replace the CSV so old and new rows use the same split basis.
- Today's changing candle stays in memory; history synchronization excludes today, according to the exchange's calendar.

The helper requests `auto_adjust=False`: its intended price basis is split-adjusted and not dividend-adjusted. It does not use adjusted-close values or calculate total returns. Split detection covers the newly fetched window; it is not a general audit of every historical CSV row.

### Market states and price display

| State | Presentation |
| --- | --- |
| Regular session | Live price on both scales; normal latest-close header price hidden. |
| Pre-market | `pre-market` note with live presentation. |
| Extended session after closing | `after hours` note with live presentation. |
| Actually closed | `market closed` note and latest-price header restored. |

Extended-hours data can contribute to the in-memory high, low, price, and volume. Completed daily history does not retain that extended-hours presentation. Classification depends on the metadata/bars provided by the helper's data source.

Live updates do not automatically expand the visible price scale. A price label outside the visible range is hidden; zoom out with `v` or pan to include it. When there are no usable bars today, no live price label can be shown.

Press `l` again to stop and reload the stock from its CSV. Switching stock/folder also stops the previous live session. History errors can stop startup; a running live helper retries subsequent requests after temporary failures.

## Classic swing strategy

`j` selects which strategy appears after the candle values on click. The selection is saved in `~/.config/candles/strategy.conf` and restored at startup. `f` opens the folder prompt.

**Classic swing strategy 2** is a binary indicator. It is 1 only when volume is at least its configured moving average, the candle is green, its upper wick is strictly less than 10% of the full high–low range, its lower wick is at least 30% of that range, its low touches or crosses the lower Bollinger band, and RSI (using the configured RSI length) is at most 5. Otherwise it is 0. Before the volume average, Bollinger bands, or RSI are available, the value is unavailable. Zero-range candles score 0. `d` → **Generate** offers the strategies; the second writes `STOCK_classic_swing_strategy2.csv`.

**Classic swing strategy 3** uses the same four conditions (volume, the combined candle shape, lower Bollinger band, and RSI) and returns 1 when at least three hold. It has the same warm-up requirements. Generate it through `d` to write `STOCK_classic_swing_strategy3.csv`; choose it for the click display with `j`.

**3green** is 1 when the open is at or below the lower Bollinger band, or at least 25% of the candle's full high–low range is below that band; volume is strictly greater than its configured moving average; and the candle is green with an upper wick no more than 20% of its full range. Otherwise it is 0. It is unavailable until the configured indicators are available. Choose it with `j` or generate `STOCK_3green.csv` through `d`.

**bb_strat** uses the original classic swing score only when the candle's high–low range touches or crosses either Bollinger band; otherwise its value is zero. A candle entirely outside the bands without intersecting either line does not count as touching. Choose it with `j` or generate `STOCK_bb_strat.csv` through `d`. Its CSV includes zeros and unavailable warm-up dates like the original strategy.

The strategy 2, strategy 3 and 3green CSVs contain both 0 and 1 values. Dates where the indicators are unavailable, such as during warm-up, have an empty value. The original classic strategy also retains all dates.

Click a candle to see `Swing` in the top bar. The selected candle is determined by horizontal click position. Positive scores use the up color, negative scores the down color. `n/a` means indicator history is insufficient. Chart values use the displayed candle period; exported data always uses daily candles from the selected stock.

Press `d` → **Generate** → **Classic swing strategy** to write `<chosen folder>/data/STOCK_classic_swing_strategy.csv`. The CSV has `date,value` columns and is immediately selectable as Data in the regression generator. Each stock date is present; warm-up dates have an empty value and are skipped when loading regression data. Generating again replaces this generated file atomically. It is a snapshot, including the current live candle if present, and must be generated again to update it.

A Classic swing strategy section is added to `~/.config/candles/settings.conf` on startup if absent. It reuses the configured RSI period/marks, Bollinger period/deviation, and volume moving-average length. The volume moving average and Bollinger bands include the scored candle, consistent with the existing indicators. No future candles are used.

The score is the weighted mean of four symmetric components, each between −1 and +1:

- RSI: +1 at/below the lower mark, −1 at/above the upper mark, otherwise zero.
- Volume: zero below the volume average; magnitude starts at `swing_volume_base` (0.2) at the average and rises linearly to 1 at twice the average. Multiply this magnitude by `2 * (close - low) / (high - low) - 1`, clamped to [−1, +1]. Thus a close at the high gives +1, at the midpoint 0, and at the low −1. Zero range or zero average volume gives zero.
- Bollinger: `(middle band - open) / half band width`, clamped to [−1, +1]. Collapsed bands give zero.
- Candle: sign follows close minus open. Magnitude multiplies a close-near-extreme factor and an opposite-wick factor. The closing-side wick's factor falls linearly from 1 to 0 as its size reaches `swing_close_body_tolerance` (0.3) times the body. The opposite-wick factor is `0.5 + 0.5*q/(1+q)`, where `q` is opposite-wick length divided by body length. Bearish criteria mirror these at the low and upper wick. Flat or zero-range candles score zero for this component.

The four `swing_*_weight` settings default to 0.25 each and are normalized by their sum. All-zero weights produce unavailable scores. The remaining active strategy settings are `swing_volume_base` and `swing_close_body_tolerance`. The older `swing_size_ratio`, `swing_close_tolerance` and `swing_wick_ratio` settings are not used by this version.

A falling candle with no upper wick and a lower wick at least as long as its body contributes positively to the candle component; a rising candle with no lower wick and an upper wick at least as long as its body contributes negatively. The magnitude is `0.75 * (1 - 1 / (1 + wick/body))`. It increases with the wick/body ratio and stays below 0.75. The volume component separately uses the close's position within the range.

## Importing external data

Press `d`, choose **Import**, and enter an **absolute path** to a two-column CSV:

```csv
date,value
2025-01-02,4.25
2025-01-03,4.50
2025-01-06,4.50
```

The required format is exactly a date followed by one finite number, separated by a comma. Use `YYYY-MM-DD` and a decimal point. The stock reader's semicolon/decimal-comma support does not apply to this importer. Do not rely on shell expansions such as `~` in the import prompt.

The importer sorts accepted rows and writes a normalized copy to `<chosen folder>/data/`. It derives the selectable name from the source filename, sanitizes it, and adds a suffix if needed. The source file remains unchanged. Importing again creates another copy rather than updating the earlier import.

Imported series can be used for either regression axis. Their values have no open/close fields, unit metadata, or time of day: Candles does not know whether a value is a day's opening level, closing level, announcement, or repeated weekly observation.

Prepare one valid row per date. Calendar-date validation and duplicate handling are incomplete, and rejected rows are not summarized.

## Regression graphs

Press `g`, then choose **Generate regression** or **Open graph**. The graph replaces the stock chart in the **same terminal**; `Esc` returns.

The current menu requires a selected folder containing at least one stock CSV, even when the regression uses only imported data and Time.

### Axis selection

Both axes offer **Time**, **Stock**, and **Data**:

| Axis | Statistical role | Current menu label |
| --- | --- | --- |
| y | Outcome/dependent variable, fitted as a function of x. | `Explanatory / y` |
| x | Predictor/regressor variable. | `Regressor / x` |

The y menu's word “Explanatory” is misleading; the fitted relationship is always **y = f(x)**. Both axes cannot be Time.

Stock values are always the **daily CSV closes**. Choosing weekly candles in the stock viewer does not change regression inputs. Graph generation reads the CSVs, not today's unsaved live candle or the currently visible chart range.

### Form options

| Option | Behavior |
| --- | --- |
| `y as change`, `x as change` | Use a change instead of the raw value. |
| Change direction | Forward starts at the sample date; backward ends at it. |
| Change sampling | Choose the closest observation or a calendar-day horizon. |
| Change type | Separate x/y selectors offering percentage, absolute, or log return (%). Active only when that axis is a change. Log return uses `100 × ln(end/start)` and requires both underlying endpoint values to be strictly positive; otherwise the pair is excluded. |
| Change period | Independent y/x periods, each 1–3650 calendar days; inactive for closest-point sampling. |
| Exclude x changes equal to 0 | Remove pairs whose calculated x change is exactly zero; only applies when x is a change. |
| Exclude y changes equal to 0 | Remove pairs whose calculated y change is exactly zero; only applies when y is a change. Saved independently of the x filter; off by default. |
| Stocks use previous close on missing dates | For a stock/data or stock/stock pair, use the stock’s close from the preceding trading session when the other axis has a weekend or holiday date. For stock/data, imported-data dates become the candidate sample dates, so those weekend/holiday values can enter the graph. Never uses a future close. |
| Point sampling | Keep all eligible dates, enforce a minimum calendar-day distance, or retain the first eligible date in every calendar month or year. Works for every axis combination. |
| y/x calculate changes using | Separate controls for each axis: `all observations` uses the full original series for change endpoints; `sampled points` uses the dates retained by point sampling. Active when that axis is a change and point sampling applies thinning. With all eligible dates selected, the original series is used; saved choices become active again when thinning is enabled. |
| Minimum sample distance | The independent 1–3650-day distance used by minimum-distance sampling. |
| `log y`, `log x` | Natural logarithms, available for non-Time levels when change mode is off. |
| Maximum polynomial degree | Accepts 0–25. A value of 0 fits only a constant; otherwise fit degrees 1 through the selected maximum. |
| Fit exactly this degree | Available at every degree, including 0 and 1. Fit only the chosen degree and retain its highest power. If intermediate-term removal is also checked, insignificant lower powers may be removed and the model refitted while the highest power stays. If the requested model cannot be fitted, report an error rather than substitute a different degree. |
| Significance alpha | Optional cutoff between 0 and 1, e.g. `0.05`. Delete the field contents to retain the adjusted-R² rule alone. |
| Remove insignificant intermediate terms | Optional, off by default. With significance enabled, remove the eligible failing slope term with the largest p-value, refit, and repeat. In exact-degree mode only powers below the chosen highest power are eligible. The intercept is retained. |
| Model family | Polynomial (default), shifted exponential, or shifted logarithmic. The latter two support levels and changes on either axis, with the selected polynomial degree. |
| Standardize polynomial input | On by default. Turn off to use the input directly; for shifted logarithmic models this input is `ln(x - shift)`. The setting is remembered. Unstandardized high-degree models can be numerically unstable. |
| Generate regression | Fit and open the graph without saving; enabled after valid axis selections. |

The form remembers its selections when you reopen it or restart. A fresh form defaults to y forward/change-period sampling, x backward/closest-point sampling, percentage changes, one-day periods, **all eligible dates** for point sampling, and maximum degree 1. Change checkboxes, logs, and the zero filter start off.

### Exact meaning of changes

Let `d` be a sample's date, `V(d)` its value, `V(previous)` the selected earlier value, and `V(next)` the selected later value.

| Direction | Percentage | Absolute |
| --- | --- | --- |
| Forward | `(V(next) / V(d) - 1) × 100` | `V(next) - V(d)` |
| Backward | `(V(d) / V(previous) - 1) × 100` | `V(d) - V(previous)` |

Percentage results are percentage numbers: `2.5` means `+2.5%`, not `250%`. Axis values do not need a trailing `%`; the new header identifies percentage changes.

“Absolute” means a **signed difference**, not an absolute magnitude. A rate moving from 5 to 8 gives `+3`; from 8 to 5 gives `-3`. For rates stored in percentage units, these are percentage-point changes. The same moves in percentage-change mode are `+60%` and `-37.5%`.

Percentage change from a zero starting value is undefined and that pair is excluded. Absolute difference works through zero, so `0 → 1` gives `+1`.

Choosing log return under x/y change type follows the same direction, closest/period settings, selected-point endpoints, and stock holiday handling. Forward log return is `100 × ln(future/current)`; backward is `100 × ln(current/previous)`. Both underlying values must be positive, even when their ratio would be positive for two negative values. The result itself may be negative or zero. For example, `100 → 110` gives approximately `+9.5310` in log-return percentage units. This is the log of the value ratio, not the log of the calculated percentage change. These settings are distinct from the level-only log-x/log-y model options, and are remembered in the regression form and described in saved graph information and suggested filenames.

### Closest point versus change period

**Closest point** uses the immediately next/previous observation in that axis's full series, according to direction. It does not skip unchanged values to search for a different price. With unique dates, consecutive observations may be separated by one day, a weekend, or a much longer gap.

**Change period** uses a target date:

- Forward: first observation **on or after** `d + period`.
- Backward: last observation **on or before** `d - period`.

A forward one-day stock change on Friday therefore compares **Friday's close with Monday's close**, assuming Monday is the next trading day. A backward one-day change on Monday compares Monday's close with Friday's close.

These periods are calendar days, not counts of trading sessions. A 30-day period is not necessarily a calendar month, and gaps can extend the actual horizon considerably. If the required endpoint does not exist, the pair is excluded.

### Date alignment and sampling

With previous-close matching enabled for two stocks, candidate dates come from both stock calendars, so swapping axes preserves the date set. Calendar-day change periods are measured from the graph sample date, including when its stock close comes from an earlier session. A selected-points setting has no effect for an axis whose change checkbox is off.

When neither axis is Time, both sources normally must have an observation on the **same exact sample date**. The optional **Stocks use previous close on missing dates** checkbox is the sole exception: it carries a stock close forward from the preceding trading session, including Friday over a weekend or the close before a market holiday. It applies only to stock values; imported data still needs an actual row for the sample date, and no future stock close is used. With one Stock and one Data source enabled, the imported-data series supplies the candidate dates, allowing its weekend/holiday rows to be considered.

Once a date matches, each change is computed from its own full series. The comparison endpoint does not need a corresponding observation in the other series.

Changing a horizon does not itself reduce the frequency of points. A forward 360-day return can be calculated at every eligible daily date, producing many overlapping windows. Use **Point sampling** to thin observations independently of that return horizon.

### Point sampling

Point sampling is active for every graph, including Stock-versus-Stock and Stock-versus-Data graphs:

| Selection | Accepted observations |
| --- | --- |
| All eligible dates | Every valid paired observation. |
| Minimum day distance | The first valid pair at least the selected number of calendar days after the previously accepted pair. |
| First eligible date each month | The first valid paired observation in each calendar month. |
| First eligible date each year | The first valid paired observation in each calendar year. |

Calendar month/year modes mean the first **eligible paired** date, not literally the 1st of a month or year. A non-trading first day, missing row, invalid change endpoint, excluded zero x change, or invalid log value therefore lets the next valid candidate in that period be used. The modes neither average observations nor force a period-end value.

When selected points are used for changes, Candles first samples paired dates, then calculates changes between those dates. The sampled dates can be endpoints even when their own graph points are later excluded by a change or filter. In this mode, an invalid change does not cause a later date in the same month or year to be substituted. Closest-point sampling uses the adjacent selected date; change-period sampling finds the first selected date on or beyond the target in the chosen direction.

For example, choose a 360-day y change and **first eligible date each year** to use roughly one non-overlapping starting point per calendar year. For PETR4 versus petrol changes, choose a minimum distance of 365 days or the calendar-year mode to avoid fitting thousands of overlapping daily returns.

Time is always represented internally in days and rendered back as dates. Existing saved forms from before 0.2.26 are migrated so their former Time-granularity interval becomes a minimum sample distance.

### Logarithms and model selection

**Shifted model families:** the final generator option also offers exponential and logarithmic curves that can work with negative or zero change observations. Let `P(z)` denote the selected polynomial, with any removed terms omitted:

- Shifted exponential: `y = c + sign × exp(P(z))`, with `z = (x - mean) / scale`. Both signs are tried, allowing positive or negative exponential amplitude. The vertical offset `c` is fitted from all observations.
- Shifted logarithmic: `y = P(z)`, with `z = (ln(x - d) - mean) / scale`. The horizontal shift `d` is fitted below the smallest observed x. Negative x and y values are allowed; the curve is undefined for x at or below `d`.

Degree 1 gives the usual shifted exponential or logarithmic curve; higher degrees add powers inside the exponent or of the shifted log. The fitted equations and shifts appear under `p` and in the creation information. Existing log-x/log-y checkboxes are greyed out for these families: the family itself defines the transformation, and regression uses the calculated change values directly.

The shifted families use iterative nonlinear least squares with multiple starting shifts (and both exponential signs). Errors and adjusted R² are measured in the original y space. The fitted shift counts as one additional parameter for adjusted R² and residual degrees of freedom. Coefficient tests account for that extra parameter using the model Jacobian and are approximate. Exact-degree selection and significance-based term removal apply to the chosen family; removing every slope produces an ordinary constant fit. Iterative fits may reach a local minimum, particularly at high degrees; see [NIST's nonlinear least-squares guidance](https://www.itl.nist.gov/div898/handbook/pmd/section1/pmd142.htm).

Without logs, the model family is:

```text
y = a0 + a1*x + a2*x^2 + ... + ad*x^d
```

`log x` replaces x with `ln(x)` before fitting and plotting; the horizontal coordinates then are **log values**. `log y` fits `ln(y)` and converts fitted values back with `exp()` for the plotted curve; the scatter's y coordinates remain original values. With log-y enabled, adjusted R² is calculated in log-y space.

Log-x drops nonpositive x values; log-y drops nonpositive y values. Both filters act before accepting a pair. A constant y produces a horizontal fit with adjusted R² shown as `n/a`, since variation in y is zero.

For every degree from 1 through your maximum, Candles attempts a fit and selects the valid model with the highest adjusted R². Setting the degree to 0 instead fits only an intercept (a constant), including when a shifted model family is selected. There is an intercept. Degree 1 is a straight line for the polynomial family; the maximum is a search limit unless exact-degree mode is checked. A degree `d` needs more than `d + 1` observations (one additional fitted parameter for shifted families with positive degree), and the generator currently requires at least four pairs overall.

High-degree fitting can be numerically unreliable in this version. The score measures fit to the same observations used to select the model; there is no separate chronological prediction test, confidence interval, or causality calculation. Overlapping return windows are not independent observations.

When significance alpha is filled in, Candles evaluates each candidate degree and applies the significance-removal rules before comparing adjusted R². By default, a highest-degree coefficient whose p-value exceeds alpha is removed, followed by a refit and another test. The process stops at a significant highest term, preserving every lower term. If no slope survives, it produces an intercept-only model. The intercept is never removed. A candidate with numerically unusable coefficient tests or a failed reduced fit is skipped, and the search continues. The usable final candidate with highest adjusted R² wins; an error is reported only if none remain. Exactly zero trailing coefficients can be removed from a constant response. Polynomial models use ordinary OLS standard errors, without Newey–West correction; shifted families use approximate nonlinear tests.

With an empty significance field, tests do not gate model selection. Exact-degree fitting also returns the requested curve when its fit succeeds, even if coefficient tests are unavailable; the `p` panel shows those results as `n/a`.

Press `p` in the graph viewer for the equation and coefficient estimates, standard errors, t statistics, and p-values, with residual degrees of freedom `n - degree - 1`. The panel defines `z = (x - mean) / scale`: coefficients and tests refer to powers of this centered/scaled x, including any log-x transformation. With log-y, the equation models `log(y)` and the plotted fit is its exponential. Tests are exploratory because model selection used the same observations. Older saved graphs calculate the tests from their saved points and model.

When intermediate-term removal is checked, residual degrees of freedom and adjusted R² use the actual number of retained coefficients (including the intercept). Tests are recalculated after each removal. The equation omits removed powers, and the coefficient table marks them as removed. Terms refer to the displayed `z` basis. Exact-degree mode preserves the chosen highest power even when it fails significance; lower terms are removable only with the intermediate-term checkbox enabled. If their tests/refit become unavailable, exact mode keeps the last usable fit. An empty significance field bypasses removal.

When a significance cutoff was chosen, the p-values use the up/green color for `p <= alpha` and down/red for `p > alpha`. Undefined tests stay uncolored. The graph stores its own cutoff; changing the generator later does not change colors in an existing graph. For version 0.2.29 graphs, the cutoff is recovered from the saved creation description when present.

For polynomial models, the `p` overlay also shows the overall F statistic, its numerator/denominator degrees of freedom, and p-value for testing all retained slopes jointly against zero. It uses the actual retained terms and, if log-y is enabled, tests in log-y space. The overall p-value uses the same significance colors. Constant models and unavailable tests show `n/a`; shifted families do not show this test. It is calculated for the displayed final model only and does not participate in fitting, significance removal, or degree selection.

## Regression examples

### Fed-rate movement followed by SPY's next 30 days

Import a dated Fed-rate series, then use:

| Setting | Value |
| --- | --- |
| y source | Stock → SPY |
| y as change | On |
| y direction / sampling / type / period | Forward / change period / percentage / 30 days |
| x source | Data → your Fed-rate series |
| x as change | On |
| x direction / sampling / type | Backward / closest point / absolute |
| Exclude zero x changes | On |
| Logs | Off |

Each pair represents the rate change ending on date `d`, versus SPY's close-to-close percentage return starting on `d` and ending at the first available observation on or after `d + 30 days`.

Absolute x changes allow zero-rate transitions. Dates still have to match SPY trading dates. A return starting at that day's close does not include any market reaction earlier on the announcement day.

### Petrol's preceding move versus PETR4's following move

Choose PETR4 as y and the petrol series as x. Enable percentage change for both, with y **forward / closest point** and x **backward / closest point**.

At a shared date `d`, this compares:

```text
x = (petrol[d] / petrol[previous observation] - 1) × 100
y = (PETR4[next trading close] / PETR4[close at d] - 1) × 100
```

It does not infer petrol's opening/closing time from its date. If the petrol file repeats a weekly value across daily rows, most daily changes are zero. Excluding zero x changes retains only update dates that also exist in the stock file.

Switching x to forward compares petrol on `d` with its next observation. If the only nonzero forward changes are anchored on Sundays and PETR4 has no Sunday rows, the zero filter can leave no usable pairs. Enable **Stocks use previous close on missing dates** if those Sunday imported-data rows should instead use PETR4’s preceding Friday close; closest-point y sampling alone does not add weekend sample dates.

### PETR4's 360-day return over time

Choose y = PETR4, enable forward percentage change with a 360-day change period, and choose x = Time. Set time granularity to year for approximately one starting point every 365 days, or day for all eligible trading dates.

## Graph viewer

Graphs use a Braille raster for scatter observations and a continuous fitted curve. By default the curve uses the up color; scatter points use the Bollinger color and become progressively closer to the down color as terminal cells accumulate observations. Apparent density changes with zoom and terminal resolution.

| Input | Action in the graph viewer |
| --- | --- |
| Arrows | Pan horizontally/vertically. |
| `z` / `x` | Zoom the x axis in/out. |
| `c` / `v` | Zoom the y axis in/out. |
| `r` | Show/hide the regression curve. |
| `b` | Enter a confidence percentage and show red mean-response confidence bands; press again to clear. |
| `e` | Enter x and open the estimated conditional distribution of Y given that x. |
| `h` | Open residual diagnostics, including for saved graphs. |
| `i` | Toggle the saved graph creation settings in an upper-right box. Opening it replaces the `p` box. |
| `s` | Open a filename prompt on the bottom line. Edit the suggested name and press Enter to save; Esc cancels. |
| `p` | Toggle the equation/coefficient-test box at the graph's upper right. Opening it replaces the `i` box. |
| Left-click | Display the clicked location's x/y coordinates on the bottom/side scales. |
| Right-click | Clear the clicked-coordinate labels. |
| `Esc` or `q` | Return to the stock viewer. |

The graph click is a **coordinate readout**, not selection of the nearest scatter observation. `r` toggles the curve here; it toggles RSI in the stock viewer. Hiding the curve does not refit or automatically rescale the graph.

While either information box is open, arrows still pan the graph, zoom and regression-line controls still work, and only clicks outside the box affect the coordinate readout. Use `j`/`k` to scroll the box and `H`/`L` to scroll its text horizontally. Press the active `i` or `p` key again to close it. Only one box is open at a time, with its footer shortcut highlighted blue. Esc returns to the stock viewer.

### Mean-response confidence bands

Press `b`, enter a confidence percentage strictly between 0 and 100 (for example `95` or `95%`), then Enter. Red Braille curves show the lower and upper **pointwise** confidence limits for `E(y | X=x)` over the observed x range. Press `b` again to remove them. The next `b` prompt is prefilled with the last successfully used value, remembered across graphs during the current run. Arrow keys edit the input; Esc cancels the prompt. The footer shows the active confidence level and highlights `b` blue. Bands follow pan and zoom independently of the regression-line toggle and work with saved graphs. Their visibility and level are not stored in graph files.

For ordinary polynomials, the limits are `fitted mean ± t * sqrt(v' Cov(beta) v)`, using the complete covariance of the retained coefficients and the residual degrees of freedom. These are mean-response intervals, without the extra observation-noise term used for prediction intervals. Shifted exponential and logarithmic models use approximate intervals from the full parameter Jacobian, including the fitted shift. Singular covariance or insufficient residual degrees of freedom produces a message instead of bands. See the [NIST mean-response interval description](https://www.itl.nist.gov/div898/handbook/pmd/section5/pmd511.htm).

For `log y`, the bands estimate the original-scale conditional mean under homoscedastic normal log errors: `exp(eta + sigma2/2)`, with an approximate lognormal interval that also accounts for uncertainty in `sigma2`. Thus they can be offset from the existing fitted curve `exp(eta)`, which represents the conditional median. This extends the [modified Cox lognormal-mean approximation](https://www.itl.nist.gov/div898/software/dataplot/refman1/auxillar/conflimi.htm) to regression leverage. The footer marks nonlinear and lognormal intervals as approximate. All intervals condition on the selected model; model selection uncertainty and residual dependence are not incorporated. Enabling bands preserves the current graph scale; zoom out if needed to see their bounds.

### Conditional distribution of Y given x

Press `e` in a regression graph, enter x, and press Enter to open a full-screen distribution page. X is entered in the graph's displayed coordinates, including log-x values when enabled. For a time x axis, a valid `YYYY-MM-DD` date is also accepted. If a clicked location is active, its x coordinate prefills the prompt. Otherwise, the prompt initially suggests the center of the graph view and remembers the last entered x while that graph remains open. You can also left-click the plot while the prompt is open to replace the field with that location's x coordinate. Click height does not affect x; clicks inside information boxes are ignored. Numeric click values retain full double precision; time coordinates use the displayed date. Esc cancels the prompt.

The horizontal axis is the value of Y and the vertical axis is probability density. A red vertical line marks `E[Y | X=x]`. Blue block bars form a density histogram of the observed Y values whose X lies near the chosen x. The mean line draws in front. The header reports the mean, standard deviation, selected observation count, and neighborhood width.

The histogram is visible when at least three observations are selected. Changing the neighborhood automatically updates its visibility.

The histogram uses adaptive binning: its initial bin width accounts for the selected observations' range, including when the graph extends much farther. Horizontal zoom narrows or widens bins, while panning at a fixed zoom keeps bin boundaries anchored. Density is normalized by the entire selected neighborhood's observation count and bin width, including selected observations outside the view. The full histogram integrates to one; a cropped view can contain less mass. Bars use only block glyphs, with unrepresentable fragments hidden. Initial density scaling includes the visible histogram when enabled; subsequent toggles and neighborhood changes preserve the current view. Pan or zoom out to reveal clipped bars.

The default observation window is `x ± 1% * |x|`. Press `m` to change that percentage (zero selects exact x matches). At x = 0, the percentage uses the full observed x range instead; if the range is also zero, only exact matches are used. The header displays the resulting half-width in x units. The last percentage is retained during the current run. This filter changes only the histogram and its automatic visibility: the mean line, variance, and view stay fixed.

Arrows pan; `z`/`x` zoom the horizontal Y-value scale and `c`/`v` zoom the density scale. Left-click reads coordinates on the scales; right-click clears them. Esc returns to the regression graph, preserving its view, confidence bands, and any information panel. Saved graphs support the same feature without refitting or resaving.

Press `p` inside this page for a two-sided prediction interval for an **individual Y** at the chosen x. The bottom prompt starts at `95` percent and accepts any finite percentage strictly between 0 and 100, with an optional `%` suffix. Enter displays the numeric lower/upper limits in the header and marks them with vertical lines in the click-label color (yellow by default); the red mean line remains visible. Press `p` again to clear the interval. The next prompt retains the last accepted level on this page; reopening the page starts at 95 again. Esc cancels the prompt. These limits follow pan/zoom, and `m` does not change them. If a limit is outside the view, its numeric value remains in the header.

For ordinary polynomials the limits are `fitted mean ± t * sqrt(sigma2 + v' Cov(beta) v)`, including observation noise and uncertainty in the fitted mean. The full fitted sample, retained coefficients, and residual degrees of freedom are used. See [NIST's individual-response prediction intervals](https://www.itl.nist.gov/div898/handbook/pmd/section5/pmd512.htm). Shifted models use the same approximate Jacobian covariance as the mean confidence bands, including uncertainty in the shift. For log-y models, prediction limits are calculated on the log-response scale and exponentiated; they are positive and generally asymmetric. Insufficient degrees of freedom or unreliable covariance shows an error instead of limits.

For ordinary and shifted models, the displayed sigma is the residual standard deviation estimated from the complete fitted sample. Domain errors and unavailable error variance are reported in the x prompt.

For log-y models, the displayed mean and standard deviation use the corresponding lognormal conditional moments on the original Y scale. The mean marker is consistent with the original-scale mean used by the confidence bands, rather than the median curve `exp(eta)`.

### Residual diagnostics

Press `k` to show or hide the focused plot's teal line: the zero line for residues, or the normal curve for the histogram. Both start visible, and each remembers its own toggle when you switch focus or hide/show the histogram. The active line shortcut is blue in the footer.

Histogram bars use block characters only; fragments too small or positioned within a cell without a matching block are hidden. The teal normal curve uses Braille and draws in front of the bars.

Press `h` in a regression graph to show residuals against observation number, in the original observation order. Braille points use the same blue-to-red overlap coloring as the regression graph, with a teal zero line drawn over them. Numeric scales appear on both sides; axes have no titles. Arrows pan and `z`/`x`, `c`/`v` zoom the residual plot. Left-click either panel to inspect coordinates; right-click clears them.

Press `t` to switch the residual plot's x axis between chronological observation order and the regression graph's x values. The latter uses the actual displayed regressor values, including any log transform; time regressors use date labels. The two modes retain their horizontal pan and zoom separately. Saved graphs retain observation order but not every original date, so the chronological mode uses observation numbers, not calendar spacing. Lag-1 autocorrelation always uses the original observation order.

A density histogram appears below the plot by default. Press `n` to hide or show it. Press `j` to switch arrow-key pan and `z`/`x`, `c`/`v` zoom between the residual plot and histogram. The footer shows `j focus: residues` or `j focus: histogram`. Each panel retains its own position and scale; switching focus and zooming keep the panel sizes fixed. Hiding the histogram returns focus to residues and preserves its view for when it is shown again.

The histogram's teal normal curve uses the residual mean and estimated error variance `sigma2 = SSE / residual degrees of freedom`. Its bars, normal curve, numeric scales, and clicked coordinates follow its current view. Horizontal zoom recomputes bins: zooming in narrows them, and zooming out widens them. Panning at a fixed zoom preserves the bin grid. Densities remain normalized against the complete fitted sample, including observations outside the view; vertical zoom only changes the density scale. Statistics and the normal curve parameters always use the full sample. The curve is omitted when the variance is undefined or zero.

The header shows the lag-1 sample autocorrelation and Shapiro–Wilk W and p-value. Lag-1 uses adjacent observations, which need not be adjacent calendar days. For log-y models, diagnostics use residuals in log-y space; other models use observed y minus fitted y. Undefined statistics show `n/a`. Above 5,000 observations, W is displayed but the approximate Shapiro–Wilk p-value is withheld. Shapiro–Wilk is a normality diagnostic, not a correction for dependent residuals.

Press Esc to return to the regression graph with its view preserved. This also works for existing saved graphs without regenerating or resaving them. The statistical implementation and license attribution are described in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The header shows x, y, selected polynomial degree, adjusted R², and total accepted point count. Press `i` for the saved creation settings. Older graphs show whatever settings were recorded in their original titles. The count includes observations outside the current zoom and observations that overlap in the same terminal cell. Graph pan/zoom and curve visibility reset when reopening the viewer.

### Saved graphs and filenames

Generating a graph does not save it. Press `s` while viewing it to open a bottom-line filename prompt, prefilled with the next available name from the naming scheme below. Edit the name and press Enter to save under `<chosen folder>/graphs/`, or press Esc to cancel. A missing `.graph` extension is added automatically. Enter a filename rather than a path; an existing name is rejected so you can choose another. Leaving an unsaved graph discards it.

Open saved graphs through `g` → **Open graph**. Reopening displays the saved snapshot; it does not recalculate using updated CSVs. Pressing `s` on a reopened graph lets you save another copy.

New filename structure:

```text
YNAME_XNAME__y-SETTINGS__x-SETTINGS[__nonzero-x][__every-Nd]__maxM-fitD_NN.graph
```

| Token | Meaning |
| --- | --- |
| `close` / `data` / `time` | Daily stock close, imported series, or Time. |
| `level` / `log-level` | Normal value or natural-log level. |
| `pct` / `abs` | Percentage or signed absolute change. |
| `fwd` / `back` | Forward/backward direction. |
| `closest` / `30d` | Adjacent observation or a 30-calendar-day target. |
| `nonzero-x` | Zero x changes were excluded. |
| `every-365d` | Time-axis sampling uses at least 365-day spacing. |
| `max25-fit3` | Searched degrees through 25; selected degree 3. |
| `01`, `02`, … | Available sequence number to avoid replacing an existing file of the same description. |

Names are sanitized for filenames and may be shortened to fit filesystem limits. Active settings are preserved in the descriptive title, including source names and direction. Inactive periods are omitted when closest-point mode is used.

This version writes **graph format 8** and can read older formats 1–7. Format 8 stores the model family, fitted shift, and exponential sign, as well as retained terms and significance cutoff. Older executables may not read format 8. Existing graphs retain their old titles because their original full settings cannot be reconstructed reliably.

The graph file stores the points, fitted model, and description, but it is not a complete editable regression project: individual sample dates and all original source provenance are not saved separately.

## Configuration and persistence

All configuration files live under `~/.config/candles/`. The application creates default settings on first launch.

### Indicator settings

| Key | Default | Accepted range |
| --- | --- | --- |
| `volume_ma_len` | `20` | 1–5000 candles |
| `bb_len` | `20` | 1–5000 candles |
| `bb_std` | `2` | Greater than 0, up to 100 |
| `rsi_len` | `3` | 1–5000 changes |
| `rsi_upper` | `97` | 0–100 |
| `rsi_lower` | `3` | 0–100 |

Use whole numbers for lengths and keep RSI's lower threshold below its upper threshold. Invalid values fall back to defaults; ordering of RSI thresholds is not enforced.

### Colors

Colors use six hexadecimal RGB digits, normally `#rrggbb`:

```ini
up         = #26a69a
down       = #ef5350
background = #131722
text       = #d1d4dc
muted      = #787b86
border     = #363a45
bollinger  = #5b8def
volume_ma  = #ff9800
rsi        = #b39ddb
rsi_hit    = #ff4081
crosshair  = #ffeb3b
market     = #ff9800
```

Some colors are reused: regression scatter uses `bollinger`, its density moves toward `down`, and the fitted curve uses `up`.

### What is remembered

- **Last folder/stock:** saved when selected successfully; quitting with `q` is not required to save them.
- **Per-ticker chart profile:** horizontal/vertical zoom, scroll offsets, display period, volume, Bollinger, and RSI toggles. Profiles are keyed by ticker, not by folder.
- **Regression form:** selected x/y stock or data, change flags/types/directions/sampling/periods, logs, zero filter, time granularity, and maximum degree. It is one shared last-used form, not one form per stock or saved graph.

A ticker without a saved profile starts with daily candles, the default scales, no pan, and indicators off. Mouse inspection, measurements, live mode, and regression viewer navigation are not persisted.

Back up the configuration directory and your data folder together to preserve a workspace. For a reversible reset of a particular profile/form file, quit Candles and rename that file before restarting; it will be recreated as settings are saved.

## Troubleshooting

| Symptom | Explanation / action |
| --- | --- |
| `needs an interactive terminal` | Launch from an actual terminal with terminal stdin/stdout. |
| `Terminal too small` | Enlarge the window. Regression headers need additional rows. |
| Missing glyphs, broken stems, or unexpected gaps | Check Unicode font coverage and the terminal's glyph spacing. |
| Mouse selects terminal text | The terminal may be intercepting clicks rather than delivering mouse reports. |
| Some shortcuts are absent from the footer | The footer clips hints to the available width. |
| No valid stock rows | Check the positional `date,open,close,max,min` format. |
| No volume | Supply positive volume in column 6; the volume panel can be toggled off with `k`. |
| Indicators initially absent | They need enough displayed-period observations. |
| `cannot run ...` | Check the configured Python executable path. |
| `yfinance is not installed` | Install it in the environment selected by the `python` setting. |
| Helper not found | Keep `grabber.py` beside the executable or configure `grabber`. |
| Unknown ticker, network error, or request limit | Check the ticker/network and the displayed helper message. |
| Live price label disappears | It may be outside the fixed visible scale or lack today's usable data. |
| No imported data choices | Select the correct folder and import using `d`. |
| No saved graph choices | Graphs belong to the selected folder's `graphs/` directory. |
| A graph opens without reflecting new CSV rows | Saved graphs are snapshots; generate again to refit. |

### Not enough matching observations

The generator needs at least four accepted pairs. Check:

1. Whether the two datasets overlap in dates.
2. Whether both have rows on the exact sample dates.
3. Whether forward/backward endpoints exist for the requested periods.
4. Whether zero-starting percentage changes are excluded.
5. Whether the zero x-change or zero y-change filter removes most or all matches.
6. Whether point sampling leaves too few samples.
7. Whether log-x removes nonpositive x values.

The current error does not report a count for each exclusion reason.

### No valid regression

The message currently mentions logs, but other causes include constant x, insufficient distinct x values for the attempted degrees, or numerical fitting problems. Check variation in x and try a lower maximum degree.

## Current limitations

- Regression pairs normally require exact shared dates. The optional stock-only previous-close setting is the only date-alignment policy; it uses a preceding close, never an interpolation or future close.
- Imported data has dates and numbers only; publication times, revisions, and within-day ordering are not modeled.
- Closest-point sampling can span arbitrary gaps and currently does not protect against duplicate dates.
- Month/year time spacing uses fixed 30/365-day intervals, unlike the stock viewer's calendar aggregation.
- A change period can extend beyond its target when observations are missing.
- Numerical fitting at high polynomial degrees is unreliable; automatic adjusted-R² selection does not resolve all numerical problems or establish forecasting performance.
- Saved graph descriptions improve interpretation but do not capture every detail needed to reproduce the original source dataset.
- Live prices depend on external service availability and timing. Today's in-memory candle is not a saved intraday archive.

## Source layout

| File | Responsibility |
| --- | --- |
| [main.c](main.c) | Main input loop, selections, persistence calls, live workflow. |
| [terminal.c](terminal.c) | Terminal mode, input/mouse reports, resize handling, buffered rendering. |
| [chart.c](chart.c) | Candles, axes, indicators, mouse inspection, measurement. |
| [stock.c](stock.c) | Stock folder scanning, CSV parsing/writing, candle aggregation. |
| [indicators.c](indicators.c) | Volume mean, Bollinger bands, and RSI calculations. |
| [data.c](data.c) | Two-column data import, scanning, and loading. |
| [regression.c](regression.c) | Regression form, sample construction, fitting, graph files, viewer. |
| [residuals.c](residuals.c) | Residual statistics, Shapiro–Wilk normality test, density histogram. |
| [scale.h](scale.h) | Shared vertical label spacing. |
| [menu.c](menu.c) | Prompts, selection menus, searchable pickers. |
| [settings.c](settings.c) | Configuration defaults and parsing. |
| [state.c](state.c) | Saved folder/ticker, chart profiles, regression form. |
| [grabber.c](grabber.c) | Starting and communicating with the Python helper. |
| [grabber.py](grabber.py) | History/live requests, exchange-session state, split checks. |
| [live.c](live.c) | Applying live readings to in-memory stock candles. |
| [Makefile](Makefile) | Build and clean commands. |
