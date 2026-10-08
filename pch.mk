# The precompiled header shared by everything built on sadkmod (sadkmod itself, the shim, the mods). Include this
# file after setting SADKMOD (the path to sadkmod), compile with $(SADKMOD_CXXFLAGS) plus your own -I and -D, add
# $(SADKMOD_PCH_FLAGS) and let the objects depend on $(SADKMOD_PCH_GCH). GCC uses a precompiled header only when the
# code-generating flags match the ones it was built with, hence the shared flags; -Winvalid-pch says when it does not.
# The .gch lives in build/ (never committed); sadkmod's make builds it after the game declarations are generated.
SADKMOD_CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -fno-exceptions -fno-rtti
SADKMOD_PCH = $(SADKMOD)/build/pch/sadkmod_pch.hpp
SADKMOD_PCH_GCH = $(SADKMOD_PCH).gch
SADKMOD_PCH_FLAGS = -include $(SADKMOD_PCH) -Winvalid-pch
