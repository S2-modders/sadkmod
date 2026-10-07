# wsock32.dll (the bridge shim), C++ on sadkmod. Linux cross build with mingw-w64 (i686).
#   make          wsock32.dll (builds sadkmod first)
#   make verify   checks every patch's expected bytes against a DRM-free SADK.exe under Wine (SADK_EXE=<path>)
CXX = i686-w64-mingw32-g++
SADKMOD = ../../sadkmod
CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I. -I$(SADKMOD)/include -I$(SADKMOD)/build/include
LIBS = $(SADKMOD)/build/libsadkmod.a -lkernel32 -ladvapi32 -lshell32 -luser32
SADK_EXE ?= $(HOME)/sadk_game/bin/SADK.exe

PARTS = config bridge mapshare
OBJ = $(PARTS:%=build/%.o) build/main.o

LIB = $(SADKMOD)/build/libsadkmod.a
GENERATED = $(SADKMOD)/build/include/.generated

wsock32.dll: $(OBJ) thunks.S wsock32.def $(LIB)
	$(CXX) -O2 -shared -s -static-libgcc -static-libstdc++ -o $@ $(OBJ) thunks.S wsock32.def $(LIBS)

# sadkmod's own make decides what is out of date; the shim relinks only when the library file changed
$(LIB) $(GENERATED): FORCE
	$(MAKE) -C $(SADKMOD)

build/%.o: %.cpp shim.hpp ordinals.h $(GENERATED)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

thunks.S wsock32.def ordinals.h &: exports.txt gen.py
	python3 gen.py

build/verify_patches.exe: tests/verify_patches.cpp $(PARTS:%=build/%.o) $(LIB)
	$(CXX) $(CXXFLAGS) -static -o $@ $< $(PARTS:%=build/%.o) $(LIBS) -lws2_32

verify: build/verify_patches.exe
	WINEDEBUG=-all wine build/verify_patches.exe "$(SADK_EXE)"

clean:
	rm -rf build wsock32.dll thunks.S wsock32.def ordinals.h

FORCE:

.PHONY: FORCE verify clean
