CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

BIN := raw-fuse-parent-worker-demo
MOUNTPOINT ?= /tmp/fuse-clone-demo

.PHONY: all build run clean

all: build

build:
	$(CC) $(CFLAGS) -o $(BIN) main.c

run: build
	mkdir -p "$(MOUNTPOINT)"
	sudo ./$(BIN) "$(MOUNTPOINT)"

clean:
	rm -f $(BIN)
