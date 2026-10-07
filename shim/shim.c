/* SAdK host bridge shim: a proxy wsock32.dll for tincat3.dll (which imports all of its sockets from
 * WSOCK32.dll by ordinal). Every export is forwarded by ordinal to the system wsock32.dll; four are
 * hooked. Protocol: docs/bridge-protocol.md. Log: wsock32_shim.txt next to the game exe.
 *
 *  startup  control connection to the lobby host's bridge port ("SADKB1 HELLO <token>"), then the
 *           reachability test: listen on the game port, ask the stub to connect back ("CHECK"); no
 *           probe within 10 s -> "BRIDGED" (this client can only host through the bridge).
 *           LobbySettings.ini [LobbyServer] ForceBridge = true skips the test and always bridges.
 *  connect  to the lobby: tag the connection ("SADKB1 LOBBY <token>" before TinCat's first byte);
 *           to a bridged game's virtual address: redirect to the relay ("SADKB1 JOIN <game id>").
 *  send     writes a pending tag first.
 *  listen   the host's match server came up: "HOSTING <port>".   closesocket  of it: "STOPPED".
 *  "OPEN <channel>" from the stub: a data connection to the stub, piped to 127.0.0.1:<host port>.
 */
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ordinals.h"

void *real_ptrs[N_EXPORTS];
static HMODULE real_dll;
static char log_path[MAX_PATH], game_root[MAX_PATH];

#define REAL(name) ((name##_fn)real_ptrs[IDX_##name])
typedef SOCKET (WINAPI *socket_fn)(int, int, int);
typedef int (WINAPI *connect_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WINAPI *send_fn)(SOCKET, const char *, int, int);
typedef int (WINAPI *recv_fn)(SOCKET, char *, int, int);
typedef int (WINAPI *listen_fn)(SOCKET, int);
typedef int (WINAPI *bind_fn)(SOCKET, const struct sockaddr *, int);
typedef SOCKET (WINAPI *accept_fn)(SOCKET, struct sockaddr *, int *);
typedef int (WINAPI *closesocket_fn)(SOCKET);
typedef int (WINAPI *select_fn)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
typedef int (WINAPI *getsockname_fn)(SOCKET, struct sockaddr *, int *);
typedef int (WINAPI *ioctlsocket_fn)(SOCKET, long, u_long *);
typedef int (WINAPI *setsockopt_fn)(SOCKET, int, int, const char *, int);
typedef struct hostent *(WINAPI *gethostbyname_fn)(const char *);
typedef unsigned long (WINAPI *inet_addr_fn)(const char *);
typedef int (WINAPI *WSAGetLastError_fn)(void);
typedef int (WINAPI *WSAStartup_fn)(WORD, LPWSADATA);
typedef int (__stdcall *__WSAFDIsSet_fn)(SOCKET, fd_set *);

static unsigned short swap16(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }

/* ── Log ─────────────────────────────────────────────────────────────────── */
static CRITICAL_SECTION log_cs;
static void log_line(const char *fmt, ...)
{
    EnterCriticalSection(&log_cs);
    FILE *f = fopen(log_path, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&log_cs);
}

/* ── State ───────────────────────────────────────────────────────────────── */
static char token[33];
static unsigned long lobby_ip;            /* network order */
static unsigned short lobby_port, bridge_port, game_port, relay_port;
static unsigned long vip;                 /* network order */
static unsigned vbase;
static volatile LONG welcomed, bridged;   /* WELCOME received / hosting goes through the bridge */
static int force_bridge;                  /* LobbySettings.ini [LobbyServer] ForceBridge = true */
static HANDLE welcome_done;               /* set once the startup handshake has finished either way */
static SOCKET control = INVALID_SOCKET;
static CRITICAL_SECTION control_cs;
static volatile SOCKET hosting_sock = INVALID_SOCKET;
static volatile unsigned short hosting_port;

enum { TAG_NONE, TAG_LOBBY, TAG_JOIN };
#define MAX_TAGS 64
static struct { SOCKET s; int kind; unsigned game; } tags[MAX_TAGS];
static CRITICAL_SECTION tags_cs;

