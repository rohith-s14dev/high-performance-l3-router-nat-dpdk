APP = dpdk_router

CC ?= gcc
PKG_CONFIG ?= pkg-config

CFLAGS += -O2 -Wall -Wextra -Iinclude $(shell $(PKG_CONFIG) --cflags libdpdk)
LDFLAGS += $(shell $(PKG_CONFIG) --libs libdpdk)

SRC = src/main.c src/port_init.c src/parser.c src/routing.c src/nat.c src/worker.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean

all: $(APP)

$(APP): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP)
