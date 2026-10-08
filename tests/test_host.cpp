// Mod host test, run under Wine or Windows from <root>\bin\test_host.exe (the Makefile sets up <root>):
// mod discovery, flags (mod.dll, marker files, none), data conflicts, overrides through CreateFileA/W, write opens
// left alone, the mesh-cache redirect, mod.dll loading with the API, hooks chained on one function (the mod's
// first, the host's next to the function), deactivating and activating mods, a mod.dll unloaded while a thread is
// inside it (busy, freed later), a downloaded .temp_ mod, and the content hash.
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

// The test mod's event callbacks report here; the host's own report with tag 100 + n.
static std::string events_seen;
extern "C" __declspec(dllexport) void SADK_CDECL test_event(int tag) { events_seen += std::to_string(tag) + " "; }
static void SADK_CDECL host_frame(void *) { test_event(101); }

// The test mod's sadkmod_stop calls this (found with GetProcAddress on the exe).
static volatile LONG stops;
extern "C" __declspec(dllexport) void SADK_CDECL test_mod_stopped() { InterlockedIncrement(&stops); }

// The host's own detour. On the thread `blocked_thread` it waits for `release` (with the mod's detour, which called
// it, still on that thread's stack).
using target_fn = int(SADK_CDECL *)(int);
static target_fn host_original;
static volatile DWORD blocked_thread;
static HANDLE inside, release;
static int SADK_CDECL plus_ten(int x)
{
    if (GetCurrentThreadId() == blocked_thread) {
        SetEvent(inside);
        WaitForSingleObject(release, 10000);
    }
    return host_original(x) + 10;
}

static volatile int thread_result;
static DWORD WINAPI call_on_thread(LPVOID)
{
    blocked_thread = GetCurrentThreadId();
    volatile target_fn call = test_target;
    thread_result = call(1);
    return 0;
}

