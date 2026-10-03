# Step-1 protocol scenarios: the real AURIX Pharos client against sim_lite (contract-validated).
#   apt: libcurl4-openssl-dev libjansson-dev libssl-dev    pip: jsonschema cryptography pillow
SRC = ../../app/src
PKGS = libcurl jansson openssl

pharos_harness: harness.c $(SRC)/pharos.c $(SRC)/pharos_http.c $(SRC)/pharos_config.c $(SRC)/sync.c $(SRC)/person_store.c $(SRC)/jpeg.c $(SRC)/image.c
	$(CC) -std=gnu11 -Wall -Wextra -O1 -I$(SRC) $^ $(shell pkg-config --cflags --libs $(PKGS)) -lpthread -lm -o $@

run: pharos_harness
	AURIX_SYNC_JITTER_MS=0 python3 run_scenarios.py ./pharos_harness
	AURIX_SYNC_JITTER_MS=0 python3 sync_scenarios.py ./pharos_harness

clean:
	rm -f pharos_harness

.PHONY: run clean
