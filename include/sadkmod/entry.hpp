// A mod's exports: SADKMOD_MAIN / SADKMOD_STOP. A header of its own because it reads SADKMOD_NO_ENTRY, which the
// precompiled header (pch.mk) must not see: GCC rejects a precompiled header when a macro it looked at is defined
// differently, and the verify build defines this one.
#pragma once
#include "mod.hpp"

// SADKMOD_MAIN(start, version, flags) defines the exports sadkmod_init (checks the interface, then calls `start`,
// bool()), sadkmod_version and sadkmod_flags. SADKMOD_STOP(stop) defines sadkmod_stop (`stop`, void()).
// SADKMOD_NO_ENTRY leaves them out, so several mods' sources can be linked into one test program.
#ifdef SADKMOD_NO_ENTRY
#define SADKMOD_MAIN(start_fn, mod_version, mod_flags)
#define SADKMOD_STOP(stop)
#else
#define SADKMOD_MAIN(start_fn, mod_version, mod_flags)                                           \
    extern "C" __declspec(dllexport) bool sadkmod_init(const sadkmod_api *api)                   \
    {                                                                                            \
        if (!api || api->version < SADKMOD_API_VERSION || api->size < sizeof(sadkmod_api))       \
            return false;                                                                        \
        ::sadk::bind_host(api);                                                                  \
        return start_fn();                                                                       \
    }                                                                                            \
    extern "C" __declspec(dllexport) std::uint32_t sadkmod_version() { return (mod_version); }   \
    extern "C" __declspec(dllexport) std::uint32_t sadkmod_flags() { return (mod_flags); }
#define SADKMOD_STOP(stop_fn) \
    extern "C" __declspec(dllexport) void sadkmod_stop() { stop_fn(); }
#endif
