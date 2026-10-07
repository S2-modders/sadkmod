#include <sadkmod/core.hpp>
#include <sadkmod/verify.hpp>

#include <windows.h>

namespace sadk {

namespace detail {
const MappedImage *verify_image[2];
}

const char *module_file(Module m) { return m == Module::sadk ? nullptr : "tincat3.dll"; }

std::uintptr_t module_base(Module m)
{
    static volatile std::uintptr_t cache[2];
    auto i = static_cast<int>(m);
    if (!cache[i]) cache[i] = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(module_file(m)));
    return cache[i];
}

std::uintptr_t resolve(Module m, std::uintptr_t static_address)
{
    auto i = static_cast<int>(m);
    if (const MappedImage *img = detail::verify_image[i]) {
        std::uintptr_t rva = static_address - img->preferred;
        return rva < img->bytes.size() ? reinterpret_cast<std::uintptr_t>(img->bytes.data()) + rva : 0;
    }
    if (m == Module::sadk) return static_address;   // no relocations: always at its preferred base
    std::uintptr_t base = module_base(m);
    return base ? base + (static_address - preferred_base(m)) : 0;
}

}  // namespace sadk