static void tag_set(SOCKET s, int kind, unsigned game)
{
    EnterCriticalSection(&tags_cs);
    for (int i = 0; i < MAX_TAGS; i++)
        if (tags[i].kind == TAG_NONE || tags[i].s == s) { tags[i].s = s; tags[i].kind = kind; tags[i].game = game; break; }
    LeaveCriticalSection(&tags_cs);
}

static int tag_take(SOCKET s, unsigned *game)
{
    int kind = TAG_NONE;
    EnterCriticalSection(&tags_cs);
    for (int i = 0; i < MAX_TAGS; i++)
        if (tags[i].kind != TAG_NONE && tags[i].s == s) { kind = tags[i].kind; *game = tags[i].game; tags[i].kind = TAG_NONE; break; }
    LeaveCriticalSection(&tags_cs);
    return kind;
}

/* ── Socket helpers (all through the real functions) ─────────────────────── */
static int send_all(SOCKET s, const char *buf, int len)
{
    DWORD start = GetTickCount();
    while (len > 0) {
        int n = REAL(send)(s, buf, len, 0);
        if (n > 0) { buf += n; len -= n; continue; }
        if (REAL(WSAGetLastError)() != WSAEWOULDBLOCK || GetTickCount() - start > 3000) return -1;
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv = {0, 100000};
        REAL(select)(0, NULL, &w, NULL, &tv);
    }
    return 0;
}

static void control_send(const char *line)
{
    EnterCriticalSection(&control_cs);
    if (control != INVALID_SOCKET) send_all(control, line, (int)strlen(line));
    LeaveCriticalSection(&control_cs);
}

