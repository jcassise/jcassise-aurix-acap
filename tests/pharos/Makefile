# Step-1 protocol scenarios: the real AURIX Pharos client against sim_lite (contract-validated).
#   apt: libcurl4-openssl-dev libjansson-dev libssl-dev    pip: jsonschema cryptography
SRC = ../../app/src
PKGS = libcurl jansson openssl

pharos_harness: harness.c $(SRC)/pharos.c $(SRC)/pharos_http.c $(SRC)/pharos_config.c
	$(CC) -std=gnu11 -Wall -Wextra -O1 -I$(SRC) $^ $(shell pkg-config --cflags --libs $(PKGS)) -lpthread -o $@

run: pharos_harness
	python3 run_scenarios.py ./pharos_harness

clean:
	rm -f pharos_harness

.PHONY: run clean
