#include <sadkmod/calls.hpp>
#include <sadkmod/patch.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/verify.hpp>

#include <windows.h>

#include <cstring>

extern "C" {
#include "hde/hde32.h"
}

namespace sadk {

namespace detail {
extern const MappedImage *verify_image[2];
}

namespace {

// The module's file as the loader would map it: in verify mode the image being verified, else the file on disk
// (mapped once). nullptr if neither is there.
const MappedImage *pristine(Module m)
{
    auto i = static_cast<int>(m);
    if (const MappedImage *v = detail::verify_image[i]) return v;
    static MappedImage images[2];
    static bool tried[2];
    if (!tried[i]) {
        tried[i] = true;
        char path[MAX_PATH] = {};
        if (m == Module::sadk)
            GetModuleFileNameA(nullptr, path, MAX_PATH);
        else if (HMODULE h = GetModuleHandleA(module_file(m)))
            GetModuleFileNameA(h, path, MAX_PATH);
        if (!path[0] || !map_image(path, images[i])) log("calls: cannot map %s", path[0] ? path : "the module's file");
    }
    return images[i].bytes.empty() ? nullptr : &images[i];
}

const std::uint8_t *at(const MappedImage *img, std::uintptr_t static_address, std::size_t n)
{
    std::uintptr_t rva = static_address - img->preferred;
    return rva < img->bytes.size() && img->bytes.size() - rva >= n ? img->bytes.data() + rva : nullptr;
}

std::int32_t rel32(const std::uint8_t *p)
{
    std::int32_t r;
    std::memcpy(&r, p, 4);
    return r;
}

}  // namespace

std::uintptr_t call_target(Module m, std::uintptr_t site)
{
    const MappedImage *img = pristine(m);
    const std::uint8_t *p = img ? at(img, site, 5) : nullptr;
    if (!p || p[0] != 0xE8) return 0;
    return site + 5 + static_cast<std::uintptr_t>(rel32(p + 1));
}

std::vector<std::uintptr_t> find_calls(Module m, const std::uintptr_t *ranges, std::size_t n, std::uintptr_t target)
{
    std::vector<std::uintptr_t> sites;
    const MappedImage *img = pristine(m);
    if (!img) return sites;
    for (std::size_t r = 0; r < n; r++) {
        std::uintptr_t a = ranges[2 * r], end = ranges[2 * r + 1];
        while (a < end) {
            const std::uint8_t *p = at(img, a, 1);
            if (!p) break;
            hde32s hs;
            unsigned len = hde32_disasm(p, &hs);
            if ((hs.flags & F_ERROR) || len == 0) {   // not an instruction: the map's body is wrong here
                log("calls: cannot decode at %08x - stopped in this range", unsigned(a));
                break;
            }
            if (hs.opcode == 0xE8 && len == 5) {
                std::uintptr_t to = a + 5 + static_cast<std::uintptr_t>(static_cast<std::int32_t>(hs.imm.imm32));
                bool hit = to == target;
                if (!hit)   // a jump thunk to the target: JMP rel32 at the call's destination
                    if (const std::uint8_t *t = at(img, to, 5); t && t[0] == 0xE9)
                        hit = to + 5 + static_cast<std::uintptr_t>(rel32(t + 1)) == target;
                if (hit) sites.push_back(a);
            }
            a += len;
        }
    }
    return sites;
}

int patch_calls(Module m, const std::uintptr_t *ranges, std::size_t n, std::uintptr_t target, const void *detour,
                const char *what, int expect, const char *where_name, const char *target_name)
{
    std::vector<std::uintptr_t> sites = find_calls(m, ranges, n, target);
    if (sites.empty() || (expect >= 0 && static_cast<int>(sites.size()) != expect)) {
        log("patch %s: %s calls %s %u times, %s - not applied", what, where_name, target_name,
            unsigned(sites.size()), expect >= 0 ? "a different number than expected" : "so nothing to redirect");
        if (expect >= 0) log("  (expected %d)", expect);
        return 0;
    }
    int done = 0;
    for (std::uintptr_t site : sites)
        done += patch_call(m, site, call_target(m, site), detour, what);
    return done;
}

}  // namespace sadk
