# p3m - Parallel POSIX Permission Manager
# Top-level Makefile: builds all tools into ./bin

CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread

BIN     := bin
SRC     := src
OBJ     := obj

# Tools are added here as they are implemented
TOOLS := $(BIN)/p3m-ls $(BIN)/p3m-ch $(BIN)/p3m-rm $(BIN)/p3m-du $(BIN)/p3m-cp $(BIN)/p3m-mv $(BIN)/p3m-find $(BIN)/p3m-diff $(BIN)/p3m-stats

# Shared engine linked into every tool
CORE := $(OBJ)/p3mcore.o

.PHONY: all clean

all: $(TOOLS)

$(BIN) $(OBJ):
	mkdir -p $@

$(OBJ)/p3mcore.o: $(SRC)/p3mcore.c $(SRC)/p3mcore.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN)/%: $(SRC)/%.c $(CORE) $(SRC)/p3mcore.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(CORE) $(LDFLAGS)

clean:
	rm -rf $(BIN) $(OBJ)
