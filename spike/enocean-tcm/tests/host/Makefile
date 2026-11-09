# Host unit tests for the ESP3 and EEP parsers (no Zephyr, no DK needed).
#   make        build and run
#   make clean

SRC_DIR := ../../src
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I$(SRC_DIR)

BIN := build/test_enocean
SRCS := $(SRC_DIR)/esp3.c $(SRC_DIR)/eep.c

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): test_enocean.c $(SRCS) $(SRC_DIR)/esp3.h $(SRC_DIR)/eep.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_enocean.c $(SRCS)

clean:
	rm -rf build
