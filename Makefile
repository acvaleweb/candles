CC      = cc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra
TARGET  = candles
SRC     = main.c terminal.c settings.c state.c stock.c data.c indicators.c chart.c menu.c grabber.c live.c regression.c regression_math.c residuals.c swing.c
OBJ     = $(SRC:.c=.o)
PYTHON ?= python3
TEST_BUILD ?= tests/bin
TEST_CFLAGS ?= $(CFLAGS) -UNDEBUG
TEST_NAMES = regression_math model_selection io indicators state diagnostics input_live grabber regression_pipeline
TEST_BINS = $(addprefix $(TEST_BUILD)/,$(TEST_NAMES))
TEST_SRC = $(filter-out main.c regression.c,$(SRC))
TEST_OBJ = $(patsubst %.c,$(TEST_BUILD)/obj/%.o,$(TEST_SRC))

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lm

%.o: %.c $(wildcard *.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
	rm -f tests/model_selection tests/regression_math
	rm -rf tests/bin

$(TEST_BUILD)/obj/%.o: %.c $(wildcard *.h)
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TEST_BUILD)/%: tests/%.c tests/test_support.h $(TEST_OBJ) regression.c $(wildcard *.h)
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -I. $< $(TEST_OBJ) -lm -o $@

# Keep the numerical API test independent of all UI/configuration objects.
$(TEST_BUILD)/regression_math: tests/regression_math.c $(TEST_BUILD)/obj/regression_math.o regression_math.h regression_types.h
	@mkdir -p $(@D)
	$(CC) $(TEST_CFLAGS) -I. $< $(TEST_BUILD)/obj/regression_math.o -lm -o $@

check: check-c check-python

check-c: $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do CANDLES_TEST_PYTHON='$(PYTHON)' "$$test"; done

check-python: $(TARGET)
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

check-sanitize:
	$(MAKE) TEST_BUILD=tests/bin/sanitize TEST_CFLAGS='-std=gnu11 -O1 -g -Wall -Wextra -UNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' check-c

.SECONDARY: $(TEST_OBJ)
.PHONY: clean check check-c check-python check-sanitize
