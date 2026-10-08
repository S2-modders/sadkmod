#include <sadkmod/game/sadk_noav/fn/CApplicationEx.hpp>
#include <sadkmod/game/sadk_noav/fn/NBase.hpp>
#include <sadkmod/game/sadk_noav/fn/NComm_Manager.hpp>
#include <sadkmod/game/sadk_noav/fn/NProperties.hpp>
#include <sadkmod/game/sadk_noav/fn/_global.hpp>
#include <sadkmod/game/sadk_noav/fn/ai.hpp>
#include <sadkmod/hook.hpp>
#include <sadkmod/events.hpp>
#include <sadkmod/host.hpp>
#include <sadkmod/mod.hpp>
#include <sadkmod/msvc.hpp>
#include <sadkmod/registry.hpp>
#include <sadkmod/runtime.hpp>

#include <shlobj.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sadk::host {

namespace {

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

std::string lower(std::string s)
{
    if (!s.empty()) CharLowerBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

const char temp_prefix[] = ".temp_";
std::string without_temp(const std::string &folder)
{
    return folder.compare(0, sizeof temp_prefix - 1, temp_prefix) == 0 ? folder.substr(sizeof temp_prefix - 1) : folder;
}

bool exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

CRITICAL_SECTION host_cs;   // the mod list; index readers do not take it
struct Lock {
    Lock() { EnterCriticalSection(&host_cs); }
    ~Lock() { LeaveCriticalSection(&host_cs); }
};
bool host_ready = [] { InitializeCriticalSection(&host_cs); return true; }();

// ── Mods ─────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Mod {
    std::string folder, name, order, dir;   // order: the registry's order key (lowercase name)
    std::wstring wdir;
    std::uint32_t version = 0, flags = 0;
    bool has_dll = false, active = false, pending_free = false;
    HMODULE dll = nullptr;
    sadkmod_init_fn init = nullptr;
    sadkmod_stop_fn stop = nullptr;
    std::unique_ptr<sadkmod_api> api;   // lives as long as the mod is listed
};
std::vector<std::unique_ptr<Mod>> all;   // sorted by order key
std::wstring wroot;

Mod *find(const char *folder)
{
    for (auto &m : all)
        if (!_stricmp(m->folder.c_str(), folder)) return m.get();
    return nullptr;
}

// ── Index ────────────────────────────────────────────────────────────────────────────────────────────────────────
// A new index is built whenever the active mods change and swapped in whole; the file hooks read the current one
// without a lock. An index that was replaced is kept (a thread may still be reading it).
struct Index {
    std::unordered_map<std::wstring, std::wstring> files;   // lowercase path below data\ -> the mod's file
    std::unordered_map<std::wstring, std::string> owner;    // the same key -> the mod's folder
    std::unordered_set<std::wstring> meshes;                // lowercase file names (no extension) of modded .kex
    std::unordered_set<std::wstring> server;                // the keys a server mod provides
};
std::atomic<const Index *> current{nullptr};
std::vector<std::unique_ptr<Index>> indexes;
std::wstring data_prefix;          // lowercase "<game>\data\"
std::wstring cache_prefix;         // lowercase "%LOCALAPPDATA%\SAdK\"
std::wstring cache_redirect_dir;   // "%LOCALAPPDATA%\SAdK\mods\" (original case)

// Each conflict is reported once (the index is built again whenever the active mods change).
std::set<std::string> reported;

void conflict_file(const std::string &first, const std::string &second, const std::wstring &key)
{
    if (!reported.insert(first + '\n' + second + '\n' + narrow(key)).second) return;
    registry::conflict("The mods \"%s\" and \"%s\" both change the same game file:\n\ndata\\%s\n\nOnly one of them can "
                       "be used. Switch one of them off (rename its folder in the game's mods folder so that it starts "
                       "with _) and start the game again.",
                       first.c_str(), second.c_str(), narrow(key).c_str());
}

// Every file below `dir` into the index as "<rel>\<name>"; returns how many.
int add_files(Index &ix, const std::wstring &dir, const std::wstring &rel, const std::string &mod, bool server)
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
            n += add_files(ix, path, key, mod, server);
            continue;
        }
        if (auto it = ix.owner.find(key); it != ix.owner.end()) {   // the first mod keeps it
            conflict_file(it->second, mod, key);
            continue;
        }
        ix.files[key] = path;
        ix.owner[key] = mod;
        if (server) ix.server.insert(key);
        std::size_t slash = key.find_last_of(L'\\'), dot = key.find_last_of(L'.');
        std::size_t from = slash == std::wstring::npos ? 0 : slash + 1;
        if (dot != std::wstring::npos && key.compare(dot, std::wstring::npos, L".kex") == 0)
            ix.meshes.insert(key.substr(from, dot - from));
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// The property scripts: the game reads them only at start-up (docs/data-loading.md).
bool is_property_script(const std::wstring &key)
{
    std::size_t slash = key.find(L'\\');   // game\... or lobby\...
    return slash != std::wstring::npos && key.compare(slash + 1, 19, L"scripts\\properties\\") == 0;
}

