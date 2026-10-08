// What the player sees.
#pragma once
#include <string_view>

namespace sadk::ui {

// A line in the pre-game room's chat: the channel the game uses for "<name> Player joined game"
// (NComm_SendUINotification S 0040e3d0, as NComm_Manager::HandleEvent S 0040e560 sends it; the game frees its own
// text right after the call, so the text only has to live through it). Logged as well. Does nothing without an NComm
// manager. [TODO: whether and where it shows during a match]
void room_message(std::string_view text);

}  // namespace sadk::ui
