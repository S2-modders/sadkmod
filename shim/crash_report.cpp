// The crash report's analysis (crash.hpp): the call stack, what each address is, the cause, the report text and the
// minidump. Runs on the report thread (crash.cpp) while the crashed thread waits.
#include "crash.hpp"

#include <sadkmod/game/sadk_noav/vars.hpp>
#include <sadkmod/host.hpp>
#include <sadkmod/registry.hpp>

#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <map>
#include <cstdio>
#include <cstring>

namespace shim::report {

namespace {

using sadk::registry::Change;

// ── DbgHelp ──────────────────────────────────────────────────────────────────────────────────────────────────────
// The system's dbghelp.dll, loaded by its full path: the game ships version 6.5 (2005) in its bin folder, which
// cannot write every part of a full dump. A second copy in the process is fine: each keeps its own state. DbgHelp is
// not thread-safe; only the report thread uses it (and a crash on the report thread itself, which has no other).
struct DbgHelp {
    HMODULE dll = nullptr;
    decltype(&::SymSetOptions) SymSetOptions = nullptr;
    decltype(&::SymInitialize) SymInitialize = nullptr;
    decltype(&::SymRefreshModuleList) SymRefreshModuleList = nullptr;
    decltype(&::SymFromAddr) SymFromAddr = nullptr;
    decltype(&::MiniDumpWriteDump) MiniDumpWriteDump = nullptr;
    decltype(&::UnDecorateSymbolName) UnDecorateSymbolName = nullptr;
    bool symbols = false;   // SymInitialize succeeded
};

template <class F>
void bind(HMODULE dll, F &f, const char *name) { f = reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(dll, name))); }

DbgHelp &dbghelp()
{
    static DbgHelp d;
    static bool tried;
    if (tried) return d;
    tried = true;
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n && n < MAX_PATH - 16) {
        std::strcat(path, "\\dbghelp.dll");
        d.dll = LoadLibraryA(path);
    }
    if (!d.dll) d.dll = LoadLibraryA("dbghelp.dll");
    if (!d.dll) return d;
    bind(d.dll, d.SymSetOptions, "SymSetOptions");
    bind(d.dll, d.SymInitialize, "SymInitialize");
    bind(d.dll, d.SymRefreshModuleList, "SymRefreshModuleList");
    bind(d.dll, d.SymFromAddr, "SymFromAddr");
    bind(d.dll, d.MiniDumpWriteDump, "MiniDumpWriteDump");
    bind(d.dll, d.UnDecorateSymbolName, "UnDecorateSymbolName");
    if (d.SymSetOptions && d.SymInitialize) {
        // names from the modules' exports only: no symbol server, no prompts (a crash must not wait on the network)
        d.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        char bin[MAX_PATH];
        std::snprintf(bin, sizeof bin, "%s\\bin", sadk::game_root());
        d.symbols = d.SymInitialize(GetCurrentProcess(), bin, TRUE);
    }
    return d;
}

// ── Modules ──────────────────────────────────────────────────────────────────────────────────────────────────────
struct ModuleRec {
    std::uintptr_t base = 0, size = 0;
    std::string path, name;   // full path, file name
    std::string mod;          // <game>\mods\<folder>\...: the folder
};

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// EnumProcessModules reads the loader's list with ReadProcessMemory and takes no lock (Toolhelp takes the loader
// lock, which the crashed thread may hold).
std::vector<ModuleRec> list_modules()
{
    std::vector<ModuleRec> out;
    HMODULE hm[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &bytes)) return out;
    std::string mods_dir = lower(std::string(sadk::game_root()) + "\\mods\\");
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; i++) {
        MODULEINFO mi{};
        char path[MAX_PATH] = "";
        if (!GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof mi)) continue;
        GetModuleFileNameExA(GetCurrentProcess(), hm[i], path, MAX_PATH);
        ModuleRec m;
        m.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
        m.size = mi.SizeOfImage;
        m.path = path;
        const char *slash = std::strrchr(path, '\\');
        m.name = slash ? slash + 1 : path;
        std::string lp = lower(m.path);
        if (lp.compare(0, mods_dir.size(), mods_dir) == 0) {
            std::string rest = m.path.substr(mods_dir.size());
            m.mod = rest.substr(0, rest.find('\\'));
        }
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const ModuleRec &a, const ModuleRec &b) { return a.base < b.base; });
    return out;
}

// ── What an address is ───────────────────────────────────────────────────────────────────────────────────────────
enum class Kind : std::uint8_t { game, tincat, shim, mod, stub, trampoline, other, none };

