# Testing Guide for Exhaustive Model Selection

## Prerequisites

1. Compile the program:
   ```bash
   cd /home/toaster/candles
   make
   ```

2. Ensure you have stock CSV data or test data available

## Test Scenarios

### Test 1: Basic Exhaustive Search (Polynomial)

**Objective**: Verify exhaustive search generates and presents model candidates

**Steps**:
1. Launch `./candles`
2. Load a folder with stock CSV files
3. Press `g` to open regression menu
4. Select "Generate regression"
5. Configure:
   - Choose Y variable (e.g., a stock)
   - Choose X variable (e.g., Time or another stock)
   - Set "Maximum polynomial degree" to 3 or 4
   - Ensure "fit exactly this degree" is NOT checked (unchecked = exhaustive search)
6. Select "Generate regression"

**Expected Results**:
- A menu should appear titled "Select model"
- Should show up to 10 models
- Each entry shows: `AIC <value> (Δ <delta>) | p=<params> | adj-R² <score>`
- Models should be sorted by AIC (ascending)
- First model should have lowest AIC
- All models should have Cp < p (implicitly, they passed the filter)

**Verify**:
- AIC values increase down the list
- Delta values show difference from top model
- Parameter counts (p) vary
- Can select any model and it generates a valid graph

---

### Test 2: Sparse Term Models

**Objective**: Verify models with missing intermediate terms

**Steps**:
1. Same setup as Test 1
2. Set "Maximum polynomial degree" to 5
3. Generate regression

**Expected Results**:
- Some models should have non-consecutive terms
- Example: A degree-5 model might include {0, 2, 5} but not {1, 3, 4}
- These models should fit successfully
- Graph should display correctly with sparse coefficients

**Verify**:
- Press `t` in the regression viewer to see coefficient tests
- Some beta coefficients should be listed as "removed"
- The equation display should show the included terms only

---

### Test 3: exact_degree Bypass

**Objective**: Verify old behavior is preserved when exact_degree is enabled

**Steps**:
1. Launch regression generator
2. Configure variables
3. Set "Maximum polynomial degree" to 3
4. Enable "[x] fit exactly this degree" checkbox
5. Generate regression

**Expected Results**:
- NO menu should appear
- Should directly generate the regression for degree 3
- Uses old R²-based selection (or the only full model)

**Verify**:
- No model selection menu
- Goes straight to graph view
- All terms from 0 to 3 are included

---

### Test 4: Different Model Families

**Objective**: Test exhaustive search with nonlinear families

**Steps**:
1. Setup regression generation
2. Set "Model family" to "shifted exponential"
3. Set max degree to 2 or 3
4. Generate (with exact_degree unchecked)

**Expected Results**:
- Menu appears with candidates
- Parameter counts include the shift parameter (+1)
- Models fit exponential curves
- Selection works correctly

**Repeat for**: "shifted logarithmic" family

---

### Test 5: Cp < p Filtering

**Objective**: Verify only models with Cp < p are presented

**Steps**:
1. Use a dataset with weak relationships (high noise)
2. Set max degree high (e.g., 7)
3. Generate regression

**Expected Results**:
- Menu may have fewer than 10 models (if many fail Cp < p)
- Message "No models with Cp < p found" if all fail the criterion
- Selected models should be reasonable (not overfitted)

**To inspect Cp values** (optional code modification for testing):
- Temporarily modify select_model_menu() to also display Cp values
- Verify all displayed Cp < p

---

### Test 6: Edge Cases

#### Test 6a: max_degree = 0
**Steps**:
1. Set max degree to 0 (constant model)
2. Generate

**Expected**: Single model (intercept only) with appropriate AIC

#### Test 6b: max_degree = 1
**Steps**:
1. Set max degree to 1
2. Generate

**Expected**: Single or two models (maybe just intercept and x^1)

#### Test 6c: Insufficient Data
**Steps**:
1. Use very small dataset (e.g., 5-10 points)
2. Set high max degree (e.g., 8)

**Expected**: 
- Error message about insufficient points or singular models
- Graceful handling (no crash)

---

### Test 7: Integration with Existing Features

**Objective**: Verify exhaustive search works with other features

#### Test 7a: With Significance Testing
**Steps**:
1. Set significance alpha (e.g., 0.05)
2. Enable "remove insignificant intermediate terms"
3. Generate with exact_degree enabled

**Expected**: Old significance-based term removal still works

#### Test 7b: Graph Saving
**Steps**:
1. Generate with exhaustive search
2. Select a sparse model
3. Press `s` to save graph
4. Name and save
5. Open the saved graph later

**Expected**:
- Graph saves successfully
- Reopening shows correct model with sparse terms
- Residuals and tests work correctly

#### Test 7c: Log Transformations
**Steps**:
1. Enable "[x] log y"
2. Generate with exhaustive search

**Expected**:
- SSE calculated in log space
- AIC and Cp correct
- Model selection menu appears
- Selected model works in residual viewer

---

## Manual Verification Checklist

For each test, verify:

- [ ] No compilation warnings
- [ ] No crashes or segfaults
- [ ] Menu displays correctly
- [ ] AIC values are reasonable (not NaN or Inf)
- [ ] User can navigate menu with arrow keys
- [ ] Selected model creates valid graph
- [ ] Graph displays scatter plot and fitted curve
- [ ] Coefficient tests panel shows correct terms
- [ ] Residual analysis works
- [ ] Graph can be saved
- [ ] Saved graph can be reopened

## Expected Performance

**Timing estimates** (on modern hardware):
- Degree 3: < 0.1 seconds
- Degree 5: < 0.5 seconds  
- Degree 7: < 2 seconds
- Degree 10: < 10 seconds (with ~1000 data points)

If performance is slower, consider:
- Dataset size (more observations → longer fitting)
- Nonlinear families (exponential/log are slower)
- Number of candidate models generated

## Troubleshooting

### Issue: "No models with Cp < p found"
**Cause**: All models have poor fit (Cp >= p)
**Solutions**:
- Try lower max degree
- Check data quality
- Use different variables

### Issue: Menu doesn't appear
**Cause**: exact_degree is enabled
**Solution**: Uncheck "fit exactly this degree"

### Issue: Very similar AIC values
**Cause**: Models fit similarly well
**Normal**: AIC delta < 2 means models are competitive; choose based on simplicity

### Issue: Crashes during generation
**Cause**: Likely memory issue or invalid data
**Check**:
- Sufficient RAM for large degree
- No NaN or Inf values in data
- Dataset has > 4 observations

## Validation Criteria

Implementation is successful if:
1. ✓ Compiles without errors
2. ✓ Exhaustive search generates candidates
3. ✓ Menu presents top 10 by AIC
4. ✓ Cp < p filter works
5. ✓ Selected models create valid graphs
6. ✓ exact_degree bypass preserved
7. ✓ Works with all model families
8. ✓ No memory leaks (use valgrind if available)
9. ✓ Graphs save and reload correctly
10. ✓ Integration with existing features maintained