std::map<std::wstring, std::wstring> property_files(const Index *ix)
{
    std::map<std::wstring, std::wstring> out;
    if (ix)
        for (auto &[k, v] : ix->files)
            if (is_property_script(k)) out[k] = v;
    return out;
}

bool file_hooks_installed;
bool properties_dirty;
void install_file_hooks();

// The index of the active mods, in order. Returns the number of files.
int rebuild_index()
{
    auto ix = std::make_unique<Index>();
    for (auto &m : all)
        if (m->active) add_files(*ix, m->wdir + L"\\data", L"", m->folder, (m->flags & SADKMOD_SERVER) != 0);
    int n = static_cast<int>(ix->files.size());
    const Index *old = current.load();
    if (property_files(old) != property_files(ix.get())) properties_dirty = true;
    current.store(ix.get());
    indexes.push_back(std::move(ix));
    if (n) install_file_hooks();
    return n;
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

// NBase::gDecryptData S 006e6660 crashes on a buffer without its header (FCC mismatch -> write to address 0).
// Every game file that reaches it has the header; a mod's file without it is plain and passes unchanged.
using Decrypt = Hook<game::fn::NBase::gDecryptData>;
const unsigned char sadk_magic[8] = {0x12, 0x18, 0x09, 0x06, 's', 'a', 'd', 'k'};

void SADK_CDECL decrypt_or_pass(void *file_name, void **data, std::uint32_t *size)
{
    if (data && *data && size && (*size < 8 || std::memcmp(*data, sadk_magic, 8) != 0)) return;
    Decrypt::original(file_name, data, size);
}

void install_file_hooks()
{
    if (file_hooks_installed) return;
    file_hooks_installed = true;
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
    log("mods: file redirect %s", ok ? "installed" : "- A HOOK FAILED, see above");
}

// ── Events (events.hpp) ──────────────────────────────────────────────────────────────────────────────────────────
// Subscriptions in load order (registry order; the host "" last). Dispatch works on a copy and skips an entry that
// left meanwhile (a callback may deactivate mods), so callbacks may subscribe and unload freely.
struct Sub {
    std::string owner;
    std::uint32_t event;
    void *fn, *ctx;
    std::uint64_t id;
};
std::vector<Sub> subs;
std::uint64_t next_sub_id = 1;
CRITICAL_SECTION subs_cs, posted_cs;
bool events_ready = [] {
    InitializeCriticalSection(&subs_cs);
    InitializeCriticalSection(&posted_cs);
    return true;
}();
struct Posted {
    std::string owner;   // dropped with its mod, like the subscriptions
    events::Callback fn;
    void *ctx;
};
std::vector<Posted> posted;
std::set<std::string> dropped_during_run;   // owners unloaded while posted calls run: skip the rest of theirs

bool add_sub(const char *owner, std::uint32_t event, void *fn, void *ctx)
{
    if (!fn) return false;
    if (event == events::posted) {   // any thread
        EnterCriticalSection(&posted_cs);
        posted.push_back(Posted{owner, reinterpret_cast<events::Callback>(fn), ctx});
        LeaveCriticalSection(&posted_cs);
        return true;
    }
    if (event > events::net_event) return false;
    EnterCriticalSection(&subs_cs);
    auto at = subs.begin();
    while (at != subs.end() && registry::order_of(at->owner.c_str(), owner) <= 0) ++at;
    subs.insert(at, Sub{owner, event, fn, ctx, next_sub_id++});
    LeaveCriticalSection(&subs_cs);
    return true;
}

void remove_subs(const std::string &owner)
{
    EnterCriticalSection(&subs_cs);
    std::erase_if(subs, [&](const Sub &s) { return s.owner == owner; });
    LeaveCriticalSection(&subs_cs);
    EnterCriticalSection(&posted_cs);
    std::erase_if(posted, [&](const Posted &p) { return p.owner == owner; });
    dropped_during_run.insert(owner);
    LeaveCriticalSection(&posted_cs);
}

template <class Call>
void dispatch(std::uint32_t event, Call call)
{
    std::vector<Sub> now;
    EnterCriticalSection(&subs_cs);
    for (auto &s : subs)
        if (s.event == event) now.push_back(s);
    LeaveCriticalSection(&subs_cs);
    for (auto &s : now) {
        EnterCriticalSection(&subs_cs);
        bool live = std::any_of(subs.begin(), subs.end(), [&](const Sub &x) { return x.id == s.id; });
        LeaveCriticalSection(&subs_cs);
        if (live) call(s);
    }
}

void dispatch_plain(std::uint32_t event)
{
    dispatch(event, [](const Sub &s) { reinterpret_cast<events::Callback>(s.fn)(s.ctx); });
}

using FrameTick = Hook<game::fn::CApplicationEx::FrameTick>;
void run_posted()
{
    std::vector<Posted> run;
    EnterCriticalSection(&posted_cs);
    run.swap(posted);
    dropped_during_run.clear();
    LeaveCriticalSection(&posted_cs);
    for (auto &p : run) {
        EnterCriticalSection(&posted_cs);
        bool dropped = dropped_during_run.count(p.owner) > 0;
        LeaveCriticalSection(&posted_cs);
        if (!dropped) p.fn(p.ctx);
    }
}

bool SADK_THISCALL on_frame_tick(game::CApplicationEx *app)
{
    run_posted();
    dispatch_plain(events::frame);
    return FrameTick::original(app);
}

using SceneCreate = Hook<game::fn::Scene_CreateGlobal>;
void SADK_CDECL on_scene_create()
{
    dispatch_plain(events::match_enter);
    SceneCreate::original();
}

using SceneDestroy = Hook<game::fn::Scene_DestroyGlobal>;
void SADK_CDECL on_scene_destroy()
{
    SceneDestroy::original();
    dispatch_plain(events::match_leave);
}

using NetShutdown = Hook<game::fn::NComm_Manager::Shutdown>;
bool SADK_THISCALL on_net_shutdown(game::NComm_Manager *m, bool keep)
{
    bool ok = NetShutdown::original(m, keep);
    dispatch_plain(events::session_end);
    return ok;
}

using NetEvent = Hook<game::fn::NComm_Manager::HandleEvent>;
bool SADK_THISCALL on_net_event(game::NComm_Manager *m, game::NComm::EventBase *ev)
{
    bool ok = NetEvent::original(m, ev);
    dispatch(events::net_event, [&](const Sub &s) { reinterpret_cast<events::NetCallback>(s.fn)(s.ctx, m, ev); });
    return ok;
}

void install_event_hooks()
{
    int n = FrameTick::install(on_frame_tick, "events: frame") +
            SceneCreate::install(on_scene_create, "events: match_enter") +
            SceneDestroy::install(on_scene_destroy, "events: match_leave") +
            NetShutdown::install(on_net_shutdown, "events: session_end") +
            NetEvent::install(on_net_event, "events: net_event");
    log("mods: %d of 5 event hooks installed", n);
}

// ── Property reload ──────────────────────────────────────────────────────────────────────────────────────────────
// The game fills its property database once, in CApplicationEx::Initialize S 004075d0. When the active mods' property
// scripts change later, it is filled again with the game's own functions when the next match is entered:
// the match_enter event (Scene_CreateGlobal S 0067e7e0, once per match from nMenu::Game::OnEnter, before the world is
// built), after every mod's own match_enter. Live (offline, test mod propreload, 2026-10-08): two matches in a row,
// record counts identical to start-up.
void SADK_CDECL reload_properties(void *)
{
    if (properties_dirty) {
        properties_dirty = false;
        auto *db = game::fn::NProperties::StaticAccess::EnsureInstance();
        game::fn::Properties_Db_ClearAll(db);
        game::fn::Properties_RegisterLuaLibrary(db);
        msvc::string name = msvc::string::small("data");
        game::fn::ai::properties::PropertiesDb::RunPropertyScript(db, &name);
        log("mods: property database filled again for the active mods (%u buildings, %u goods)", db->buildingMapSize,
            db->goodMapSize);
    }
}

// ── Unloading ────────────────────────────────────────────────────────────────────────────────────────────────────
// True if `v` lies in [lo, hi).
inline bool inside(std::uintptr_t v, std::uintptr_t lo, std::uintptr_t hi) { return v >= lo && v < hi; }

// Does the stack from `from` up to its top hold a value inside [lo, hi)?
bool stack_points_into(std::uintptr_t from, std::uintptr_t lo, std::uintptr_t hi)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void *>(from), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    auto top = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    for (std::uintptr_t p = from & ~std::uintptr_t(3); p + 4 <= top; p += 4)
        if (inside(*reinterpret_cast<const std::uintptr_t *>(p), lo, hi)) return true;
    return false;
}

