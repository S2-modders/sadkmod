# The precompiled header shared by everything built on sadkmod (sadkmod itself, the shim, the mods). Include this
# file after setting SADKMOD (the path to sadkmod), compile with $(SADKMOD_CXXFLAGS) plus your own -I and -D, add
# $(SADKMOD_PCH_FLAGS) and let the objects depend on $(SADKMOD_PCH_GCH). GCC uses a precompiled header only when the
# code-generating flags match the ones it was built with, hence the shared flags; -Winvalid-pch says when it does not.
# The .gch lives in build/ (never committed); sadkmod's make builds it after the game declarations are generated.
SADKMOD_CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -fno-exceptions -fno-rtti
SADKMOD_PCH = $(SADKMOD)/build/pch/sadkmod_pch.hpp
SADKMOD_PCH_GCH = $(SADKMOD_PCH).gch
SADKMOD_PCH_FLAGS = -include $(SADKMOD_PCH) -Winvalid-pch

# Memory. A normal compile peaks at about 0.9 GB, generating the header at about 1.6 GB [measured 2026-10-09]. Every
# compiler process runs under SADKMOD_LIMIT (4 GB of address space), so a runaway one fails with "out of memory"
# instead of taking the machine down: a damaged .gch once made each compile grow past 14 GB and the 16 GB machine
# thrashed. The .gch is written to a temporary file and renamed when complete, so a killed build cannot leave a damaged
# one. Parallel jobs: one per 3 GB available (MemAvailable), at most the core count.
SADKMOD_LIMIT = ulimit -v 4000000;
SADKMOD_JOBS := $(shell awk -v cores=$$(nproc) '/MemAvailable/ { j = int($$2 / 3000000); if (j < 1) j = 1; if (j > cores) j = cores; print j }' /proc/meminfo 2>/dev/null || echo 1)
