# Step-1 protocol scenarios: the real AURIX Pharos client against sim_lite (contract-validated).
#   apt: libcurl4-openssl-dev libjansson-dev libssl-dev    pip: jsonschema cryptography pillow
SRC = ../../app/src
PKGS = libcurl jansson openssl glib-2.0

pharos_harness: harness.c $(SRC)/pharos.c $(SRC)/pharos_http.c $(SRC)/pharos_config.c $(SRC)/sync.c $(SRC)/person_store.c $(SRC)/jpeg.c $(SRC)/image.c $(SRC)/tracker.c $(SRC)/events.c $(SRC)/event_queue.c $(SRC)/access.c
	$(CC) -std=gnu11 -Wall -Wextra -O1 -I$(SRC) $^ $(shell pkg-config --cflags --libs $(PKGS)) -lpthread -lm -o $@

run: pharos_harness
	AURIX_SYNC_JITTER_MS=0 python3 run_scenarios.py ./pharos_harness
	AURIX_SYNC_JITTER_MS=0 python3 sync_scenarios.py ./pharos_harness
	AURIX_SYNC_JITTER_MS=0 python3 event_scenarios.py ./pharos_harness

clean:
	rm -f pharos_harness pharos_harness_asan; rm -rf asan-logs

.PHONY: run clean

# The same three suites against an AddressSanitizer/UBSan build; any sanitizer report fails the run.
# (Memory bugs often look fine in a normal build - see 0.8.2's use-after-free.)
asan:
	$(CC) -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -I$(SRC) harness.c \
	  $(SRC)/pharos.c $(SRC)/pharos_http.c $(SRC)/pharos_config.c $(SRC)/sync.c $(SRC)/person_store.c $(SRC)/jpeg.c \
	  $(SRC)/image.c $(SRC)/tracker.c $(SRC)/events.c $(SRC)/event_queue.c $(SRC)/access.c \
	  $(shell pkg-config --cflags --libs $(PKGS)) -lpthread -lm -o pharos_harness_asan
	rm -rf asan-logs && mkdir asan-logs
	for s in run_scenarios sync_scenarios event_scenarios; do \
	  ASAN_OPTIONS=detect_leaks=1:log_path=asan-logs/$$s UBSAN_OPTIONS=print_stacktrace=1:log_path=asan-logs/u$$s \
	  AURIX_SYNC_JITTER_MS=0 python3 $$s.py ./pharos_harness_asan || exit 1; done
	@if [ -n "$$(ls asan-logs)" ]; then cat asan-logs/* | head -60; echo "sanitizer reports above"; exit 1; fi
	@echo "sanitizers: no reports"