static SOCKET tcp_connect(unsigned long ip, unsigned short port, int timeout_ms)
{
    SOCKET s = REAL(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = swap16(port); a.sin_addr.s_addr = ip;
    u_long nb = 1;
    REAL(ioctlsocket)(s, FIONBIO, &nb);
    if (REAL(connect)(s, (struct sockaddr *)&a, sizeof a) != 0 && REAL(WSAGetLastError)() != WSAEWOULDBLOCK) {
        REAL(closesocket)(s); return INVALID_SOCKET;
    }
    fd_set w, e; FD_ZERO(&w); FD_SET(s, &w); FD_ZERO(&e); FD_SET(s, &e);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (REAL(select)(0, NULL, &w, &e, &tv) != 1 || !((__WSAFDIsSet_fn)real_ptrs[IDX___WSAFDIsSet])(s, &w)) {
        REAL(closesocket)(s); return INVALID_SOCKET;
    }
    nb = 0;
    REAL(ioctlsocket)(s, FIONBIO, &nb);
    return s;
}

/* Read one '\n'-terminated line (without it); 0 on success. */
static int recv_line(SOCKET s, char *out, int cap, int timeout_ms)
{
    int n = 0;
    for (;;) {
        if (timeout_ms >= 0) {
            fd_set r; FD_ZERO(&r); FD_SET(s, &r);
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (REAL(select)(0, &r, NULL, NULL, &tv) != 1) return -1;
        }
        char c;
        if (REAL(recv)(s, &c, 1, 0) != 1) return -1;
        if (c == '\n') { out[n] = 0; return 0; }
        if (n < cap - 1) out[n++] = c;
    }
}

/* ── Data channel: stub <-> 127.0.0.1:<host port> ────────────────────────── */
static DWORD WINAPI channel_thread(LPVOID arg)
{
    unsigned channel = (unsigned)(UINT_PTR)arg;
    SOCKET up = tcp_connect(lobby_ip, bridge_port, 5000);
    SOCKET local = tcp_connect(0x0100007f, hosting_port ? hosting_port : game_port, 3000);
    if (up == INVALID_SOCKET || local == INVALID_SOCKET) {
        log_line("channel %u: could not connect (stub %s, local %s)", channel,
                 up == INVALID_SOCKET ? "FAILED" : "ok", local == INVALID_SOCKET ? "FAILED" : "ok");
        if (up != INVALID_SOCKET) REAL(closesocket)(up);
        if (local != INVALID_SOCKET) REAL(closesocket)(local);
        return 0;
    }
    char line[96];
    sprintf(line, "SADKB1 DATA %s %u\n", token, channel);
    send_all(up, line, (int)strlen(line));
    log_line("channel %u: joiner connected to the local match server", channel);
    char buf[16384];
    for (;;) {
        fd_set r; FD_ZERO(&r); FD_SET(up, &r); FD_SET(local, &r);
        if (REAL(select)(0, &r, NULL, NULL, NULL) <= 0) break;
        SOCKET from = ((__WSAFDIsSet_fn)real_ptrs[IDX___WSAFDIsSet])(up, &r) ? up : local;
        SOCKET to = from == up ? local : up;
        int n = REAL(recv)(from, buf, sizeof buf, 0);
        if (n <= 0 || send_all(to, buf, n) != 0) break;
    }
    REAL(closesocket)(up);
    REAL(closesocket)(local);
    log_line("channel %u: closed", channel);
    return 0;
}

/* ── Startup: config, control connection, reachability test ──────────────── */
static void read_config(void)
{
    char path[MAX_PATH], host[256];
    snprintf(path, sizeof path, "%s\\data\\lobby\\config\\LobbySettings.ini", game_root);
    GetPrivateProfileStringA("LobbyServer", "Host", "", host, sizeof host, path);
    lobby_port = (unsigned short)GetPrivateProfileIntA("LobbyServer", "Port", 7070, path);
    char fb[16];
    GetPrivateProfileStringA("LobbyServer", "ForceBridge", "false", fb, sizeof fb, path);
    char *f = fb;
    while (*f == ' ' || *f == '"') f++;
    force_bridge = !_strnicmp(f, "true", 4) || *f == '1' || !_strnicmp(f, "yes", 3);
    char *h = host;                                         /* tolerate Host = "1.2.3.4" */
    while (*h == ' ' || *h == '"') h++;
    for (char *e = h + strlen(h); e > h && (e[-1] == ' ' || e[-1] == '"'); ) *--e = 0;
    lobby_ip = REAL(inet_addr)(h);
    if (lobby_ip == INADDR_NONE && *h) {
        struct hostent *he = REAL(gethostbyname)(h);
        if (he && he->h_addr_list[0]) memcpy(&lobby_ip, he->h_addr_list[0], 4);
    }
    snprintf(path, sizeof path, "%s\\data\\game\\settings\\network.ini", game_root);
    game_port = (unsigned short)GetPrivateProfileIntA("Basics", "gamePort", 5479, path);
    char own[MAX_PATH];
    snprintf(own, sizeof own, "%s\\bin\\sadk_bridge.ini", game_root);
    bridge_port = (unsigned short)GetPrivateProfileIntA("Bridge", "port", 7072, own);
    log_line("config: lobby %s:%u (%s), game port %u, bridge port %u, ForceBridge %s", h, lobby_port,
             lobby_ip == INADDR_NONE ? "UNRESOLVED" : "resolved", game_port, bridge_port,
             force_bridge ? "true" : "false");
}

static int reachability_test(void)
{
    SOCKET l = REAL(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = swap16(game_port);
    if (l == INVALID_SOCKET || REAL(bind)(l, (struct sockaddr *)&a, sizeof a) != 0 || REAL(listen)(l, 4) != 0) {
        log_line("reachability: cannot listen on port %u (error %d) - assuming bridged", game_port,
                 REAL(WSAGetLastError)());
        if (l != INVALID_SOCKET) REAL(closesocket)(l);
        return 0;
    }
    char nonce[17], line[96];
    sprintf(nonce, "%08lx%08lx", (unsigned long)GetTickCount(), (unsigned long)GetCurrentProcessId() * 2654435761UL);
    sprintf(line, "CHECK %u %s\n", game_port, nonce);
    control_send(line);
    int ok = 0;
    DWORD start = GetTickCount();
    while (!ok && GetTickCount() - start < 10000) {
        fd_set r; FD_ZERO(&r); FD_SET(l, &r);
        struct timeval tv = {0, 250000};
        if (REAL(select)(0, &r, NULL, NULL, &tv) != 1) continue;
        SOCKET in = REAL(accept)(l, NULL, NULL);
        if (in == INVALID_SOCKET) continue;
        char got[96], want[64];
        sprintf(want, "SADKB1 PROBE %s", nonce);
        if (recv_line(in, got, sizeof got, 3000) == 0 && strcmp(got, want) == 0) ok = 1;
        REAL(closesocket)(in);
    }
    REAL(closesocket)(l);
    return ok;
}

static DWORD WINAPI startup_thread(LPVOID arg)
{
    (void)arg;
    /* The token names this client to the stub; generated here, not in DllMain (loader lock). */
    unsigned char rnd[16];
    HCRYPTPROV prov;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (!CryptGenRandom(prov, sizeof rnd, rnd)) prov = 0;
        CryptReleaseContext(prov, 0);
    } else {
        prov = 0;
    }
    if (!prov) { srand(GetTickCount() ^ GetCurrentProcessId()); for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)rand(); }
    for (int i = 0; i < 16; i++) sprintf(token + 2 * i, "%02x", rnd[i]);
    WSADATA wsa;
    REAL(WSAStartup)(MAKEWORD(1, 1), &wsa);
    read_config();
    SOCKET c = lobby_ip == INADDR_NONE ? INVALID_SOCKET : tcp_connect(lobby_ip, bridge_port, 5000);
    char line[256];
    if (c == INVALID_SOCKET) {
        log_line("no bridge on the lobby server (port %u) - plain pass-through", bridge_port);
        SetEvent(welcome_done);
        return 0;
    }
    sprintf(line, "SADKB1 HELLO %s\n", token);
    send_all(c, line, (int)strlen(line));
    if (recv_line(c, line, sizeof line, 5000) != 0 ||
        sscanf(line, "WELCOME relay=%hu vbase=%u", &relay_port, &vbase) != 2) {
        log_line("bridge did not answer HELLO - plain pass-through");
        REAL(closesocket)(c);
        SetEvent(welcome_done);
        return 0;
    }
    char *v = strstr(line, "vip=");
    vip = v ? REAL(inet_addr)(v + 4) : INADDR_NONE;
    EnterCriticalSection(&control_cs);
    control = c;
    LeaveCriticalSection(&control_cs);
    InterlockedExchange(&welcomed, 1);
    SetEvent(welcome_done);
    log_line("bridge connected: token %.8s..., relay port %u, virtual ports from %u at %s", token, relay_port,
             vbase, v ? v + 4 : "?");

    int reachable = 0;
    if (force_bridge) {
        log_line("reachability: not tested (ForceBridge = true) - hosting goes through the bridge");
    } else {
        reachable = reachability_test();
        log_line("reachability: port %u %s", game_port,
                 reachable ? "is reachable from the server - hosting works directly"
                           : "is NOT reachable from the server - hosting goes through the bridge");
    }
    if (!reachable) {
        InterlockedExchange(&bridged, 1);
        control_send("BRIDGED\n");
    }
    for (;;) {                                           /* stub -> shim: OPEN <channel> (CHECKED is info) */
        if (recv_line(c, line, sizeof line, -1) != 0) break;
        unsigned channel;
        if (sscanf(line, "OPEN %u", &channel) == 1)
            CloseHandle(CreateThread(NULL, 0, channel_thread, (LPVOID)(UINT_PTR)channel, 0, NULL));
    }
    log_line("bridge control connection lost");
    EnterCriticalSection(&control_cs);
    REAL(closesocket)(control);
    control = INVALID_SOCKET;
    LeaveCriticalSection(&control_cs);
    InterlockedExchange(&welcomed, 0);
    return 0;
}

