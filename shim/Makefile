CC = i686-w64-mingw32-gcc

wsock32.dll: shim.c thunks.S wsock32.def ordinals.h
	$(CC) -O2 -shared -s -static-libgcc -o $@ shim.c thunks.S wsock32.def -lkernel32

thunks.S wsock32.def ordinals.h: exports.txt gen.py
	python3 gen.py

clean:
	rm -f wsock32.dll thunks.S wsock32.def ordinals.h