struct Where {
    std::uintptr_t address = 0;
    Kind kind = Kind::none;
    const ModuleRec *module = nullptr;
    std::string label;                // "NNavy::Ship::dtor+0x23", "mods\x\mod.dll+0x1a2b", ...
    std::uintptr_t entry = 0;         // game code: the entry of its function (address in this process), if known
    const Change *change = nullptr;   // a hook's stub or trampoline, or the patch these bytes belong to
    bool sure = true;                 // a frame of the call stack: see walk()
};

struct Scene {
    std::vector<ModuleRec> modules;
    std::vector<Change> changes;
    bool changes_ok = false;
    std::vector<sadk::host::ModInfo> mods;
    bool mods_ok = false;
    std::uintptr_t exe = 0, tincat = 0, shim = 0;
    bool named = false;   // the supported SADK.exe: the Ghidra map's names apply

    Scene()
    {
        modules = list_modules();
        changes_ok = sadk::registry::snapshot(changes, 300);
        mods_ok = sadk::host::mods_snapshot(mods, 300);
        exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
        tincat = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("tincat3.dll"));
        shim = reinterpret_cast<std::uintptr_t>(shim_module());
        named = sadk::exe_is_supported();
        DbgHelp &d = dbghelp();
        if (d.symbols && d.SymRefreshModuleList) d.SymRefreshModuleList(GetCurrentProcess());   // DLLs loaded since
    }

    const ModuleRec *module_of(std::uintptr_t a) const
    {
        auto it = std::upper_bound(modules.begin(), modules.end(), a,
                                   [](std::uintptr_t v, const ModuleRec &m) { return v < m.base; });
        if (it == modules.begin()) return nullptr;
        --it;
        return a < it->base + it->size ? &*it : nullptr;
    }

    // The game's name for an address of SADK.exe / tincat3.dll, or "".
    std::string game_name(Kind k, std::uintptr_t a, std::uintptr_t *entry) const
    {
        if (!named) return "";
        sadk::Module m = k == Kind::game ? sadk::Module::sadk : sadk::Module::tincat3;
        std::uintptr_t base = k == Kind::game ? exe : tincat;
        std::uint32_t stat = static_cast<std::uint32_t>(a - base + sadk::preferred_base(m)), at = 0;
        const char *name = symbols::find(m, stat, &at);
        if (!name) return "";
        if (entry) *entry = at - sadk::preferred_base(m) + base;
        return format("%s+0x%x", name, unsigned(stat - at));
    }

    std::string module_offset(const ModuleRec &m, std::uintptr_t a) const
    {
        std::string where = m.mod.empty() ? m.name : "mods\\" + m.mod + m.path.substr(m.path.rfind('\\'));
        return format("%s+0x%x", where.c_str(), unsigned(a - m.base));
    }

    std::string export_name(std::uintptr_t a) const
    {
        DbgHelp &d = dbghelp();
        if (!d.symbols || !d.SymFromAddr) return "";
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto *si = reinterpret_cast<SYMBOL_INFO *>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (!d.SymFromAddr(GetCurrentProcess(), a, &disp, si)) return "";
        if (disp > 0x10000) return "";   // far from the nearest export: not that function
        return format("%s+0x%x", si->Name, unsigned(disp));
    }

    // The start of the exported function an address lies in (a system DLL's), or 0.
    std::uintptr_t export_entry(std::uintptr_t a) const
    {
        DbgHelp &d = dbghelp();
        if (!d.symbols || !d.SymFromAddr) return 0;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto *si = reinterpret_cast<SYMBOL_INFO *>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (!d.SymFromAddr(GetCurrentProcess(), a, &disp, si) || disp > 0x1000) return 0;
        return static_cast<std::uintptr_t>(si->Address);
    }

    Where where(std::uintptr_t a) const
    {
        Where w;
        w.address = a;
        w.module = module_of(a);
        for (auto &c : changes) {
            if (c.kind == Change::patch && a >= c.at && a < c.at + c.size) w.change = &c;
            if (!w.module && c.kind == Change::hook) {
                if (c.stub && a >= c.stub && a < c.stub + 8) { w.kind = Kind::stub; w.change = &c; }
                if (c.trampoline && a >= c.trampoline && a < c.trampoline + 64) { w.kind = Kind::trampoline; w.change = &c; }
            }
        }
        if (w.kind == Kind::stub || w.kind == Kind::trampoline) {
            Where target = where(w.change->at);
            w.entry = w.change->at;
            w.label = format("%s of %s (hook machinery)", w.kind == Kind::stub ? "the hook stub" : "the first instructions",
                             target.label.c_str());
            return w;
        }
        if (!w.module) {
            w.label = "(not in any module)";
            return w;
        }
        if (w.module->base == exe) w.kind = Kind::game;
        else if (tincat && w.module->base == tincat) w.kind = Kind::tincat;
        else if (w.module->base == shim) w.kind = Kind::shim;
        else if (!w.module->mod.empty()) w.kind = Kind::mod;
        else w.kind = Kind::other;
        if (w.kind == Kind::game || w.kind == Kind::tincat) {
            std::string n = game_name(w.kind, a, &w.entry);
            w.label = n.empty() ? module_offset(*w.module, a) : w.module->name + "  " + n;
        } else if (w.kind == Kind::other) {
            std::string n = export_name(a);
            w.label = n.empty() ? module_offset(*w.module, a) : w.module->name + "  " + n;
        } else {
            w.label = module_offset(*w.module, a);
        }
        if (w.change) w.label += format("  [bytes patched by %s: \"%s\"]", owner(w.change->owner).c_str(), w.change->what.c_str());
        return w;
    }

    static std::string owner(const std::string &o) { return o.empty() ? "the mod loader" : "mod \"" + o + "\""; }

    // Was this mod.dll built with frame pointers (sadkmod since 0d955c2, pch.mk)? Its exported sadkmod_init, which
    // calls the mod's start function, then begins with PUSH EBP; MOV EBP,ESP (55 89 E5). An older build, or a helper
    // DLL without that export, is not known to keep frames. Read from the loaded image without faulting; once a module.
    bool keeps_frames(const ModuleRec &m) const
    {
        auto it = frames_known.find(m.base);
        if (it != frames_known.end()) return it->second;
        bool yes = false;
        std::uint32_t lfanew = 0, dir = 0, n = 0, names = 0, ords = 0, funcs = 0;
        if (safe_read(m.base + 0x3c, &lfanew, 4) && safe_read(m.base + lfanew + 0x78, &dir, 4) && dir &&
            safe_read(m.base + dir + 0x18, &n, 4) && safe_read(m.base + dir + 0x1c, &funcs, 4) &&
            safe_read(m.base + dir + 0x20, &names, 4) && safe_read(m.base + dir + 0x24, &ords, 4))
            for (std::uint32_t i = 0; i < n && i < 4096; i++) {
                std::uint32_t name = 0;
                char text[16] = {};
                if (!safe_read(m.base + names + 4 * i, &name, 4) || !safe_read(m.base + name, text, 13)) continue;
                if (std::memcmp(text, "sadkmod_init", 13) != 0) continue;
                std::uint16_t ord = 0;
                std::uint32_t rva = 0;
                std::uint8_t b[3];
                yes = safe_read(m.base + ords + 2 * i, &ord, 2) && safe_read(m.base + funcs + 4 * ord, &rva, 4) &&
                      safe_read(m.base + rva, b, 3) && b[0] == 0x55 && b[1] == 0x89 && b[2] == 0xE5;
                break;
            }
        frames_known[m.base] = yes;
        return yes;
    }
    mutable std::map<std::uintptr_t, bool> frames_known;

    // A function by its entry: its name, or its address.
    std::string function(std::uintptr_t entry) const
    {
        const ModuleRec *m = module_of(entry);
        Kind k = !m ? Kind::none : m->base == exe ? Kind::game : m->base == tincat ? Kind::tincat : Kind::other;
        std::string n = k == Kind::game || k == Kind::tincat ? game_name(k, entry, nullptr) : "";
        return n.empty() ? format("%08X", unsigned(entry)) : n.substr(0, n.rfind('+'));
    }

    // The hooks on a function (its entry), by owner, in call order: "mod \"a\", the mod loader".
    std::string hooks_on(std::uintptr_t entry, const char *only_owner = nullptr) const
    {
        std::string s;
        for (auto &c : changes)
            if (c.kind == Change::hook && c.at == entry && (!only_owner || c.owner == only_owner))
                s += (s.empty() ? "" : ", ") + owner(c.owner);
        return s;
    }
};

