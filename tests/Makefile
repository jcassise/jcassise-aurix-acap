# Host-side tests for the hardware-independent modules (alignment, image ops, matching).
SRC = ../app/src
CFLAGS += -O2 -std=gnu11 -Wall -Wextra -I$(SRC)

test_core: test_core.c $(SRC)/align.c $(SRC)/image.c $(SRC)/match.c $(SRC)/yunet.c $(SRC)/tensor.c $(SRC)/embed_meta.c $(SRC)/sysinfo.c $(SRC)/capacity.c $(SRC)/jpeg.c $(SRC)/tracker.c $(SRC)/access.c $(SRC)/snapshot.c
	$(CC) $(CFLAGS) $^ $(shell pkg-config --cflags --libs glib-2.0 jansson) -lm -o $@

run: test_core
	./test_core

clean:
	rm -f test_core

.PHONY: run clean
