// DllMain, the forwarding table and start-up (shim.hpp lists the parts).
#include "shim.hpp"

#include <sadkmod/game/sadk_noav/module.hpp>

#include <cstring>

using sadk::log;

extern "C" {
void *real_ptrs[N_EXPORTS];   // read by the forwarding thunks (thunks.S)
}

static DWORD WINAPI startup_thread(LPVOID)
{
    bridge_make_token();
    if (!sadk::exe_is_supported()) {
        log("SADK.exe MD5 %s is not the supported DRM-free build (%s) - the shim stays inactive",
            sadk::file_md5(sadk::exe_path()).c_str(), sadk::game::md5);
        bridge_inactive();
        return 0;
    }
    read_config();
    // Before the game loads anything: the lobby scene (and its billboard textures) loads before the login. Safe this
    // early because only the DRM-free build is accepted, whose code is never packed.
    apply_map_sharing();
    apply_billboards();
    bridge_startup();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
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
    CloseHandle(CreateThread(nullptr, 0, startup_thread, nullptr, 0, nullptr));
    return TRUE;
}
