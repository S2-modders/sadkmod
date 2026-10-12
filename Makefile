# sadkmod: static library + generated game declarations. Linux cross build with mingw-w64 (i686).
#   make            build/libsadkmod.a and build/include/sadkmod/game/...
#   make test       build the tests and run them under Wine (needs a DRM-free SADK.exe: SADK_EXE=<path>)
# A mod compiles with -I<sadkmod>/include -I<sadkmod>/build/include and links build/libsadkmod.a.
SADKMOD = .
include pch.mk
MAKEFLAGS += -j$(SADKMOD_JOBS)
CXX = i686-w64-mingw32-g++
CC = i686-w64-mingw32-gcc
AR = i686-w64-mingw32-ar
CXXFLAGS = $(SADKMOD_CXXFLAGS) -Iinclude -Ibuild/include -Ithird_party/minhook/include -Ithird_party/minhook/src
CFLAGS = -O2 -Ithird_party/minhook/include
SOURCEMAP = sourcemap
SADK_EXE ?= $(HOME)/sadk_game/bin/SADK.exe

SRC = $(wildcard src/*.cpp)
MINHOOK = third_party/minhook/src/hook.c third_party/minhook/src/buffer.c \
          third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde32.c
OBJ = $(SRC:src/%.cpp=build/obj/%.o) $(MINHOOK:third_party/minhook/src/%.c=build/obj/minhook/%.o)
GENERATED = build/include/.generated

all: build/libsadkmod.a $(SADKMOD_PCH_GCH)

build/macros.txt: gen/macro_headers.hpp
	@mkdir -p build
	$(CXX) -std=c++23 -dM -E -x c++ $< > $@

$(GENERATED): gen/gen_game_headers.py build/macros.txt $(wildcard $(SOURCEMAP)/*/*.json.gz)
	python3 gen/gen_game_headers.py $(SOURCEMAP) build/include --macros build/macros.txt
	touch $@

# The precompiled header (pch.mk): copied into build/pch/ so that its .gch sits next to it, where GCC looks.
$(SADKMOD_PCH): pch/sadkmod_pch.hpp
	@mkdir -p $(dir $@)
	cp $< $@

$(SADKMOD_PCH_GCH): $(SADKMOD_PCH) $(wildcard include/sadkmod/*.hpp) $(GENERATED)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) -x c++-header -o $@.tmp $< && mv $@.tmp $@

build/obj/%.o: src/%.cpp $(wildcard include/sadkmod/*.hpp) $(GENERATED) $(SADKMOD_PCH_GCH)
	@mkdir -p $(dir $@)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -c -o $@ $<

build/obj/minhook/%.o: third_party/minhook/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/libsadkmod.a: $(OBJ)
	rm -f $@
	$(AR) rcs $@ $^

TESTS = build/tests/test_sadkmod.exe
build/tests/%.exe: tests/%.cpp build/libsadkmod.a $(SADKMOD_PCH_GCH)
	@mkdir -p $(dir $@)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -static -o $@ $< build/libsadkmod.a -ladvapi32 -lshell32 -luser32

build/tests/test_mod.dll: tests/test_mod.cpp build/libsadkmod.a $(SADKMOD_PCH_GCH)
	@mkdir -p $(dir $@)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -shared -static-libgcc -static-libstdc++ -o $@ $< build/libsadkmod.a -ladvapi32 -lshell32 -luser32

# The host test runs from a scratch game folder: build/hostroot/bin/test_host.exe
build/tests/test_host.exe: tests/test_host.cpp build/libsadkmod.a $(SADKMOD_PCH_GCH)
	@mkdir -p $(dir $@)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -static -o $@ $< build/libsadkmod.a -ladvapi32 -lshell32 -luser32

test: $(TESTS) build/tests/test_host.exe build/tests/test_mod.dll
	WINEDEBUG=-all wine build/tests/test_sadkmod.exe "$(SADK_EXE)"
	rm -rf build/hostroot && mkdir -p build/hostroot/bin
	cp build/tests/test_host.exe build/tests/test_mod.dll build/hostroot/bin/
	WINEDEBUG=-all wine build/hostroot/bin/test_host.exe

clean:
	rm -rf build

.PHONY: all test clean
