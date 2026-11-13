# Candles architecture

The [README](README.md) is the user manual. This document describes the current code boundaries, not a proposed replacement architecture.

## Data flow

`main.c` owns application state, the daily stock, any aggregated candles, live-helper state, and the stock view. It dispatches input and passes the selected data to the renderer. `grabber.c` communicates with the Python helper; completed rows are written through `stock.c`, while live readings update the in-memory stock.

Regression generation reloads the selected daily CSV/data files. `regression.c` aligns dates, applies previous-close matching, point sampling, change endpoints, zero filters, and logs. It passes finite paired arrays to `regression_math.c`, applies model-selection policy, then opens a viewer. The viewers and graph file handling remain in `regression.c`.

## Numerical boundary

`regression_types.h` has no terminal, menu, stock, or settings dependencies. It defines `RegressionModel`, `RegressionGraph`, and model-family identifiers. The graph owns its x/y/fitted arrays; `regression_graph_free()` releases them. Numerical functions borrow these arrays and do not save state or perform terminal I/O.

`regression_math.h` exposes fitting/evaluation, term-mask helpers, coefficient tests, the polynomial overall F test, and confidence/prediction intervals. Its fit caller supplies prediction storage and a term mask. Bit k selects power k; zero masks on older saved models mean all powers through the saved degree.

The extraction preserves the existing algorithms: normal equations for polynomial fitting, damped nonlinear least squares for shifted families, and Jacobian-based nonlinear inference. It does not change standardization, model selection, graph formats, or settings formats. Log-y coefficient tests operate on the fitted log-response scale; interval routines retain their existing log-to-original-scale transformations.

`tests/regression_math.c` links only the numerical object and the math library. The integration test in `tests/model_selection.c` still includes the regression implementation to reach private search/configuration functions and replaces menu/persistence calls with test doubles.

## Model selection and persistence

With exact-degree fitting off, selection estimates a reference variance from the full maximum-degree fit, enumerates subsets containing the intercept and highest power, filters by Cp, and offers up to ten models ranked by AIC. Significance trimming is used in exact-degree mode. User-facing details and formulae live in the [model-selection section of the manual](README.md#choosing-a-model).

Generation returns an explicit success/cancel/error result to its menu wrapper; the public menu returns 1 for a ready graph, 0 for cancellation, and -1 for an error. Cancellation must never open a graph with empty arrays.

Regression form persistence currently uses a positional, versioned record inside `graphs.conf`; the parser supports historical versions. Graph files use a separate versioned text format and save model coefficients, term masks, paired observations, predictions, and descriptions. Preserve legacy loading when changing either format. A graph file does not contain a complete copy of its original source CSVs.

## Remaining boundaries to improve

- Candidate search still stores all successful fits before retaining ten. Both subset enumeration and its memory use grow exponentially; extracting the numerical functions does not solve that cost.
- Date alignment, change sampling, persistence, menus, and all graph viewers remain together in `regression.c`. These can be separated in later behavior-preserving steps.
- Generator actions use positional menu indexes. Some saved graph metadata is inferred from the description text. Named actions and explicit metadata would reduce coupling.
- Strategy names and dispatch are repeated across modules, and stock rendering recalculates indicators/strategy prefixes during redraw. A shared strategy registry and invalidated calculation cache would address those separately.

## Modules

| File | Responsibility |
| --- | --- |
| [main.c](main.c) | Main input loop, selections, persistence calls, live workflow. |
| [terminal.c](terminal.c) | Terminal mode, input/mouse reports, resize handling, buffered rendering. |
| [chart.c](chart.c) | Candles, axes, indicators, mouse inspection, measurement. |
| [stock.c](stock.c) | Stock folder scanning, CSV parsing/writing, candle aggregation. |
| [indicators.c](indicators.c) | Volume mean, Bollinger bands, and RSI calculations. |
| [swing.c](swing.c) | Swing strategies and indicator CSV export. |
| [data.c](data.c) | Two-column data import, scanning, and loading. |
| [regression.c](regression.c) | Regression form, sample construction, model selection, graph files, and viewers. |
| [regression_math.c](regression_math.c) | Numerical fitting, evaluation, coefficient/F tests, confidence and prediction intervals. |
| [regression_types.h](regression_types.h) | Model family identifiers and plain model/graph data structures. |
| [regression_math.h](regression_math.h) | Numerical API and inference result types; independent of terminal/configuration headers. |
| [residuals.c](residuals.c) | Residual statistics, Shapiro–Wilk normality test, density histogram. |
| [scale.h](scale.h) | Shared vertical label spacing. |
| [menu.c](menu.c) | Prompts, selection menus, searchable pickers. |
| [settings.c](settings.c) | Configuration defaults and parsing. |
| [state.c](state.c) | Saved folder/ticker, chart profiles, regression form. |
| [grabber.c](grabber.c) | Starting and communicating with the Python helper. |
| [grabber.py](grabber.py) | History/live requests, exchange-session state, split checks. |
| [live.c](live.c) | Applying live readings to in-memory stock candles. |
| [Makefile](Makefile) | Build, clean, and automated checks. |
