#include <sadkmod/game/sadk_noav/fn/NBase.hpp>
#include <sadkmod/hook.hpp>
#include <sadkmod/host.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/registry.hpp>
#include <sadkmod/runtime.hpp>

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sadk::host {

namespace {

// ── Index ────────────────────────────────────────────────────────────────────────────────────────────────────────
// Built once before the hooks go in, read-only afterwards: no locking on the lookup path.
std::unordered_map<std::wstring, std::wstring> index;   // lowercase path below data\ -> the mod's file
std::unordered_set<std::wstring> modded_meshes;          // lowercase file names (no extension) of modded .kex
std::wstring data_prefix;                                // lowercase "<game>\data\"
std::wstring cache_prefix;                               // lowercase "%LOCALAPPDATA%\SAdK\"
std::wstring cache_redirect_dir;                         // "%LOCALAPPDATA%\SAdK\mods\" (original case)

struct ModInfo {
    std::string name, dir;
    std::wstring wdir;
};
std::vector<ModInfo> mods;

std::wstring widen(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s)
{
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool skipped(const wchar_t *name) { return name[0] == L'.' || name[0] == L'_'; }

// Every file below `dir` into the index as "<rel>\<name>"; returns how many.
int add_files(const std::wstring &dir, const std::wstring &rel, const std::string &mod)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
        std::wstring path = dir + L"\\" + fd.cFileName;
        std::wstring key = lower(rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += add_files(path, key, mod);
            continue;
        }
        if (index.count(key)) log("mods: %s overrides an earlier mod's data\\%s", mod.c_str(), narrow(key).c_str());
        index[key] = path;
        std::size_t slash = key.find_last_of(L'\\'), dot = key.find_last_of(L'.');
        if (dot != std::wstring::npos && key.compare(dot, std::wstring::npos, L".kex") == 0)
            modded_meshes.insert(key.substr(slash == std::wstring::npos ? 0 : slash + 1,
                                            dot - (slash == std::wstring::npos ? 0 : slash + 1)));
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

std::wstring full_path(const wchar_t *path)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(path, MAX_PATH * 2, buf, nullptr);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

// ── File hooks ───────────────────────────────────────────────────────────────────────────────────────────────────
using create_file_a_fn = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using create_file_w_fn = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
create_file_a_fn real_create_file_a;
create_file_w_fn real_create_file_w;

bool is_write(DWORD access, DWORD disposition)
{
    return (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) ||
           disposition == CREATE_ALWAYS || disposition == CREATE_NEW || disposition == TRUNCATE_EXISTING;
}

HANDLE WINAPI create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                            DWORD flags, HANDLE templ)
{
    const wchar_t *to = name ? redirect(name, is_write(access, disposition)) : nullptr;
    return real_create_file_w(to ? to : name, access, share, sa, disposition, flags, templ);
}

HANDLE WINAPI create_file_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                            DWORD flags, HANDLE templ)
{
    if (name) {
        wchar_t wide[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2) > 0)
            if (const wchar_t *to = redirect(wide, is_write(access, disposition)))
                return real_create_file_w(to, access, share, sa, disposition, flags, templ);
    }
    return real_create_file_a(name, access, share, sa, disposition, flags, templ);
}

// ── Plain files through the decrypter ───────────────────────────────────────────────────────────────────────────
// NBase::gDecryptData S 006e6660 crashes on a buffer without its header (FCC mismatch -> write to address 0).
// Every game file that reaches it has the header; a mod's file without it is plain and passes unchanged.
using Decrypt = Hook<game::fn::NBase::gDecryptData>;
const unsigned char sadk_magic[8] = {0x12, 0x18, 0x09, 0x06, 's', 'a', 'd', 'k'};

void SADK_CDECL decrypt_or_pass(void *file_name, void **data, std::uint32_t *size)
{
    if (data && *data && size && (*size < 8 || std::memcmp(*data, sadk_magic, 8) != 0)) return;
    Decrypt::original(file_name, data, size);
}

// ── mod.dll ──────────────────────────────────────────────────────────────────────────────────────────────────────
void SADK_CDECL host_log_line(const char *mod, const char *text) { log("[%s] %s", mod, text); }

// Everything a mod changes is recorded under its name (registry.hpp); the log label names the mod as well.
std::string label(const char *mod, const char *what) { return std::string("[") + mod + "] " + what; }

bool SADK_CDECL host_hook(const char *mod, void *target, void *detour, void **original, const char *what)
{
    return registry::hook(mod, target, detour, original, label(mod, what).c_str());
}

bool SADK_CDECL host_unhook(const char *mod, void *target, void *detour) { return registry::unhook(mod, target, detour); }

bool SADK_CDECL host_patch(const char *mod, std::uint32_t module, std::uintptr_t address, const std::uint8_t *expect,
                           const std::uint8_t *replace, std::size_t n, const char *what)
{
    auto m = static_cast<Module>(module);
    auto *at = module <= static_cast<std::uint32_t>(Module::tincat3) ? reinterpret_cast<std::uint8_t *>(resolve(m, address))
                                                                     : nullptr;
    if (!at) {
        log("patch [%s] %s at %08x: module not loaded - not applied", mod, what, unsigned(address));
        return false;
    }
    return registry::patch(mod, at, address, expect, replace, n, label(mod, what).c_str());
}

bool SADK_CDECL host_write_slot(const char *mod, void **slot, void *value, void **previous, const char *what)
{
    return registry::write_slot(mod, slot, value, previous, label(mod, what).c_str());
}

}  // namespace

