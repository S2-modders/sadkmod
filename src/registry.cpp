#include <sadkmod/patch.hpp>
#include <sadkmod/registry.hpp>
#include <sadkmod/runtime.hpp>

#include <MinHook.h>
#include <windows.h>

#include <climits>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace sadk::registry {

namespace {

CRITICAL_SECTION cs;

struct Lock {
    Lock() { EnterCriticalSection(&cs); }
    ~Lock() { LeaveCriticalSection(&cs); }
};

bool ready()
{
    static bool ok = [] {
        InitializeCriticalSection(&cs);
        MH_STATUS st = MH_Initialize();
        return st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
    }();
    return ok;
}

// ── Order ────────────────────────────────────────────────────────────────────────────────────────────────────────
// Lower rank runs first (outermost). The host ("") is always innermost, next to the game's code; an owner without
// a rank comes just before it. Equal ranks keep the order of installation.
std::map<std::string, int> ranks;

int rank_of(const std::string &owner)
{
    if (owner.empty()) return INT_MAX;
    auto it = ranks.find(owner);
    return it == ranks.end() ? INT_MAX - 1 : it->second;
}

template <class L>
std::size_t position(const std::vector<L> &links, const std::string &owner)
{
    int r = rank_of(owner);
    std::size_t i = 0;
    while (i < links.size() && rank_of(links[i].owner) <= r) i++;
    return i;
}

// ── Hooks ────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Link {
    std::string owner, what;
    void *detour;
    void **original;   // the owner's variable that leads on to the next older detour
};

// One per hooked target, never freed: a thread may still pass through its stub at any time.
struct Chain {
    void *volatile top = nullptr;   // where the stub jumps: the newest detour, or the trampoline
    void *trampoline = nullptr;     // MinHook's: the game's own code
    std::uint8_t *stub = nullptr;   // JMP [top]; the MinHook hook leads here
    std::vector<Link> links;        // in call order: links[0] runs first
};
std::map<void *, Chain *> chains;

std::uint8_t *new_stub(void *volatile *top)
{
    static std::uint8_t *page, *next;
    if (!page || next + 8 > page + 4096) {
        page = next = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!page) return nullptr;
    }
    std::uint8_t *s = next;
    next += 8;
    s[0] = 0xFF;   // JMP DWORD PTR [top]
    s[1] = 0x25;
    std::uint32_t a = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(top));
    std::memcpy(s + 2, &a, 4);
    s[6] = s[7] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), s, 8);
    return s;
}

// Removes links[i] from its chain: whatever led to it now leads to what it led to.
void unlink(Chain *c, std::size_t i)
{
    void *next = i + 1 < c->links.size() ? c->links[i + 1].detour : c->trampoline;
    if (i == 0)
        c->top = next;
    else
        *c->links[i - 1].original = next;
    c->links.erase(c->links.begin() + static_cast<std::ptrdiff_t>(i));
}

// ── Table slots ──────────────────────────────────────────────────────────────────────────────────────────────────
struct SlotLink {
    std::string owner, what;
    void *value;
    void **previous;   // the owner's copy of the entry below it (may be null)
};
struct Slot {
    void *base;                   // the entry before anyone changed it
    std::vector<SlotLink> links;  // in call order: links[0] is the entry in the table
};
std::map<void **, Slot> slots;

// ── Patches ──────────────────────────────────────────────────────────────────────────────────────────────────────
struct PatchRecord {
    std::string owner, what;
    std::uint8_t *at;
    std::uintptr_t static_address;
    std::vector<std::uint8_t> before, after;
};
std::vector<PatchRecord> patches;

}  // namespace

void set_rank(const char *owner, int rank)
{
    if (!ready()) return;
    Lock lock;
    ranks[owner] = rank;
}

bool hook(const char *owner, void *target, void *detour, void **original, const char *what)
{
    if (!ready() || !target || !detour || !original) {
        log("hook %s: MinHook unavailable or no target", what);
        return false;
    }
    Lock lock;
    Chain *&c = chains[target];
    if (!c) c = new Chain;
    for (auto &l : c->links)
        if (l.detour == detour) {
            log("hook %s at %p: this detour is hooked there already", what, target);
            return false;
        }
    bool first = !c->stub;
    if (first) {
        c->stub = new_stub(&c->top);
        MH_STATUS st = c->stub ? MH_CreateHook(target, c->stub, &c->trampoline) : MH_ERROR_MEMORY_ALLOC;
        if (st != MH_OK) {
            log("hook %s at %p: %s", what, target, MH_StatusToString(st));
            c->stub = nullptr;
            return false;
        }
        c->top = c->trampoline;
    }
    std::size_t i = position(c->links, owner);
    *original = i < c->links.size() ? c->links[i].detour : c->trampoline;   // before anything leads to it
    if (i == 0)
        c->top = detour;
    else
        *c->links[i - 1].original = detour;
    c->links.insert(c->links.begin() + static_cast<std::ptrdiff_t>(i), Link{owner, what, detour, original});
    if (first) {
        MH_STATUS st = MH_EnableHook(target);
        if (st != MH_OK) {
            log("hook %s at %p: %s", what, target, MH_StatusToString(st));
            c->links.clear();
            c->top = c->trampoline;
            return false;
        }
    }
    log("hook %s at %p: installed%s", what, target, c->links.size() > 1 ? " (in a chain, by load order)" : "");
    return true;
}