// No thread runs in [lo, hi) or has a value pointing there on its stack. Our own thread is checked from `own_from`
// up (the caller's frame: what lies below it belongs to the host's own calls). While the others are paused, nothing
// here allocates or logs: a paused thread may hold the heap's or the log's lock.
bool no_thread_in(std::uintptr_t lo, std::uintptr_t hi, std::uintptr_t own_from, int *busy_thread)
{
    *busy_thread = 0;
    if (stack_points_into(own_from, lo, hi)) {
        *busy_thread = static_cast<int>(GetCurrentThreadId());
        return false;
    }
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                ids.push_back(te.th32ThreadID);
        CloseHandle(snap);
    }
    std::vector<HANDLE> threads;
    threads.reserve(ids.size());
    for (DWORD id : ids)
        if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id))
            threads.push_back(t);
    std::vector<char> paused(threads.size(), 0);
    for (std::size_t i = 0; i < threads.size(); i++) paused[i] = SuspendThread(threads[i]) != static_cast<DWORD>(-1);
    bool clear = true;
    for (std::size_t i = 0; i < threads.size() && clear; i++) {
        if (!paused[i]) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(threads[i], &ctx)) continue;   // also waits until the thread has stopped
        if (inside(ctx.Eip, lo, hi) || stack_points_into(ctx.Esp, lo, hi)) {
            clear = false;
            *busy_thread = static_cast<int>(ids[i]);
        }
    }
    for (std::size_t i = 0; i < threads.size(); i++) {
        if (paused[i]) ResumeThread(threads[i]);
        CloseHandle(threads[i]);
    }
    return clear;
}

