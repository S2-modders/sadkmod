// The host bridge (docs/bridge-protocol.md; server side sadk_lobby/bridge.py).
//
//  startup  control connection to the lobby host's bridge port ("SADKB1 HELLO <token>"), then the
//           reachability test: listen on the game port, ask the stub to connect back ("CHECK"); no
//           probe within 10 s -> "BRIDGED" (this client can only host through the bridge).
//           LobbySettings.ini [LobbyServer] ForceBridge = true skips the test and always bridges.
//  connect  to the lobby: tag the connection ("SADKB1 LOBBY <token>" before TinCat's first byte);
//           to a bridged game's virtual address: redirect to the relay ("SADKB1 JOIN <game id>").
//  send     writes a pending tag first.
//  listen   the host's match server came up: "HOSTING <port>".   closesocket  of it: "STOPPED".
//  "OPEN <channel>" from the stub: a data connection to the stub, piped to 127.0.0.1:<host port>.
#include "shim.hpp"

#include <wincrypt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using sadk::log;

static unsigned short swap16(unsigned short v) { return static_cast<unsigned short>((v << 8) | (v >> 8)); }

// ── State ───────────────────────────────────────────────────────────────────────────────────────────────────────
static char token[33];
static unsigned long lobby_ip = INADDR_NONE;   // network order
static unsigned short relay_port;
static unsigned long vip;                      // network order
static unsigned vbase;
static volatile LONG welcomed, bridged;        // WELCOME received / hosting goes through the bridge
static HANDLE welcome_done;                    // set once the startup handshake has finished either way
static SOCKET control = INVALID_SOCKET;
static CRITICAL_SECTION control_cs;
static volatile SOCKET hosting_sock = INVALID_SOCKET;
static volatile unsigned short hosting_port;

enum { TAG_NONE, TAG_LOBBY, TAG_JOIN };
constexpr int MAX_TAGS = 64;
static struct {
    SOCKET s;
    int kind;
    unsigned game;
} tags[MAX_TAGS];
static CRITICAL_SECTION tags_cs;

// Called from DllMain: only what cannot fail and needs no other DLL.
void bridge_init()
{
    InitializeCriticalSection(&control_cs);
    InitializeCriticalSection(&tags_cs);
    welcome_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
}

void bridge_inactive() { SetEvent(welcome_done); }

static void tag_set(SOCKET s, int kind, unsigned game)
{
    EnterCriticalSection(&tags_cs);
    for (auto &t : tags)
        if (t.kind == TAG_NONE || t.s == s) {
            t.s = s;
            t.kind = kind;
            t.game = game;
            break;
        }
    LeaveCriticalSection(&tags_cs);
}

static int tag_take(SOCKET s, unsigned *game)
{
    int kind = TAG_NONE;
    EnterCriticalSection(&tags_cs);
    for (auto &t : tags)
        if (t.kind != TAG_NONE && t.s == s) {
            kind = t.kind;
            *game = t.game;
            t.kind = TAG_NONE;
            break;
        }
    LeaveCriticalSection(&tags_cs);
    return kind;
}

// ── Socket helpers (all through the real functions) ─────────────────────────────────────────────────────────────
static bool fd_isset(SOCKET s, fd_set *set) { return REAL(__WSAFDIsSet)(s, set) != 0; }

static int send_all(SOCKET s, const char *buf, int len)
{
    DWORD start = GetTickCount();
    while (len > 0) {
        int n = REAL(send)(s, buf, len, 0);
        if (n > 0) {
            buf += n;
            len -= n;
            continue;
        }
        if (REAL(WSAGetLastError)() != WSAEWOULDBLOCK || GetTickCount() - start > 3000) return -1;
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {0, 100000};
        REAL(select)(0, nullptr, &w, nullptr, &tv);
    }
    return 0;
}

static void control_send(const char *line)
{
    EnterCriticalSection(&control_cs);
    if (control != INVALID_SOCKET) send_all(control, line, static_cast<int>(std::strlen(line)));
    LeaveCriticalSection(&control_cs);
}

