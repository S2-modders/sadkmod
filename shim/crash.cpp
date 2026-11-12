// The crash report (crash.hpp): the handlers, the errors the game survives, the report thread and the files.
#include "crash.hpp"

#include <sadkmod/game/sadk_noav/fn/_global.hpp>
#include <sadkmod/game/sadk_noav/fn/std_.hpp>
#include <sadkmod/game/sadk_noav/vars.hpp>
#include <sadkmod/hook.hpp>
#include <sadkmod/host.hpp>
#include <sadkmod/msvc.hpp>

#include <miniz.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using sadk::log;

namespace shim::report {

std::string format(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < int(sizeof buf)) return std::string(buf, n < 0 ? 0 : n);
    std::string s(n + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(s.data(), s.size(), fmt, ap);
    va_end(ap);
    s.resize(n);
    return s;
}

const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (the C runtime found a damaged stack)";
    case 0xC000000D: return "STATUS_INVALID_PARAMETER (the C runtime got an invalid parameter)";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000017: return "STATUS_NO_MEMORY";
    case 0xE06D7363: return "C++ exception";
    case 0x40000015: return "STATUS_FATAL_APP_EXIT";
    default: return "exception";
    }
}

bool safe_read(std::uintptr_t address, void *out, std::size_t n)
{
    SIZE_T got = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, n, &got) && got == n;
}

}  // namespace shim::report