bool unhook(const char *owner, void *target, void *detour)
{
    if (!ready()) return false;
    Lock lock;
    auto it = chains.find(target);
    if (it == chains.end()) return false;
    Chain *c = it->second;
    for (std::size_t i = 0; i < c->links.size(); i++)
        if (c->links[i].detour == detour && c->links[i].owner == owner) {
            log("hook %s at %p: removed", c->links[i].what.c_str(), target);
            unlink(c, i);
            return true;
        }
    return false;
}

bool write_slot(const char *owner, void **slot, void *value, void **previous, const char *what)
{
    if (!ready() || !slot) return false;
    Lock lock;
    auto [it, fresh] = slots.try_emplace(slot);
    Slot &s = it->second;
    if (fresh) s.base = *slot;
    std::size_t i = position(s.links, owner);
    if (previous) *previous = i < s.links.size() ? s.links[i].value : s.base;
    bool ok = true;
    if (i == 0)
        ok = write_memory(slot, &value, sizeof value);
    else if (s.links[i - 1].previous)
        *s.links[i - 1].previous = value;
    else
        log("hook %s (table slot %p): the entry before it keeps no previous - it is never reached", what,
            static_cast<void *>(slot));
    if (ok) s.links.insert(s.links.begin() + static_cast<std::ptrdiff_t>(i), SlotLink{owner, what, value, previous});
    log("hook %s (table slot %p): %s", what, static_cast<void *>(slot), ok ? "installed" : "write failed");
    return ok;
}

bool patch(const char *owner, std::uint8_t *at, std::uintptr_t static_address, const std::uint8_t *expect,
           const std::uint8_t *replace, std::size_t n, const char *what)
{
    if (!ready()) return false;
    Lock lock;
    if (IsBadReadPtr(at, n)) {
        log("patch %s at %08x: not readable - not applied", what, unsigned(static_address));
        return false;
    }
    bool applied = std::memcmp(at, replace, n) == 0;
    if (!applied) {
        if (std::memcmp(at, expect, n) != 0) {
            log("patch %s at %08x: unexpected bytes - not applied", what, unsigned(static_address));
            return false;
        }
        if (!write_memory(at, replace, n)) return false;
        log("patch %s at %08x: applied", what, unsigned(static_address));
    }
    patches.push_back(PatchRecord{owner, what, at, static_address, {expect, expect + n}, {replace, replace + n}});
    return true;
}

Removed remove_owner(const char *owner)
{
    Removed r;
    if (!ready()) return r;
    Lock lock;
    for (auto &[target, c] : chains)
        for (std::size_t i = c->links.size(); i-- > 0;)
            if (c->links[i].owner == owner) {
                log("hook %s at %p: removed", c->links[i].what.c_str(), target);
                unlink(c, i);
                r.hooks++;
            }
    for (auto &[slot, s] : slots)
        for (std::size_t i = s.links.size(); i-- > 0;)
            if (s.links[i].owner == owner) {
                void *next = i + 1 < s.links.size() ? s.links[i + 1].value : s.base;
                if (i == 0)
                    write_memory(slot, &next, sizeof next);
                else if (s.links[i - 1].previous)
                    *s.links[i - 1].previous = next;
                log("hook %s (table slot %p): removed", s.links[i].what.c_str(), static_cast<void *>(slot));
                s.links.erase(s.links.begin() + static_cast<std::ptrdiff_t>(i));
                r.slots++;
            }
    for (std::size_t i = patches.size(); i-- > 0;) {
        PatchRecord &p = patches[i];
        if (p.owner != owner) continue;
        bool shared = false;
        for (auto &q : patches)
            if (&q != &p && q.owner != owner && q.at == p.at && q.after == p.after) shared = true;
        if (shared) {
            log("patch %s at %08x: kept (another mod made the same patch)", p.what.c_str(), unsigned(p.static_address));
        } else if (std::memcmp(p.at, p.after.data(), p.after.size()) == 0) {
            write_memory(p.at, p.before.data(), p.before.size());
            log("patch %s at %08x: undone", p.what.c_str(), unsigned(p.static_address));
        } else {
            log("patch %s at %08x: CHANGED SINCE - left as it is", p.what.c_str(), unsigned(p.static_address));
            r.not_restored++;
        }
        patches.erase(patches.begin() + static_cast<std::ptrdiff_t>(i));
        r.patches++;
    }
    return r;
}

int count_owned(const char *owner)
{
    if (!ready()) return 0;
    Lock lock;
    int n = 0;
    for (auto &[t, c] : chains)
        for (auto &l : c->links) n += l.owner == owner;
    for (auto &[slot, s] : slots)
        for (auto &l : s.links) n += l.owner == owner;
    for (auto &p : patches) n += p.owner == owner;
    return n;
}

}  // namespace sadk::registry
