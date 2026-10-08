#include <sadkmod/events.hpp>
#include <sadkmod/host.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/verify.hpp>

namespace sadk::events {

// In a mod the host keeps the subscription under the mod's name; in the host itself under "". Verify mode runs no game.
bool subscribe(Event e, void *fn, void *ctx)
{
    if (const sadkmod_api *h = host_api()) return h->subscribe(mod_name(), e, fn, ctx);
    if (verifying()) return true;
    return host::subscribe("", e, fn, ctx);
}

bool on_frame(Callback fn, void *ctx) { return subscribe(frame, reinterpret_cast<void *>(fn), ctx); }
bool on_match_enter(Callback fn, void *ctx) { return subscribe(match_enter, reinterpret_cast<void *>(fn), ctx); }
bool on_match_leave(Callback fn, void *ctx) { return subscribe(match_leave, reinterpret_cast<void *>(fn), ctx); }
bool on_session_end(Callback fn, void *ctx) { return subscribe(session_end, reinterpret_cast<void *>(fn), ctx); }
bool on_net_event(NetCallback fn, void *ctx) { return subscribe(net_event, reinterpret_cast<void *>(fn), ctx); }
bool post(Callback fn, void *ctx) { return subscribe(posted, reinterpret_cast<void *>(fn), ctx); }

}  // namespace sadk::events
