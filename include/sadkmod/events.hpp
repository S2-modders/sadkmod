// Game events: the host hooks the game once and calls every mod that subscribed, so mods need not each hook the same
// functions. Callbacks run on the game's main thread, in load order (registry.hpp; the host's own last). A mod's
// subscriptions end when it is unloaded.
//
//     static void each_frame(void *) { ... }
//     static void entering(void *) { sadk::log("a match is about to be built"); }
//     sadk::events::on_frame(each_frame);
//     sadk::events::on_match_enter(entering);
//     sadk::events::post([](void *) { /* on the main thread at the next frame */ });   // from any thread
//
// Where they come from (docs/data-loading.md, docs/s2tftp.md):
//   frame          every frame, before the game's: CApplicationEx::FrameTick S 00401bd0 (menus, lobby and match alike)
//   match_enter    a match is entered, before its world is built: Scene_CreateGlobal S 0067e7e0, from
//                  nMenu::Game::OnEnter S 005eed00 (offline and online)
//   match_leave    the match's scene is gone: after Scene_DestroyGlobal S 0067e8a0, from nMenu::Game::OnLeave S 005eb030
//   session_end    a network session ends (match or pre-game room left, kicked, a new join or host begins): after
//                  NComm_Manager::Shutdown S 0040b410
//   net_event      every event of the match network, after the game handled it: NComm_Manager::HandleEvent S 0040e560
//                  (the event's typeId says which, e.g. 0x30001 UserInformation, 0x30003 GameInformation); the event
//                  is valid during the callback
#pragma once
#include <cstdint>

#include "core.hpp"

namespace sadk::game {
struct NComm_Manager;
namespace NComm {
struct EventBase;
}
}  // namespace sadk::game

namespace sadk::events {

enum Event : std::uint32_t { posted = 0, frame = 1, match_enter = 2, match_leave = 3, session_end = 4, net_event = 5 };

using Callback = void (*)(void *ctx);
using NetCallback = void (*)(void *ctx, game::NComm_Manager *manager, game::NComm::EventBase *event);

bool on_frame(Callback fn, void *ctx = nullptr);
bool on_match_enter(Callback fn, void *ctx = nullptr);
bool on_match_leave(Callback fn, void *ctx = nullptr);
bool on_session_end(Callback fn, void *ctx = nullptr);
bool on_net_event(NetCallback fn, void *ctx = nullptr);

// Runs `fn(ctx)` once on the main thread at the start of the next frame. Safe from any thread: the way to call game
// functions from a thread of your own.
bool post(Callback fn, void *ctx = nullptr);

// The untyped form behind all of them (also sadkmod_api::subscribe).
bool subscribe(Event e, void *fn, void *ctx);

}  // namespace sadk::events
