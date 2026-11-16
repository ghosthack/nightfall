CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
CFLAGS  = -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00
LDFLAGS = -mwindows -municode -s -static-libgcc
LIBS    = -lshell32 -luser32 -lgdi32 -ladvapi32

build/nightfall.exe: src/nightfall.c src/resource.h build/res.o
	$(CC) $(CFLAGS) -o $@ src/nightfall.c build/res.o $(LDFLAGS) $(LIBS)

build/res.o: res/nightfall.rc res/nightfall.ico res/nightfall.manifest src/resource.h
	@mkdir -p build
	$(WINDRES) res/nightfall.rc -O coff -o $@

res/nightfall.ico: tools/make_icon.py
	python3 tools/make_icon.py $@

clean:
	rm -rf build
.PHONY: clean
