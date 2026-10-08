// Map sharing: patches to SADK.exe in memory (docs/BINARY_PATCHES.md, "Map sharing").
// The client already transfers a missing map from the host (S2TFTP over the match connection,
// NComm::Manager::HandleEvent S 0040e560), but only for maps the host advertises with type 3
// (Documents\SAdK\maps) AND "download allowed" (+0x228), which OnMapSelected always sends as 0.
//   1. OnMapSelected S 00454ad0: advertise every map as type 3 with downloads allowed.
//   2. SelectMapDialog::RefreshMapList S 0045a300: also list Documents\SAdK\maps (location 3).
//   3. S2Tftp_Session_ReadNextBlock S 00426ac0 (host serving a file): only maps, never anything else;
//      a map that is not in Documents\SAdK\maps is served from the game's own map folder.
//   4. S2TftpSession::CloseFile S 00427990 (joiner finishing a download): the host-chosen file name
//      may only land in Documents\SAdK\maps as .s2m/.bmp.
//
// Download progress, refresh and the ready guard: starting a match while the joiner still lacks the map crashes
// the client, so "ready" is refused until the map is present and no download is running. Progress goes to the
// pre-game room through the game's own NComm_SendUINotification S 0040e3d0 (the "Player joined game" channel).
// When the last download completes the joiner sends PlayerReady(0) once: the host answers every ready change by
// re-broadcasting the game information, and the joiner then finds the map and refreshes the room.
#include "shim.hpp"

#include <sadkmod/game/sadk_noav/fn/NComm.hpp>
#include <sadkmod/game/sadk_noav/fn/NComm_Manager.hpp>
#include <sadkmod/game/sadk_noav/fn/_global.hpp>

#include <shlobj.h>

#include <cstdio>
#include <cstring>

using sadk::log;
namespace fn = sadk::game::fn;
using GameFILE = sadk::game::mbstring_h::FILE;   // the game's C runtime FILE, not ours

// The call sites below call these CRT jump thunks (JMP to _free / _remove); the patches expect them.
constexpr std::uintptr_t THUNK_REMOVE = 0x006f6789;   // -> _remove S 006f675f
constexpr int MAP_LOCATION_USER = 3;

static char maps_dir[MAX_PATH];                       // <My Documents>\SAdK\maps\ (with the backslash)

// The file part of `path` if it is <maps_dir><plain name>.s2m|.bmp, else nullptr.
static const char *map_file_name(const char *path)
{
    std::size_t n = std::strlen(maps_dir);
    if (!maps_dir[0] || _strnicmp(path, maps_dir, n) != 0) return nullptr;
    const char *name = path + n;
    std::size_t len = std::strlen(name);
    if (len < 5 || len > 120 || std::strstr(name, "..")) return nullptr;
    for (const char *c = name; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':' || static_cast<unsigned char>(*c) < 0x20) return nullptr;
    if (_stricmp(name + len - 4, ".s2m") != 0 && _stricmp(name + len - 4, ".bmp") != 0) return nullptr;
    return name;
}

// NComm_SendUINotification takes a std::string* (the game passes the result of a string builder; Ghidra still
// types the argument as int). The observers only read or copy it; the text must outlive the call.
static void room_message(const char *text)
{
    static char held[256];
    std::snprintf(held, sizeof held, "%s", text);
    sadk::msvc::string str = sadk::msvc::string::borrow(held, std::strlen(held));
    if (void *mgr = fn::NComm_GetManager())
        fn::NComm_SendUINotification(mgr, static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&str)));
}

static volatile LONG downloads_active;
static unsigned long download_bytes, download_reported;
static DWORD download_last_activity;

static bool download_running()
{
    if (downloads_active > 0 && GetTickCount() - download_last_activity > 60000) {
        log("map share: download stalled for 60 s - no longer blocking ready");
        InterlockedExchange(&downloads_active, 0);
    }
    return downloads_active > 0;
}

static void download_finished()
{
    if (InterlockedDecrement(&downloads_active) <= 0) {
        InterlockedExchange(&downloads_active, 0);
        if (void *mgr = fn::NComm_GetManager())            // host re-broadcasts -> room refreshes
            fn::NComm_Manager::SendPlayerReadyEvent(static_cast<sadk::game::NComm_Manager *>(mgr), false);
    }
}

static bool session_map_present(void *mgr)
{
    // NetGUID (vtable + 4 dwords, 0x14 bytes) and NCore::UUID (constructed, GUID at +8..+0x17; the map search
    // compares it there): both get room to spare. The name comes back as a std::string the game may allocate.
    alignas(4) unsigned char guid[32] = {}, uuid[64] = {};
    sadk::msvc::string name = sadk::msvc::string::small("");
    std::int32_t type = 0;
    auto *info = sadk::at<sadk::game::NComm::EventGameInformation>(mgr, 0xdc);
    auto *g = static_cast<sadk::game::NComm::NetGUID *>(fn::NComm::EventGameInformation::GetMapGuid(info, guid));
    fn::NComm::NetGUID::ToUUID(g, uuid);
    bool found = fn::GameFile_FindMapByGuidAnyType(uuid, &name, &type);
    if (!name.is_inline()) sadk::game_free(name.heap_text);
    return found;
}

static GameFILE *SADK_CDECL shim_open_temp(sadk::msvc::string *out_path)
{
    GameFILE *f = fn::S2Tftp_OpenTempFile(out_path);
    if (f) {
        InterlockedIncrement(&downloads_active);
        download_bytes = download_reported = 0;
        download_last_activity = GetTickCount();
        room_message("Downloading a map file from the host...");
        log("map share: download started");
    }
    return f;
}

