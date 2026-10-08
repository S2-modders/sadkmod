// sadkmod self-test, run under Wine or Windows: test_sadkmod.exe [<DRM-free SADK.exe>]
// Without the game: msvc layouts, patches and MinHook hooks on functions of this program.
// With SADK.exe: the generated declarations against the real binary, in verify mode (nothing is executed).
#include <sadkmod/game/sadk_noav/fn/S2CE.hpp>
#include <sadkmod/game/sadk_noav/fn/LobbyMenu.hpp>
#include <sadkmod/game/sadk_noav/fn/NComm_Manager.hpp>
#include <sadkmod/game/sadk_noav/fn/_global.hpp>
#include <sadkmod/game/sadk_noav/fn/ai.hpp>
#include <sadkmod/sadkmod.hpp>

#include <windows.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

static int failures, checks;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

// Conflicts end the game by default; here they are only recorded.
static int conflicts;
static char last_conflict[1024];
static void SADK_CDECL record_conflict(const char *text)
{
    conflicts++;
    std::snprintf(last_conflict, sizeof last_conflict, "%s", text);
}

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
    static constexpr const char *name = "counter_add";
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
    CHECK(AddHook::install(counter_add_hooked));    // logged under its name, "counter_add"
    CHECK(call(&c, 1, false) == 102);            // hooked: +101
    CHECK(call(&c, 1, true) == 304);             // twice: +202
    CHECK(AddHook::original(&c, 1, false) == 305);
    CHECK(AddHook::remove() && !AddHook::installed());
    CHECK(call(&c, 1, false) == 306);

    // table slot
    int(SADK_THISCALL * previous)(void *) = nullptr;
    CHECK(sadk::hook_slot(&table.get, get_two, &previous, "test slot"));
    CHECK(table.get(nullptr) == 2 && previous == get_one);

    // patch(): expected bytes; a second patch on the same bytes is a conflict; unexpected bytes
    static unsigned char code[8] = {0x8B, 0x56, 0x28, 0x8B, 0xC8, 0xFF, 0xD2, 0x90};
    auto site = reinterpret_cast<std::uintptr_t>(code);   // Module::sadk resolves to the address itself
    sadk::Bytes old{0x8B, 0x56, 0x28, 0x8B, 0xC8, 0xFF, 0xD2};
    sadk::Bytes repl = sadk::Bytes{0x8B, 0xC8} + sadk::call_to(sadk::Module::sadk, site + 2, (const void *)get_two);
    CHECK(sadk::patch(site, old, repl, "test patch"));
    CHECK(std::memcmp(code, repl.b.data(), 7) == 0);
    CHECK(!sadk::patch(site + 4, {0xC8}, {0x90}, "test patch overlapping") && conflicts == 1);
    static unsigned char other[3] = {1, 2, 3};
    CHECK(!sadk::patch(reinterpret_cast<std::uintptr_t>(other), {1, 2, 4}, {9, 9, 9}, "test mismatch") && other[2] == 3);
    std::int32_t rel;
    std::memcpy(&rel, code + 3, 4);
    CHECK(site + 2 + 5 + rel == reinterpret_cast<std::uintptr_t>(get_two));
}

// ── Registry: owned chains that can lose any member ──────────────────────────────────────────────────────────────
extern "C" __attribute__((noinline)) int SADK_CDECL chained(int x)
{
    volatile int k = x;
    return k + 1;
}
using chained_fn = int(SADK_CDECL *)(int);
static chained_fn orig_a, orig_b, orig_c;
static int SADK_CDECL add_ten(int x) { return orig_a(x) + 10; }        // owner a, oldest
static int SADK_CDECL times_two(int x) { return orig_b(x) * 2; }       // owner b
static int SADK_CDECL minus_three(int x) { return orig_c(x) - 3; }     // owner c, newest

static int SADK_THISCALL get_three(void *) { return 3; }
static int(SADK_THISCALL *slot_prev_a)(void *);
static int(SADK_THISCALL *slot_prev_b)(void *);
static Vtbl table2 = {get_one};

