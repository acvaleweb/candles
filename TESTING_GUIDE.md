# Testing Candles

Run commands from the repository directory. The [README](README.md) describes application controls; [ARCHITECTURE.md](ARCHITECTURE.md) describes module boundaries.

## Automated checks

```sh
make
make check
```

`make check` compiles and runs two C test programs:

| Test | Coverage |
| --- | --- |
| `tests/regression_math.c` | Closed-form simple OLS versus matrix fitting with standardization on/off; adjusted R², coefficient standard errors, t/F consistency, and 95% mean/prediction intervals; sparse terms, legacy masks, constant response, insufficient observations, constant x, log-y transformations, and shifted exponential/logarithmic fits. |
| `tests/model_selection.c` | Unique term subsets for degrees 0–6, required intercept/highest terms, and repeated cancellation/reopening/selection through the real generation path. |

The numerical test links only `regression_math.o` and `-lm`. The selection integration test substitutes menu and state calls, uses a temporary date/value fixture, and does not read or write user configuration. Its source includes `regression.c` to reach private functions.

Passing these checks does not establish every statistical approximation, exhaustive-search metric, legacy graph version, terminal/font behavior, or live Yahoo response. Manual scenarios below are checks to perform, not a claim that they have all passed.

Build products, including the two test executables, are ignored by Git. `make clean` removes the executable, object files, and test executables.

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