static SOCKET tcp_connect(unsigned long ip, unsigned short port, int timeout_ms)
{
    SOCKET s = REAL(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = swap16(port);
    a.sin_addr.s_addr = ip;
    u_long nb = 1;
    REAL(ioctlsocket)(s, FIONBIO, &nb);
    if (REAL(connect)(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 &&
        REAL(WSAGetLastError)() != WSAEWOULDBLOCK) {
        REAL(closesocket)(s);
        return INVALID_SOCKET;
    }
    fd_set w, e;
    FD_ZERO(&w);
    FD_SET(s, &w);
    FD_ZERO(&e);
    FD_SET(s, &e);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (REAL(select)(0, nullptr, &w, &e, &tv) != 1 || !fd_isset(s, &w)) {
        REAL(closesocket)(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    REAL(ioctlsocket)(s, FIONBIO, &nb);
    return s;
}

// Read one '\n'-terminated line (without it); 0 on success.
static int recv_line(SOCKET s, char *out, int cap, int timeout_ms)
{
    int n = 0;
    for (;;) {
        if (timeout_ms >= 0) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (REAL(select)(0, &r, nullptr, nullptr, &tv) != 1) return -1;
        }
        char c;
        if (REAL(recv)(s, &c, 1, 0) != 1) return -1;
        if (c == '\n') {
            out[n] = 0;
            return 0;
        }
        if (n < cap - 1) out[n++] = c;
    }
}

// ── Data channel: stub <-> 127.0.0.1:<host port> ────────────────────────────────────────────────────────────────
static DWORD WINAPI channel_thread(LPVOID arg)
{
    unsigned channel = static_cast<unsigned>(reinterpret_cast<UINT_PTR>(arg));
    SOCKET up = tcp_connect(lobby_ip, cfg.bridge_port, 5000);
    SOCKET local = tcp_connect(0x0100007f, hosting_port ? hosting_port : cfg.game_port, 3000);
    if (up == INVALID_SOCKET || local == INVALID_SOCKET) {
        log("channel %u: could not connect (stub %s, local %s)", channel, up == INVALID_SOCKET ? "FAILED" : "ok",
            local == INVALID_SOCKET ? "FAILED" : "ok");
        if (up != INVALID_SOCKET) REAL(closesocket)(up);
        if (local != INVALID_SOCKET) REAL(closesocket)(local);
        return 0;
    }
    char line[96];
    std::sprintf(line, "SADKB1 DATA %s %u\n", token, channel);
    send_all(up, line, static_cast<int>(std::strlen(line)));
    log("channel %u: joiner connected to the local match server", channel);
    char buf[16384];
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(up, &r);
        FD_SET(local, &r);
        if (REAL(select)(0, &r, nullptr, nullptr, nullptr) <= 0) break;
        SOCKET from = fd_isset(up, &r) ? up : local;
        SOCKET to = from == up ? local : up;
        int n = REAL(recv)(from, buf, sizeof buf, 0);
        if (n <= 0 || send_all(to, buf, n) != 0) break;
    }
    REAL(closesocket)(up);
    REAL(closesocket)(local);
    log("channel %u: closed", channel);
    return 0;
}

// ── Startup: control connection, reachability test ──────────────────────────────────────────────────────────────
static int reachability_test()
{
    SOCKET l = REAL(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = swap16(cfg.game_port);
    if (l == INVALID_SOCKET || REAL(bind)(l, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 ||
        REAL(listen)(l, 4) != 0) {
        log("reachability: cannot listen on port %u (error %d) - assuming bridged", cfg.game_port,
            REAL(WSAGetLastError)());
        if (l != INVALID_SOCKET) REAL(closesocket)(l);
        return 0;
    }
    char nonce[17], line[96];
    std::sprintf(nonce, "%08lx%08lx", static_cast<unsigned long>(GetTickCount()),
                 static_cast<unsigned long>(GetCurrentProcessId()) * 2654435761UL);
    std::sprintf(line, "CHECK %u %s\n", cfg.game_port, nonce);
    control_send(line);
    int ok = 0;
    DWORD start = GetTickCount();
    while (!ok && GetTickCount() - start < 10000) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(l, &r);
        struct timeval tv = {0, 250000};
        if (REAL(select)(0, &r, nullptr, nullptr, &tv) != 1) continue;
        SOCKET in = REAL(accept)(l, nullptr, nullptr);
        if (in == INVALID_SOCKET) continue;
        char got[96], want[64];
        std::sprintf(want, "SADKB1 PROBE %s", nonce);
        if (recv_line(in, got, sizeof got, 3000) == 0 && std::strcmp(got, want) == 0) ok = 1;
        REAL(closesocket)(in);
    }
    REAL(closesocket)(l);
    return ok;
}

// The token names this client to the stub; generated on the start-up thread, not in DllMain (loader lock).
void bridge_make_token()
{
    unsigned char rnd[16];
    HCRYPTPROV prov;
    bool ok = false;
    if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        ok = CryptGenRandom(prov, sizeof rnd, rnd);
        CryptReleaseContext(prov, 0);
    }
    if (!ok) {
        std::srand(GetTickCount() ^ GetCurrentProcessId());
        for (auto &b : rnd) b = static_cast<unsigned char>(std::rand());
    }
    for (int i = 0; i < 16; i++) std::sprintf(token + 2 * i, "%02x", rnd[i]);
    WSADATA wsa;
    REAL(WSAStartup)(MAKEWORD(1, 1), &wsa);
}

void bridge_startup()
{
    const char *h = cfg.host.c_str();
    lobby_ip = REAL(inet_addr)(h);
    if (lobby_ip == INADDR_NONE && *h) {
        struct hostent *he = REAL(gethostbyname)(h);
        if (he && he->h_addr_list[0]) std::memcpy(&lobby_ip, he->h_addr_list[0], 4);
    }
    log("config: lobby %s:%u (%s), game port %u, bridge port %u, ForceBridge %s, DisableBillboards %s", h,
        cfg.lobby_port, lobby_ip == INADDR_NONE ? "UNRESOLVED" : "resolved", cfg.game_port, cfg.bridge_port,
        cfg.force_bridge ? "true" : "false", cfg.disable_billboards ? "true" : "false");
    SOCKET c = lobby_ip == INADDR_NONE ? INVALID_SOCKET : tcp_connect(lobby_ip, cfg.bridge_port, 5000);
    char line[256];
    if (c == INVALID_SOCKET) {
        log("no bridge on the lobby server (port %u) - plain pass-through", cfg.bridge_port);
        SetEvent(welcome_done);
        return;
    }
    std::sprintf(line, "SADKB1 HELLO %s\n", token);
    send_all(c, line, static_cast<int>(std::strlen(line)));
    if (recv_line(c, line, sizeof line, 5000) != 0 ||
        std::sscanf(line, "WELCOME relay=%hu vbase=%u", &relay_port, &vbase) != 2) {
        log("bridge did not answer HELLO - plain pass-through");
        REAL(closesocket)(c);
        SetEvent(welcome_done);
        return;
    }
    char *v = std::strstr(line, "vip=");
    vip = v ? REAL(inet_addr)(v + 4) : INADDR_NONE;
    EnterCriticalSection(&control_cs);
    control = c;
    LeaveCriticalSection(&control_cs);
    InterlockedExchange(&welcomed, 1);
    SetEvent(welcome_done);
    log("bridge connected: token %.8s..., relay port %u, virtual ports from %u at %s", token, relay_port, vbase,
        v ? v + 4 : "?");

    int reachable = 0;
    if (cfg.force_bridge) {
        log("reachability: not tested (ForceBridge = true) - hosting goes through the bridge");
    } else {
        reachable = reachability_test();
        log("reachability: port %u %s", cfg.game_port,
            reachable ? "is reachable from the server - hosting works directly"
                      : "is NOT reachable from the server - hosting goes through the bridge");
    }
    if (!reachable) {
        InterlockedExchange(&bridged, 1);
        control_send("BRIDGED\n");
    }
    for (;;) {                                   // stub -> shim: OPEN <channel> (CHECKED is info)
        if (recv_line(c, line, sizeof line, -1) != 0) break;
        unsigned channel;
        if (std::sscanf(line, "OPEN %u", &channel) == 1)
            CloseHandle(CreateThread(nullptr, 0, channel_thread, reinterpret_cast<LPVOID>(UINT_PTR(channel)), 0, nullptr));
    }
    log("bridge control connection lost");
    EnterCriticalSection(&control_cs);
    REAL(closesocket)(control);
    control = INVALID_SOCKET;
    LeaveCriticalSection(&control_cs);
    InterlockedExchange(&welcomed, 0);
}

// ── Hooked exports (wsock32.def: connect, send, listen, closesocket) ────────────────────────────────────────────
extern "C" int WINAPI shim_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    auto *in = reinterpret_cast<const sockaddr_in *>(name);
    if (name && namelen >= static_cast<int>(sizeof *in) && in->sin_family == AF_INET) {
        WaitForSingleObject(welcome_done, 3000);     // a fast login must not overtake HELLO
        unsigned short port = swap16(in->sin_port);
        if (welcomed && in->sin_addr.s_addr == lobby_ip && port == cfg.lobby_port) {
            tag_set(s, TAG_LOBBY, 0);
            log("connect: lobby connection tagged");
        } else if (welcomed && in->sin_addr.s_addr == vip && port >= vbase && port < vbase + 20000) {
            sockaddr_in to = *in;
            to.sin_addr.s_addr = lobby_ip;
            to.sin_port = swap16(relay_port);
            tag_set(s, TAG_JOIN, port - vbase);
            log("connect: bridged game %u -> relay port %u", port - vbase, relay_port);
            return REAL(connect)(s, reinterpret_cast<sockaddr *>(&to), sizeof to);
        }
    }
    return REAL(connect)(s, name, namelen);
}

extern "C" int WINAPI shim_send(SOCKET s, const char *buf, int len, int flags)
{
    unsigned game = 0;
    int kind = tag_take(s, &game);
    if (kind != TAG_NONE) {
        char line[96];
        if (kind == TAG_LOBBY) std::sprintf(line, "SADKB1 LOBBY %s\n", token);
        else std::sprintf(line, "SADKB1 JOIN %u\n", game);
        if (send_all(s, line, static_cast<int>(std::strlen(line))) != 0) log("send: could not write the bridge tag");
    }
    return REAL(send)(s, buf, len, flags);
}

extern "C" int WINAPI shim_listen(SOCKET s, int backlog)
{
    int rc = REAL(listen)(s, backlog);
    sockaddr_in a;
    int len = sizeof a;
    unsigned port = 0;
    if (REAL(getsockname)(s, reinterpret_cast<sockaddr *>(&a), &len) == 0) port = swap16(a.sin_port);
    log("listen(socket %u, backlog %d) on port %u -> %d", static_cast<unsigned>(s), backlog, port, rc);
    if (rc == 0 && port == cfg.game_port) {
        hosting_sock = s;
        hosting_port = static_cast<unsigned short>(port);
        char line[32];
        std::sprintf(line, "HOSTING %u\n", port);
        control_send(line);
        log("hosting on port %u%s", port, bridged ? " - joiners come through the bridge" : "");
    }
    return rc;
}

extern "C" int WINAPI shim_closesocket(SOCKET s)
{
    unsigned game;
    tag_take(s, &game);
    if (s == hosting_sock && s != INVALID_SOCKET) {
        hosting_sock = INVALID_SOCKET;
        control_send("STOPPED\n");
        log("hosting stopped");
    }
    return REAL(closesocket)(s);
}
