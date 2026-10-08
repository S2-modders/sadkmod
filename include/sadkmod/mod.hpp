// The mod interface: what a mod.dll exports and what the host (the shim) gives it.
//
// A mod is a folder <game>\mods\<name>\ with
//   data\...   files that replace (or add to) the game's data\... files, plain (not encrypted), same paths
//   mod.dll    optional code; helper DLLs it needs may sit next to it
//   .client, .server, .lobby   (only for a mod without mod.dll) empty files naming what the mod changes (flags below)
// Folders are processed in name order (case-insensitive). Two mods replacing the same data file, or patching the
// same bytes, are a conflict: the game shows which and closes (registry.hpp). A folder whose name starts with '_' or
// '.' is skipped (a quick way to switch a mod off).
//
// What a mod changes, as flags that combine (a mod without any is not loaded):
//   SADKMOD_CLIENT   only this player's game (looks, interface)
//   SADKMOD_SERVER   what happens in a match: every player of a match needs the same server mods
//   SADKMOD_LOBBY    the lobby (village): the lobby server may require it of everyone
// Every installed mod is loaded at start-up. Mods can also be loaded and unloaded later (host.hpp).
//
// mod.dll exports, written with the macros below:
//
//     #include <sadkmod/sadkmod.hpp>
//     static bool start() {                    // a client mod: once, before the game's own start-up code
//         sadk::log("hello from %s", sadk::mod_name());
//         return Load::install(my_detour, "texture loads");   // hooks go through the host's shared registry
//     }
//     static void stop() { ... }               // optional: before the mod is unloaded
//     SADKMOD_MAIN(start, 3, SADKMOD_CLIENT)   // version 3; flags
//     SADKMOD_STOP(stop)
//
//   sadkmod_init(api)   start: install hooks and patches. At start-up it runs on the game's main thread before
//                       SADK.exe's entry point (the game is not initialised yet: do the real work from the hooks
//                       later).
//   sadkmod_version()   the mod's own version, an integer; with the content hash it tells copies apart
//   sadkmod_flags()     SADKMOD_CLIENT | SADKMOD_SERVER | SADKMOD_LOBBY, what the mod changes
//   sadkmod_stop()      optional, before the mod is unloaded: stop its threads, take back whatever it gave the
//                       game (callbacks, buffers the game still reads). The host removes the mod's hooks, patches
//                       and table slots itself afterwards.
// sadkmod_version and sadkmod_flags must be plain functions without side effects: the host calls them to sort the
// mods before any mod starts. A mod.dll without them is not used.
//
// sadkmod's log(), Hook<> / hook_function(), patch() and hook_slot() in a mod are routed to the host: one log
// file, and one registry in which several mods may hook the same function. They run in load order (folder-name
// order, whenever each one installs its hook): the first mod's detour runs first and reaches the next through its
// `original`; the host's own hooks are always last, next to the game's code. The registry records which mod made each change, so that
// unloading a mod takes back exactly its own. Changes made around it (write_memory, own MinHook) cannot be taken
// back: a mod that should be unloadable uses only the routed functions.
#pragma once
#include <cstddef>
#include <cstdint>

#define SADKMOD_API_VERSION 2

#define SADKMOD_CLIENT 1u
#define SADKMOD_SERVER 2u
#define SADKMOD_LOBBY 4u

extern "C" {

struct sadkmod_api {
    // version 1
    std::uint32_t version;     // SADKMOD_API_VERSION of the host
    std::uint32_t size;        // sizeof(sadkmod_api) of the host; fields are only ever appended
    const char *mod_name;      // the mod's folder name
    const char *mod_dir;       // <game>\mods\<name> (no trailing backslash)
    const char *game_root;     // <game>
    void (*log_line)(const char *mod, const char *text);
    bool (*hook)(const char *mod, void *target, void *detour, void **original, const char *what);
    // version 2
    bool (*unhook)(const char *mod, void *target, void *detour);
    // `module` is a sadk::Module, `address` a static (Ghidra) address of it
    bool (*patch)(const char *mod, std::uint32_t module, std::uintptr_t address, const std::uint8_t *expect,
                  const std::uint8_t *replace, std::size_t n, const char *what);
    bool (*write_slot)(const char *mod, void **slot, void *value, void **previous, const char *what);
    // the host's mods (host.hpp), for a mod that manages others (e.g. one that downloads a match's server mods)
    void (*list_mods)(void (*each)(const struct sadkmod_modinfo *mod, void *ctx), void *ctx);
    bool (*add_mod)(const char *dir);            // a folder outside the scan (<game>\mods\.temp_x): listed, inactive
    bool (*forget_mod)(const char *folder);      // an inactive mod leaves the list
    bool (*activate_mod)(const char *folder);
    int (*deactivate_mod)(const char *folder);   // 0 done, 1 busy (freed later), 2 not found, 3 not active
    bool (*mod_hash)(const char *folder, char out[33]);
    void *host_module;   // the host's own DLL (the shim, <game>\bin\wsock32.dll): mods may hook its exports
    void (*redirect_server_mods)(bool on);   // host.hpp: off on this thread = server mods' files not redirected
    int (*free_pending)();                    // retries freeing unloaded mod.dlls; returns how many still wait
};

struct sadkmod_modinfo {
    const char *folder;      // its folder name, e.g. "20_rules" or ".temp_20_rules"; identifies it
    const char *name;        // the same without ".temp_"
    const char *dir;         // full path
    std::uint32_t version;   // sadkmod_version(); 0 without mod.dll
    std::uint32_t flags;     // SADKMOD_CLIENT | SADKMOD_SERVER | SADKMOD_LOBBY
    bool active;
    bool pending_free;       // unloaded, its mod.dll not freed yet
};

typedef bool (*sadkmod_init_fn)(const sadkmod_api *api);
typedef std::uint32_t (*sadkmod_version_fn)();
typedef std::uint32_t (*sadkmod_flags_fn)();
typedef void (*sadkmod_stop_fn)();
}

namespace sadk {

// In a mod: binds sadkmod's routing to the host (done by SADKMOD_MAIN). nullptr in the host itself.
void bind_host(const sadkmod_api *api);
const sadkmod_api *host_api();
const char *mod_name();   // "" outside a mod
const char *mod_dir();    // "" outside a mod

}  // namespace sadk

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
