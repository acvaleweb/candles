# Testing Candles

Run commands from the repository directory. The [README](README.md) describes application controls; [ARCHITECTURE.md](ARCHITECTURE.md) describes module boundaries.

## Automated checks

```sh
make check
make check-sanitize
```

`make check` builds Candles, runs nine C test programs, and runs the Python helper/terminal tests. It needs a C compiler, `make`, and Python 3 with its standard library. It does not need yfinance, stock downloads, a GUI, a real interactive terminal, or third-party testing libraries. Python tests allocate their own pseudo-terminals.

`make check-sanitize` rebuilds the C tests and their application modules with AddressSanitizer and UndefinedBehaviorSanitizer. Undefined behavior is fatal. This target needs compiler/runtime support for those sanitizers. It does not instrument the executable used by the Python terminal tests.

| Test | Coverage |
| --- | --- |
| `tests/regression_math.c` | Closed-form OLS versus matrix fitting; standardization, adjusted R², coefficient tests, overall F test, mean/prediction intervals, sparse/legacy masks, log-y transformations, invalid fits, and shifted families. Links only the numerical object and `-lm`. |
| `tests/model_selection.c` | Unique subsets for degrees 0–6, required terms, and repeated cancel/reopen/select cycles with menu/persistence doubles. |
| `tests/io.c` | Stock delimiters, decimal commas, BOM, sorting, invalid numeric rows, OHLC normalization, append/newline handling, replacement failure cleanup, folder scanning, calendar aggregation, live reference aggregation, and data-import naming/zero values. |
| `tests/indicators.c` | Moving averages, population-standard-deviation Bollinger bands, Wilder RSI, classic shape/volume direction, binary strategy thresholds and wick/band boundaries, warmup blanks, and exported binary zeroes. |
| `tests/state.c` | Settings defaults/validation, paths with spaces, configuration preservation, independent ticker profiles, malformed profiles, regression form storage, standardization, and remembered strategy. |
| `tests/diagnostics.c` | Residual variance, lag-1 and Durbin–Watson statistics, Shapiro–Wilk properties, histogram density normalization during zoom/pan, and evenly spaced axis values. |
| `tests/input_live.c` | Prompt insertion, UTF-8 cursor/backspace behavior, full buffers, live OHLC normalization, stale readings, and preservation of the reference values used for scales. |
| `tests/grabber.c` | Fragmented helper messages, split replacement, session states, errors/EOF, and real child startup/shutdown with a local fake helper. |
| `tests/regression_pipeline.c` | Independent x/y change periods/directions, percentage/absolute/log returns, zero filters, sampled endpoints for either/both axes, month/year thinning with Time, weekend prior-close matching on either stock axis, form roundtrips, all eight legacy/current graph versions, sparse model persistence, and corrupt graph rejection. |
| `tests/test_helper.py` | Offline Yahoo-helper doubles: split/refetch versus append, date limits, empty history, pre/regular/post session boundaries, extended-hours OHLC, and error formatting. |
| `tests/test_terminal.py` | Actual executable through a PTY: search, menu wrapping, mouse selection, measurement/cancellation, resizing, candidate cancellation/reopening, graph panels and pan, residual focus, prediction intervals, filename cursor editing, saving, and terminal-mode restoration. |

C file fixtures live in private temporary directories. Tests that use real persistence set a private `HOME`; the original selection test substitutes state calls. Python terminal tests also use private homes and data folders. No test contacts Yahoo or modifies the user's data/settings. Temporary fixtures are cleaned up on normal test completion and handled check failures.

Generated objects/binaries live under ignored `tests/bin/`; sanitizer outputs use `tests/bin/sanitize/`. `make clean` removes these, the application executable/objects, and older test binaries. Tests explicitly keep assertions enabled even if normal build flags contain `-DNDEBUG`.

For a subset or another compiler:

```sh
make check-c
make check-python
make CC=clang TEST_BUILD=tests/bin/clang check-c
```

Run `make clean` before changing compiler/build flags in an existing build directory. Tests stop the command with a nonzero exit status on failure. The [GitHub Actions workflow](.github/workflows/tests.yml) runs the full suite and C sanitizers with GCC and Clang on pushes and pull requests; it does not change branch-protection settings.