/* ── Map sharing: patches to SADK.exe in memory (docs/BINARY_PATCHES.md, "Map sharing") ───────────
 * The client already transfers a missing map from the host (S2TFTP over the match connection,
 * NComm::Manager::HandleEvent S 0040e560), but only for maps the host advertises with type 3
 * (Documents\SAdK\maps) AND "download allowed" (+0x228), which OnMapSelected always sends as 0.
 *   1. OnMapSelected S 00454ad0: advertise every map as type 3 with downloads allowed.
 *   2. SelectMapDialog::RefreshMapList S 0045a300: also list Documents\SAdK\maps (location 3).
 *   3. S2Tftp_Session_ReadNextBlock S 00426ac0 (host serving a file): only maps, never anything else;
 *      a map that is not in Documents\SAdK\maps is served from the game's own map folder.
 *   4. S2TftpSession::CloseFile S 00427990 (joiner finishing a download): the host-chosen file name
 *      may only land in Documents\SAdK\maps as .s2m/.bmp.
 * Applied on the first connect (the exe's code is unpacked by then, also for the SecuROM build),
 * and only where the original bytes match exactly. */
typedef int (__cdecl *fopen_s_fn)(FILE **, const char *, const char *);
typedef int (__cdecl *rename_fn)(const char *, const char *);
typedef int (__cdecl *remove_fn)(const char *);
typedef void (__stdcall *append_maps_fn)(int, void *, void *, float *);
#define GAME_FOPEN_S ((fopen_s_fn)0x006f5f5f)
#define GAME_RENAME  ((rename_fn)0x006f6731)
#define GAME_REMOVE  ((remove_fn)0x006f6789)
#define GAME_APPEND_MAPS ((append_maps_fn)0x00459f40)
#define MAP_LOCATION_USER 3

