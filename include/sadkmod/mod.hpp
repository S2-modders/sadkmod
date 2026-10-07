// The mod interface: what a mod.dll exports and what the host (the shim) gives it.
//
// A mod is a folder <game>\mods\<name>\ with
//   data\...   files that replace (or add to) the game's data\... files, plain (not encrypted), same paths
//   mod.dll    optional code; helper DLLs it needs may sit next to it
// Folders are processed in name order (case-insensitive); for the same data file the later folder wins. A folder
// whose name starts with '_' or '.' is skipped (a quick way to switch a mod off).
//
// mod.dll exports one function, written with SADKMOD_MAIN:
//
//     #include <sadkmod/sadkmod.hpp>
//     static bool start() {                    // runs once, before the game's own start-up code
//         sadk::log("hello from %s", sadk::mod_name());
//         return Load::install(my_detour, "texture loads");   // hooks go through the host's shared registry
//     }
//     SADKMOD_MAIN(start)
//
// The host calls it on the game's main thread before SADK.exe's entry point runs, only on the supported (DRM-free)
// build. The game is not initialised yet: install hooks and patches here, do the real work from them later.
// sadkmod's log() and Hook<> / hook_function() in a mod are routed to the host: one log file, and one hook
// registry in which several mods may hook the same function (the most recently loaded mod runs first and reaches
// the others through its `original`).
#pragma once
#include <cstddef>
#include <cstdint>

#define SADKMOD_API_VERSION 1

extern "C" {

struct sadkmod_api {
    std::uint32_t version;     // SADKMOD_API_VERSION of the host
    std::uint32_t size;        // sizeof(sadkmod_api) of the host; fields are only ever appended
    const char *mod_name;      // the mod's folder name
    const char *mod_dir;       // <game>\mods\<name> (no trailing backslash)
    const char *game_root;     // <game>
    void (*log_line)(const char *mod, const char *text);
    bool (*hook)(const char *mod, void *target, void *detour, void **original, const char *what);
};

typedef bool (*sadkmod_init_fn)(const sadkmod_api *api);
}

namespace sadk {

// In a mod: binds sadkmod's routing to the host (done by SADKMOD_MAIN). nullptr in the host itself.
void bind_host(const sadkmod_api *api);
const sadkmod_api *host_api();
const char *mod_name();   // "" outside a mod
const char *mod_dir();    // "" outside a mod

}  // namespace sadk

// Defines the exported sadkmod_init(api) that checks the interface version and calls `start` (bool()).
// SADKMOD_NO_ENTRY leaves it out, so several mods' sources can be linked into one test program.
#ifdef SADKMOD_NO_ENTRY
#define SADKMOD_MAIN(start)
#else
#define SADKMOD_MAIN(start)                                                                      \
    extern "C" __declspec(dllexport) bool sadkmod_init(const sadkmod_api *api)                   \
    {                                                                                            \
        if (!api || api->version != SADKMOD_API_VERSION || api->size < sizeof(sadkmod_api))      \
            return false;                                                                        \
        ::sadk::bind_host(api);                                                                  \
        return start();                                                                          \
    }
#endif
