// Function hooks.
//
// Hook<F> (MinHook): replaces the start of the game function F with a jump to your function, the detour. Every
// caller is affected. Hook<F>::original calls the unhooked function (a trampoline that runs the overwritten
// instructions and jumps back). A detour can change the arguments, the result, both, or skip the original:
//
//     namespace fn = sadk::game::fn;
//     using Load = sadk::Hook<fn::S2CE::CTexture::CreateFromFile>;
//
//     static bool SADK_THISCALL load(sadk::game::S2CE::CTexture *t, void *path, bool a, bool b, bool c) {
//         bool ok = Load::original(t, path, a, b, c);    // the game's code
//         if (ok) { ... }                               // look at or change what it produced
//         return ok;
//     }
//     Load::install(load);                              // logged as "S2CE::CTexture::CreateFromFile"
//
// The detour's type must match the declaration exactly (calling convention included), else it does not compile:
// copy the parameter list from the declaration's comment. All hooks go through one registry per process; when
// several mods hook the same function, the most recently installed detour runs first and its `original` leads
// through the others to the game.
//
// hook_slot: replaces one entry of a vtable (or any table of function pointers) instead; only calls through that
// table are affected.
#pragma once
#include "core.hpp"

namespace sadk {

// Untyped layer (MinHook). `original` receives the trampoline. Logged; false on failure.
bool hook_function(void *target, void *detour, void **original, const char *what);
bool unhook_function(void *target);
bool write_slot(void **slot, void *value, void **previous, const char *what);

template <auto F>
struct Hook {
    using pointer = typename decltype(F)::pointer;
    static inline pointer original = nullptr;
    static bool installed() { return original != nullptr; }
    // `what` names the hook in the log; by default the function's own name.
    static bool install(pointer detour, const char *what = nullptr) {
        return hook_function(reinterpret_cast<void *>(F.get()), reinterpret_cast<void *>(detour),
                             reinterpret_cast<void **>(&original), what && *what ? what : F.name);
    }
};

// Typed vtable slot: `slot` is the address of the entry in the game's table (e.g. &vtbl->CreateFromFile).
template <class P>
bool hook_slot(P *slot, P detour, P *previous, const char *what) {
    return write_slot(reinterpret_cast<void **>(slot), reinterpret_cast<void *>(detour),
                      reinterpret_cast<void **>(previous), what);
}

}  // namespace sadk
