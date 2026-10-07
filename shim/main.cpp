// DllMain, the forwarding table, start-up and mod loading (shim.hpp lists the parts).
//
// Start-up order: SADK.exe imports tincat3.dll, which imports WSOCK32, so Windows loads the shim before the game's
// entry point runs. DllMain (under the loader lock) only opens the log, loads the real wsock32.dll and replaces the
// first 5 bytes of SADK.exe's entry point with a JMP to entry_detour. That runs once on the main thread, after every
// DLL is loaded and before any game code: it puts the 5 bytes back, checks the exe, reads the config, applies the
// map-sharing patches, loads the mods (sadkmod host: data overrides and mod.dll files) and starts the bridge thread,
// then calls the real entry point. If the shim is loaded later instead (LoadLibrary, e.g. a test), the same start-up
// runs on a thread.
#include "shim.hpp"

#include <sadkmod/game/sadk_noav/module.hpp>

#include <cstring>

using sadk::log;

extern "C" {
void *real_ptrs[N_EXPORTS];   // read by the forwarding thunks (thunks.S)
}

static unsigned char *entry_point;
static unsigned char entry_saved[5];

static DWORD WINAPI bridge_thread(LPVOID)
{
    bridge_make_token();
    bridge_startup();
    return 0;
}

static void start(const char *where)
{
    if (!sadk::exe_is_supported()) {
        log("SADK.exe MD5 %s is not the supported DRM-free build (%s) - the shim stays inactive",
            sadk::file_md5(sadk::exe_path()).c_str(), sadk::game::md5);
        bridge_inactive();
        return;
    }
    log("start-up %s", where);
    read_config();
    apply_map_sharing();
    sadk::host::Summary m = sadk::host::start_mods();
    log("mods: %d folder%s, %d data file%s, %d mod.dll started%s", m.mods, m.mods == 1 ? "" : "s", m.files,
        m.files == 1 ? "" : "s", m.dlls, m.dll_failures ? " - SOME FAILED, see above" : "");
    CloseHandle(CreateThread(nullptr, 0, bridge_thread, nullptr, 0, nullptr));
}

// Takes the place of SADK.exe's entry point for one call (the loader calls it like a thread procedure).
extern "C" DWORD WINAPI entry_detour(void *arg)
{
    sadk::write_memory(entry_point, entry_saved, sizeof entry_saved);
    start("before the game's entry point");
    return reinterpret_cast<DWORD(SADK_CDECL *)(void *)>(entry_point)(arg);   // WinMainCRTStartup; does not return
}

static DWORD WINAPI late_start_thread(LPVOID)
{
    start("on a thread (the shim was loaded after the game started)");
    return 0;
}

static bool hook_entry_point()
{
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleA(nullptr));
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    entry_point = base + nt->OptionalHeader.AddressOfEntryPoint;
    std::memcpy(entry_saved, entry_point, sizeof entry_saved);
    unsigned char jmp[5] = {0xE9};
    std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(entry_detour) -
                                                 (reinterpret_cast<std::uintptr_t>(entry_point) + 5));
    std::memcpy(jmp + 1, &rel, 4);
    return sadk::write_memory(entry_point, jmp, sizeof jmp);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    char log_path[MAX_PATH], me[MAX_PATH], sys[MAX_PATH];
    std::strcpy(log_path, sadk::exe_path());
    char *slash = std::strrchr(log_path, '\\');
    std::strcpy(slash ? slash + 1 : log_path, "wsock32_shim.txt");
    sadk::log_open(log_path);
    bridge_init();
    GetModuleFileNameA(self, me, MAX_PATH);

    GetSystemDirectoryA(sys, MAX_PATH);                       // SysWOW64 for this 32-bit process
    std::strcat(sys, "\\wsock32.dll");
    HMODULE real_dll = LoadLibraryA(sys);
    int missing = 0;
    for (int i = 0; i < N_EXPORTS; i++) {
        real_ptrs[i] = real_dll ? reinterpret_cast<void *>(GetProcAddress(real_dll, MAKEINTRESOURCEA(ordinals[i]))) : nullptr;
        if (!real_ptrs[i]) missing++;
    }

    log("shim loaded (build " __DATE__ " " __TIME__ ", C++/sadkmod): pid %lu, exe %s, shim %s, real %s (%s), %d of %d exports resolved",
        GetCurrentProcessId(), sadk::exe_path(), me, sys, real_dll ? "ok" : "LOAD FAILED", N_EXPORTS - missing, N_EXPORTS);
    if (!real_dll) return FALSE;
    // reserved != NULL: loaded with the process (static import), so the entry point has not run yet.
    if (!reserved || !hook_entry_point()) CloseHandle(CreateThread(nullptr, 0, late_start_thread, nullptr, 0, nullptr));
    return TRUE;
}
