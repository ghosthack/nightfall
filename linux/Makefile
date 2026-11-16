CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS     = gtk4
VERSION ?= 0.0.0

build/nightfall: src/nightfall.c
	@mkdir -p build
	$(CC) $(CFLAGS) -DNIGHTFALL_VERSION='"$(VERSION)"' $(shell pkg-config --cflags $(PKGS)) -o $@ $< $(shell pkg-config --libs $(PKGS))
	strip $@

clean:
	rm -rf build
.PHONY: clean
