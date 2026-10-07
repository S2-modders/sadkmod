#include <sadkmod/patch.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/verify.hpp>

#include <windows.h>

#include <cstring>

namespace sadk {

namespace detail {
extern VerifyCounts counts;
}

static Bytes rel32(std::uint8_t op, std::uintptr_t from_runtime, std::uintptr_t to)
{
    Bytes b{op, 0, 0, 0, 0};
    std::int32_t d = static_cast<std::int32_t>(to - (from_runtime + 5));
    std::memcpy(&b.b[1], &d, 4);
    return b;
}

// Relative jumps are computed from the real runtime address of the site, also in verify mode (the expected
// bytes of an original call do not depend on where the image is mapped).
Bytes call_to(Module m, std::uintptr_t at, const void *target)
{
    std::uintptr_t site = m == Module::sadk || !module_base(m) ? at : module_base(m) + (at - preferred_base(m));
    return rel32(0xE8, site, reinterpret_cast<std::uintptr_t>(target));
}

Bytes jmp_to(Module m, std::uintptr_t at, const void *target)
{
    std::uintptr_t site = m == Module::sadk || !module_base(m) ? at : module_base(m) + (at - preferred_base(m));
    return rel32(0xE9, site, reinterpret_cast<std::uintptr_t>(target));
}

Bytes call_to(Module, std::uintptr_t at, std::uintptr_t static_target) { return rel32(0xE8, at, static_target); }

Bytes nops(std::size_t n)
{
    Bytes b;
    for (std::size_t i = 0; i < n && i < b.b.size(); i++) b.b[b.n++] = 0x90;
    return b;
}

bool write_memory(void *address, const void *data, std::size_t size)
{
    DWORD old;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(address, data, size);
    VirtualProtect(address, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

bool patch(Module m, std::uintptr_t address, const Bytes &expect, const Bytes &replace, const char *what)
{
    auto *p = reinterpret_cast<std::uint8_t *>(resolve(m, address));
    if (expect.n != replace.n || !p) {
        log("patch %s at %08x: %s - not applied", what, unsigned(address), p ? "length mismatch" : "module not loaded");
        return false;
    }
    if (verifying()) {
        bool ok = std::memcmp(p, expect.b.data(), expect.n) == 0;
        (ok ? detail::counts.matched : detail::counts.mismatched)++;
        log("verify %s at %08x: %s", what, unsigned(address), ok ? "expected bytes found" : "EXPECTED BYTES NOT FOUND");
        return ok;
    }
    if (IsBadReadPtr(p, expect.n) || std::memcmp(p, expect.b.data(), expect.n) != 0) {
        if (!IsBadReadPtr(p, replace.n) && std::memcmp(p, replace.b.data(), replace.n) == 0) return true;
        log("patch %s at %08x: unexpected bytes - not applied", what, unsigned(address));
        return false;
    }
    if (!write_memory(p, replace.b.data(), replace.n)) return false;
    log("patch %s at %08x: applied", what, unsigned(address));
    return true;
}

bool patch_call(Module m, std::uintptr_t address, std::uintptr_t original, const void *target, const char *what)
{
    return patch(m, address, call_to(m, address, original), call_to(m, address, target), what);
}

}  // namespace sadk
