#include "shim.hpp"

#include <cstdio>

Config cfg;

void read_config()
{
    sadk::Ini lobby(sadk::game_path("data\\lobby\\config\\LobbySettings.ini"));
    cfg.host = lobby.get("LobbyServer", "Host");
    cfg.lobby_port = static_cast<unsigned short>(lobby.get_int("LobbyServer", "Port", 7070));
    cfg.force_bridge = lobby.get_bool("LobbyServer", "ForceBridge", false);
    cfg.disable_billboards = lobby.get_bool("LobbyServer", "DisableBillboards", false);
    cfg.billboard_texture = lobby.get("LobbyServer", "BillboardTexture", "sign_ad0.dds");
    std::string rect = lobby.get("LobbyServer", "BillboardRect", "305,680,730,1005");
    int *r = cfg.billboard_rect;
    cfg.billboard_crop = std::sscanf(rect.c_str(), "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4 && r[2] > r[0] &&
                         r[3] > r[1];
    cfg.game_port = static_cast<unsigned short>(
        sadk::Ini(sadk::game_path("data\\game\\settings\\network.ini")).get_int("Basics", "gamePort", 5479));
    cfg.bridge_port =
        static_cast<unsigned short>(sadk::Ini(sadk::game_path("bin\\sadk_bridge.ini")).get_int("Bridge", "port", 7072));
}
