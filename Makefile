CC      = cc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra
TARGET  = candles
SRC     = main.c terminal.c settings.c state.c stock.c data.c indicators.c chart.c menu.c grabber.c live.c regression.c residuals.c swing.c
OBJ     = $(SRC:.c=.o)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lm

%.o: %.c $(wildcard *.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: clean