// ── The stack ────────────────────────────────────────────────────────────────────────────────────────────────────
// The crashed thread's stack from ESP up: the copy taken when an error happened, or (a fatal crash, the thread
// waits) read now, at most 256 KB.
struct Stack {
    std::vector<std::uint8_t> own;
    const std::uint8_t *data = nullptr;
    std::uintptr_t base = 0, size = 0;

    explicit Stack(const Event &e)
    {
        if (e.stack.data) {
            data = e.stack.data;
            base = e.stack.base;
            size = e.stack.size;
            return;
        }
        base = e.context->Esp & ~3u;
        MEMORY_BASIC_INFORMATION mbi{};   // the committed stack, from the page of ESP up to the stack's base
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(base), &mbi, sizeof mbi)) return;
        std::uintptr_t top = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        own.resize(std::min<std::uintptr_t>(top > base ? top - base : 0, 256 << 10));
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base), own.data(), own.size(), &got);
        data = own.data();
        size = got;
    }
    std::uintptr_t top() const { return base + size; }
    bool get(std::uintptr_t a, std::uint32_t &v) const
    {
        if (a < base || a + 4 > base + size) return false;
        std::memcpy(&v, data + (a - base), 4);
        return true;
    }
};

// Is `ret` the address right after a CALL instruction? b[i] is the byte at ret - 7 + i.
bool after_call(std::uintptr_t ret)
{
    std::uint8_t b[7];
    if (ret < 7 || !safe_read(ret - 7, b, 7)) return false;
    auto rm = [](std::uint8_t m) { return m & 7; };
    if (b[2] == 0xE8) return true;                                                          // E8 rel32
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) return true;                                 // FF /2 reg
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0x10 && rm(b[6]) != 4 && rm(b[6]) != 5) return true;   // FF /2 [reg]
    if (b[4] == 0xFF && b[5] == 0x14) return true;                                          // FF /2 [sib]
    if (b[4] == 0xFF && (b[5] & 0xF8) == 0x50 && rm(b[5]) != 4) return true;                // FF /2 [reg+disp8]
    if (b[3] == 0xFF && b[4] == 0x54) return true;                                          // FF /2 [sib+disp8]
    if (b[1] == 0xFF && b[2] == 0x15) return true;                                          // FF /2 [abs32]
    if (b[1] == 0xFF && (b[2] & 0xF8) == 0x90 && rm(b[2]) != 4) return true;                // FF /2 [reg+disp32]
    if (b[0] == 0xFF && b[1] == 0x94) return true;                                          // FF /2 [sib+disp32]
    return false;
}