namespace shim::crash {

using namespace shim::report;

namespace {

// ── State ────────────────────────────────────────────────────────────────────────────────────────────────────────
HMODULE self;
DWORD tick0;
char stamp[32];          // the session's start
DWORD main_tid;
bool game;               // the supported SADK.exe
bool started;
std::uintptr_t exe_lo, exe_hi, tincat_lo, tincat_hi, self_lo, self_hi;
char default_dir[MAX_PATH];   // <My Documents>\SAdK\dumps (CApplicationEx::Initialize S 004075d0 step 3)
char shim_log_path[MAX_PATH];

HANDLE worker, wake, done;
DWORD worker_tid;

// A spin lock for state the exception handlers touch (they must not wait on anything a crashed thread may hold
// for long, and a CRITICAL_SECTION of a thread that died would never be released).
struct Spin {
    volatile LONG v = 0;
    bool lock(DWORD wait_ms = INFINITE)
    {
        for (DWORD start = GetTickCount(); InterlockedExchange(&v, 1);) {
            if (wait_ms != INFINITE && GetTickCount() - start >= wait_ms) return false;
            YieldProcessor();
            Sleep(0);
        }
        return true;
    }
    void unlock() { InterlockedExchange(&v, 0); }
};

std::uintptr_t range_of(HMODULE h, std::uintptr_t &hi)
{
    if (!h) return hi = 0;
    auto *base = reinterpret_cast<std::uint8_t *>(h);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    hi = reinterpret_cast<std::uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
    return reinterpret_cast<std::uintptr_t>(base);
}

void make_stamp(char *out, std::size_t n, const SYSTEMTIME &t)
{
    std::snprintf(out, n, "%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// ── Logs ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// The last N lines, each a time and a text; a text that repeats the one before only counts.
template <int N, int W>
struct Ring {
    char when[N][16];
    char lines[N][W];
    int repeats[N];
    int next = 0, count = 0;
    Spin lock;
    void add(const char *time, const char *text)
    {
        if (!lock.lock(50)) return;
        int last = (next - 1 + N) % N;
        if (count && !std::strncmp(lines[last], text, W - 1)) {
            repeats[last]++;
            std::snprintf(when[last], sizeof when[last], "%s", time);
        } else {
            std::snprintf(when[next], sizeof when[next], "%s", time);
            std::snprintf(lines[next], W, "%s", text);
            repeats[next] = 1;
            next = (next + 1) % N;
            if (count < N) count++;
        }
        lock.unlock();
    }
    std::string text()
    {
        std::string s;
        if (!lock.lock(300)) return "  (busy)\n";
        for (int i = 0; i < count; i++) {
            int k = (next - count + i + N) % N;
            s += format("  %s  %s%s\n", when[k], lines[k], repeats[k] > 1 ? format("  (%d times, the last at this time)", repeats[k]).c_str() : "");
        }
        lock.unlock();
        return s.empty() ? "  (nothing)\n" : s;
    }
};
Ring<200, 256> game_ring;
Ring<60, 300> shim_ring;

void on_shim_log(const char *line)   // "YYYY-MM-DD hh:mm:ss  text"
{
    char time[16];
    std::snprintf(time, sizeof time, "%.8s", std::strlen(line) > 19 ? line + 11 : "");
    shim_ring.add(time, std::strlen(line) > 21 ? line + 21 : line);
}

// Copies a string of `unit`-byte characters (1 or 2) without faulting: up to `max` characters, until a NUL or an
// unreadable page. Text inside the game's own image (its format strings) is read directly; anything else a page at a
// time with safe_read. Returns the characters copied; `bad` when not even the first could be read.
std::size_t read_string(std::uintptr_t p, void *out, std::size_t max, int unit, bool &bad)
{
    auto *o = static_cast<std::uint8_t *>(out);
    std::size_t n = 0;
    bad = false;
    if (p >= exe_lo && p + max * unit <= exe_hi) {
        for (; n < max; n++) {
            std::uint16_t c = unit == 2 ? *reinterpret_cast<const std::uint16_t *>(p + 2 * n) : *reinterpret_cast<const std::uint8_t *>(p + n);
            if (!c) break;
            std::memcpy(o + n * unit, &c, unit);
        }
        return n;
    }
    while (n < max) {
        std::uintptr_t a = p + n * unit;
        std::size_t chunk = std::min<std::size_t>((max - n) * unit, 4096 - (a & 4095)) / unit;
        if (!chunk) chunk = 1;
        if (!safe_read(a, o + n * unit, chunk * unit)) {
            bad = n == 0;
            return n;
        }
        for (std::size_t i = 0; i < chunk; i++, n++) {
            std::uint16_t c = 0;
            std::memcpy(&c, o + n * unit, unit);
            if (!c) return n;
        }
    }
    return n;
}

// printf for the game's log lines, safe against bad arguments: a level the game does not log (above g_LogLevel) has
// never been formatted, and a call with a wrong argument there must not crash the game now. Strings are read with
// safe_read; every other value is formatted on its own.
void safe_vformat(char *out, std::size_t cap, const char *fmt_ptr, va_list ap)
{
    char fmt[512];
    bool bad;
    std::size_t fl = read_string(reinterpret_cast<std::uintptr_t>(fmt_ptr), fmt, sizeof fmt - 1, 1, bad);
    fmt[fl] = 0;
    std::size_t o = 0;
    auto put = [&](const char *s, std::size_t n) {
        n = std::min(n, cap - 1 - o);
        std::memcpy(out + o, s, n);
        o += n;
    };
    for (std::size_t i = 0; i < fl && o + 1 < cap;) {
        if (fmt[i] != '%') {
            put(&fmt[i++], 1);
            continue;
        }
        std::size_t start = i++;
        if (fmt[i] == '%') {
            put("%", 1);
            i++;
            continue;
        }
        std::string spec = "%";
        while (fmt[i] && std::strchr("-+ #0", fmt[i])) spec += fmt[i++];
        if (fmt[i] == '*') { spec += std::to_string(va_arg(ap, int)); i++; }
        else while (fmt[i] >= '0' && fmt[i] <= '9') spec += fmt[i++];
        int prec = -1;
        if (fmt[i] == '.') {
            spec += fmt[i++];
            if (fmt[i] == '*') { prec = va_arg(ap, int); spec += std::to_string(prec); i++; }
            else { prec = 0; while (fmt[i] >= '0' && fmt[i] <= '9') { prec = prec * 10 + (fmt[i] - '0'); spec += fmt[i++]; } }
        }
        int len = 0;   // 0 int, 2 64-bit, 3 short/narrow, 4 long/wide
        if (fmt[i] == 'h') { i++; if (fmt[i] == 'h') i++; len = 3; }
        else if (fmt[i] == 'l') { i++; if (fmt[i] == 'l') { i++; len = 2; } else len = 4; }
        else if (fmt[i] == 'I' && fmt[i + 1] == '6' && fmt[i + 2] == '4') { i += 3; len = 2; }
        else if (fmt[i] == 'I' && fmt[i + 1] == '3' && fmt[i + 2] == '2') i += 3;
        else if (fmt[i] == 'I' || fmt[i] == 'z' || fmt[i] == 't' || fmt[i] == 'L') i++;
        else if (fmt[i] == 'j') { i++; len = 2; }
        else if (fmt[i] == 'w') { i++; len = 4; }
        char conv = fmt[i++];
        char piece[300];
        piece[0] = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (len == 2) std::snprintf(piece, sizeof piece, (spec + "ll" + conv).c_str(), va_arg(ap, long long));
            else std::snprintf(piece, sizeof piece, (spec + conv).c_str(), va_arg(ap, int));
            break;
        case 'c': case 'C': {
            int c = va_arg(ap, int);
            std::snprintf(piece, sizeof piece, (spec + 'c').c_str(), c > 0 && c < 128 ? c : '?');
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            std::snprintf(piece, sizeof piece, (spec + conv).c_str(), va_arg(ap, double));
            break;
        case 'p':
            std::snprintf(piece, sizeof piece, "%08X", va_arg(ap, unsigned));
            break;
        case 's': case 'S': {
            std::uintptr_t p = reinterpret_cast<std::uintptr_t>(va_arg(ap, void *));
            bool wide = (conv == 'S' && len != 3) || len == 4;
            char text[201];
            std::uint16_t w[200];
            std::size_t max = prec >= 0 ? std::min<std::size_t>(prec, 200) : 200;
            if (!p) {
                std::strcpy(text, "(null)");
            } else if (wide) {
                std::size_t n = read_string(p, w, max, 2, bad);
                for (std::size_t k = 0; k < n; k++) text[k] = w[k] < 128 ? static_cast<char>(w[k]) : '?';
                text[n] = 0;
            } else {
                text[read_string(p, text, max, 1, bad)] = 0;
            }
            if (p && bad) std::strcpy(text, "(bad pointer)");
            std::snprintf(piece, sizeof piece, (spec + 's').c_str(), text);
            break;
        }
        case 'n':
            (void)va_arg(ap, void *);
            break;
        default:   // not understood: the rest as it is
            put(fmt + start, fl - start);
            out[o] = 0;
            return;
        }
        put(piece, std::strlen(piece));
    }
    out[o] = 0;
}

}  // namespace

// Log_Printf S 006e5950 (cdecl, variadic) enters here through log_printf_thunk, which passes its arguments on and
// then jumps to the game's function with the stack as it was.
extern "C" void *shim_log_printf_original;
void *shim_log_printf_original;
extern "C" void SADK_CDECL shim_log_capture(int level, const char *fmt, va_list ap)
{
    char text[230], line[256];
    safe_vformat(text, sizeof text, fmt, ap);
    for (char *c = text; *c; c++)
        if (*c == '\n' || *c == '\r') *c = ' ';
    SYSTEMTIME t;
    GetLocalTime(&t);
    char time[16];
    std::snprintf(time, sizeof time, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::snprintf(line, sizeof line, "%d  %s", level, text);
    game_ring.add(time, line);
}
extern "C" void shim_log_printf_thunk();
asm(R"(
    .text
    .globl _shim_log_printf_thunk
_shim_log_printf_thunk:
    leal 12(%esp), %eax
    pushl %eax
    pushl 12(%esp)
    pushl 12(%esp)
    call _shim_log_capture
    addl $12, %esp
    jmp *_shim_log_printf_original
)");

namespace {

// ── Errors the game survives ─────────────────────────────────────────────────────────────────────────────────────
// Each place (exception code and address) is recorded the first time; later ones only count. The handler copies the
// exception, the registers and the top of the stack into a free slot; the report thread writes it out once it is
// 300 ms old, unless a fatal crash claimed it meanwhile (then it is the crash, not a survived error).
struct Site {
    DWORD code;
    std::uintptr_t address;
    LONG count;
    char label[200];   // its place, for the summary (written by the report thread)
};
Site sites[256];
int n_sites;
LONG not_recorded;    // places beyond the table
Spin sites_lock;

struct Slot {
    volatile LONG state;   // 0 free, 1 being filled, 2 ready, 3 claimed by a fatal crash
    DWORD tick, tid;
    int site;
    SYSTEMTIME time;
    EXCEPTION_RECORD record;
    CONTEXT context;
    std::uint32_t stack_base, stack_size;
    std::uint8_t stack[16384];
};
Slot slots[8];

char errors_path[MAX_PATH];   // "" until the first error is written
int errors_written;
bool errors_zipped;           // moved into a crash's ZIP

// Modules other than the game's and the shim's: is it a mod's (below <game>\mods\)? A small cache by handle.
struct ModCache { HMODULE h; bool mod; };
ModCache mod_cache[64];
int n_mod_cache;
Spin mod_cache_lock;
char mods_prefix[MAX_PATH];
std::size_t mods_prefix_len;

bool is_mod_module(std::uintptr_t a)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(a), &h) || !h)
        return false;
    mod_cache_lock.lock();
    for (int i = 0; i < n_mod_cache; i++)
        if (mod_cache[i].h == h) {
            bool m = mod_cache[i].mod;
            mod_cache_lock.unlock();
            return m;
        }
    mod_cache_lock.unlock();
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(h, path, MAX_PATH);
    bool m = n > mods_prefix_len && !_strnicmp(path, mods_prefix, mods_prefix_len);
    mod_cache_lock.lock();
    if (n_mod_cache < 64) mod_cache[n_mod_cache++] = ModCache{h, m};
    mod_cache_lock.unlock();
    return m;
}

bool ours_or_game(std::uintptr_t a)
{
    return (a >= exe_lo && a < exe_hi) || (a >= tincat_lo && a < tincat_hi) || (a >= self_lo && a < self_hi) ||
           is_mod_module(a);
}

// -1: not to be recorded (known place, or the table is full)
int count_site(DWORD code, std::uintptr_t address)
{
    sites_lock.lock();
    for (int i = 0; i < n_sites; i++)
        if (sites[i].code == code && sites[i].address == address) {
            sites[i].count++;
            sites_lock.unlock();
            return -1;
        }
    int i = -1;
    if (n_sites < 256) {
        i = n_sites++;
        sites[i] = Site{code, address, 1, ""};
    } else {
        not_recorded++;
    }
    sites_lock.unlock();
    return i;
}

volatile LONG fatal_state;   // 0 none, 1 a report is being written, 2 written

LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xC0000000) != 0xC0000000) return EXCEPTION_CONTINUE_SEARCH;   // errors only
    if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;   // fatal; almost no stack left
    if (fatal_state || GetCurrentThreadId() == worker_tid) return EXCEPTION_CONTINUE_SEARCH;
    std::uintptr_t at = reinterpret_cast<std::uintptr_t>(r->ExceptionAddress);
    bool mine = ours_or_game(at);
    if (!mine && r->ExceptionCode == 0xE06D7363 && r->NumberParameters >= 3)   // a C++ throw: by its ThrowInfo
        mine = ours_or_game(static_cast<std::uintptr_t>(r->ExceptionInformation[2]));
    if (!mine) return EXCEPTION_CONTINUE_SEARCH;
    int site = count_site(r->ExceptionCode, at);
    if (site < 0) return EXCEPTION_CONTINUE_SEARCH;
    for (Slot &s : slots) {
        if (InterlockedCompareExchange(&s.state, 1, 0) != 0) continue;
        s.tick = GetTickCount();
        s.tid = GetCurrentThreadId();
        s.site = site;
        GetLocalTime(&s.time);
        s.record = *r;
        s.record.ExceptionRecord = nullptr;
        s.context = *ep->ContextRecord;
        // the stack from ESP to its base: committed memory of this thread, read directly
        NT_TIB *tib;   // the thread's TIB (FS:[0x18] points at itself)
        asm("movl %%fs:0x18, %0" : "=r"(tib));
        std::uintptr_t esp = s.context.Esp, top = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        s.stack_base = static_cast<std::uint32_t>(esp);
        s.stack_size = esp < top && esp >= reinterpret_cast<std::uintptr_t>(tib->StackLimit)
                           ? static_cast<std::uint32_t>(std::min<std::uintptr_t>(top - esp, sizeof s.stack)) : 0;
        if (s.stack_size) std::memcpy(s.stack, reinterpret_cast<void *>(esp), s.stack_size);
        InterlockedExchange(&s.state, 2);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedIncrement(&not_recorded);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ── Files ────────────────────────────────────────────────────────────────────────────────────────────────────────
// The game's dump folder: g_CrashDirectory S 00882808 (a std::string, set by CApplicationEx::Initialize from -log_path
// or <My Documents>\SAdK\dumps), or that default before the game set it.
std::string dump_dir()
{
    std::string dir;
    if (game) {
        sadk::msvc::string s;
        if (safe_read(0x00882808, &s, sizeof s) && s.size && s.size < MAX_PATH) {
            char buf[MAX_PATH];
            std::uintptr_t text = s.is_inline() ? 0x00882808 + 4 : reinterpret_cast<std::uintptr_t>(s.heap_text);
            if (safe_read(text, buf, s.size)) dir.assign(buf, s.size);
        }
    }
    if (dir.empty()) dir = default_dir;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    for (std::size_t i = 3; i <= dir.size(); i++)   // create it (and its parents) if the game has not yet
        if (i == dir.size() || dir[i] == '\\') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
    return dir;
}

bool write_file(const std::string &path, const std::string &text, bool append)
{
    HANDLE f = CreateFileA(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    CloseHandle(f);
    return ok;
}

bool read_file(const std::string &path, std::string &out)
{
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Keeps the newest `keep` files of a pattern (the time in their names sorts them).
void prune(const std::string &dir, const char *pattern, std::size_t keep)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do names.push_back(fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    for (std::size_t i = 0; i + keep < names.size(); i++) DeleteFileA((dir + "\\" + names[i]).c_str());
}

void prune_all(const std::string &dir)
{
    prune(dir, "crash_*.zip", 5);
    prune(dir, "crashdump_*.dmp", 5);
    prune(dir, "crashlog_*.txt", 5);
    prune(dir, "errors_*.txt", 10);
}

std::string mods_line()
{
    std::vector<sadk::host::ModInfo> mods;
    if (!sadk::host::mods_snapshot(mods, 300)) return "(the mod list was busy)";
    std::string s;
    for (auto &m : mods)
        if (m.active) s += (s.empty() ? "" : ", ") + format("%s (version %u)", m.folder.c_str(), m.version);
    return s.empty() ? "none" : s;
}

void write_error(Slot &s)
{
    Event e;
    e.record = &s.record;
    e.context = &s.context;
    e.thread_id = s.tid;
    e.main_thread = s.tid == main_tid;
    e.source = "an exception the game caught: it played on";
    e.stack = StackCopy{s.stack, s.stack_base, s.stack_size};
    e.time = s.time;
    std::string cause, text = describe(e, false, cause);
    if (s.site >= 0) {   // its place, for the summary: the "Where:" line
        std::size_t w = text.find("Where:");
        std::string where = w == std::string::npos ? "" : text.substr(w + 13, text.find('\n', w) - w - 13);
        std::snprintf(sites[s.site].label, sizeof sites[s.site].label, "%s", where.c_str());
    }
    if (!errors_path[0]) {
        std::string path = dump_dir() + "\\errors_" + stamp + ".txt";
        std::string head = format("Errors the game survived: exceptions that something caught, after which the game played on.\n"
                                  "Session started %s; sadkmod shim (wsock32.dll) built " __DATE__ " " __TIME__ ".\n"
                                  "Each place is listed once, when it first happened; how often each came back is "
                                  "summed up at the end when the game closes.\nMods: %s\n",
                                  stamp, mods_line().c_str());
        if (!write_file(path, head, false)) return;
        std::snprintf(errors_path, sizeof errors_path, "%s", path.c_str());
    }
    std::string sep(79, '-');
    if (write_file(errors_path, "\n" + sep + "\n" + text, true)) errors_written++;
    log("crash report: an error the game survived (%s), written to %s", cause.c_str(), errors_path);
}

// Writes the ready errors that are old enough (all of them with `all`); drops those a fatal crash claimed.
void flush_errors(bool all)
{
    for (Slot &s : slots) {
        if (s.state == 3) {
            InterlockedExchange(&s.state, 0);
            continue;
        }
        if (s.state != 2 || (!all && GetTickCount() - s.tick < 300)) continue;
        write_error(s);
        InterlockedExchange(&s.state, 0);
    }
}

// The end of the errors file: how often each place came back. Once, when the game closes or crashes.
void write_repeats()
{
    static bool written;
    if (written || !errors_path[0] || errors_zipped) return;
    written = true;
    std::string sum;
    for (int i = 0; i < n_sites; i++)
        if (sites[i].count > 1)
            sum += format("  %5ld times  %s (0x%08lX) at %s\n", sites[i].count, exception_name(sites[i].code), sites[i].code,
                          sites[i].label[0] ? sites[i].label : format("%08X", unsigned(sites[i].address)).c_str());
    if (not_recorded) sum += format("  %ld more not recorded (too many places at once)\n", not_recorded);
    if (!sum.empty()) write_file(errors_path, "\n" + std::string(79, '-') + "\nRepeated while the game ran:\n" + sum, true);
}

// ── Fatal crashes ────────────────────────────────────────────────────────────────────────────────────────────────
struct Fatal {
    EXCEPTION_RECORD record;
    CONTEXT context;
    DWORD tid;
    const char *source;
    const char *what;
    char what_buf[160];
    SYSTEMTIME time;
};
Fatal fatal_req;
volatile LONG fatal_pending;
char fatal_summary[600];   // for the game's own log line (WinMain prints its call-stack text)

bool add_to_zip(mz_zip_archive &z, const char *name, const std::string &path, bool big, std::string &note)
{
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;   // not there: nothing to add
    bool ok;
    if (big) {
        ok = mz_zip_writer_add_file(&z, name, path.c_str(), nullptr, 0, MZ_BEST_SPEED);
    } else {   // logs the game still holds open: read with sharing, add from memory
        std::string data;
        ok = read_file(path, data) && mz_zip_writer_add_mem(&z, name, data.data(), data.size(), MZ_BEST_COMPRESSION);
    }
    if (!ok) note += format("%s not packed (%s); ", name, mz_zip_get_error_string(mz_zip_get_last_error(&z)));
    return ok || !big;
}

void handle_fatal()
{
    Fatal &f = fatal_req;
    Event e;
    e.record = &f.record;
    e.context = &f.context;
    e.thread_id = f.tid;
    e.main_thread = f.tid == main_tid;
    e.source = f.source;
    e.what = f.what;
    e.time = f.time;
    flush_errors(true);   // the errors before it, complete in their file before it is packed
    std::string cause, text = describe(e, true, cause);
    std::string dir = dump_dir();
    char t[32];
    make_stamp(t, sizeof t, f.time);
    std::string log_name = std::string("crashlog_") + t + ".txt", dump_name = std::string("crashdump_") + t + ".dmp";
    std::string log_path = dir + "\\" + log_name, dump_path = dir + "\\" + dump_name, zip_path = dir + "\\crash_" + t + ".zip";
    write_file(log_path, text, false);
    std::string note;
    DWORD t0 = GetTickCount();
    bool dumped = write_dump(dump_path.c_str(), e, text, note);
    write_file(log_path, format("\nDump: %s, written in %.1f s\n", note.c_str(), (GetTickCount() - t0) / 1000.0), true);
    log("crash report: %s - %s; dump: %s", cause.c_str(), exception_name(f.record.ExceptionCode), note.c_str());

    // The ZIP: everything a report needs in one file.
    write_repeats();
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    std::string zip_note;
    DWORD t1 = GetTickCount();
    std::string errors_name = errors_path[0] ? std::strrchr(errors_path, '\\') + 1 : "";
    bool zipped = mz_zip_writer_init_file_v2(&z, zip_path.c_str(), 0, 0);
    if (zipped) {
        zipped = add_to_zip(z, log_name.c_str(), log_path, false, zip_note) &&
                 (!dumped || add_to_zip(z, dump_name.c_str(), dump_path, true, zip_note)) &&
                 (errors_name.empty() || add_to_zip(z, errors_name.c_str(), errors_path, false, zip_note));
        add_to_zip(z, "Logfile_0.txt", dir + "\\Logfile_0.txt", false, zip_note);
        add_to_zip(z, "wsock32_shim.txt", shim_log_path, false, zip_note);
        zipped = mz_zip_writer_finalize_archive(&z) && zipped;
        mz_zip_writer_end(&z);
    }
    if (zipped) {
        DeleteFileA(log_path.c_str());
        DeleteFileA(dump_path.c_str());
        if (!errors_name.empty()) {
            DeleteFileA(errors_path);
            errors_zipped = true;
        }
        LARGE_INTEGER size{};
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(zip_path.c_str(), GetFileExInfoStandard, &fa))
            size.QuadPart = (LONGLONG(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        log("crash report: %s (%lld MB, packed in %.1f s)%s%s", zip_path.c_str(), size.QuadPart >> 20,
            (GetTickCount() - t1) / 1000.0, zip_note.empty() ? "" : " - ", zip_note.c_str());
    } else {
        DeleteFileA(zip_path.c_str());
        log("crash report: packing failed (%s) - the files stay as they are: %s, %s", zip_note.c_str(), log_path.c_str(),
            dumped ? dump_path.c_str() : "no dump");
    }
    prune_all(dir);
    std::snprintf(fatal_summary, sizeof fatal_summary, "\nCause: %s\nThe crash report: %s\n", cause.c_str(),
                  zipped ? zip_path.c_str() : log_path.c_str());
}

DWORD WINAPI worker_main(void *)
{
    for (;;) {
        WaitForSingleObject(wake, 200);
        if (fatal_pending) {
            handle_fatal();
            fatal_pending = 0;
            SetEvent(done);
            continue;
        }
        if (!fatal_state) flush_errors(false);
    }
}

// Reports a fatal crash once; returns when the report is written. False if one was reported already (then a crash
// on a second thread waits here until the first report is complete, so that the process does not end under it).
bool fatal(const EXCEPTION_RECORD *record, const CONTEXT *context, const char *source, const char *what)
{
    if (!started) return false;
    if (InterlockedCompareExchange(&fatal_state, 1, 0) != 0) {
        if (fatal_state == 1 && GetCurrentThreadId() != worker_tid) WaitForSingleObject(done, 15 * 60 * 1000);
        return false;
    }
    Fatal &f = fatal_req;
    f.record = *record;
    f.record.ExceptionRecord = nullptr;
    f.context = *context;
    f.tid = GetCurrentThreadId();
    f.source = source;
    f.what = what ? std::strncpy(f.what_buf, what, sizeof f.what_buf - 1) : nullptr;
    GetLocalTime(&f.time);
    // the error slot the exception handler filled for this very exception: it is the crash, not a survived error
    for (Slot &s : slots)
        if (s.state == 2 && s.tid == f.tid && s.record.ExceptionCode == record->ExceptionCode &&
            s.record.ExceptionAddress == record->ExceptionAddress &&
            InterlockedCompareExchange(&s.state, 3, 2) == 2 && s.site >= 0)
            InterlockedDecrement(&sites[s.site].count);
    if (!worker || GetCurrentThreadId() == worker_tid) {
        handle_fatal();   // the report thread itself crashed (or there is none): write it here
    } else {
        fatal_pending = 1;
        SetEvent(wake);
        WaitForSingleObject(done, 15 * 60 * 1000);
    }
    InterlockedExchange(&fatal_state, 2);
    return true;
}

// An exit without an exception: a made-up record (STATUS_FATAL_APP_EXIT) at the caller of the hooked function.
// `frame` is the detour's frame: [0] the caller's EBP, [1] the return address into the caller.
void fatal_exit(const std::uint32_t *frame, const char *source, const char *what)
{
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&c);
    c.Ebp = frame[0];
    c.Eip = frame[1];
    c.Esp = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(frame + 2));
    EXCEPTION_RECORD r{};
    r.ExceptionCode = 0x40000015;
    r.ExceptionAddress = reinterpret_cast<void *>(static_cast<std::uintptr_t>(c.Eip));
    fatal(&r, &c, source, what);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
    fatal(ep->ExceptionRecord, ep->ContextRecord, "Windows' last-chance filter (no handler caught it)", nullptr);
    return EXCEPTION_EXECUTE_HANDLER;   // ends the process
}

// SetUnhandledExceptionFilter: the shim's filter stays. A later caller (the C runtime's start-up, a DLL) is told it
// was set and gets back what it set before.
LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI *set_filter_original)(LPTOP_LEVEL_EXCEPTION_FILTER);
LPTOP_LEVEL_EXCEPTION_FILTER others;
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI keep_filter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(InterlockedExchangePointer(reinterpret_cast<void **>(&others), reinterpret_cast<void *>(f)));
}

