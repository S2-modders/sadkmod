// The game's function names for the crash report (crash.hpp): the table gen_symbols.py built from the Ghidra map,
// inflated on first use into memory of its own (VirtualAlloc, not the heap: a crash may have broken the heap).
#include "crash.hpp"

#include <miniz.h>

#include <cstring>

extern "C" const unsigned char shim_symbols_blob[];
extern "C" const unsigned char shim_symbols_blob_end[];

namespace shim::symbols {

namespace {

struct Func { std::uint32_t entry, name; };
struct Range { std::uint32_t start, end, func; };
struct Table { const Func *funcs = nullptr; const Range *ranges = nullptr; std::uint32_t n_funcs = 0, n_ranges = 0; };

Table tables[2];
const char *names;
std::uint32_t names_size;
volatile LONG state;   // 0 not read, 1 reading, 2 ready, 3 failed

bool inflate_table()
{
    std::size_t packed = static_cast<std::size_t>(shim_symbols_blob_end - shim_symbols_blob);
    if (packed < 8) return false;
    std::uint32_t raw;
    std::memcpy(&raw, shim_symbols_blob, 4);
    auto *out = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, raw, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!out) return false;
    std::size_t got = tinfl_decompress_mem_to_mem(out, raw, shim_symbols_blob + 4, packed - 4, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (got != raw || std::memcmp(out, "SYM1", 4) != 0) return false;
    const std::uint8_t *p = out + 4, *end = out + raw;
    auto u32 = [&](std::uint32_t &v) {
        if (end - p < 4) return false;
        std::memcpy(&v, p, 4);
        p += 4;
        return true;
    };
    std::uint32_t modules;
    if (!u32(modules) || modules < 2) return false;
    for (std::uint32_t m = 0; m < modules; m++) {
        std::uint32_t nf, nr;
        if (!u32(nf) || !u32(nr) || std::size_t(end - p) < nf * sizeof(Func) + nr * sizeof(Range)) return false;
        if (m < 2) tables[m] = Table{reinterpret_cast<const Func *>(p), reinterpret_cast<const Range *>(p + nf * sizeof(Func)), nf, nr};
        p += nf * sizeof(Func) + nr * sizeof(Range);
    }
    if (!u32(names_size) || std::size_t(end - p) < names_size) return false;
    names = reinterpret_cast<const char *>(p);
    return true;
}

bool ready()
{
    if (state == 2) return true;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        InterlockedExchange(&state, inflate_table() ? 2 : 3);
        return state == 2;
    }
    while (state == 1) Sleep(1);
    return state == 2;
}

}  // namespace

const char *find(sadk::Module m, std::uint32_t static_address, std::uint32_t *entry)
{
    if (!ready()) return nullptr;
    const Table &t = tables[static_cast<int>(m)];
    // the last range starting at or before the address
    std::uint32_t lo = 0, hi = t.n_ranges;
    while (lo < hi) {
        std::uint32_t mid = (lo + hi) / 2;
        if (t.ranges[mid].start <= static_address) lo = mid + 1; else hi = mid;
    }
    // ranges of different functions do not overlap, but a function's later chunk may start inside an earlier range
    for (std::uint32_t i = lo; i-- > 0 && lo - i <= 4;) {
        const Range &r = t.ranges[i];
        if (static_address >= r.start && static_address < r.end && r.func < t.n_funcs) {
            const Func &f = t.funcs[r.func];
            if (f.name >= names_size) return nullptr;
            if (entry) *entry = f.entry;
            return names + f.name;
        }
    }
    return nullptr;
}

}  // namespace shim::symbols