bool return_address(const Scene &s, std::uintptr_t v) { return s.module_of(v) && after_call(v); }

// Could the call before `ret` have led to `pc`? 1: yes, a direct call (E8) whose target, followed through JMP thunks
// (an import thunk, an incremental-link thunk), holds pc or is a hooked function (pc then runs in a detour, a stub or a
// trampoline); -1: no, a direct call elsewhere; 0: not known, any other call could have gone anywhere.
int leads_to(const Scene &s, std::uintptr_t ret, std::uintptr_t pc)
{
    std::uint8_t op;
    std::int32_t rel;
    if (!safe_read(ret - 5, &op, 1) || op != 0xE8 || !safe_read(ret - 4, &rel, 4)) return 0;
    std::uintptr_t target = ret + rel;
    for (int hop = 0; hop < 3; hop++) {
        if (!s.hooks_on(target).empty()) return 1;
        if (target <= pc && pc - target < 0x40000) return 1;
        std::uint8_t b[6];
        if (!safe_read(target, b, 6)) return -1;
        if (b[0] == 0xE9) target = target + 5 + *reinterpret_cast<std::int32_t *>(b + 1);   // JMP rel32
        else if (b[0] == 0xFF && b[1] == 0x25) {                                              // JMP [abs32]
            std::uint32_t slot = *reinterpret_cast<std::uint32_t *>(b + 2), to = 0;
            if (!safe_read(slot, &to, 4)) return -1;
            target = to;
        } else
            return -1;
    }
    return -1;
}

// Does the code at `pc` run with EBP as its frame pointer? 1 yes, 0 no, -1 not known. A function keeps a frame if it
// starts with PUSH EBP; MOV EBP,ESP (MSVC 8B EC, GCC 89 E5; after an optional MOV EDI,EDI) and pc lies past that. The
// game was built with frame pointer omission: its functions are checked by their entry in the Ghidra map, a system
// DLL's by its exports. sadkmod's code keeps a frame in every function that calls another (a leaf may have none, so
// the innermost frame is not known): the shim's always, a mod's if it was built so (Scene::keeps_frames); an older
// mod is not known either, so its frame pointer is followed only where a direct call proves the step.
int keeps_frame(const Scene &s, std::uintptr_t pc, bool innermost)
{
    Where w = s.where(pc);
    if (w.kind == Kind::shim) return innermost ? -1 : 1;
    if (w.kind == Kind::mod) return innermost || !s.keeps_frames(*w.module) ? -1 : 1;
    if (w.kind == Kind::stub || w.kind == Kind::none) return 0;
    if (w.kind == Kind::trampoline) return -1;
    std::uintptr_t entry = w.entry ? w.entry : s.export_entry(pc);
    if (!entry) return -1;
    std::uint8_t b[5];
    if (!safe_read(entry, b, 5)) return -1;
    std::uintptr_t at = entry;
    if (b[0] == 0x8B && b[1] == 0xFF) at += 2;   // hot-patch MOV EDI,EDI
    const std::uint8_t *p = b + (at - entry);
    if (p[0] != 0x55 || !((p[1] == 0x8B && p[2] == 0xEC) || (p[1] == 0x89 && p[2] == 0xE5))) return 0;
    return pc >= at + 3 ? 1 : 0;
}

