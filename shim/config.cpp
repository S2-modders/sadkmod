#include "shim.hpp"

#include <cstdio>

Config cfg;

void read_config()
{
    sadk::Ini lobby(sadk::game_path("data\\lobby\\config\\LobbySettings.ini"));
    cfg.host = lobby.get("LobbyServer", "Host");
    cfg.lobby_port = static_cast<unsigned short>(lobby.get_int("LobbyServer", "Port", 7070));
    cfg.force_bridge = lobby.get_bool("LobbyServer", "ForceBridge", false);
    cfg.game_port = static_cast<unsigned short>(
        sadk::Ini(sadk::game_path("data\\game\\settings\\network.ini")).get_int("Basics", "gamePort", 5479));
    cfg.bridge_port =
        static_cast<unsigned short>(sadk::Ini(sadk::game_path("bin\\sadk_bridge.ini")).get_int("Bridge", "port", 7072));
}
