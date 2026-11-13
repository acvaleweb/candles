CC      = cc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra
TARGET  = candles
SRC     = main.c terminal.c settings.c state.c stock.c data.c indicators.c chart.c menu.c grabber.c live.c regression.c regression_math.c residuals.c swing.c
OBJ     = $(SRC:.c=.o)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lm

%.o: %.c $(wildcard *.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
	rm -f tests/model_selection tests/regression_math

check: $(OBJ)
	$(CC) $(CFLAGS) -I. tests/regression_math.c regression_math.o -lm -o tests/regression_math
	./tests/regression_math
	$(CC) $(CFLAGS) -I. tests/model_selection.c $(filter-out main.o regression.o,$(OBJ)) -lm -o tests/model_selection
	./tests/model_selection

.PHONY: clean check
