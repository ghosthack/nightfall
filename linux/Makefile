CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS     = gtk4

build/nightfall: src/nightfall.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKGS)) -o $@ $< $(shell pkg-config --libs $(PKGS))
	strip $@

clean:
	rm -rf build
.PHONY: clean