static void test_registry()
{
    namespace reg = sadk::registry;
    volatile chained_fn call = chained;
    auto *t = reinterpret_cast<void *>(chained);
    reg::set_order("a", "10_a");                                 // load order a, b, c
    reg::set_order("b", "20_b");
    reg::set_order("c", "30_c");
    CHECK(reg::hook("c", t, (void *)minus_three, (void **)&orig_c, "c -3"));   // installed out of order
    CHECK(reg::hook("a", t, (void *)add_ten, (void **)&orig_a, "a +10"));
    CHECK(reg::hook("b", t, (void *)times_two, (void **)&orig_b, "b x2"));
    CHECK(!reg::hook("c", t, (void *)minus_three, (void **)&orig_c, "c -3 again"));   // same detour twice
    CHECK(call(1) == ((1 + 1) - 3) * 2 + 10);                    // runs a, b, c, game: 8
    CHECK(reg::remove_owner("b").hooks == 1);                    // the middle one goes
    CHECK(call(1) == (1 + 1) - 3 + 10);                          // 9
    CHECK(reg::remove_owner("c").hooks == 1);                    // the last before the game
    CHECK(call(1) == 12);
    CHECK(reg::unhook("a", t, (void *)add_ten));                 // the last: straight to the game's code
    CHECK(call(1) == 2);
    CHECK(reg::hook("b", t, (void *)times_two, (void **)&orig_b, "b x2 again"));   // the hook is reused
    CHECK(call(1) == 4);
    CHECK(reg::hook("a", t, (void *)add_ten, (void **)&orig_a, "a +10 again"));    // goes before b
    CHECK(call(1) == (1 + 1) * 2 + 10);
    CHECK(!reg::unhook("a", t, (void *)times_two));              // b's detour is not a's to remove
    CHECK(reg::remove_owner("a").hooks == 1 && reg::remove_owner("b").hooks == 1 && call(1) == 2);

    // table slots in load order: a is the entry in the table, b below it; installing b later puts it below a
    CHECK(reg::write_slot("a", (void **)&table2.get, (void *)get_two, (void **)&slot_prev_a, "slot a"));
    CHECK(reg::write_slot("b", (void **)&table2.get, (void *)get_three, (void **)&slot_prev_b, "slot b"));
    CHECK(table2.get(nullptr) == 2 && slot_prev_a == get_three && slot_prev_b == get_one);
    CHECK(reg::remove_owner("a").slots == 1);
    CHECK(table2.get(nullptr) == 3 && slot_prev_b == get_one);
    CHECK(reg::remove_owner("b").slots == 1 && table2.get(nullptr) == 1);

    // a 6-byte forwarding thunk, JMP DWORD PTR [slot], as the shim exports (gamehostbridge hooks those)
    static void *slot = reinterpret_cast<void *>(chained);
    auto *thunk = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    thunk[0] = 0xFF;
    thunk[1] = 0x25;
    void **at = &slot;
    std::memcpy(thunk + 2, &at, 4);
    std::memset(thunk + 6, 0xCC, 10);
    volatile chained_fn via = reinterpret_cast<chained_fn>(thunk);
    CHECK(via(1) == 2);
    CHECK(reg::hook("a", thunk, (void *)add_ten, (void **)&orig_a, "thunk +10") && via(1) == 12 && call(1) == 2);
    CHECK(reg::remove_owner("a").hooks == 1 && via(1) == 2);

    // patches: one per address (a second one, even the same, is a conflict naming both); undone; changed bytes are
    // left alone
    static std::uint8_t code[4] = {1, 2, 3, 4};
    const std::uint8_t before[2] = {2, 3}, after[2] = {8, 9};
    CHECK(reg::patch("a", code + 1, 0x1001, before, after, 2, "patch a"));
    int seen = conflicts;
    CHECK(!reg::patch("b", code + 2, 0x1002, (const std::uint8_t[]){9}, (const std::uint8_t[]){5}, 1, "patch b"));
    CHECK(conflicts == seen + 1 && std::strstr(last_conflict, "\"a\" and \"b\"") && std::strstr(last_conflict, "00001002"));
    CHECK(code[1] == 8 && code[2] == 9 && reg::count_owned("a") == 1 && reg::count_owned("b") == 0);
    CHECK(reg::remove_owner("a").patches == 1 && code[1] == 2 && code[2] == 3);
    CHECK(reg::patch("b", code + 2, 0x1002, (const std::uint8_t[]){3}, (const std::uint8_t[]){5}, 1, "patch b"));
    CHECK(reg::remove_owner("b").patches == 1 && code[2] == 3);   // free again once a is gone
    CHECK(reg::patch("a", code, 0x1000, (const std::uint8_t[]){1}, (const std::uint8_t[]){7}, 1, "patch a 2"));
    code[0] = 5;                                                   // someone else changed it since
    auto r = reg::remove_owner("a");
    CHECK(r.patches == 1 && r.not_restored == 1 && code[0] == 5);
    CHECK(reg::count_owned("a") == 0);
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
    CHECK(std::strcmp(fn::S2CE::CTexture::CreateFromFile.name, "S2CE::CTexture::CreateFromFile") == 0);
    // sadk::calls: the call sites assetshare used to patch by address, found from the bodies in the map
    using V = std::vector<std::uintptr_t>;
    CHECK(sadk::calls(fn::S2Tftp_Session_ReadNextBlock, fn::_fopen_s) == V{0x00426b14});
    CHECK(sadk::calls(fn::ai::net::S2TftpSession::CloseFile, fn::_rename) == (V{0x00427b29, 0x00427b7e}));
    CHECK(sadk::calls(fn::S2Tftp_Session_WriteBlock, fn::S2Tftp_OpenTempFile) == V{0x00426ca5});
    CHECK(sadk::calls(fn::S2Tftp_Session_WriteBlock, fn::_fwrite) == V{0x00426d14});
    CHECK(sadk::calls(fn::LobbyMenu::SelectMapDialog::RefreshMapList, fn::SelectMapDialog_AppendMapFiles) == V{0x0045a381});
    CHECK(sadk::calls(fn::LobbyMenu::SetupGameDialog::HandleButtonClicks, fn::NComm_Manager::SendPlayerReadyEvent) ==
          V{0x00457f51});
    auto removes = sadk::calls(fn::ai::net::S2TftpSession::CloseFile, fn::_remove);   // through the CRT jump thunk
    CHECK(std::find(removes.begin(), removes.end(), 0x00427a08) != removes.end());
    CHECK(sadk::call_target(sadk::Module::sadk, 0x00427a08) == 0x006f6789);          // the thunk itself
    auto requests = sadk::calls(fn::NComm_Manager::HandleEvent, fn::NComm_Manager::RequestFileFromHost);
    CHECK(requests == (V{0x0040f256, 0x0040f30d}));                                 // across HandleEvent's 2 ranges
    for (auto r : removes) std::printf("CloseFile calls _remove at %08x\n", unsigned(r));
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
    sadk::registry::set_conflict_handler(record_conflict);
    test_local();
    test_registry();
    if (argc > 1) test_game(argv[1]);
    std::printf("%d checks, %d failed (log: %s)\n", checks, failures, log);
    return failures ? 1 : 0;
}
