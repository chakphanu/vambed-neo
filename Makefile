# SPDX-License-Identifier: GPL-3.0-or-later
# Needs mbelib-neo (libmbe-neo.pc) and codec2 (codec2.pc) visible to pkg-config.

PKG_CONFIG ?= pkg-config
PKGS       := libmbe-neo codec2
PREFIX     ?= /usr/local

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

SRC := src/vambed_neo.c src/vn_codec.c
OBJ := $(SRC:src/%.c=build/obj/%.o)

all: build/vambed-neo

build/vambed-neo: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/obj/%.o: src/%.c src/vn_codec.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: build/vambed-neo
	install -D -m 0755 build/vambed-neo $(DESTDIR)$(PREFIX)/bin/vambed-neo

clean:
	rm -f build/vambed-neo $(OBJ)

.PHONY: all install clean