bool try_free(Mod &m, std::uintptr_t own_from)
{
    if (!m.dll) return true;
    auto base = reinterpret_cast<std::uintptr_t>(m.dll);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    int busy = 0;
    if (!no_thread_in(base, base + nt->OptionalHeader.SizeOfImage, own_from, &busy)) {
        log("mods: %s\\mod.dll not freed yet: thread %d is in it or points into it", m.folder.c_str(), busy);
        m.pending_free = true;
        return false;
    }
    FreeLibrary(m.dll);
    m.dll = nullptr;
    m.init = nullptr;
    m.stop = nullptr;
    m.pending_free = false;
    log("mods: %s\\mod.dll freed", m.folder.c_str());
    return true;
}

// ── mod.dll and the API ──────────────────────────────────────────────────────────────────────────────────────────
void SADK_CDECL host_log_line(const char *mod, const char *text) { log("[%s] %s", mod, text); }

// Everything a mod changes is recorded under its folder name (registry.hpp); the log label names the mod as well.
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

void SADK_CDECL api_list_mods(void (*each)(const sadkmod_modinfo *, void *), void *ctx)
{
    for (auto &i : mods()) {
        sadkmod_modinfo info{i.folder.c_str(), i.name.c_str(), i.dir.c_str(), i.version, i.flags, i.active, i.pending_free};
        each(&info, ctx);
    }
}
bool SADK_CDECL api_add_mod(const char *dir) { return add_mod(dir); }
bool SADK_CDECL api_forget_mod(const char *folder) { return forget_mod(folder); }
bool SADK_CDECL api_activate_mod(const char *folder) { return activate(folder); }
int SADK_CDECL api_deactivate_mod(const char *folder) { return static_cast<int>(deactivate(folder)); }
void SADK_CDECL api_redirect_server_mods(bool on) { redirect_server_mods(on); }
int SADK_CDECL api_free_pending() { return free_pending(); }
bool SADK_CDECL api_subscribe(const char *mod, std::uint32_t event, void *fn, void *ctx) { return add_sub(mod, event, fn, ctx); }
bool SADK_CDECL api_mod_hash(const char *folder, char out[33])
{
    std::string h = content_hash(folder);
    std::snprintf(out, 33, "%s", h.c_str());
    return h.size() == 32;
}

