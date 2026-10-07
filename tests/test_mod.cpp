// A mod.dll for test_host: hooks test_target (exported by test_host.exe), which the host has hooked already.
#include <sadkmod/sadkmod.hpp>

#include <windows.h>

using target_fn = int(SADK_CDECL *)(int);
static target_fn original;

static int SADK_CDECL doubled(int x) { return original(x) * 2; }

static bool start()
{
    sadk::log("test mod in %s", sadk::mod_dir());
    void *target = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA(nullptr), "test_target"));
    return sadk::hook_function(target, reinterpret_cast<void *>(doubled), reinterpret_cast<void **>(&original),
                               "test_target x2");
}

SADKMOD_MAIN(start)
