// A mod.dll for test_host: hooks test_target (exported by test_host.exe), which the host has hooked already.
#include <sadkmod/sadkmod.hpp>

#include <windows.h>

using target_fn = int(SADK_CDECL *)(int);
static target_fn original;

static int SADK_CDECL doubled(int x) { return original(x) * 2; }

// Events: each call is reported to the test program (exported by it) with a tag.
using event_fn = void(SADK_CDECL *)(int);
static void report(int tag)
{
    if (auto f = reinterpret_cast<event_fn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA(nullptr), "test_event"))))
        f(tag);
}

static bool start()
{
    sadk::events::on_frame([](void *) { report(1); });
    sadk::events::on_match_enter([](void *ctx) { report(*static_cast<int *>(ctx)); }, new int(2));
    sadk::events::post([](void *) { report(3); });
    sadk::log("test mod in %s", sadk::mod_dir());
    void *target = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA(nullptr), "test_target"));
    return sadk::hook_function(target, reinterpret_cast<void *>(doubled), reinterpret_cast<void **>(&original),
                               "test_target x2");
}

static void stop()
{
    using stopped_fn = void(SADK_CDECL *)();
    if (auto f = reinterpret_cast<stopped_fn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA(nullptr), "test_mod_stopped"))))
        f();
}

SADKMOD_MAIN(start, 1, SADKMOD_CLIENT)
SADKMOD_STOP(stop)
