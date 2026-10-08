// The precompiled header for code on sadkmod (pch.mk): the generated game declarations, sadkmod's own headers and the
// common system headers. Parsing the game declarations takes about 10 s per source file; from the precompiled header,
// under one. Everything here must not depend on a macro a project defines differently (entry.hpp stays out).
#pragma once
#include <sadkmod/calls.hpp>
#include <sadkmod/core.hpp>
#include <sadkmod/events.hpp>
#include <sadkmod/hook.hpp>
#include <sadkmod/host.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/mods.hpp>
#include <sadkmod/msvc.hpp>
#include <sadkmod/patch.hpp>
#include <sadkmod/registry.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/ui.hpp>
#include <sadkmod/verify.hpp>
#include <sadkmod/game/sadk_noav/all.hpp>

#include <winsock2.h>   // before windows.h, as the shim and gamebridge need
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