// The call stack: the frame pointer where the code keeps one, otherwise the next return address above ESP that
// could have called it. A frame is sure while every step to it had evidence: a frame pointer the function is known to
// keep, a direct call that leads to the code below, or the innermost function's own return address right at ESP. A
// return address found by scanning past a function without a frame may instead be left over from an earlier call.
struct Frame {
    std::uintptr_t address;
    bool sure;
};

std::vector<Frame> walk(const Stack &st, const Event &e, const Scene &s)
{
    std::vector<Frame> frames{{e.context->Eip, true}};
    std::uintptr_t pc = e.context->Eip, esp = e.context->Esp, ebp = e.context->Ebp;
    int link = 0;   // leads_to() of the candidate found
    // The first return address in [from, to) whose call could have led to pc; with `proven`, only one whose direct
    // call did (a caller through a pointer cannot be proven, but a leftover is rarely a direct call of this function).
    auto candidate = [&](std::uintptr_t from, std::uintptr_t to, std::uintptr_t &slot, bool proven = false) -> std::uintptr_t {
        std::uint32_t v;
        for (std::uintptr_t a = from & ~3u; a + 4 <= to && a + 4 <= st.top(); a += 4)
            if (st.get(a, v) && return_address(s, v) && (link = leads_to(s, v, pc)) >= (proven ? 1 : 0)) {
                slot = a;
                return v;
            }
        return 0;
    };
    bool sure = true;
    while (frames.size() < 64) {
        bool innermost = frames.size() == 1;
        int frame = keeps_frame(s, pc, innermost);
        std::uintptr_t ret = 0, slot = 0, next_ebp = ebp;
        std::uint32_t saved = 0, fp_ret = 0;
        int fp_link = -1;
        bool fp = ebp >= esp && st.get(ebp, saved) && st.get(ebp + 4, fp_ret) && return_address(s, fp_ret) &&
                  (fp_link = leads_to(s, fp_ret, pc)) >= 0;
        bool strong = false;
        if (frame == -1) {   // nearer than the frame pointer's
            ret = candidate(esp, fp ? ebp + 4 : esp + 0x10000, slot);
            strong = ret && (link == 1 || (innermost && slot == (esp & ~3u)));
        }
        if (!ret && frame != 0 && fp) {
            ret = fp_ret;
            slot = ebp + 4;
            next_ebp = saved;
            strong = frame == 1 || fp_link == 1;
        }
        if (!ret && !innermost && frame == 0) {   // past a function without a frame: a proven caller first
            ret = candidate(esp, esp + 0x10000, slot, true);
            strong = ret != 0;
        }
        if (!ret) {
            ret = candidate(esp, esp + 0x10000, slot);
            strong = ret && (link == 1 || (innermost && slot == (esp & ~3u)));
        }
        if (!ret) break;
        sure = sure && strong;
        frames.push_back(Frame{ret, sure});
        pc = ret;
        esp = slot + 4;
        ebp = next_ebp;
    }
    return frames;
}

// Return addresses on the stack that the walk did not pass (some may be left over from earlier calls).
std::vector<std::uintptr_t> scan(const Stack &st, const Scene &s, const std::vector<Frame> &walked)
{
    std::vector<std::uintptr_t> out;
    for (std::uintptr_t a = st.base; a + 4 <= std::min<std::uintptr_t>(st.top(), st.base + 0x10000) && out.size() < 48; a += 4) {
        std::uint32_t v;
        if (!st.get(a, v) || std::any_of(walked.begin(), walked.end(), [&](const Frame &f) { return f.address == v; }))
            continue;
        if (std::find(out.begin(), out.end(), v) != out.end()) continue;
        if (return_address(s, v)) out.push_back(v);
    }
    return out;
}

// ── The cause ────────────────────────────────────────────────────────────────────────────────────────────────────
bool is_code_of_ours(Kind k) { return k == Kind::mod || k == Kind::shim; }
bool is_game(Kind k) { return k == Kind::game || k == Kind::tincat || k == Kind::stub || k == Kind::trampoline; }

std::string who(const Where &w) { return w.kind == Kind::mod ? "mod \"" + w.module->mod + "\"" : "the mod loader (wsock32.dll)"; }