template <class F>
F export_of(HMODULE h, const char *name)
{
    return reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(h, name)));
}

// Loads the folder's mod.dll (if not loaded) and reads its exports. False (logged) if it cannot be used.
bool load_dll(Mod &m)
{
    if (m.dll) return true;
    std::wstring path = m.wdir + L"\\mod.dll";
    m.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // its helpers: its folder
    if (!m.dll) {
        log("mods: %s\\mod.dll did not load (error %lu) - not used", m.folder.c_str(), GetLastError());
        return false;
    }
    m.init = export_of<sadkmod_init_fn>(m.dll, "sadkmod_init");
    auto version = export_of<sadkmod_version_fn>(m.dll, "sadkmod_version");
    auto flags = export_of<sadkmod_flags_fn>(m.dll, "sadkmod_flags");
    m.stop = export_of<sadkmod_stop_fn>(m.dll, "sadkmod_stop");
    if (!m.init || !version || !flags) {
        log("mods: %s\\mod.dll lacks %s - not used (built for an older sadkmod?)", m.folder.c_str(),
            !m.init ? "sadkmod_init" : !version ? "sadkmod_version" : "sadkmod_flags");
        FreeLibrary(m.dll);
        m.dll = nullptr;
        return false;
    }
    m.version = version();
    m.flags = flags();
    return true;
}

