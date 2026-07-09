# p3m - Parallel POSIX Permission Manager
# Top-level Makefile: builds all tools into ./bin

CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread

BIN     := bin
SRC     := src

# Tools are added here as they are implemented, e.g.:
# TOOLS := $(BIN)/p3m-chmod
TOOLS :=

.PHONY: all clean

all: $(TOOLS)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/%: $(SRC)/%.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(BIN)
