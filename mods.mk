# Builds a folder of mods on sadkmod: each mod is a folder next to the including Makefile and is built into
# build/<name>/, the folder to copy to <game>\mods\<name>\. The including Makefile sets SADKMOD (the path to
# sadkmod) and includes this file:
#   SADKMOD ?= ../sadkmod
#   include $(SADKMOD)/mods.mk
#   make          every mod (builds sadkmod first)
#   make verify   checks every mod's patches against a DRM-free SADK.exe under Wine (SADK_EXE=<path>); needs
#                 tests/verify_mods.cpp, which lists the mods' start functions
# A mod is a folder with one or more .cpp files (and optionally files to ship next to mod.dll, e.g. .ini,
# and a data/ folder that is copied as is). A folder without .cpp files is a data-only mod: its data/ folder and
# its flag files (.client / .server / .lobby) are copied. The folders tests/ and build/ are not mods.
CXX = i686-w64-mingw32-g++
include $(SADKMOD)/pch.mk
MAKEFLAGS += -j$(SADKMOD_JOBS)
CXXFLAGS = $(SADKMOD_CXXFLAGS) -I$(SADKMOD)/include -I$(SADKMOD)/build/include
LIB = $(SADKMOD)/build/libsadkmod.a
GENERATED = $(SADKMOD)/build/include/.generated
LIBS = $(LIB) -lkernel32 -ladvapi32 -lshell32 -luser32 -lgdi32 -lws2_32
SADK_EXE ?= $(HOME)/sadk_game/bin/SADK.exe

MODS = $(patsubst %/,%,$(filter-out tests/ build/,$(wildcard */)))
CODE_MODS = $(foreach m,$(MODS),$(if $(wildcard $(m)/*.cpp),$(m)))
DATA_MODS = $(filter-out $(CODE_MODS),$(MODS))
DLLS = $(CODE_MODS:%=build/%/mod.dll)
SOURCES = $(foreach m,$(CODE_MODS),$(wildcard $(m)/*.cpp))
# Each source once for the mod.dll and once for verify_mods.exe (SADKMOD_NO_ENTRY: no exports, entry.hpp), both from
# sadkmod's precompiled header; -MMD keeps track of the headers each one includes.
OBJ_DIR = build/obj
VERIFY_OBJ_DIR = build/obj-verify

all: $(DLLS) $(DATA_MODS:%=data-%)

# A mod without code: its data folder and its flag files (.client / .server / .lobby name what it changes).
data-%: FORCE
	@rm -rf build/$* && mkdir -p build/$*
	@if [ -d $*/data ]; then cp -r $*/data build/$*/; fi
	@for f in $*/.client $*/.server $*/.lobby; do if [ -e $$f ]; then cp $$f build/$*/; fi; done

$(LIB) $(GENERATED) $(SADKMOD_PCH_GCH): FORCE
	$(MAKE) -C $(SADKMOD)

$(OBJ_DIR)/%.o: %.cpp $(SADKMOD_PCH_GCH)
	@mkdir -p $(dir $@)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -MMD -MP -c -o $@ $<

$(VERIFY_OBJ_DIR)/%.o: %.cpp $(SADKMOD_PCH_GCH)
	@mkdir -p $(dir $@)
	$(SADKMOD_LIMIT) $(CXX) $(CXXFLAGS) $(SADKMOD_PCH_FLAGS) -DSADKMOD_NO_ENTRY -MMD -MP -c -o $@ $<

-include $(SOURCES:%.cpp=$(OBJ_DIR)/%.d) $(SOURCES:%.cpp=$(VERIFY_OBJ_DIR)/%.d)

.SECONDEXPANSION:
# (no % inside the functions: in a pattern rule make puts the stem there before they run)
build/%/mod.dll: $$(addprefix $(OBJ_DIR)/,$$(addsuffix .o,$$(basename $$(wildcard $$*/*.cpp)))) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) -shared -s -static-libgcc -static-libstdc++ -o $@ $(filter %.o,$^) $(LIBS)
	@i686-w64-mingw32-objdump -p $@ | grep -q sadkmod_init || { echo "$@ exports no sadkmod_init"; rm -f $@; exit 1; }
	@for f in $(wildcard $*/*.ini $*/*.txt); do cp $$f $(dir $@); done
	@rm -rf $(dir $@)data
	@if [ -d $*/data ]; then cp -r $*/data $(dir $@); fi

build/verify_mods.exe: $(VERIFY_OBJ_DIR)/tests/verify_mods.o $(SOURCES:%.cpp=$(VERIFY_OBJ_DIR)/%.o) $(LIB)
	@mkdir -p build
	$(CXX) -static -o $@ $(filter %.o,$^) $(LIBS)

verify: build/verify_mods.exe
	WINEDEBUG=-all wine build/verify_mods.exe "$(SADK_EXE)"

clean:
	rm -rf build

FORCE:

.PHONY: all verify clean FORCE