static char maps_dir[MAX_PATH];           /* <My Documents>\SAdK\maps\ */

/* The file part of `path` if it is <maps_dir><plain name>.s2m|.bmp, else NULL. */
static const char *map_file_name(const char *path)
{
    size_t n = strlen(maps_dir);
    if (!maps_dir[0] || _strnicmp(path, maps_dir, n) != 0) return NULL;
    const char *name = path + n;
    size_t len = strlen(name);
    if (len < 5 || len > 120 || strstr(name, "..")) return NULL;
    for (const char *c = name; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':' || (unsigned char)*c < 0x20) return NULL;
    if (_stricmp(name + len - 4, ".s2m") != 0 && _stricmp(name + len - 4, ".bmp") != 0) return NULL;
    return name;
}

/* ── Download progress, refresh and the ready guard ─────────────────────────────────────────────
 * Starting a match while the joiner still lacks the map crashes the client, so "ready" is refused
 * until the map is present and no download is running. Progress goes to the pre-game room through
 * the game's own NComm_SendUINotification S 0040e3d0 (the "Player joined game" channel). When the
 * last download completes the joiner sends PlayerReady(0) once: the host answers every ready change
 * by re-broadcasting the game information, and the joiner then finds the map and refreshes the room. */
#define THISCALL __attribute__((thiscall))
typedef void *(__cdecl *get_mgr_fn)(void);
typedef void (THISCALL *notify_fn)(void *, const char *);
typedef char (THISCALL *send_ready_fn)(void *, int);
typedef void *(THISCALL *get_guid_fn)(void *, void *);
typedef void (THISCALL *to_uuid_fn)(void *, void *);
typedef char (__cdecl *find_map_fn)(void *, void *, int *);
typedef FILE *(__cdecl *open_temp_fn)(void *);
typedef size_t (__cdecl *fwrite_fn)(const void *, size_t, size_t, FILE *);
typedef void (__cdecl *free_fn)(void *);
#define GAME_GET_MANAGER   ((get_mgr_fn)0x00408290)       /* NComm_GetManager */
#define GAME_NOTIFY        ((notify_fn)0x0040e3d0)        /* NComm_SendUINotification(mgr, text) */
#define GAME_SEND_READY    ((send_ready_fn)0x00408d80)    /* NComm_Manager::SendPlayerReadyEvent */
#define GAME_GET_MAP_GUID  ((get_guid_fn)0x00413c20)      /* EventGameInformation::GetMapGuid */
#define GAME_GUID_TO_UUID  ((to_uuid_fn)0x004163e0)       /* NetGUID::ToUUID */
#define GAME_FIND_MAP      ((find_map_fn)0x005ac2f0)      /* GameFile_FindMapByGuidAnyType */
#define GAME_OPEN_TEMP     ((open_temp_fn)0x004262e0)     /* S2Tftp_OpenTempFile */
#define GAME_FWRITE        ((fwrite_fn)0x006f5197)
#define GAME_FREE          ((free_fn)0x006f231e)

static volatile LONG downloads_active;
static unsigned long download_bytes, download_reported;
static DWORD download_last_activity;

static void room_message(const char *text)
{
    void *mgr = GAME_GET_MANAGER();
    if (mgr) GAME_NOTIFY(mgr, text);
}

