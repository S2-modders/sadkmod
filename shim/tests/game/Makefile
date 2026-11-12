# What ../game_crash_test.sh needs: the crashtest mod (build/crashtest/mod.dll) and a stand-in PhysXLoader.dll
# (physx_stub.c). Not a mod to install.
SADKMOD ?= ../../..
include $(SADKMOD)/mods.mk

all: build/PhysXLoader.dll

build/PhysXLoader.dll: physx_stub.c
	@mkdir -p build
	i686-w64-mingw32-gcc -O2 -shared -s -o $@ $< -Wl,--kill-at