Summary build_index(const char *root)
{
    Summary s;
    index.clear();
    modded_meshes.clear();
    mods.clear();
    std::wstring wroot = widen(root);
    data_prefix = lower(wroot + L"\\data\\");
    wchar_t local[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK) {   // as Sys_GetLocalAppDataPath
        cache_prefix = lower(std::wstring(local) + L"\\SAdK\\");
        cache_redirect_dir = std::wstring(local) + L"\\SAdK\\mods\\";
    }
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wroot + L"\\mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !skipped(fd.cFileName)) names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring &a, const std::wstring &b) { return lower(a) < lower(b); });
    for (auto &n : names) {
        ModInfo m{narrow(n), narrow(wroot + L"\\mods\\" + n), wroot + L"\\mods\\" + n};
        registry::set_rank(m.name.c_str(), static_cast<int>(mods.size()));   // hooks run in folder-name order
        int files = add_files(m.wdir + L"\\data", L"", m.name);
        log("mods: %s (%d data file%s%s)", m.name.c_str(), files, files == 1 ? "" : "s",
            GetFileAttributesW((m.wdir + L"\\mod.dll").c_str()) != INVALID_FILE_ATTRIBUTES ? ", mod.dll" : "");
        mods.push_back(std::move(m));
    }
    s.mods = static_cast<int>(mods.size());
    s.files = static_cast<int>(index.size());
    return s;
}

const wchar_t *redirect(const wchar_t *path, bool write)
{
    if (index.empty()) return nullptr;
    thread_local wchar_t result[MAX_PATH * 2];   // trivially destructible: no thread-exit hooks needed
    auto give = [&](const std::wstring &p) -> const wchar_t * {
        if (p.size() >= MAX_PATH * 2) return nullptr;
        std::wmemcpy(result, p.c_str(), p.size() + 1);
        return result;
    };
    std::wstring full = full_path(path);
    if (full.empty()) return nullptr;
    std::wstring low = lower(full);
    if (!write && low.size() > data_prefix.size() && low.compare(0, data_prefix.size(), data_prefix) == 0) {
        auto it = index.find(low.substr(data_prefix.size()));
        if (it == index.end()) return nullptr;
        return give(it->second);
    }
    // The mesh cache of a modded .KEX: <cache>\<mesh>[_<variant>].mshraw -> <cache>\mods\<same name>
    if (!modded_meshes.empty() && !cache_prefix.empty() && low.size() > cache_prefix.size() + 7 &&
        low.compare(0, cache_prefix.size(), cache_prefix) == 0 && low.compare(low.size() - 7, 7, L".mshraw") == 0) {
        std::wstring file = low.substr(cache_prefix.size());
        if (file.find(L'\\') != std::wstring::npos) return nullptr;
        std::wstring stem = file.substr(0, file.size() - 7);
        bool modded = modded_meshes.count(stem) > 0;
        for (std::size_t u = stem.find(L'_'); !modded && u != std::wstring::npos; u = stem.find(L'_', u + 1))
            modded = modded_meshes.count(stem.substr(0, u)) > 0;   // <mesh>_<variant>
        if (!modded) return nullptr;
        return give(cache_redirect_dir + full.substr(cache_prefix.size()));
    }
    return nullptr;
}

Summary start_mods()
{
    Summary s = build_index(game_root());
    if (!s.files && !s.mods) {
        log("mods: none");
        return s;
    }
    if (s.files) {
        if (!cache_redirect_dir.empty()) {   // %LOCALAPPDATA%\SAdK may not exist yet on a first start
            CreateDirectoryW(cache_redirect_dir.substr(0, cache_redirect_dir.size() - 5).c_str(), nullptr);
            CreateDirectoryW(cache_redirect_dir.c_str(), nullptr);
        }
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        bool ok = hook_function(reinterpret_cast<void *>(GetProcAddress(k32, "CreateFileW")),
                                reinterpret_cast<void *>(create_file_w), reinterpret_cast<void **>(&real_create_file_w),
                                "mod files (CreateFileW)") &&
                  hook_function(reinterpret_cast<void *>(GetProcAddress(k32, "CreateFileA")),
                                reinterpret_cast<void *>(create_file_a), reinterpret_cast<void **>(&real_create_file_a),
                                "mod files (CreateFileA)");
        ok = Decrypt::install(decrypt_or_pass, "mod files: plain files pass gDecryptData") && ok;
        log("mods: %d data file%s redirected%s", s.files, s.files == 1 ? "" : "s", ok ? "" : " - A HOOK FAILED, see above");
    }
    static std::vector<std::unique_ptr<sadkmod_api>> apis;   // must outlive the mods
    for (auto &m : mods) {
        std::wstring dll = m.wdir + L"\\mod.dll";
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // its helpers: its folder
        auto init = h ? reinterpret_cast<sadkmod_init_fn>(reinterpret_cast<void *>(GetProcAddress(h, "sadkmod_init")))
                      : nullptr;
        if (!init) {
            log("mods: %s\\mod.dll %s", m.name.c_str(), h ? "has no sadkmod_init export - not used" : "did not load");
            if (h) FreeLibrary(h);
            s.dll_failures++;
            continue;
        }
        auto api = std::make_unique<sadkmod_api>();
        api->version = SADKMOD_API_VERSION;
        api->size = sizeof(sadkmod_api);
        api->mod_name = m.name.c_str();
        api->mod_dir = m.dir.c_str();
        api->game_root = game_root();
        api->log_line = host_log_line;
        api->hook = host_hook;
        api->unhook = host_unhook;
        api->patch = host_patch;
        api->write_slot = host_write_slot;
        bool ok = init(api.get());
        apis.push_back(std::move(api));
        log("mods: %s\\mod.dll %s", m.name.c_str(), ok ? "started" : "reported a failure (or wants a newer host)");
        (ok ? s.dlls : s.dll_failures)++;
    }
    return s;
}

}  // namespace sadk::host