static int download_running(void)
{
    if (downloads_active > 0 && GetTickCount() - download_last_activity > 60000) {
        log_line("map share: download stalled for 60 s - no longer blocking ready");
        InterlockedExchange(&downloads_active, 0);
    }
    return downloads_active > 0;
}

static void download_finished(void)
{
    if (InterlockedDecrement(&downloads_active) <= 0) {
        InterlockedExchange(&downloads_active, 0);
        void *mgr = GAME_GET_MANAGER();
        if (mgr) GAME_SEND_READY(mgr, 0);                 /* host re-broadcasts -> room refreshes */
    }
}

static int session_map_present(void *mgr)
{
    unsigned char guid[32] = {0}, uuid[16] = {0};
    unsigned char name[28] = {0};                          /* MSVC std::string: +4 buf, +0x14 size, +0x18 cap */
    int type = 0;
    *(unsigned *)(name + 0x18) = 15;
    void *g = GAME_GET_MAP_GUID((char *)mgr + 0xdc, guid);
    GAME_GUID_TO_UUID(g, uuid);
    int found = GAME_FIND_MAP(uuid, name, &type) != 0;
    if (*(unsigned *)(name + 0x18) >= 16) GAME_FREE(*(void **)(name + 4));
    return found;
}

static FILE *__cdecl shim_open_temp(void *out_path)
{
    FILE *f = GAME_OPEN_TEMP(out_path);
    if (f) {
        InterlockedIncrement(&downloads_active);
        download_bytes = download_reported = 0;
        download_last_activity = GetTickCount();
        room_message("Downloading a map file from the host...");
        log_line("map share: download started");
    }
    return f;
}

static size_t __cdecl shim_fwrite(const void *buf, size_t size, size_t count, FILE *f)
{
    size_t n = GAME_FWRITE(buf, size, count, f);
    download_bytes += n * size;
    download_last_activity = GetTickCount();
    if (download_bytes - download_reported >= 256 * 1024) {
        char msg[96];
        download_reported = download_bytes;
        snprintf(msg, sizeof msg, "Map download: %lu KB received...", download_bytes / 1024);
        room_message(msg);
    }
    return n;
}

static int __cdecl shim_abort_remove(const char *path)
{
    log_line("map share: download aborted");
    room_message("Map download aborted.");
    int rc = GAME_REMOVE(path);
    download_finished();
    return rc;
}

static char THISCALL shim_send_ready(void *mgr, int ready)
{
    if ((ready & 0xff) && (download_running() || !session_map_present(mgr))) {
        room_message(download_running() ? "The map is still downloading - you can get ready once it is complete."
                                        : "You don't have this map (yet) - you can't get ready.");
        log_line("map share: ready refused (%s)", download_running() ? "download running" : "map missing");
        return GAME_SEND_READY(mgr, 0);
    }
    return GAME_SEND_READY(mgr, ready);
}

static int __cdecl shim_tftp_fopen(FILE **f, const char *path, const char *mode)
{
    const char *name = map_file_name(path);
    *f = NULL;
    if (!name) {
        log_line("map share: refused a request for %s (not a map file)", path);
        return 2;                                           /* ENOENT -> !TFTP_ERROR_FILENOTFOUND */
    }
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        log_line("map share: sending %s", name);
        return GAME_FOPEN_S(f, path, mode);
    }
    char stock[MAX_PATH];
    snprintf(stock, sizeof stock, "%s\\data\\game\\maps\\Freegamemaps\\%s", game_root, name);
    if (GetFileAttributesA(stock) != INVALID_FILE_ATTRIBUTES) {
        log_line("map share: sending %s from the game's map folder", name);
        return GAME_FOPEN_S(f, stock, mode);
    }
    log_line("map share: %s not found", name);
    return 2;
}

static int __cdecl shim_tftp_rename(const char *from, const char *to)
{
    if (!map_file_name(to)) {
        log_line("map share: refused to store a download as %s", to);
        GAME_REMOVE(from);
        download_finished();
        return -1;
    }
    int rc = GAME_RENAME(from, to);
    char msg[160];
    snprintf(msg, sizeof msg, "Map file received: %s (%lu KB).", to + strlen(maps_dir), download_bytes / 1024);
    room_message(msg);
    log_line("map share: received %s", to + strlen(maps_dir));
    download_finished();
    return rc;
}

