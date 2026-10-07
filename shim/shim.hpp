// SAdK host bridge shim: a proxy wsock32.dll for tincat3.dll (which imports all of its sockets from WSOCK32.dll
// by ordinal). Every export is forwarded by ordinal to the system wsock32.dll; four are hooked (bridge.cpp). On the
// DRM-free SADK.exe it is also the mod host (sadkmod/README.md, mods/README.md) and patches the game for map
// sharing (mapshare.cpp), built on sadkmod.
//
//   main.cpp        DllMain, the forwarding table, start-up (before SADK.exe's entry point) and mod loading
//   config.cpp      LobbySettings.ini / network.ini / sadk_bridge.ini
//   bridge.cpp      the host bridge (docs/bridge-protocol.md): control connection, reachability, data channels,
//                   the hooked socket functions
//   mapshare.cpp    map sharing (docs/BINARY_PATCHES.md, "Map sharing")
#pragma once
#include <winsock2.h>
#include <windows.h>

#include <sadkmod/sadkmod.hpp>

#include <string>

#include "ordinals.h"

extern "C" void *real_ptrs[N_EXPORTS];   // the system wsock32.dll's exports, by index (gen.py)

#define REAL(name) (reinterpret_cast<name##_fn>(real_ptrs[IDX_##name]))
typedef SOCKET(WINAPI *socket_fn)(int, int, int);
typedef int(WINAPI *connect_fn)(SOCKET, const struct sockaddr *, int);
typedef int(WINAPI *send_fn)(SOCKET, const char *, int, int);
typedef int(WINAPI *recv_fn)(SOCKET, char *, int, int);
typedef int(WINAPI *listen_fn)(SOCKET, int);
typedef int(WINAPI *bind_fn)(SOCKET, const struct sockaddr *, int);
typedef SOCKET(WINAPI *accept_fn)(SOCKET, struct sockaddr *, int *);
typedef int(WINAPI *closesocket_fn)(SOCKET);
typedef int(WINAPI *select_fn)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
typedef int(WINAPI *getsockname_fn)(SOCKET, struct sockaddr *, int *);
typedef int(WINAPI *ioctlsocket_fn)(SOCKET, long, u_long *);
typedef struct hostent *(WINAPI *gethostbyname_fn)(const char *);
typedef unsigned long(WINAPI *inet_addr_fn)(const char *);
typedef int(WINAPI *WSAGetLastError_fn)(void);
typedef int(WINAPI *WSAStartup_fn)(WORD, LPWSADATA);
typedef int(WINAPI *__WSAFDIsSet_fn)(SOCKET, fd_set *);

struct Config {
    std::string host;                     // LobbySettings.ini [LobbyServer] Host
    unsigned short lobby_port = 7070;     //   Port
    bool force_bridge = false;            //   ForceBridge: always host through the bridge
    unsigned short game_port = 5479;      // network.ini [Basics] gamePort
    unsigned short bridge_port = 7072;    // bin\sadk_bridge.ini [Bridge] port
};
extern Config cfg;
void read_config();

void bridge_init();                       // DllMain: locks and the start-up event
void bridge_make_token();                 // start-up thread: random token, WSAStartup
void bridge_startup();                    // after the patches: control connection, reachability test, OPEN loop
void bridge_inactive();                   // unsupported exe: release anything waiting for the bridge
void apply_map_sharing();
