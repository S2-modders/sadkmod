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

/* ── Hooks ───────────────────────────────────────────────────────────────── */
int WINAPI shim_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)name;
    if (name && namelen >= (int)sizeof *in && in->sin_family == AF_INET) {
        WaitForSingleObject(welcome_done, 3000);       /* a fast login must not overtake HELLO */
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
