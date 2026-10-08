// Mod host test, run under Wine or Windows from <root>\bin\test_host.exe (the Makefile sets up <root>):
// mod discovery and order, data overrides through CreateFileA/W, write opens left alone, the mesh-cache redirect,
// mod.dll loading with the API, and two hooks chained on one function (host first, then the mod).
#include <sadkmod/sadkmod.hpp>

#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

static int failures, checks;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            failures++;                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

extern "C" __declspec(dllexport) __attribute__((noinline)) int SADK_CDECL test_target(int x)
{
    volatile int k = x;
    return k + 1;
}

using target_fn = int(SADK_CDECL *)(int);
static target_fn host_original;
static int SADK_CDECL plus_ten(int x) { return host_original(x) + 10; }

static void put(const std::string &path, const char *text)
{
    for (std::size_t i = 0; (i = path.find('\\', i + 1)) != std::string::npos;) CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fputs(text, f);
        std::fclose(f);
    }
}

static std::string read(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    char buf[64] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(h);
    return buf;
}

// Conflicts end the game by default; here they are only recorded.
static int conflicts;
static char last_conflict[1024];
static void SADK_CDECL record_conflict(const char *text)
{
    conflicts++;
    std::snprintf(last_conflict, sizeof last_conflict, "%s", text);
}

int main()
{
    sadk::registry::set_conflict_handler(record_conflict);
    std::string root = sadk::game_root();
    sadk::log_open((root + "\\test_host.log").c_str());
    put(root + "\\data\\lobby\\config\\npc_bodyparts.xml", "ORIGINAL");
    put(root + "\\data\\lobby\\config\\untouched.xml", "UNTOUCHED");
    put(root + "\\mods\\a_first\\data\\lobby\\config\\npc_bodyparts.xml", "MOD A");
    put(root + "\\mods\\b_second\\data\\Lobby\\Config\\NPC_bodyparts.xml", "MOD B");   // the same file, other case: conflict
    put(root + "\\mods\\b_second\\data\\lobby\\avatars\\testmesh.KEX", "MESH");
    put(root + "\\mods\\_switched_off\\data\\lobby\\config\\untouched.xml", "SHOULD NOT APPEAR");
    CopyFileA((root + "\\bin\\test_mod.dll").c_str(), (root + "\\mods\\b_second\\mod.dll").c_str(), FALSE);

    CHECK(sadk::hook_function(reinterpret_cast<void *>(test_target), reinterpret_cast<void *>(plus_ten),
                              reinterpret_cast<void **>(&host_original), "test_target +10"));
    sadk::host::Summary s = sadk::host::start_mods();
    CHECK(s.mods == 2 && s.files == 2 && s.dlls == 1 && s.dll_failures == 0);   // A's file (first wins) and B's mesh
    CHECK(conflicts == 1 && std::strstr(last_conflict, "\"a_first\" and \"b_second\"") &&
          std::strstr(last_conflict, "lobby\\config\\npc_bodyparts.xml"));
    // (The gDecryptData hook fails here, as SADK.exe is not loaded: logged, not checked.)

    // Data overrides: relative and absolute paths, A and W, mixed case and forward slashes.
    SetCurrentDirectoryA((root + "\\bin").c_str());
    CHECK(read("..\\data\\lobby\\config\\npc_bodyparts.xml") == "MOD A");
    CHECK(read((root + "/DATA/lobby/config/npc_bodyparts.xml").c_str()) == "MOD A");
    CHECK(read("..\\data\\lobby\\config\\untouched.xml") == "UNTOUCHED");
    {
        std::wstring w = L"..\\data\\lobby\\config\\npc_bodyparts.xml";
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        char buf[16] = {};
        DWORD got = 0;
        CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, buf, 15, &got, nullptr) && std::string(buf) == "MOD A");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    // Writes into data\ go to the game's own file.
    CHECK(sadk::host::redirect(L"..\\data\\lobby\\config\\npc_bodyparts.xml", true) == nullptr);
    // The mesh cache of a modded .KEX goes to %LOCALAPPDATA%\SAdK\mods\, others stay.
    char local[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    std::string cache = std::string(local) + "\\SAdK\\";
    HANDLE c = CreateFileA((cache + "testmesh_v1.mshraw").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(c != INVALID_HANDLE_VALUE);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
    CHECK(GetFileAttributesA((cache + "mods\\testmesh_v1.mshraw").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA((cache + "testmesh_v1.mshraw").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(sadk::host::redirect(std::wstring(cache.begin(), cache.end()).append(L"othermesh.mshraw").c_str(), true) == nullptr);
    DeleteFileA((cache + "mods\\testmesh_v1.mshraw").c_str());

    // Chained hooks: the mod's x2 runs first, then the host's +10 (the host is always next to the game's code), then
    // the function: (1 + 1 + 10) * 2.
    volatile target_fn call = test_target;
    CHECK(call(1) == 24);
    // Everything the mod did is recorded under its name and can be taken back: only the host's +10 remains.
    CHECK(sadk::registry::count_owned("b_second") == 1);
    CHECK(sadk::registry::remove_owner("b_second").hooks == 1);
    CHECK(call(1) == 12);

    std::printf("%d checks, %d failed (log: %s\\test_host.log)\n", checks, failures, root.c_str());
    if (FILE *f = std::fopen((root + "\\test_host.log").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) std::fputs(line, stdout);
        std::fclose(f);
    }
    return failures ? 1 : 0;
}