// The verdict: which mod caused the crash, or that it is the game's own. `detail` explains it.
std::string cause(const Scene &s, const std::vector<Where> &f, std::string &detail)
{
    if (f.empty()) return "unknown";
    const Where &at = f[0];
    if (at.change && at.change->kind == Change::patch) {
        detail = format("The crash is in game code that %s patched (\"%s\").", Scene::owner(at.change->owner).c_str(),
                        at.change->what.c_str());
        return at.change->owner.empty() ? "the mod loader (wsock32.dll), in its patch" : "mod \"" + at.change->owner + "\"";
    }
    if (is_code_of_ours(at.kind)) {
        detail = format("The crash is in the code of %s.", who(at).c_str());
        return who(at);
    }
    std::size_t g = 0;   // the first frame in the game's or our code
    if (at.kind == Kind::other || at.kind == Kind::none) {
        while (g < f.size() && f[g].sure && !is_game(f[g].kind) && !is_code_of_ours(f[g].kind)) g++;
        if (g == f.size() || !f[g].sure) {
            detail = format("Neither the game's nor a mod's code is on the stack; the crash is in %s.",
                            at.module ? at.module->name.c_str() : "code outside every module");
            return "unknown";
        }
        if (is_code_of_ours(f[g].kind)) {
            detail = format("The crash is in %s, called from the code of %s.",
                            at.module ? at.module->name.c_str() : "code outside every module", who(f[g]).c_str());
            return who(f[g]);
        }
        detail = format("The crash is in %s, called from the game's code. ",
                        at.module ? at.module->name.c_str() : "code outside every module");
    }
    // In the game's code (or called from it): is a mod's or the shim's code further up?
    std::string passed, maybe;
    std::uintptr_t chain = 0;   // the hooked function the last of our frames passed the call on to
    for (std::size_t j = g + 1; j < f.size(); j++) {
        if (!f[j].sure) {   // perhaps left over from an earlier call: named, not blamed
            if (is_code_of_ours(f[j].kind) && maybe.find(who(f[j])) == std::string::npos)
                maybe += (maybe.empty() ? "" : ", ") + who(f[j]);
            continue;
        }
        if (!is_code_of_ours(f[j].kind)) {
            chain = 0;
            continue;
        }
        // Did that code pass the call on to the game through its hook? The frame below it is then the hooked
        // function, or the detour of the next mod on the same function.
        const Where &callee = f[j - 1];
        std::string owner = f[j].kind == Kind::mod ? f[j].module->mod : "";
        std::uintptr_t target = is_code_of_ours(callee.kind) ? chain : callee.entry;
        if (target && !s.hooks_on(target, owner.c_str()).empty()) {
            passed += format("%s%s on %s", passed.empty() ? "" : "; ", who(f[j]).c_str(), s.function(target).c_str());
            chain = target;
            continue;
        }
        detail += format("The game's code crashed after %s called it directly (frame %u): that code may have passed "
                         "it something wrong.", who(f[j]).c_str(), unsigned(j));
        return "probably " + who(f[j]);
    }
    if (passed.empty())
        detail += "No mod code is on the stack.";
    else
        detail += "Mod code on the stack only passed the call on to the game through its hooks (" + passed + ").";
    if (!maybe.empty())
        detail += " Further up, the stack may hold code of " + maybe + " (frames marked ?: perhaps left over from an "
                  "earlier call).";
    return "the game itself (a native crash)";
}

// ── Text ─────────────────────────────────────────────────────────────────────────────────────────────────────────
std::string cxx_type(const EXCEPTION_RECORD *r)
{
    if (r->ExceptionCode != 0xE06D7363 || r->NumberParameters < 3) return "";
    std::uint32_t info = static_cast<std::uint32_t>(r->ExceptionInformation[2]), types = 0, n = 0, type = 0, desc = 0;
    if (!safe_read(info + 12, &types, 4) || !safe_read(types, &n, 4) || !n || !safe_read(types + 4, &type, 4) ||
        !safe_read(type + 4, &desc, 4))
        return "";
    char name[160] = {};
    for (std::size_t i = 0; i + 1 < sizeof name && safe_read(desc + 8 + i, &name[i], 1) && name[i]; i++) {}
    DbgHelp &d = dbghelp();
    char out[256];
    if (name[0] == '.' && d.UnDecorateSymbolName && d.UnDecorateSymbolName(name + 1, out, sizeof out, 0x2800)) return out;
    return name;
}

std::string exception_text(const Event &e)
{
    if (e.what) return e.what;
    const EXCEPTION_RECORD *r = e.record;
    std::string s = format("%s (0x%08lX)", exception_name(r->ExceptionCode), r->ExceptionCode);
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        s += format(": %s address 0x%08lX", op == 0 ? "reading" : op == 1 ? "writing" : op == 8 ? "executing" : "accessing",
                    static_cast<unsigned long>(r->ExceptionInformation[1]));
    } else if (r->ExceptionCode == 0xE06D7363) {
        std::string t = cxx_type(r);
        if (!t.empty()) s += ": " + t;
    }
    return s;
}