// ── The game's code ──────────────────────────────────────────────────────────────────────────────────────────────
namespace fn = sadk::game::fn;
using GameCrash = sadk::Hook<fn::Crash_HandleException>;
using Abort = sadk::Hook<fn::_abort>;
using AmsgExit = sadk::Hook<fn::__amsg_exit>;
void (*purecall_original)();

// The game's filter around its main loop (WinMain S 00407cf0, scope table S 0084aed0): instead of its own report
// (Crash_HandleException S 006e63d0: crashdump.dmp, crashlog.txt) the shim's. Returning 1 runs WinMain's handler
// (S 00407f3a), which logs "EXCEPTION OCCURED" with the call-stack text at g_CrashReport +4 and ends the game.
std::int32_t SADK_CDECL on_game_crash(void *pointers)
{
    // Once: writing the text below uses the game's heap, which may be what broke; a fault there runs this filter
    // again, and it must then let WinMain's handler end the game instead of trying again.
    static volatile LONG entered;
    if (InterlockedIncrement(&entered) > 1) return 1;
    auto *ep = static_cast<EXCEPTION_POINTERS *>(pointers);
    fatal(ep->ExceptionRecord, ep->ContextRecord, "the game's crash handler (WinMain)", nullptr);
    if (fatal_summary[0]) {   // the text WinMain logs: the game's std::string at g_CrashReport +4
        auto *text = reinterpret_cast<sadk::msvc::string *>(reinterpret_cast<std::uint8_t *>(sadk::game::var::g_CrashReport.get()) + 4);
        fn::std_::string::assign(text, fatal_summary, static_cast<std::uint32_t>(std::strlen(fatal_summary)));
    }
    return 1;
}

