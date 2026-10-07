# sadkmod: static library + generated game declarations. Linux cross build with mingw-w64 (i686).
#   make            build/libsadkmod.a and build/include/sadkmod/game/...
#   make test       build the tests and run them under Wine (needs a DRM-free SADK.exe: SADK_EXE=<path>)
# A mod compiles with -I<sadkmod>/include -I<sadkmod>/build/include and links build/libsadkmod.a.
CXX = i686-w64-mingw32-g++
CC = i686-w64-mingw32-gcc
AR = i686-w64-mingw32-ar
CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -Iinclude -Ibuild/include -Ithird_party/minhook/include
CFLAGS = -O2 -Ithird_party/minhook/include
SOURCEMAP = ../sourcemap
SADK_EXE ?= $(HOME)/sadk_game/bin/SADK.exe

SRC = $(wildcard src/*.cpp)
MINHOOK = third_party/minhook/src/hook.c third_party/minhook/src/buffer.c \
          third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde32.c
OBJ = $(SRC:src/%.cpp=build/obj/%.o) $(MINHOOK:third_party/minhook/src/%.c=build/obj/minhook/%.o)
GENERATED = build/include/.generated

all: build/libsadkmod.a

build/macros.txt: gen/macro_headers.hpp
	@mkdir -p build
	$(CXX) -std=c++23 -dM -E -x c++ $< > $@

$(GENERATED): gen/gen_game_headers.py build/macros.txt $(wildcard $(SOURCEMAP)/*/*.json.gz)
	python3 gen/gen_game_headers.py $(SOURCEMAP) build/include --macros build/macros.txt
	touch $@

build/obj/%.o: src/%.cpp $(wildcard include/sadkmod/*.hpp) $(GENERATED)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/obj/minhook/%.o: third_party/minhook/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/libsadkmod.a: $(OBJ)
	rm -f $@
	$(AR) rcs $@ $^

TESTS = build/tests/test_sadkmod.exe
build/tests/%.exe: tests/%.cpp build/libsadkmod.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -static -o $@ $< build/libsadkmod.a -ladvapi32

test: $(TESTS)
	WINEDEBUG=-all wine build/tests/test_sadkmod.exe "$(SADK_EXE)"

clean:
	rm -rf build

.PHONY: all test clean
