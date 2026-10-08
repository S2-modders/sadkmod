#include <sadkmod/hook.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/registry.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/verify.hpp>

namespace sadk {

namespace detail {
extern VerifyCounts counts;
}

// In a mod everything goes to the host's registry (registry.hpp) under the mod's name; in the host, under "".
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
    return registry::hook("", target, detour, original, what);
}

bool unhook_function(void *target, void *detour)
{
    if (const sadkmod_api *h = host_api()) return h->unhook(mod_name(), target, detour);
    return registry::unhook("", target, detour);
}

bool write_slot(void **slot, void *value, void **previous, const char *what)
{
    if (verifying()) {
        log("verify slot %s: not checked (vtables are runtime data)", what);
        return true;
    }
    if (const sadkmod_api *h = host_api()) return h->write_slot(mod_name(), slot, value, previous, what);
    return registry::write_slot("", slot, value, previous, what);
}

}  // namespace sadk
