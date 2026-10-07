// Function hooks.
//
// Hook<F> (MinHook): replaces the start of the game function F with a jump to our function. Every caller is
// affected. Hook<F>::original calls the unhooked function (a trampoline that runs the overwritten instructions
// and jumps back), so a hook usually looks like
//
//     using CreateFromFile = sadk::Hook<sadk::game::fn::S2CE::CTexture::CreateFromFile>;
//     static bool SADK_THISCALL my_load(sadk::game::S2CE::CTexture *t, void *path, bool a, bool b, bool c) {
//         ...
//         return CreateFromFile::original(t, path, a, b, c);
//     }
//     CreateFromFile::install(my_load, "texture loads");
//
// The detour's type must match the declaration exactly (calling convention included), else it does not compile.
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
    static bool install(pointer detour, const char *what) {
        return hook_function(reinterpret_cast<void *>(F.get()), reinterpret_cast<void *>(detour),
                             reinterpret_cast<void **>(&original), what);
    }
};

// Typed vtable slot: `slot` is the address of the entry in the game's table (e.g. &vtbl->CreateFromFile).
template <class P>
bool hook_slot(P *slot, P detour, P *previous, const char *what) {
    return write_slot(reinterpret_cast<void **>(slot), reinterpret_cast<void *>(detour),
                      reinterpret_cast<void **>(previous), what);
}

}  // namespace sadk
