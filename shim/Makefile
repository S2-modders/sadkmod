# wsock32.dll (the shim: proxy, mod host, crash report), C++ on sadkmod. Linux cross build with mingw-w64 (i686).
#   make          wsock32.dll (builds sadkmod first)
#   make test     loads it into a test program under Wine: forwarding, inactive outside SADK.exe; then the crash
#                 report outside the game (tests/crash_test.cpp)
#   make test-game   the crash report in the real game under Wine (GAME=<install>, default ~/sadk_game)
CXX = i686-w64-mingw32-g++
CC = i686-w64-mingw32-gcc
SADKMOD = ..
include $(SADKMOD)/pch.mk
MINIZ = $(SADKMOD)/third_party/miniz
MINIZ_FLAGS = -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
CXXFLAGS = $(SADKMOD_CXXFLAGS) -I. -I$(SADKMOD)/include -I$(SADKMOD)/build/include -I$(MINIZ) $(MINIZ_FLAGS)
LIBS = $(SADKMOD)/build/libsadkmod.a -lkernel32 -ladvapi32 -lshell32 -luser32 -lpsapi
CRASH_OBJ = build/crash.o build/crash_report.o build/symbols.o build/symbols_blob.o build/miniz.o
OBJ = build/main.o $(CRASH_OBJ)

LIB = $(SADKMOD)/build/libsadkmod.a
GENERATED = $(SADKMOD)/build/include/.generated

wsock32.dll: $(OBJ) thunks.S wsock32.def $(LIB)
	$(CXX) -O2 -shared -s -static-libgcc -static-libstdc++ -o $@ $(OBJ) thunks.S wsock32.def $(LIBS)

# sadkmod's own make decides what is out of date; the shim relinks only when the library file changed
$(LIB) $(GENERATED) $(SADKMOD_PCH_GCH) &: FORCE
	$(MAKE) -C $(SADKMOD)

build/%.o: %.cpp shim.hpp crash.hpp ordinals.h $(GENERATED) $(SADKMOD_PCH_GCH)
	@mkdir -p build
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -c -o $@ $<

# The game's function names for the crash report, from the Ghidra map.
build/symbols.bin: gen_symbols.py $(wildcard $(SADKMOD)/sourcemap/*/sourcemap.json.gz)
	@mkdir -p build
	python3 gen_symbols.py $(SADKMOD)/sourcemap $@

build/symbols_blob.o: symbols.S build/symbols.bin
	$(CXX) -c -o $@ symbols.S

build/miniz.o: $(MINIZ)/miniz.c $(MINIZ)/miniz.h
	@mkdir -p build
	$(CC) -O2 $(MINIZ_FLAGS) -w -c -o $@ $<

thunks.S wsock32.def ordinals.h &: exports.txt gen.py
	python3 gen.py

build/load_shim.exe: tests/load_shim.cpp
	@mkdir -p build
	$(CXX) -O2 -static -o $@ $< -lws2_32

# The crash report outside the game: a test program with the report linked in and a test mod in its mods folder.
build/crash_test.exe: tests/crash_test.cpp $(CRASH_OBJ) $(LIB)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -static -o $@ $< $(CRASH_OBJ) $(LIBS)

build/crash_mod.dll: tests/crash_mod.cpp
	$(CXX) -O2 -fno-omit-frame-pointer -shared -static-libgcc -o $@ $<

test: wsock32.dll build/load_shim.exe build/crash_test.exe build/crash_mod.dll
	cp wsock32.dll build/shim_under_test.dll   # as wsock32.dll Wine would load its own instead
	cd build && rm -f wsock32_shim.txt && WINEDEBUG=-all wine load_shim.exe shim_under_test.dll
	sh tests/crash_test.sh

# The crash report in the real game: SADK.exe (DRM-free) from GAME crashed in every way tests/game/crashtest knows.
GAME ?= $(HOME)/sadk_game
test-game: wsock32.dll
	$(MAKE) -C tests/game
	sh tests/game_crash_test.sh "$(GAME)"

clean:
	rm -rf build wsock32.dll thunks.S wsock32.def ordinals.h tests/game/build

FORCE:

.PHONY: FORCE test test-game clean