#define CALLER_FRAME static_cast<const std::uint32_t *>(__builtin_frame_address(0))

void SADK_CDECL on_abort()
{
    fatal_exit(CALLER_FRAME, "the C runtime's abort() (_abort S 006fdfcc)", "abort() was called: the C runtime ends the game");
    Abort::original();
}

void SADK_CDECL on_purecall()
{
    fatal_exit(CALLER_FRAME, "the C runtime's __purecall (S 006f371b)",
               "a pure virtual function was called (an object used after it was destroyed?): runtime error R6025");
    purecall_original();
}

void SADK_CDECL on_amsg_exit(std::int32_t code)
{
    char what[120];
    std::snprintf(what, sizeof what, "the C runtime ended the game: runtime error R60%02d", int(code));
    fatal_exit(CALLER_FRAME, "the C runtime's __amsg_exit (S 006f99ed)", what);
    AmsgExit::original(code);
}

}  // namespace

void start(bool is_game)
{
    if (started) return;
    game = is_game;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&start), &self);
    tick0 = GetTickCount();
    SYSTEMTIME now;
    GetLocalTime(&now);
    make_stamp(stamp, sizeof stamp, now);
    main_tid = GetCurrentThreadId();
    exe_lo = range_of(GetModuleHandleA(nullptr), exe_hi);
    tincat_lo = range_of(GetModuleHandleA("tincat3.dll"), tincat_hi);
    self_lo = range_of(self, self_hi);
    std::snprintf(mods_prefix, sizeof mods_prefix, "%s\\mods\\", sadk::game_root());
    mods_prefix_len = std::strlen(mods_prefix);
    std::snprintf(shim_log_path, sizeof shim_log_path, "%s", sadk::exe_path());
    if (char *slash = std::strrchr(shim_log_path, '\\')) std::strcpy(slash + 1, "wsock32_shim.txt");
    char docs[MAX_PATH];
    if (game && SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
        std::snprintf(default_dir, sizeof default_dir, "%s\\SAdK\\dumps", docs);
    else
        std::snprintf(default_dir, sizeof default_dir, "%s\\dumps", sadk::game_root());

    prepare();   // dbghelp: now, not during a crash
    sadk::log_tap(on_shim_log);
    wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    done = CreateEventA(nullptr, TRUE, FALSE, nullptr);   // manual: every waiting thread wakes
    worker = CreateThread(nullptr, 1 << 20, worker_main, nullptr, 0, &worker_tid);
    started = true;

    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    if (void *suef = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetUnhandledExceptionFilter")))
        sadk::hook_function(suef, reinterpret_cast<void *>(keep_filter), reinterpret_cast<void **>(&set_filter_original),
                            "crash report: SetUnhandledExceptionFilter keeps the shim's filter");
    if (game) {
        GameCrash::install(on_game_crash, "crash report: the game's crash handler");
        sadk::hook_function(reinterpret_cast<void *>(fn::Log_Printf.get()), reinterpret_cast<void *>(shim_log_printf_thunk),
                            &shim_log_printf_original, "crash report: the game's log, every level");
        Abort::install(on_abort, "crash report: abort()");
        sadk::hook_function(reinterpret_cast<void *>(fn::__purecall.get()), reinterpret_cast<void *>(on_purecall),
                            reinterpret_cast<void **>(&purecall_original), "crash report: pure virtual calls");
        AmsgExit::install(on_amsg_exit, "crash report: C runtime errors");
    }
    prune_all(dump_dir());
    log("crash report: ready (session %s, report folder %s%s, dbghelp %s)", stamp, game ? "the game's, now " : "",
        dump_dir().c_str(), worker ? "and report thread" : "NO REPORT THREAD");
}

void stop()
{
    if (!started) return;
    // The report thread is gone (the process is ending): write what is pending here, without the analysis (no
    // DbgHelp under the loader lock), and sum up the repeats.
    std::string tail;
    for (Slot &s : slots)
        if (s.state == 2)
            tail += format("\n%s at %08X (thread %lu): in the last moments before the game closed, not analysed\n",
                           exception_name(s.record.ExceptionCode), unsigned(reinterpret_cast<std::uintptr_t>(s.record.ExceptionAddress)), s.tid);
    if (errors_path[0] && !errors_zipped && !tail.empty()) write_file(errors_path, tail, true);
    write_repeats();
    // empty report files of this session go
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (errors_path[0] && GetFileAttributesExA(errors_path, GetFileExInfoStandard, &fa) && !fa.nFileSizeLow && !fa.nFileSizeHigh)
        DeleteFileA(errors_path);
}

}  // namespace shim::crash

namespace shim::report {
HMODULE shim_module() { return crash::self; }
DWORD start_tick() { return crash::tick0; }
const char *session_stamp() { return crash::stamp; }
std::string recent_game_log() { return crash::game ? crash::game_ring.text() : "  (not the game)\n"; }
std::string recent_shim_log() { return crash::shim_ring.text(); }
std::string errors_file(int *count)
{
    *count = crash::errors_written;
    return crash::errors_path;
}
}  // namespace shim::report