static bool active(const char *folder)
{
    for (auto &m : sadk::host::mods())
        if (m.folder == folder) return m.active;
    return false;
}

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
    put(root + "\\mods\\a_first\\.client", "");
    put(root + "\\mods\\b_second\\data\\Lobby\\Config\\NPC_bodyparts.xml", "MOD B");   // the same file, other case: conflict
    put(root + "\\mods\\b_second\\data\\lobby\\avatars\\testmesh.KEX", "MESH");
    put(root + "\\mods\\_switched_off\\data\\lobby\\config\\untouched.xml", "SHOULD NOT APPEAR");
    put(root + "\\mods\\c_no_flags\\data\\lobby\\config\\untouched.xml", "NO FLAGS: NOT LOADED");
    put(root + "\\mods\\d_server\\.server", "");
    put(root + "\\mods\\d_server\\data\\game\\settings\\rules.xml", "SERVER RULES");
    put(root + "\\data\\game\\settings\\rules.xml", "GAME RULES");
    CopyFileA((root + "\\bin\\test_mod.dll").c_str(), (root + "\\mods\\b_second\\mod.dll").c_str(), FALSE);

    CHECK(sadk::hook_function(reinterpret_cast<void *>(test_target), reinterpret_cast<void *>(plus_ten),
                              reinterpret_cast<void **>(&host_original), "test_target +10"));
    sadk::host::Summary s = sadk::host::start_mods();
    CHECK(s.mods == 3 && s.skipped == 1 && s.files == 3 && s.dlls == 1);   // A's file (first wins), B's mesh, D's rules
    for (auto &m : sadk::host::mods()) {
        if (m.folder == "a_first") CHECK(m.flags == SADKMOD_CLIENT && !m.has_dll && m.active);
        if (m.folder == "b_second") CHECK(m.flags == SADKMOD_CLIENT && m.version == 1 && m.has_dll && m.active);
        if (m.folder == "d_server") CHECK(m.flags == SADKMOD_SERVER && m.active);
        CHECK(m.folder != "c_no_flags");
    }
    CHECK(conflicts == 1 && std::strstr(last_conflict, "\"a_first\" and \"b_second\"") &&
          std::strstr(last_conflict, "lobby\\config\\npc_bodyparts.xml"));
    // (The gDecryptData hook fails here, as SADK.exe is not loaded: logged, not checked.)

    // Data overrides: relative and absolute paths, A and W, mixed case and forward slashes.
    SetCurrentDirectoryA((root + "\\bin").c_str());
    CHECK(read("..\\data\\lobby\\config\\npc_bodyparts.xml") == "MOD A");
    CHECK(read((root + "/DATA/lobby/config/npc_bodyparts.xml").c_str()) == "MOD A");
    CHECK(read("..\\data\\lobby\\config\\untouched.xml") == "UNTOUCHED");
    CHECK(read("..\\data\\game\\settings\\rules.xml") == "SERVER RULES");
    sadk::host::redirect_server_mods(false);   // as for the build checksum: server mods' files left out
    CHECK(read("..\\data\\game\\settings\\rules.xml") == "GAME RULES");
    CHECK(read("..\\data\\lobby\\config\\npc_bodyparts.xml") == "MOD A");   // a client mod's still counts
    sadk::host::redirect_server_mods(true);
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
    CHECK(sadk::registry::count_owned("b_second") == 1);   // recorded under its folder name

    // Events: the mod's callbacks first, the host's last; a posted call runs once, at the next frame.
    sadk::host::subscribe("", sadk::events::frame, reinterpret_cast<void *>(host_frame), nullptr);
    sadk::host::raise(sadk::events::frame);
    CHECK(events_seen == "3 1 101 ");
    events_seen.clear();
    sadk::host::raise(sadk::events::frame);
    sadk::host::raise(sadk::events::match_enter);
    CHECK(events_seen == "1 101 2 ");

    // sadk::mods in the host: the same list and operations a mod gets through the API.
    {
        auto viaMods = sadk::mods::list();
        auto viaHost = sadk::host::mods();
        CHECK(viaMods.size() == viaHost.size() && !viaMods.empty() && viaMods[0].folder == viaHost[0].folder);
        CHECK(sadk::mods::content_hash("a_first") == sadk::host::content_hash("a_first"));
        sadk::msvc::owned_string shortText(std::string_view("inline"));   // (longer text needs the game's heap)
        CHECK(shortText.is_inline() && shortText.str() == "inline");
    }

    // A data-only server mod off and on again.
    CHECK(sadk::host::deactivate("d_server") == sadk::host::Unload::done && !active("d_server"));
    CHECK(read("..\\data\\game\\settings\\rules.xml") == "GAME RULES");
    CHECK(sadk::host::deactivate("d_server") == sadk::host::Unload::not_active);
    CHECK(sadk::host::activate("d_server") && read("..\\data\\game\\settings\\rules.xml") == "SERVER RULES");

    // Unloading the mod.dll while another thread is inside its detour: busy, everything undone, freed later.
    inside = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    release = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, call_on_thread, nullptr, 0, nullptr);
    CHECK(WaitForSingleObject(inside, 10000) == WAIT_OBJECT_0);
    std::string dll = root + "\\mods\\b_second\\mod.dll";
    CHECK(sadk::host::deactivate("b_second") == sadk::host::Unload::busy && stops == 1);
    CHECK(sadk::registry::count_owned("b_second") == 0);
    blocked_thread = 0;
    CHECK(call(1) == 12);                                      // only the host's +10 is left
    CHECK(GetModuleHandleA(dll.c_str()) != nullptr);           // still mapped
    CHECK(!sadk::host::activate("b_second"));                  // not while it waits to be freed
    SetEvent(release);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    CHECK(thread_result == 24);                                // that call ran through the mod to the end
    CHECK(sadk::host::free_pending() == 0 && GetModuleHandleA(dll.c_str()) == nullptr);
    CHECK(sadk::host::activate("b_second") && call(1) == 24);  // loaded and started again
    CHECK(sadk::host::deactivate("b_second") == sadk::host::Unload::done && stops == 2 && call(1) == 12);
    events_seen.clear();
    sadk::host::raise(sadk::events::frame);   // unloaded: only the host's own callback is left
    CHECK(events_seen == "101 ");

    // A downloaded mod: outside the scan until added, listed inactive, then activated; .temp_ is not in its name.
    std::string dl = root + "\\mods\\.temp_e_download";
    put(dl + "\\.server", "");
    put(dl + "\\data\\game\\settings\\extra.xml", "DOWNLOADED");
    CHECK(!active(".temp_e_download") && sadk::host::add_mod(dl.c_str()) && !sadk::host::add_mod(dl.c_str()));
    bool listed = false;
    for (auto &m : sadk::host::mods())
        if (m.folder == ".temp_e_download") listed = m.name == "e_download" && !m.active && m.flags == SADKMOD_SERVER;
    CHECK(listed);
    std::string hash = sadk::host::content_hash(".temp_e_download");
    CHECK(hash.size() == 32 && hash == sadk::host::content_hash(".temp_e_download"));
    CHECK(sadk::host::activate(".temp_e_download") && read("..\\data\\game\\settings\\extra.xml") == "DOWNLOADED");
    put(dl + "\\data\\game\\settings\\extra.xml", "CHANGED");
    CHECK(sadk::host::content_hash(".temp_e_download") != hash);
    CHECK(!sadk::host::forget_mod(".temp_e_download"));        // not while active
    CHECK(sadk::host::deactivate(".temp_e_download") == sadk::host::Unload::done);
    CHECK(sadk::host::forget_mod(".temp_e_download") && !active(".temp_e_download"));

    // A mod activated later that clashes with an active one: a conflict; the first keeps the file.
    std::string clash = root + "\\mods\\.temp_f_clash";
    put(clash + "\\.server", "");
    put(clash + "\\data\\game\\settings\\rules.xml", "CLASH");
    int seen = conflicts;
    CHECK(sadk::host::add_mod(clash.c_str()) && sadk::host::activate(".temp_f_clash"));
    CHECK(conflicts == seen + 1 && std::strstr(last_conflict, "\"d_server\" and \".temp_f_clash\""));
    CHECK(read("..\\data\\game\\settings\\rules.xml") == "SERVER RULES");
    CHECK(sadk::host::deactivate(".temp_f_clash") == sadk::host::Unload::done && sadk::host::forget_mod(".temp_f_clash"));

    std::printf("%d checks, %d failed (log: %s\\test_host.log)\n", checks, failures, root.c_str());
    if (FILE *f = std::fopen((root + "\\test_host.log").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) std::fputs(line, stdout);
        std::fclose(f);
    }
    return failures ? 1 : 0;
}
