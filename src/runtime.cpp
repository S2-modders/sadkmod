#include <sadkmod/game/sadk_noav/fn/_global.hpp>
#include <sadkmod/game/sadk_noav/module.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/msvc.hpp>
#include <sadkmod/runtime.hpp>

#include <windows.h>
#include <wincrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sadk {

// ── Log ──────────────────────────────────────────────────────────────────────────────────────────────────────────
static CRITICAL_SECTION log_cs;
static char log_file[MAX_PATH];
static bool log_ready;

void log_open(const char *path)
{
    if (!log_ready) InitializeCriticalSection(&log_cs);
    std::snprintf(log_file, sizeof log_file, "%s", path);
    log_ready = true;
}

void log(const char *fmt, ...)
{
    if (const sadkmod_api *h = host_api()) {          // in a mod: one line into the host's log
        char text[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(text, sizeof text, fmt, ap);
        va_end(ap);
        h->log_line(mod_name(), text);
        return;
    }
    if (!log_ready) return;
    EnterCriticalSection(&log_cs);
    if (FILE *f = std::fopen(log_file, "a")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }
    LeaveCriticalSection(&log_cs);
}

// ── Paths ────────────────────────────────────────────────────────────────────────────────────────────────────────
const char *exe_path()
{
    static char path[MAX_PATH];
    if (!path[0]) GetModuleFileNameA(nullptr, path, MAX_PATH);
    return path;
}

const char *game_root()
{
    static char root[MAX_PATH];
    if (!root[0]) {
        std::snprintf(root, sizeof root, "%s", exe_path());     // <root>\bin\SADK.exe -> <root>
        for (int up = 0; up < 2; up++)
            if (char *p = std::strrchr(root, '\\')) *p = 0;
    }
    return root;
}

std::string game_path(const char *relative) { return std::string(game_root()) + '\\' + relative; }

// ── Exe check ────────────────────────────────────────────────────────────────────────────────────────────────────
std::string file_md5(const char *path)
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        static unsigned char buf[65536];
        DWORD got;
        bool ok = true;
        while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got)
            if (!CryptHashData(hash, buf, got, 0)) { ok = false; break; }
        unsigned char digest[16];
        DWORD len = sizeof digest;
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
            char hex[33];
            for (int i = 0; i < 16; i++) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
            out = hex;
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(f);
    return out;
}

bool exe_is_supported()
{
    if (host_api()) return true;                      // the host only loads mods into the supported build
    static int state = -1;
    if (state < 0) state = file_md5(exe_path()) == game::md5;
    return state == 1;
}

// ── Settings ─────────────────────────────────────────────────────────────────────────────────────────────────────
std::string Ini::get(const char *section, const char *key, const char *fallback) const
{
    char v[1024];
    GetPrivateProfileStringA(section, key, fallback, v, sizeof v, path.c_str());
    std::string s = v;
    auto junk = [](char c) { return c == ' ' || c == '"' || c == '\t'; };
    while (!s.empty() && junk(s.back())) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && junk(s[i])) i++;
    return s.substr(i);
}

int Ini::get_int(const char *section, const char *key, int fallback) const
{
    return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, path.c_str()));
}

bool Ini::get_bool(const char *section, const char *key, bool fallback) const
{
    std::string v = get(section, key, fallback ? "true" : "false");
    return !_strnicmp(v.c_str(), "true", 4) || !_strnicmp(v.c_str(), "yes", 3) || v[0] == '1';
}

void *proc_address(const char *dll, const char *name)
{
    HMODULE m = GetModuleHandleA(dll);
    return m ? reinterpret_cast<void *>(GetProcAddress(m, name)) : nullptr;
}

// ── Mod side (mod.hpp) ───────────────────────────────────────────────────────────────────────────────────────────
static const sadkmod_api *bound_host;
void bind_host(const sadkmod_api *api) { bound_host = api; }
const sadkmod_api *host_api() { return bound_host; }
const char *mod_name() { return bound_host ? bound_host->mod_name : ""; }
const char *mod_dir() { return bound_host ? bound_host->mod_dir : ""; }

// ── Game heap ────────────────────────────────────────────────────────────────────────────────────────────────────
void *game_malloc(std::size_t size) { return game::fn::_malloc(size); }
void game_free(void *p) { game::fn::_free(p); }

// ── msvc::string ─────────────────────────────────────────────────────────────────────────────────────────────────
namespace msvc {

bool string::equals_icase(std::string_view s) const
{
    return s.size() == size && _strnicmp(data(), s.data(), size) == 0;
}

string string::small(std::string_view text)
{
    string s{};
    std::size_t n = text.size() < 15 ? text.size() : 15;
    std::memcpy(s.inline_text, text.data(), n);
    s.size = static_cast<std::uint32_t>(n);
    s.capacity = 15;
    return s;
}

string string::borrow(const char *text, std::size_t length)
{
    if (length < 16) return small({text, length});
    string s{};
    s.heap_text = const_cast<char *>(text);
    s.size = static_cast<std::uint32_t>(length);
    s.capacity = static_cast<std::uint32_t>(length);
    return s;
}

}  // namespace msvc
}  // namespace sadk
