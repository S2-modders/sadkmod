# wsock32.dll (the shim: proxy and mod host), C++ on sadkmod. Linux cross build with mingw-w64 (i686).
#   make          wsock32.dll (builds sadkmod first)
#   make test     loads it into a test program under Wine: forwarding, inactive outside SADK.exe
CXX = i686-w64-mingw32-g++
SADKMOD = ../../sadkmod
CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I. -I$(SADKMOD)/include -I$(SADKMOD)/build/include
LIBS = $(SADKMOD)/build/libsadkmod.a -lkernel32 -ladvapi32 -lshell32 -luser32
OBJ = build/main.o

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

build/load_shim.exe: tests/load_shim.cpp
	@mkdir -p build
	$(CXX) -O2 -static -o $@ $< -lws2_32

test: wsock32.dll build/load_shim.exe
	cp wsock32.dll build/shim_under_test.dll   # as wsock32.dll Wine would load its own instead
	cd build && rm -f wsock32_shim.txt && WINEDEBUG=-all wine load_shim.exe shim_under_test.dll

clean:
	rm -rf build wsock32.dll thunks.S wsock32.def ordinals.h

FORCE:

.PHONY: FORCE test clean
