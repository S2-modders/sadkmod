// Where a game function calls another, found in the game binary: no call sites to look up by hand.
//
//     namespace fn = sadk::game::fn;
//     for (auto site : sadk::calls(fn::ai::net::S2TftpSession::CloseFile, fn::_rename))
//         sadk::log("CloseFile calls _rename at %08x", unsigned(site));
//     // every call to _rename inside CloseFile goes to my_rename instead; there must be exactly 2
//     sadk::patch_calls(fn::ai::net::S2TftpSession::CloseFile, fn::_rename, (void *)my_rename, "rename check", 2);
//
// `where` is a generated declaration of a function with its body (the Ghidra map's code ranges, core.hpp Body);
// `what` a declaration of the same module. Found is every CALL rel32 (E8) inside `where` whose target is `what`, or
// whose target is a jump thunk (a lone JMP rel32) to `what`, as the game's calls into the C runtime often are.
// Calls through a register, a vtable or an import table are not found.
//
// The code is read from the module's file on disk (mapped once per process), not from memory: there it may already
// carry hooks and patches of other mods, which would also throw the decoder off. In verify mode (verify.hpp) it is
// read from the image being verified. Each site is a static (Ghidra) address, ready for patch_call.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core.hpp"

namespace sadk {

// The untyped form: body as 2 * n static addresses (start, end, start, end ...).
std::vector<std::uintptr_t> find_calls(Module m, const std::uintptr_t *ranges, std::size_t n, std::uintptr_t target);

// Where the CALL rel32 at `site` goes, as in the file (a thunk's own address, not the thunk's target); 0 if there is
// no CALL rel32 at `site`.
std::uintptr_t call_target(Module m, std::uintptr_t site);

template <class W, class T>
std::vector<std::uintptr_t> calls(const W &, const T &)
{
    static_assert(W::body.size > 0, "sadk::calls: `where` has no body in the map (not a function of the map?)");
    static_assert(W::module == T::module, "sadk::calls: `where` and `what` are in different modules");
    return find_calls(W::module, W::body.data(), W::body.size, T::address);
}

// Redirects every call to `what` inside `where` to `detour`: patch_call on each site, so each one is checked and
// recorded like any patch. `expect` is how many calls there must be (-1: any number, at least one); if the count
// differs, nothing is patched. Returns the number of calls redirected.
int patch_calls(Module m, const std::uintptr_t *ranges, std::size_t n, std::uintptr_t target, const void *detour,
                const char *what, int expect, const char *where_name, const char *target_name);

template <class W, class T>
int patch_calls(const W &, const T &, const void *detour, const char *what, int expect = -1)
{
    static_assert(W::body.size > 0, "sadk::patch_calls: `where` has no body in the map (not a function of the map?)");
    static_assert(W::module == T::module, "sadk::patch_calls: `where` and `what` are in different modules");
    return patch_calls(W::module, W::body.data(), W::body.size, T::address, detour, what, expect, W::name, T::name);
}

}  // namespace sadk
