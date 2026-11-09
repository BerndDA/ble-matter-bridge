# Host unit tests for the BTHome parser (no Zephyr, no DK needed).
#   make        build and run
#   make clean

SRC_DIR := ../../../src/sensors
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I$(SRC_DIR)

BIN := build/test_bthome

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): test_bthome.c $(SRC_DIR)/bthome.c $(SRC_DIR)/bthome.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_bthome.c $(SRC_DIR)/bthome.c

clean:
	rm -rf build
