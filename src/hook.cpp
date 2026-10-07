#include <sadkmod/hook.hpp>
#include <sadkmod/patch.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/verify.hpp>

#include <MinHook.h>

namespace sadk {

namespace detail {
extern VerifyCounts counts;
}

static bool minhook_ready()
{
    static MH_STATUS init = MH_Initialize();
    return init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
}

bool hook_function(void *target, void *detour, void **original, const char *what)
{
    if (verifying()) {
        // In verify mode only the location is checked: the target must be the start of mapped code.
        bool ok = target != nullptr;
        (ok ? detail::counts.matched : detail::counts.mismatched)++;
        log("verify hook %s: %s", what, ok ? "target inside the image" : "TARGET NOT IN THE IMAGE");
        return ok;
    }
    if (!minhook_ready() || !target) {
        log("hook %s: MinHook unavailable or no target", what);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK) st = MH_EnableHook(target);
    log("hook %s at %p: %s", what, target, st == MH_OK ? "installed" : MH_StatusToString(st));
    return st == MH_OK;
}

bool unhook_function(void *target)
{
    return minhook_ready() && MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK;
}

bool write_slot(void **slot, void *value, void **previous, const char *what)
{
    if (verifying()) {
        log("verify slot %s: not checked (vtables are runtime data)", what);
        return true;
    }
    if (previous) *previous = *slot;
    bool ok = write_memory(slot, &value, sizeof value);
    log("hook %s (table slot %p): %s", what, static_cast<void *>(slot), ok ? "installed" : "write failed");
    return ok;
}

}  // namespace sadk