std::string time_text(const SYSTEMTIME &t)
{
    return format("%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void stack_text(std::string &out, const Scene &s, const std::vector<Where> &f, const std::vector<std::uintptr_t> &extra)
{
    out += "\nCall stack\n";
    for (std::size_t i = 0; i < f.size(); i++)
        out += format("  #%-2u%c %08X  %s\n", unsigned(i), f[i].sure ? ' ' : '?', unsigned(f[i].address), f[i].label.c_str());
    if (std::any_of(f.begin(), f.end(), [](const Where &w) { return !w.sure; }))
        out += "  (? = found by scanning past a function without a frame pointer: may be left over from an earlier call)\n";
    if (!extra.empty()) {
        out += "\nOther return addresses on the stack (found by scanning it; some may be left over from earlier calls)\n";
        for (auto a : extra) out += format("      %08X  %s\n", unsigned(a), s.where(a).label.c_str());
    }
}

void registers_text(std::string &out, const CONTEXT &c)
{
    out += format("\nRegisters\n  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\n"
                  "  EBP=%08lX ESP=%08lX EIP=%08lX EFLAGS=%08lX\n",
                  c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp, c.Eip, c.EFlags);
}

void mods_text(std::string &out, const Scene &s)
{
    out += "\nMods\n";
    if (!s.mods_ok) out += "  (the list was busy: the crashed thread may have been changing it)\n";
    else if (s.mods.empty()) out += "  none\n";
    for (auto &m : s.mods) {
        std::string what;
        if (m.flags & 1) what += "client ";
        if (m.flags & 2) what += "server ";
        if (m.flags & 4) what += "lobby ";
        out += format("  %-24s version %-4u %-20s %s%s%s\n", m.folder.c_str(), m.version, what.c_str(),
                      m.active ? "active" : "inactive", m.has_dll ? ", mod.dll" : ", data only",
                      m.pending_free ? ", unloaded (mod.dll still mapped)" : "");
    }
    out += "\nChanges to the game's code\n";
    if (!s.changes_ok) out += "  (the registry was busy: the crashed thread may have been changing it)\n";
    for (auto &c : s.changes) {
        const char *kind = c.kind == Change::hook ? "hook " : c.kind == Change::slot ? "slot " : "patch";
        std::string at = s.where(c.at).label;
        if (c.kind == Change::patch) at = format("%08X (%u bytes)", unsigned(c.at), unsigned(c.size));
        out += format("  %s %-60s %s: \"%s\"\n", kind, at.c_str(), Scene::owner(c.owner).c_str(), c.what.c_str());
    }
}

void memory_text(std::string &out)
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    PROCESS_MEMORY_COUNTERS_EX pm{};
    out += "\nMemory\n";
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pm), sizeof pm))
        out += format("  the game: %lu MB private, %lu MB in RAM (peak %lu MB)\n", pm.PrivateUsage >> 20,
                      pm.WorkingSetSize >> 20, pm.PeakWorkingSetSize >> 20);
    if (GlobalMemoryStatusEx(&ms)) {
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(reinterpret_cast<char *>(GetModuleHandleA(nullptr)) +
                                                          reinterpret_cast<IMAGE_DOS_HEADER *>(GetModuleHandleA(nullptr))->e_lfanew);
        bool laa = nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE;
        out += format("  address space: %llu of %llu MB used (%s)\n", (ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20,
                      ms.ullTotalVirtual >> 20, laa ? "large address aware" : "not large address aware: 2 GB");
        out += format("  the computer: %llu of %llu MB RAM in use (%lu %%)\n", (ms.ullTotalPhys - ms.ullAvailPhys) >> 20,
                      ms.ullTotalPhys >> 20, ms.dwMemoryLoad);
    }
}

void system_text(std::string &out, const Scene &s)
{
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (auto f = reinterpret_cast<RtlGetVersion_t>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"))))
        f(&v);
    const char *wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") ? " (Wine)" : "";
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    out += format("\nSystem\n  Windows %lu.%lu.%lu%s, %lu processors\n  command line: %s\n", v.dwMajorVersion,
                  v.dwMinorVersion, v.dwBuildNumber, wine, si.dwNumberOfProcessors, GetCommandLineA());
    out += "\nModules\n";
    for (auto &m : s.modules)
        out += format("  %08X %08X  %s\n", unsigned(m.base), unsigned(m.size), m.path.c_str());
}

std::string game_text(const Scene &s)
{
    if (!s.named) return "not the supported SADK.exe (no names)";
    std::int32_t build = 0, tick = -1;
    std::uint32_t sys = 0;
    safe_read(reinterpret_cast<std::uintptr_t>(sadk::game::var::g_BuildVersion.get()), &build, 4);
    std::string t = format("SADK.exe build %d", int(build));
    if (safe_read(reinterpret_cast<std::uintptr_t>(sadk::game::var::g_pGameSystem.get()), &sys, 4) && sys &&
        safe_read(sys + 0x88, &tick, 4) && tick >= 0)
        t += format(", logic tick %d", int(tick));   // NLogic::System +0x88 tickCounter (docs/sim/logic-core.md)
    else
        t += ", no match running";
    return t;
}

}  // namespace

