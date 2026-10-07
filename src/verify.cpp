#include <sadkmod/verify.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sadk {

namespace detail {
extern const MappedImage *verify_image[2];
VerifyCounts counts;
}

bool map_image(const char *path, MappedImage &out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<std::uint8_t> file;
    std::uint8_t buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
    std::fclose(f);
    auto u16 = [&](std::size_t o) { return o + 2 <= file.size() ? std::uint16_t(file[o] | file[o + 1] << 8) : 0; };
    auto u32 = [&](std::size_t o) {
        return o + 4 <= file.size() ? std::uint32_t(file[o] | file[o + 1] << 8 | file[o + 2] << 16 | std::uint32_t(file[o + 3]) << 24) : 0;
    };
    if (u16(0) != 0x5a4d) return false;                      // MZ
    std::size_t pe = u32(0x3c);
    if (u32(pe) != 0x4550) return false;                      // PE\0\0
    unsigned sections = u16(pe + 6), opt_size = u16(pe + 20);
    std::size_t opt = pe + 24;
    if (u16(opt) != 0x10b) return false;                      // PE32
    out.preferred = u32(opt + 28);
    std::uint32_t image_size = u32(opt + 56), headers_size = u32(opt + 60);
    out.bytes.assign(image_size, 0);
    std::memcpy(out.bytes.data(), file.data(), std::min<std::size_t>({headers_size, file.size(), image_size}));
    for (unsigned i = 0; i < sections; i++) {
        std::size_t s = opt + opt_size + 40 * i;
        std::uint32_t vsize = u32(s + 8), rva = u32(s + 12), raw_size = u32(s + 16), raw = u32(s + 20);
        std::uint32_t n = std::min(vsize ? vsize : raw_size, raw_size);
        if (rva >= image_size || raw >= file.size()) continue;
        n = std::min<std::uint32_t>({n, image_size - rva, static_cast<std::uint32_t>(file.size() - raw)});
        std::memcpy(out.bytes.data() + rva, file.data() + raw, n);
    }
    return true;
}

void verify_with(Module m, const MappedImage *image) { detail::verify_image[static_cast<int>(m)] = image; }

bool verifying() { return detail::verify_image[0] || detail::verify_image[1]; }

VerifyCounts verify_counts() { return detail::counts; }

}  // namespace sadk