These checks protect the specified behaviors, not every input, numerical approximation, external API response, font, or terminal implementation. Shapiro tests check analytic small cases and invariants; they are not a complete external validation of every p-value. High-degree model search and every nonlinear candidate-scoring policy are not exhaustively validated.

## Manual regression checks

Use disposable CSVs and data folders. Different checkouts share settings under `HOME` and can update the same CSVs. To isolate manual testing, create a temporary home and launch with it:

```sh
candles_test_home=$(mktemp -d)
HOME="$candles_test_home" ./candles
```

Choose a test data folder inside the application. Leave live mode off unless testing the helper deliberately.

### Candidate selection and cancellation

1. Choose overlapping variables with at least four pairs, variation in x, and a noisy relationship. Select the polynomial family, maximum degree 3, and leave exact-degree fitting unchecked.
2. Generate. If candidates pass the Cp filter, verify the picker shows at most ten candidates, sorted by nondecreasing AIC; equal AIC is allowed. At degree 3 there are at most seven subsets across degrees 1–3 before filtering.
3. Press Esc from the picker. Verify the stock viewer is usable; reopen `g` and generate again.
4. Select a candidate, then press `p`. Verify the displayed equation matches the retained terms and the graph is usable. Similar rounded menu metrics alone do not prove two models are duplicates.

Datasets are not guaranteed to produce ten candidates, sparse models, or any candidate passing Cp. The full reference fit may also fail before the picker opens. Record the data and settings when investigating an unexpected result.

### Exact degree and inference

1. Enable exact-degree fitting and choose degree 3. Leave alpha empty; generate and verify there is no candidate picker and all four polynomial terms remain.
2. Set alpha and enable intermediate-term removal. Generate and inspect `p`: the highest power and intercept stay, while lower powers may be removed. P-values at or below alpha are green and those above it are red; unavailable values remain uncolored.
3. In exact mode, test degree 0 and degree 1. Verify constant and straight-line fits respectively. A degree-1 model has no removable intermediate slopes.
4. Toggle standardization and compare full polynomial predictions. Coefficient values and intercept p-values can change with the basis even when the curve agrees.
5. For a positive response, test log-y. The equation/test scale should be logarithmic while the plotted response stays on its original scale.
6. Test shifted exponential and shifted logarithmic families at low degrees. Verify successful curves, the shift in `p`, and graceful errors for unsuitable inputs. Nonlinear inference is approximate.

### Alignment and change sampling

Use small date/value files where endpoints can be checked by hand. Cover forward/backward changes, closest point versus calendar-day horizon, all observations versus sampled endpoints, independent x/y periods, and percentage/absolute/log returns. Include a weekend, a missing trading date, a zero starting value, and the x/y zero-change filters. Verify previous-close matching never uses a future stock close.

### Viewers and saved graphs

- Save with `s`, edit the proposed filename using arrows, and reopen through `g` → Open graph. Verify points, fitted curve, retained terms, and the saved significance cutoff.
- Toggle `p` and `i`; verify they exclude each other, pan/zoom still works, and clicks inside the panel do not select graph coordinates.
- Test residuals with `h`, focus switching with `j`, histogram visibility with `n`, and line visibility with `k`. Check pan/zoom and the residual x-axis toggle `t`.
- Test `b` mean-response bands and `e` conditional histogram. Inside `e`, test `p` prediction intervals and `m` observation-window changes.
- Resize at small and large terminal sizes. Check axes, panel clipping, and Braille line/point layering.

## Recording failures and performance

Include the model family, maximum/exact degree, standardization, alpha, sampling/change settings, accepted point count, and a reproducible data fixture. Distinguish fitting errors from unavailable inference and from cancelled menus.

Measure timings and memory on the actual dataset and hardware. Exhaustive search tries `2^(d−1)` subsets at each positive degree d and currently stores all successful candidates' predictions before choosing ten. The UI's degree cap of 25 is not a practical exhaustive-search guarantee. Avoid high-degree performance tests without accounting for that cost; nonlinear fits add iterative work per candidate.
