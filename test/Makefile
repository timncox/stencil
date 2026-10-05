# Native tests for the stencil engine. `make -C test` runs them.
CC      ?= cc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   := build

all: $(BUILD)/test_stencil
	./$(BUILD)/test_stencil

$(BUILD)/test_stencil: test_stencil.c ../core/stencil.c ../core/stencil.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../core -o $@ test_stencil.c ../core/stencil.c -lm

clean:
	rm -rf $(BUILD)

.PHONY: all clean