using game_size_t = sadk::game::crtdefs_h::size_t;
static game_size_t SADK_CDECL shim_fwrite(void *buf, game_size_t size, game_size_t count, GameFILE *f)
{
    game_size_t n = fn::_fwrite(buf, size, count, f);
    download_bytes += n * size;
    download_last_activity = GetTickCount();
    if (download_bytes - download_reported >= 256 * 1024) {
        char msg[96];
        download_reported = download_bytes;
        std::snprintf(msg, sizeof msg, "Map download: %lu KB received...", download_bytes / 1024);
        room_message(msg);
    }
    return n;
}

static std::int32_t SADK_CDECL shim_abort_remove(char *path)
{
    log("map share: download aborted");
    room_message("Map download aborted.");
    std::int32_t rc = fn::_remove(path);
    download_finished();
    return rc;
}

static bool SADK_THISCALL shim_send_ready(sadk::game::NComm_Manager *mgr, bool ready)
{
    if (ready && (download_running() || !session_map_present(mgr))) {
        room_message(download_running() ? "The map is still downloading - you can get ready once it is complete."
                                        : "You don't have this map (yet) - you can't get ready.");
        log("map share: ready refused (%s)", download_running() ? "download running" : "map missing");
        return fn::NComm_Manager::SendPlayerReadyEvent(mgr, false);
    }
    return fn::NComm_Manager::SendPlayerReadyEvent(mgr, ready);
}

static std::uint32_t SADK_CDECL shim_tftp_fopen(GameFILE **f, char *path, char *mode)
{
    const char *name = map_file_name(path);
    *f = nullptr;
    if (!name) {
        log("map share: refused a request for %s (not a map file)", path);
        return 2;                                           // ENOENT -> !TFTP_ERROR_FILENOTFOUND
    }
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        log("map share: sending %s", name);
        return fn::_fopen_s(f, path, mode);
    }
    std::string stock = sadk::game_path("data\\game\\maps\\Freegamemaps\\") + name;
    if (GetFileAttributesA(stock.c_str()) != INVALID_FILE_ATTRIBUTES) {
        log("map share: sending %s from the game's map folder", name);
        return fn::_fopen_s(f, stock.data(), mode);
    }
    log("map share: %s not found", name);
    return 2;
}

static std::int32_t SADK_CDECL shim_tftp_rename(char *from, char *to)
{
    if (!map_file_name(to)) {
        log("map share: refused to store a download as %s", to);
        fn::_remove(from);
        download_finished();
        return -1;
    }
    std::int32_t rc = fn::_rename(from, to);
    char msg[160];
    std::snprintf(msg, sizeof msg, "Map file received: %s (%lu KB).", to + std::strlen(maps_dir), download_bytes / 1024);
    room_message(msg);
    log("map share: received %s", to + std::strlen(maps_dir));
    download_finished();
    return rc;
}

static void SADK_STDCALL shim_append_maps(std::int32_t location, void *list, void *names, float *rgba)
{
    static float user_colour[4] = {1.0f, 0.85f, 0.55f, 1.0f};   // custom maps: warm tint
    fn::SelectMapDialog_AppendMapFiles(location, list, names, rgba);
    fn::SelectMapDialog_AppendMapFiles(MAP_LOCATION_USER, list, names, user_colour);
}

void apply_map_sharing()
{
    char docs[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs) == S_OK) {   // as Sys_GetMyDocumentsPath
        char d[MAX_PATH];
        std::snprintf(d, sizeof d, "%s\\SAdK", docs);
        CreateDirectoryA(d, nullptr);
        std::snprintf(maps_dir, sizeof maps_dir, "%s\\SAdK\\maps\\", docs);
        CreateDirectoryA(maps_dir, nullptr);
    }
    // OnMapSelected: MOV EDX,[ESP+0x21C] / MOV EAX,[ESI+0xB6C] / PUSH ECX / PUSH EBX / PUSH EDX / PUSH EAX
    //   -> MOV EAX,[ESI+0xB6C] / PUSH ECX / PUSH 1 (download allowed) / PUSH 3 (type) / PUSH EAX / NOP x5
    int ok = sadk::patch(0x00454c37,
                         {0x8B, 0x94, 0x24, 0x1C, 0x02, 0x00, 0x00, 0x8B, 0x86, 0x6C, 0x0B, 0x00, 0x00, 0x51, 0x53, 0x52, 0x50},
                         {0x8B, 0x86, 0x6C, 0x0B, 0x00, 0x00, 0x51, 0x6A, 0x01, 0x6A, 0x03, 0x50, 0x90, 0x90, 0x90, 0x90, 0x90},
                         "map advertised downloadable");
    ok += sadk::patch_call(0x0045a381, fn::SelectMapDialog_AppendMapFiles, (void *)shim_append_maps,
                           "map list + Documents\\SAdK\\maps");
    ok += sadk::patch_call(0x00426b14, fn::_fopen_s, (void *)shim_tftp_fopen, "map server filter");
    ok += sadk::patch_call(0x00427b29, fn::_rename, (void *)shim_tftp_rename, "download destination check 1");
    ok += sadk::patch_call(0x00427b7e, fn::_rename, (void *)shim_tftp_rename, "download destination check 2");
    ok += sadk::patch_call(0x00457f51, fn::NComm_Manager::SendPlayerReadyEvent, (void *)shim_send_ready,
                           "ready guard (Ready button)");
    ok += sadk::patch_call(0x00426ca5, fn::S2Tftp_OpenTempFile.address, (void *)shim_open_temp, "download start");
    ok += sadk::patch_call(0x00426d14, fn::_fwrite, (void *)shim_fwrite, "download progress");
    ok += sadk::patch_call(0x00427a08, THUNK_REMOVE, (void *)shim_abort_remove, "download abort");
    log("map sharing: %d of 9 patches active (maps folder %s)", ok, maps_dir);
}
