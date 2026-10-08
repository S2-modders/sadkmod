// The registry of every change made to the game's code: hooks, table slots and byte patches, each with its owner
// (a mod's name; "" for the host itself). It lives in the host; sadkmod's hook_function / hook_slot / patch go
// here directly in the host and through sadkmod_api in a mod.
//
// Order: several detours on one target run in load order. Each owner has an order key (set_order; the host uses the
// lowercase folder name, without the ".temp_" of a downloaded mod): the key that sorts first runs first and its
// `original` leads to the next. The host itself ("") is always last, next to the game's code; an owner without a
// key comes just before the host. Detours of the same key run in the order they were installed. So the order does
// not depend on when a mod installs its hooks, also for a mod added later.
//
// Hooks: one MinHook hook per target, which jumps through a small stub to the first detour. Every detour's
// `original` leads to the next one and the last to MinHook's trampoline (the game's code). Removing a detour points
// the one before it (or the stub) past it, so a mod in the middle of a chain can go.
// The MinHook hook itself stays in place once made: with no detour left the stub leads straight to the trampoline.
// Table slots chain the same way through the `previous` pointers their owners keep. Patches keep the bytes they
// replaced.
//
// Conflicts: one patch per address. A patch overlapping one already made (by any owner) is a conflict: the handler is called with a message naming both owners and the address. By default it shows
// that message in a message box and ends the process, since the game cannot run with only half of a mod. The host
// reports two mods replacing the same data file the same way.
//
// remove_owner takes back everything of one owner. It does not make unloading safe on its own: a thread may still
// run in the owner's code, which host.cpp checks before freeing a mod.dll.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sadk::registry {

void set_order(const char *owner, const char *key);

typedef void (*ConflictHandler)(const char *text);
void set_conflict_handler(ConflictHandler h);   // tests; nullptr restores the default (message box, then exit)
void conflict(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // logs it, then calls the handler

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
