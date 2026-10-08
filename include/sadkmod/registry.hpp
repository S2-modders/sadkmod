// The registry of every change made to the game's code: hooks, table slots and byte patches, each with its owner
// (a mod's name; "" for the host itself). It lives in the host; sadkmod's hook_function / hook_slot / patch go
// here directly in the host and through sadkmod_api in a mod.
//
// Hooks: one MinHook hook per target, which jumps through a small stub to the newest detour. Every detour's
// `original` leads to the next older one and the oldest to MinHook's trampoline (the game's code). Removing a
// detour points the next newer one's `original` (or the stub) past it, so a mod in the middle of a chain can go.
// The MinHook hook itself stays in place once made: with no detour left the stub leads straight to the trampoline.
// Table slots chain the same way through the `previous` pointers their owners keep. Patches keep the bytes they
// replaced; a patch that two owners made alike is undone when the last of them goes.
//
// remove_owner takes back everything of one owner. It does not make unloading safe on its own: a thread may still
// run in the owner's code, which host.cpp checks before freeing a mod.dll.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sadk::registry {

bool hook(const char *owner, void *target, void *detour, void **original, const char *what);
bool unhook(const char *owner, void *target, void *detour);
bool write_slot(const char *owner, void **slot, void *value, void **previous, const char *what);
// `at` is the address in this process; `static_address` only names it in the log.
bool patch(const char *owner, std::uint8_t *at, std::uintptr_t static_address, const std::uint8_t *expect,
           const std::uint8_t *replace, std::size_t n, const char *what);

struct Removed {
    int hooks = 0, slots = 0, patches = 0;
    int not_restored = 0;   // patches whose bytes were changed since by something else: left as they are
};
Removed remove_owner(const char *owner);

// For tests: how many detours / slot entries / patch records an owner has.
int count_owned(const char *owner);

}  // namespace sadk::registry
