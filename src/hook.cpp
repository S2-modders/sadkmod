#include <sadkmod/hook.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/patch.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/verify.hpp>

#include <MinHook.h>
#include <windows.h>

#include <map>

namespace sadk {

namespace detail {
extern VerifyCounts counts;
}

// The registry (host only): one MinHook instance for the process. A second hook on a target that is already
// hooked hooks the previous detour instead, so the newest detour runs first and its `original` leads through the
// earlier detours to the game's code. Hooks are never removed while chained.
static CRITICAL_SECTION registry_cs;
static std::map<void *, void *> *chain_top;   // target -> the detour the target currently jumps to

static bool registry_ready()
{
    static bool ready = [] {
        InitializeCriticalSection(&registry_cs);
        chain_top = new std::map<void *, void *>;
        MH_STATUS st = MH_Initialize();
        return st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
    }();
    return ready;
}

bool hook_function(void *target, void *detour, void **original, const char *what)
{
    if (const sadkmod_api *h = host_api()) return h->hook(mod_name(), target, detour, original, what);
    if (verifying()) {
        // In verify mode only the location is checked: the target must be the start of mapped code.
        bool ok = target != nullptr;
        (ok ? detail::counts.matched : detail::counts.mismatched)++;
        log("verify hook %s: %s", what, ok ? "target inside the image" : "TARGET NOT IN THE IMAGE");
        return ok;
    }
    if (!registry_ready() || !target) {
        log("hook %s: MinHook unavailable or no target", what);
        return false;
    }
    EnterCriticalSection(&registry_cs);
    auto it = chain_top->find(target);
    void *at = it == chain_top->end() ? target : it->second;
    MH_STATUS st = MH_CreateHook(at, detour, original);
    if (st == MH_OK) st = MH_EnableHook(at);
    if (st == MH_OK) (*chain_top)[target] = detour;
    LeaveCriticalSection(&registry_cs);
    log("hook %s at %p: %s%s", what, target, st == MH_OK ? "installed" : MH_StatusToString(st),
        st == MH_OK && at != target ? " (chained after an earlier hook)" : "");
    return st == MH_OK;
}

bool unhook_function(void *target)
{
    if (host_api() || !registry_ready()) return false;
    EnterCriticalSection(&registry_cs);
    auto it = chain_top->find(target);
    bool ok = it != chain_top->end() && MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK;
    if (ok) chain_top->erase(it);   // hooks chained onto its detour are no longer reached either
    LeaveCriticalSection(&registry_cs);
    return ok;
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
