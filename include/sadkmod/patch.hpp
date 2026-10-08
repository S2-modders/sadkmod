// Byte patches in the game's code and data. Every patch states the bytes it expects and is applied only where they
// match exactly. Each result is logged. One patch per address: a patch overlapping an earlier one is a conflict
// that ends the game with a message naming both (registry.hpp). Every patch is recorded with the
// bytes it replaced and its owner (in a mod: the mod), so unloading the mod undoes it (registry.hpp).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

#include "core.hpp"

namespace sadk {

// Up to 32 bytes of machine code or data, built from literals and the helpers below.
struct Bytes {
    std::array<std::uint8_t, 32> b{};
    std::size_t n = 0;

    Bytes() = default;
    Bytes(std::initializer_list<std::uint8_t> v) { for (auto x : v) b[n++] = x; }
    Bytes &operator+=(const Bytes &o) { for (std::size_t i = 0; i < o.n; i++) b[n++] = o.b[i]; return *this; }
    friend Bytes operator+(Bytes a, const Bytes &o) { return a += o; }
    std::span<const std::uint8_t> span() const { return {b.data(), n}; }
};

// E8 rel32 / E9 rel32 at `at` (a static address of module m) to an absolute target in this process.
Bytes call_to(Module m, std::uintptr_t at, const void *target);
Bytes jmp_to(Module m, std::uintptr_t at, const void *target);
// The same for a target that is itself a static address of module m (an original call, for `expect`).
Bytes call_to(Module m, std::uintptr_t at, std::uintptr_t static_target);
Bytes nops(std::size_t n);

// Replaces `expect` at the static address with `replace` (same length). Returns true if the bytes are now in place.
bool patch(Module m, std::uintptr_t address, const Bytes &expect, const Bytes &replace, const char *what);

// The common case: a CALL rel32 at `address` that calls `original` (static) is redirected to `target`.
bool patch_call(Module m, std::uintptr_t address, std::uintptr_t original, const void *target, const char *what);

// The same with the original call target given as its declaration (only its address is used), e.g.
//     sadk::patch_call(0x00427b29, sadk::game::fn::_rename, (void *)my_rename, "download destination check");
template <class G>
    requires requires { G::address; G::module; }
bool patch_call(std::uintptr_t address, const G &, const void *target, const char *what) {
    return patch_call(G::module, address, G::address, target, what);
}

// Shorthands for SADK.exe.
inline bool patch(std::uintptr_t address, const Bytes &expect, const Bytes &replace, const char *what) {
    return patch(Module::sadk, address, expect, replace, what);
}
inline bool patch_call(std::uintptr_t address, std::uintptr_t original, const void *target, const char *what) {
    return patch_call(Module::sadk, address, original, target, what);
}

// Writes raw bytes, changing the page protection for the write. No checks and no record (not undone when a mod is
// unloaded): use patch() for game code.
bool write_memory(void *address, const void *data, std::size_t size);

}  // namespace sadk
