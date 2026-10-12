#include <sadkmod/game/sadk_noav/fn/NComm_Manager.hpp>
#include <sadkmod/game/sadk_noav/fn/_global.hpp>
#include <sadkmod/msvc.hpp>
#include <sadkmod/runtime.hpp>
#include <sadkmod/ui.hpp>

#include <cstdint>
#include <string>

namespace sadk::ui {

void room_message(std::string_view text)
{
    std::string line(text);
    log("room: %s", line.c_str());
    game::NComm_Manager *mgr = game::fn::NComm_GetManager();
    if (!mgr) return;
    msvc::owned_string s(line);
    // SendUINotification takes a std::string* (Ghidra still types the argument as int)
    game::fn::NComm_Manager::SendUINotification(mgr, static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&s)));
}

}  // namespace sadk::ui
