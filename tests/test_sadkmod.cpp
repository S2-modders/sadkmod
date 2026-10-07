// sadkmod self-test, run under Wine or Windows: test_sadkmod.exe [<DRM-free SADK.exe>]
// Without the game: msvc layouts, patches and MinHook hooks on functions of this program.
// With SADK.exe: the generated declarations against the real binary, in verify mode (nothing is executed).
#include <sadkmod/game/sadk_noav/fn/S2CE.hpp>
#include <sadkmod/game/sadk_noav/fn/_global.hpp>
#include <sadkmod/sadkmod.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>

static int failures, checks;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

// ── A thiscall "game function" of our own, hooked with Hook<> ────────────────────────────────────────────────────
struct Counter {
    int value;
};
using add_fn = int(SADK_THISCALL *)(Counter *, int, bool);

__attribute__((noinline)) int SADK_THISCALL counter_add(Counter *c, int n, bool twice)
{
    volatile int k = n;                      // a body long enough for a 5-byte jump
    c->value += twice ? 2 * k : k;
    return c->value;
}

struct LocalFn {                             // the shape of a generated Fn<>, pointing into this program
    using pointer = add_fn;
    pointer get() const { return counter_add; }
};
inline constexpr LocalFn counter_add_fn{};
using AddHook = sadk::Hook<counter_add_fn>;

static int SADK_THISCALL counter_add_hooked(Counter *c, int n, bool twice)
{
    return AddHook::original(c, n + 100, twice);     // changes the argument, then runs the original
}

// ── vtable slot hook ─────────────────────────────────────────────────────────────────────────────────────────────
struct Vtbl {
    int(SADK_THISCALL *get)(void *);
};
static int SADK_THISCALL get_one(void *) { return 1; }
static int SADK_THISCALL get_two(void *) { return 2; }
static Vtbl table = {get_one};

static void test_local()
{
    // msvc::string
    auto s = sadk::msvc::string::small("ad0.tga");
    CHECK(s.is_inline() && s.size == 7 && s.view() == "ad0.tga" && s.equals_icase("AD0.TGA"));
    static const char long_text[] = "a text longer than fifteen characters";
    auto b = sadk::msvc::string::borrow(long_text);
    CHECK(!b.is_inline() && b.data() == long_text && b.size == sizeof long_text - 1);

    // MinHook through Hook<>
    Counter c{0};
    volatile add_fn call = counter_add;          // call through a pointer so the compiler cannot inline
    CHECK(call(&c, 1, false) == 1);
    CHECK(AddHook::install(counter_add_hooked, "test counter_add"));
    CHECK(call(&c, 1, false) == 102);            // hooked: +101
    CHECK(call(&c, 1, true) == 304);             // twice: +202
    CHECK(AddHook::original(&c, 1, false) == 305);
    CHECK(sadk::unhook_function(reinterpret_cast<void *>(counter_add)));
    CHECK(call(&c, 1, false) == 306);

    // table slot
    int(SADK_THISCALL * previous)(void *) = nullptr;
    CHECK(sadk::hook_slot(&table.get, get_two, &previous, "test slot"));
    CHECK(table.get(nullptr) == 2 && previous == get_one);

    // patch(): expected bytes, already applied, mismatch
    static unsigned char code[8] = {0x8B, 0x56, 0x28, 0x8B, 0xC8, 0xFF, 0xD2, 0x90};
    auto site = reinterpret_cast<std::uintptr_t>(code);   // Module::sadk resolves to the address itself
    sadk::Bytes old{0x8B, 0x56, 0x28, 0x8B, 0xC8, 0xFF, 0xD2};
    sadk::Bytes repl = sadk::Bytes{0x8B, 0xC8} + sadk::call_to(sadk::Module::sadk, site + 2, (const void *)get_two);
    CHECK(sadk::patch(site, old, repl, "test patch"));
    CHECK(std::memcmp(code, repl.b.data(), 7) == 0);
    CHECK(sadk::patch(site, old, repl, "test patch again"));                  // already applied
    CHECK(!sadk::patch(site, sadk::Bytes{1, 2, 3, 4, 5, 6, 7}, repl.n == 7 ? sadk::Bytes{9, 9, 9, 9, 9, 9, 9} : repl,
                       "test mismatch"));
    std::int32_t rel;
    std::memcpy(&rel, code + 3, 4);
    CHECK(site + 2 + 5 + rel == reinterpret_cast<std::uintptr_t>(get_two));
}

static void test_game(const char *exe)
{
    std::string md5 = sadk::file_md5(exe);
    CHECK(md5 == sadk::game::md5);
    if (md5 != sadk::game::md5) {
        std::printf("%s is not the DRM-free build (%s) - skipping the game checks\n", exe, md5.c_str());
        return;
    }
    static sadk::MappedImage image;
    CHECK(sadk::map_image(exe, image) && image.preferred == 0x400000);
    sadk::verify_with(sadk::Module::sadk, &image);
    // GetTexture's file load (00504806) and the CALL rel32 the map list patch expects (0045a381 -> 00459f40)
    CHECK(sadk::patch(0x00504806, {0x8B, 0x56, 0x28, 0x8B, 0xC8, 0xFF, 0xD2}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
                      "billboard site"));
    CHECK(sadk::patch_call(0x0045a381, 0x00459f40, nullptr, "map list call"));
    CHECK(!sadk::patch(0x00504806, {0, 0, 0}, {1, 1, 1}, "deliberate mismatch"));
    // generated declarations resolve into the image; CreateFromFile starts at a function prologue in the image
    namespace fn = sadk::game::fn;
    auto *p = reinterpret_cast<const std::uint8_t *>(fn::S2CE::CTexture::CreateFromFile.get());
    CHECK(p == image.bytes.data() + (0x004e80d0 - 0x400000));
    CHECK(fn::_free.address == 0x006f23fe && fn::_malloc.address == 0x006f3ada);
    sadk::verify_with(sadk::Module::sadk, nullptr);
    auto n = sadk::verify_counts();
    CHECK(n.matched == 2 && n.mismatched == 1);
}

int main(int argc, char **argv)
{
    char log[MAX_PATH];
    GetTempPathA(MAX_PATH, log);
    std::strcat(log, "test_sadkmod.log");
    DeleteFileA(log);
    sadk::log_open(log);
    test_local();
    if (argc > 1) test_game(argv[1]);
    std::printf("%d checks, %d failed (log: %s)\n", checks, failures, log);
    return failures ? 1 : 0;
}