static void __stdcall shim_append_maps(int location, void *list, void *names, float *rgba)
{
    static float user_colour[4] = {1.0f, 0.85f, 0.55f, 1.0f};   /* custom maps: warm tint */
    GAME_APPEND_MAPS(location, list, names, rgba);
    GAME_APPEND_MAPS(MAP_LOCATION_USER, list, names, user_colour);
}

static int patch_bytes(unsigned addr, const unsigned char *expect, const unsigned char *repl, size_t n,
                       const char *what)
{
    unsigned char *p = (unsigned char *)(UINT_PTR)addr;
    DWORD old;
    if (IsBadReadPtr(p, n) || memcmp(p, expect, n) != 0) {
        if (memcmp(p, repl, n) == 0) return 1;              /* already applied */
        log_line("patch %s at %08x: unexpected bytes - not applied", what, addr);
        return 0;
    }
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, repl, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    log_line("patch %s at %08x: applied", what, addr);
    return 1;
}

static int patch_call(unsigned addr, unsigned expect_target, void *new_target, const char *what)
{
    unsigned char expect[5] = {0xE8}, repl[5] = {0xE8};
    *(int *)(expect + 1) = (int)(expect_target - (addr + 5));
    *(int *)(repl + 1) = (int)((unsigned)(UINT_PTR)new_target - (addr + 5));
    return patch_bytes(addr, expect, repl, 5, what);
}

static void apply_map_patches(void)
{
    static volatile LONG done;
    if (InterlockedExchange(&done, 1)) return;
    char docs[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs) == S_OK) {   /* as Sys_GetMyDocumentsPath */
        char d[MAX_PATH];
        snprintf(d, sizeof d, "%s\\SAdK", docs);
        CreateDirectoryA(d, NULL);
        snprintf(maps_dir, sizeof maps_dir, "%s\\SAdK\\maps\\", docs);
        CreateDirectoryA(maps_dir, NULL);
    }
    /* OnMapSelected: MOV EDX,[ESP+0x21C] / MOV EAX,[ESI+0xB6C] / PUSH ECX / PUSH EBX / PUSH EDX / PUSH EAX
       -> MOV EAX,[ESI+0xB6C] / PUSH ECX / PUSH 1 (download allowed) / PUSH 3 (type) / PUSH EAX / NOP x5 */
    static const unsigned char sel_old[17] = {0x8B,0x94,0x24,0x1C,0x02,0x00,0x00, 0x8B,0x86,0x6C,0x0B,0x00,0x00,
                                              0x51,0x53,0x52,0x50};
    static const unsigned char sel_new[17] = {0x8B,0x86,0x6C,0x0B,0x00,0x00, 0x51, 0x6A,0x01, 0x6A,0x03, 0x50,
                                              0x90,0x90,0x90,0x90,0x90};
    int ok = patch_bytes(0x00454c37, sel_old, sel_new, sizeof sel_old, "map advertised downloadable");
    ok += patch_call(0x0045a381, 0x00459f40, (void *)shim_append_maps, "map list + Documents\\SAdK\\maps");
    ok += patch_call(0x00426b14, 0x006f5f5f, (void *)shim_tftp_fopen, "map server filter");
    ok += patch_call(0x00427b29, 0x006f6731, (void *)shim_tftp_rename, "download destination check 1");
    ok += patch_call(0x00427b7e, 0x006f6731, (void *)shim_tftp_rename, "download destination check 2");
    ok += patch_call(0x00457f51, 0x00408d80, (void *)shim_send_ready, "ready guard (Ready button)");
    ok += patch_call(0x00426ca5, 0x004262e0, (void *)shim_open_temp, "download start");
    ok += patch_call(0x00426d14, 0x006f5197, (void *)shim_fwrite, "download progress");
    ok += patch_call(0x00427a08, 0x006f6789, (void *)shim_abort_remove, "download abort");
    log_line("map sharing: %d of 9 patches active (maps folder %s)", ok, maps_dir);
}