// A folder -> a Mod with its flags, or nullptr (logged) if it is not to be loaded.
std::unique_ptr<Mod> read_mod(const std::wstring &dir, const std::wstring &folder)
{
    auto m = std::make_unique<Mod>();
    m->folder = narrow(folder);
    m->name = without_temp(m->folder);
    m->order = lower(m->name);
    m->dir = narrow(dir);
    m->wdir = dir;
    m->has_dll = exists(dir + L"\\mod.dll");
    if (m->has_dll) {
        if (!load_dll(*m)) return nullptr;
    } else {
        m->flags = (exists(dir + L"\\.client") ? SADKMOD_CLIENT : 0) | (exists(dir + L"\\.server") ? SADKMOD_SERVER : 0) |
                   (exists(dir + L"\\.lobby") ? SADKMOD_LOBBY : 0);
    }
    if (!m->flags) {
        log("mods: %s says nothing about what it changes (%s) - not loaded", m->folder.c_str(),
            m->has_dll ? "sadkmod_flags() is 0" : "no .client, .server or .lobby file");
        if (m->dll) FreeLibrary(m->dll);
        return nullptr;
    }
    registry::set_order(m->folder.c_str(), m->order.c_str());
    auto api = std::make_unique<sadkmod_api>();
    api->version = SADKMOD_API_VERSION;
    api->size = sizeof(sadkmod_api);
    api->mod_name = m->folder.c_str();
    api->mod_dir = m->dir.c_str();
    api->game_root = game_root();
    api->log_line = host_log_line;
    api->hook = host_hook;
    api->unhook = host_unhook;
    api->patch = host_patch;
    api->write_slot = host_write_slot;
    api->list_mods = api_list_mods;
    api->add_mod = api_add_mod;
    api->forget_mod = api_forget_mod;
    api->activate_mod = api_activate_mod;
    api->deactivate_mod = api_deactivate_mod;
    api->mod_hash = api_mod_hash;
    api->redirect_server_mods = api_redirect_server_mods;
    api->free_pending = api_free_pending;
    api->subscribe = api_subscribe;
    HMODULE self = nullptr;   // the module this code is linked into: the host
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&add_mod), &self);
    api->host_module = self;
    m->api = std::move(api);
    std::string what;
    for (auto [bit, word] : {std::pair{SADKMOD_CLIENT, "client"}, {SADKMOD_SERVER, "server"}, {SADKMOD_LOBBY, "lobby"}})
        if (m->flags & bit) what += (what.empty() ? "" : "+") + std::string(word);
    log("mods: %s: %s, version %u%s", m->folder.c_str(), what.c_str(), m->version, m->has_dll ? ", mod.dll" : "");
    return m;
}

void insert_sorted(std::unique_ptr<Mod> m)
{
    auto at = std::find_if(all.begin(), all.end(), [&](const std::unique_ptr<Mod> &o) {
        return o->order > m->order || (o->order == m->order && lower(o->folder) > lower(m->folder));
    });
    all.insert(at, std::move(m));
}

// Starts an active mod's mod.dll. On failure everything it did is undone.
bool start_dll(Mod &m, std::uintptr_t own_from)
{
    if (!m.has_dll) return true;
    if (!load_dll(m)) return false;
    bool ok = m.init(m.api.get());
    log("mods: %s\\mod.dll %s", m.folder.c_str(), ok ? "started" : "reported a failure - undoing what it did");
    if (!ok) {
        registry::remove_owner(m.folder.c_str());
        try_free(m, own_from);
    }
    return ok;
}

void deactivate_locked(Mod &m, std::uintptr_t own_from)
{
    if (m.has_dll && m.stop) m.stop();
    registry::Removed r = registry::remove_owner(m.folder.c_str());
    remove_subs(m.folder);
    m.active = false;
    rebuild_index();
    log("mods: %s deactivated (%d hooks, %d table slots, %d patches undone%s)", m.folder.c_str(), r.hooks, r.slots,
        r.patches, r.not_restored ? ", SOME PATCHED BYTES HAD CHANGED - left as they are" : "");
    try_free(m, own_from);
}

}  // namespace

