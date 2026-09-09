CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config
CFLAGS += $(shell $(PKG_CONFIG) --cflags x11 cairo-xlib gtk+-3.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs x11 cairo-xlib gtk+-3.0) -lm

.PHONY: all clean run demo

all: codex-pet

codex-pet: main.c config.c settings.c config.h settings.h
	$(CC) $(CFLAGS) -o $@ main.c config.c settings.c $(LDLIBS)

run: codex-pet
	./codex-pet

demo: codex-pet
	./codex-pet --demo

clean:
	rm -f codex-pet