std::string describe(const Event &e, bool fatal, std::string &verdict)
{
    Scene s;
    Stack st(e);
    std::vector<Frame> raw = walk(st, e, s);
    std::vector<Where> frames;
    for (auto &r : raw) {
        frames.push_back(s.where(r.address));
        frames.back().sure = r.sure;
    }
    std::vector<std::uintptr_t> extra = scan(st, s, raw);
    std::string detail;
    verdict = cause(s, frames, detail);
    bool game_cause = verdict.rfind("the game", 0) == 0;
    if (game_cause && s.changes_ok)
        for (auto &c : s.changes)
            if (!c.owner.empty()) {
                detail += " Mods can still change the game's state without being on the stack: if this crash comes "
                          "back, try without the mods that change the game's code (below).";
                break;
            }

    std::string out;
    DWORD up = (GetTickCount() - start_tick()) / 1000;
    if (fatal) out += "Die Siedler - Aufbruch der Kulturen crashed. Report of the sadkmod shim (wsock32.dll).\n\n";
    out += format("Time:        %s (%lu:%02lu:%02lu after the start)\n", time_text(e.time).c_str(), up / 3600, up / 60 % 60, up % 60);
    out += "Cause:       " + verdict + "\n";
    out += "             " + detail + "\n";
    out += "What:        " + exception_text(e) + "\n";
    out += "Where:       " + (frames.empty() ? std::string("?") : format("%08X  %s", unsigned(frames[0].address), frames[0].label.c_str())) + "\n";
    out += format("Thread:      %lu%s\n", e.thread_id, e.main_thread ? " (the game's main thread)" : "");
    out += std::string("Reported by: ") + e.source + "\n";
    out += "Game:        " + game_text(s) + "\n";
    if (fatal) out += "Shim:        built " __DATE__ " " __TIME__ "\n";
    stack_text(out, s, frames, extra);
    registers_text(out, *e.context);
    if (!fatal) return out;
    mods_text(out, s);
    int errors = 0;
    std::string ef = errors_file(&errors);
    out += format("\nErrors the game survived before, this session: %d%s%s\n", errors, ef.empty() ? "" : " (", ef.empty() ? "" : (ef + ")").c_str());
    out += "\nThe game's log (the last lines, every level; Logfile_0.txt has only level 1)\n" + recent_game_log();
    out += "\nThe shim's log (the last lines; all of it is in wsock32_shim.txt)\n" + recent_shim_log();
    memory_text(out);
    system_text(out, s);
    return out;
}

bool write_dump(const char *path, const Event &e, const std::string &comment, std::string &note)
{
    DbgHelp &d = dbghelp();
    if (!d.MiniDumpWriteDump) {
        note = "no dbghelp.dll: no dump";
        return false;
    }
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        note = format("cannot create %s (error %lu)", path, GetLastError());
        return false;
    }
    EXCEPTION_POINTERS ep{const_cast<EXCEPTION_RECORD *>(e.record), const_cast<CONTEXT *>(e.context)};
    MINIDUMP_EXCEPTION_INFORMATION mei{e.thread_id, &ep, FALSE};
    MINIDUMP_USER_STREAM us{CommentStreamA, static_cast<ULONG>(comment.size() + 1), const_cast<char *>(comment.c_str())};
    MINIDUMP_USER_STREAM_INFORMATION usi{1, &us};
    // full memory, its layout, handles, thread times, unloaded modules; skip unreadable memory (0x20000)
    const DWORD kinds[] = {0x2 | 0x800 | 0x4 | 0x1000 | 0x20 | 0x20000, 0x2 | 0x800 | 0x4 | 0x20, 0x2, 0x0};
    for (DWORD k : kinds) {
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        if (d.MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, static_cast<MINIDUMP_TYPE>(k),
                                e.record ? &mei : nullptr, &usi, nullptr)) {
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            note = format("dump type 0x%lx, %lld MB", k, size.QuadPart >> 20);
            CloseHandle(f);
            return true;
        }
        note += format("dump type 0x%lx failed (error 0x%lx); ", k, GetLastError());
    }
    CloseHandle(f);
    DeleteFileA(path);
    return false;
}

void prepare() { dbghelp(); }

}  // namespace shim::report
