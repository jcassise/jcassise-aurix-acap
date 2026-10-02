# Host-side tests for the hardware-independent modules (alignment, image ops, matching).
SRC = ../app/src
CFLAGS += -O2 -std=gnu11 -Wall -Wextra -I$(SRC)

test_core: test_core.c $(SRC)/align.c $(SRC)/image.c $(SRC)/match.c $(SRC)/yunet.c $(SRC)/tensor.c
	$(CC) $(CFLAGS) $^ -lm -o $@

run: test_core
	./test_core

clean:
	rm -f test_core

.PHONY: run clean