/* ── Hooks ───────────────────────────────────────────────────────────────── */
int WINAPI shim_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)name;
    if (name && namelen >= (int)sizeof *in && in->sin_family == AF_INET) {
        WaitForSingleObject(welcome_done, 3000);       /* a fast login must not overtake HELLO */
        apply_map_patches();
        unsigned short port = swap16(in->sin_port);
        if (welcomed && in->sin_addr.s_addr == lobby_ip && port == lobby_port) {
            tag_set(s, TAG_LOBBY, 0);
            log_line("connect: lobby connection tagged");
        } else if (welcomed && in->sin_addr.s_addr == vip && port >= vbase && port < vbase + 20000) {
            struct sockaddr_in to = *in;
            to.sin_addr.s_addr = lobby_ip;
            to.sin_port = swap16(relay_port);
            tag_set(s, TAG_JOIN, port - vbase);
            log_line("connect: bridged game %u -> relay port %u", port - vbase, relay_port);
            return REAL(connect)(s, (const struct sockaddr *)&to, sizeof to);
        }
    }
    return REAL(connect)(s, name, namelen);
}

int WINAPI shim_send(SOCKET s, const char *buf, int len, int flags)
{
    unsigned game = 0;
    int kind = tag_take(s, &game);
    if (kind != TAG_NONE) {
        char line[96];
        if (kind == TAG_LOBBY) sprintf(line, "SADKB1 LOBBY %s\n", token);
        else sprintf(line, "SADKB1 JOIN %u\n", game);
        if (send_all(s, line, (int)strlen(line)) != 0) log_line("send: could not write the bridge tag");
    }
    return REAL(send)(s, buf, len, flags);
}

int WINAPI shim_listen(SOCKET s, int backlog)
{
    int rc = REAL(listen)(s, backlog);
    struct sockaddr_in a;
    int len = sizeof a;
    unsigned port = 0;
    if (REAL(getsockname)(s, (struct sockaddr *)&a, &len) == 0) port = swap16(a.sin_port);
    log_line("listen(socket %u, backlog %d) on port %u -> %d", (unsigned)s, backlog, port, rc);
    if (rc == 0 && port == game_port) {
        hosting_sock = s;
        hosting_port = (unsigned short)port;
        char line[32];
        sprintf(line, "HOSTING %u\n", port);
        control_send(line);
        log_line("hosting on port %u%s", port, bridged ? " - joiners come through the bridge" : "");
    }
    return rc;
}

int WINAPI shim_closesocket(SOCKET s)
{
    unsigned game;
    tag_take(s, &game);
    if (s == hosting_sock && s != INVALID_SOCKET) {
        hosting_sock = INVALID_SOCKET;
        control_send("STOPPED\n");
        log_line("hosting stopped");
    }
    return REAL(closesocket)(s);
}

/* ── Load ────────────────────────────────────────────────────────────────── */
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    InitializeCriticalSection(&log_cs);
    InitializeCriticalSection(&control_cs);
    InitializeCriticalSection(&tags_cs);

    char exe[MAX_PATH], me[MAX_PATH], sys[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    GetModuleFileNameA(self, me, MAX_PATH);
    strcpy(log_path, exe);
    char *slash = strrchr(log_path, '\\');
    strcpy(slash ? slash + 1 : log_path, "wsock32_shim.txt");
    strcpy(game_root, exe);                                   /* <root>\bin\SADK.exe -> <root> */
    for (int up = 0; up < 2; up++) { char *p = strrchr(game_root, '\\'); if (p) *p = 0; }

    GetSystemDirectoryA(sys, MAX_PATH);                       /* SysWOW64 for this 32-bit process */
    strcat(sys, "\\wsock32.dll");
    real_dll = LoadLibraryA(sys);
    int missing = 0;
    for (int i = 0; i < N_EXPORTS; i++) {
        real_ptrs[i] = real_dll ? (void *)GetProcAddress(real_dll, MAKEINTRESOURCEA(ordinals[i])) : NULL;
        if (!real_ptrs[i]) missing++;
    }

    log_line("shim loaded: pid %lu, exe %s, shim %s, real %s (%s), %d of %d exports resolved",
             GetCurrentProcessId(), exe, me, sys, real_dll ? "ok" : "LOAD FAILED", N_EXPORTS - missing, N_EXPORTS);
    if (!real_dll) return FALSE;
    welcome_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, startup_thread, NULL, 0, NULL));
    return TRUE;
}