Summary start_mods()
{
    std::uintptr_t own = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    Lock lock;
    Summary s;
    wroot = widen(game_root());
    data_prefix = lower(wroot + L"\\data\\");
    wchar_t local[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK) {   // as Sys_GetLocalAppDataPath
        cache_prefix = lower(std::wstring(local) + L"\\SAdK\\");
        cache_redirect_dir = std::wstring(local) + L"\\SAdK\\mods\\";
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wroot + L"\\mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_')
                continue;
            if (auto m = read_mod(wroot + L"\\mods\\" + fd.cFileName, fd.cFileName))
                insert_sorted(std::move(m));
            else
                s.skipped++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (all.empty()) {
        log("mods: none");
        return s;
    }
    if (exe_is_supported()) {   // the game's own functions: only on the build they describe
        install_event_hooks();
        add_sub("", events::match_enter, reinterpret_cast<void *>(reload_properties), nullptr);
    }
    for (auto &m : all) m->active = true;
    s.files = rebuild_index();
    properties_dirty = false;   // the game has not read them yet: its own start-up reads the modded files
    for (auto &m : all) {
        if (!m->has_dll) continue;
        if (start_dll(*m, own)) {
            s.dlls++;
        } else {
            m->active = false;
            s.skipped++;
        }
    }
    if (std::any_of(all.begin(), all.end(), [](const std::unique_ptr<Mod> &m) { return !m->active; }))
        s.files = rebuild_index();   // a mod.dll failed: its data goes too
    properties_dirty = false;
    for (auto &m : all) s.mods += m->active;
    return s;
}

std::vector<ModInfo> mods()
{
    Lock lock;
    std::vector<ModInfo> out;
    for (auto &m : all)
        out.push_back(ModInfo{m->folder, m->name, m->dir, m->version, m->flags, m->has_dll, m->active, m->pending_free});
    return out;
}

bool add_mod(const char *dir)
{
    Lock lock;
    std::wstring wdir = widen(dir);
    while (!wdir.empty() && (wdir.back() == L'\\' || wdir.back() == L'/')) wdir.pop_back();
    std::wstring folder = wdir.substr(wdir.find_last_of(L"\\/") + 1);
    if (find(narrow(folder).c_str())) {
        log("mods: %s is listed already", narrow(folder).c_str());
        return false;
    }
    auto m = read_mod(wdir, folder);
    if (!m) return false;
    insert_sorted(std::move(m));
    return true;
}

bool forget_mod(const char *folder)
{
    Lock lock;
    for (auto it = all.begin(); it != all.end(); ++it)
        if (!_stricmp((*it)->folder.c_str(), folder)) {
            if ((*it)->active || (*it)->pending_free) return false;
            if ((*it)->dll) FreeLibrary((*it)->dll);   // loaded to read its flags, never started
            log("mods: %s no longer listed", folder);
            all.erase(it);
            return true;
        }
    return false;
}

bool activate(const char *folder)
{
    std::uintptr_t own = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    Lock lock;
    Mod *m = find(folder);
    if (!m || m->active || m->pending_free) {
        log("mods: cannot activate %s (%s)", folder, !m ? "unknown" : m->active ? "active already" : "waiting to be freed");
        return false;
    }
    m->active = true;
    rebuild_index();
    if (!start_dll(*m, own)) {
        m->active = false;
        rebuild_index();
        return false;
    }
    log("mods: %s activated", m->folder.c_str());
    return true;
}

Unload deactivate(const char *folder)
{
    std::uintptr_t own = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    Lock lock;
    Mod *m = find(folder);
    if (!m) return Unload::not_found;
    if (!m->active) return Unload::not_active;
    deactivate_locked(*m, own);
    return m->pending_free ? Unload::busy : Unload::done;
}

int free_pending()
{
    std::uintptr_t own = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    Lock lock;
    int waiting = 0;
    for (auto &m : all)
        if (m->pending_free && !try_free(*m, own)) waiting++;
    return waiting;
}

std::string content_hash(const char *folder)
{
    std::wstring dir;
    {
        Lock lock;
        Mod *m = find(folder);
        if (!m) return {};
        dir = m->wdir;
    }
    std::vector<std::pair<std::wstring, std::wstring>> files;   // lowercase relative path, full path
    std::vector<std::wstring> todo{L""};
    while (!todo.empty()) {
        std::wstring rel = todo.back();
        todo.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
            std::wstring r = rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                todo.push_back(r);
            else
                files.emplace_back(lower(r), dir + L"\\" + r);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return out;
    if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        bool ok = true;
        std::vector<unsigned char> buf(65536);
        for (auto &[rel, full] : files) {
            std::string name = narrow(rel);
            ok = ok && CryptHashData(hash, reinterpret_cast<const BYTE *>(name.c_str()), static_cast<DWORD>(name.size() + 1), 0);
            HANDLE f = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                ok = false;
                continue;
            }
            DWORD got;
            while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got)
                ok = CryptHashData(hash, buf.data(), got, 0);
            CloseHandle(f);
        }
        unsigned char digest[16];
        DWORD len = sizeof digest;
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
            char hex[33];
            for (int i = 0; i < 16; i++) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
            out = hex;
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

bool subscribe(const char *owner, std::uint32_t event, void *fn, void *ctx) { return add_sub(owner, event, fn, ctx); }

void raise(std::uint32_t event)
{
    if (event == events::frame) run_posted();
    if (event != events::net_event) dispatch_plain(event);
}

thread_local bool skip_server_files;
void redirect_server_mods(bool on) { skip_server_files = !on; }

const wchar_t *redirect(const wchar_t *path, bool write)
{
    const Index *ix = current.load();
    if (!ix || ix->files.empty()) return nullptr;
    thread_local wchar_t result[MAX_PATH * 2];   // trivially destructible: no thread-exit hooks needed
    auto give = [&](const std::wstring &p) -> const wchar_t * {
        if (p.size() >= MAX_PATH * 2) return nullptr;
        std::wmemcpy(result, p.c_str(), p.size() + 1);
        return result;
    };
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(path, MAX_PATH * 2, buf, nullptr);
    if (!n || n >= MAX_PATH * 2) return nullptr;
    std::wstring full(buf, n);
    std::wstring low = lower(full);
    if (!write && low.size() > data_prefix.size() && low.compare(0, data_prefix.size(), data_prefix) == 0) {
        auto it = ix->files.find(low.substr(data_prefix.size()));
        if (it == ix->files.end()) return nullptr;
        if (skip_server_files && ix->server.count(it->first)) return nullptr;
        return give(it->second);
    }
    // The mesh cache of a modded .KEX: <cache>\<mesh>[_<variant>].mshraw -> <cache>\mods\<same name>
    if (!ix->meshes.empty() && !cache_prefix.empty() && low.size() > cache_prefix.size() + 7 &&
        low.compare(0, cache_prefix.size(), cache_prefix) == 0 && low.compare(low.size() - 7, 7, L".mshraw") == 0) {
        std::wstring file = low.substr(cache_prefix.size());
        if (file.find(L'\\') != std::wstring::npos) return nullptr;
        std::wstring stem = file.substr(0, file.size() - 7);
        bool modded = ix->meshes.count(stem) > 0;
        for (std::size_t u = stem.find(L'_'); !modded && u != std::wstring::npos; u = stem.find(L'_', u + 1))
            modded = ix->meshes.count(stem.substr(0, u)) > 0;   // <mesh>_<variant>
        if (!modded) return nullptr;
        return give(cache_redirect_dir + full.substr(cache_prefix.size()));
    }
    return nullptr;
}

}  // namespace sadk::host
