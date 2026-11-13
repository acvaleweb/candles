# Changelog

## Unreleased

- Extract regression fitting, evaluation, statistical tests, and intervals into a numerical module with no terminal or configuration dependencies. Preserve calculation algorithms and selection behavior.
- Add standalone numerical checks against closed-form OLS, coefficient tests, mean/prediction intervals, sparse models, log-y transformations, and shifted-family fits.
- Consolidate regression usage into README, add an architecture guide, and correct the testing instructions. Remove overlapping implementation reports and unverified performance estimates.
- Clarify Git ignore rules for application/test binaries, compiler output, coverage files, and Python caches. No build artifacts were tracked at the time of the audit.

## 4.1

- Enumerate only optional powers below the required highest degree, so each candidate model occurs once.
- Distinguish cancelled model selection from successful generation. Esc returns to the stock viewer without opening an empty graph.
- Add `make check` coverage for unique subsets and repeated cancel/reopen/select cycles without reading or writing the user's configuration.

## 4.0

- Add exhaustive term-subset search within the selected model family.
- Filter candidates by Mallows' Cp and offer up to ten candidates ranked by AIC.
- Retain direct fitting and significance-based intermediate-term removal through exact-degree mode.

Earlier source snapshots are retained outside this checkout in the workspace's `legacy/` folder. Git history is used for new source versions.
