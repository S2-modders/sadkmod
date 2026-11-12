// The crash report: the shim's replacement for the game's crash handler, always loaded with the shim.
//
// Fatal crashes. The game catches a crash on its main thread in WinMain (filter Crash_HandleException S 006e63d0,
// docs/sim/app-shell.md §3.12) and writes crashdump.dmp (a small dump: stacks only) and crashlog.txt (addresses
// without names), overwriting the previous ones. The shim takes over that filter and also catches what the game's
// filter never sees: crashes on other threads (SetUnhandledExceptionFilter, kept against later callers), the C runtime's
// exits without an exception (_abort S 006fdfcc, __purecall S 006f371b, __amsg_exit S 006f99ed) and the runtime's
// reports that call UnhandledExceptionFilter directly (__invoke_watson S 006f25e2, ___report_gsfailure S 007001bc).
// A crash writes, into the game's dump folder (the game's g_CrashDirectory S 00882808: -log_path, else
// <My Documents>\SAdK\dumps), crash_<time>.zip with
//   crashlog_<time>.txt   what crashed and why: the cause (a mod, the mod loader or the game itself), the exception,
//                         the call stack with the game's function names, the mods and their changes to the game's
//                         code, the last lines of the game's log (every level) and of the shim's log, memory, system
//   crashdump_<time>.dmp  a full-memory minidump (every object of the game), the crash log as its comment stream
//   errors_<start>.txt    the errors the game survived this session (below), if any
//   Logfile_0.txt, wsock32_shim.txt   the game's and the shim's logs as they were
// The files are written loose first and packed into the ZIP afterwards; if packing fails they stay loose. The newest
// 5 crashes are kept.
//
// Errors the game survives. An exception raised in the game's, the shim's or a mod's code that something catches
// (the game plays on) goes into errors_<start of the session>.txt in the same folder, with its cause and call stack:
// the first time per place, later ones only counted (a summary of the counts is added when the game closes). The
// file is created at the first such error, so a session without one leaves nothing behind; the newest 10 are kept.
// Exceptions that Windows' and the drivers' own code raises and catches are not the game's errors and are left out.
//
// Everything is written by a thread of the shim's own (started with the report), while the crashed thread waits:
// it has a whole stack (a stack overflow leaves the crashed thread almost none) and the dump shows the crashed
// thread as it was.
#pragma once
#include "shim.hpp"

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace shim::crash {

// Starts the crash report. `game` = the supported SADK.exe: also take over the game's crash handler, its log and the
// C runtime's exits. Call it on the main thread before the mods load, so that a crash while they start is reported.
void start(bool game);
// DLL_PROCESS_DETACH: writes the errors still pending and the summary, removes empty report files.
void stop();

}  // namespace shim::crash

// The game's function names (symbols.cpp, from the Ghidra map).
namespace shim::symbols {
// The function of SADK.exe (or tincat3.dll) whose code holds the static address: its "namespace::name" and entry, or
// nullptr.
const char *find(sadk::Module m, std::uint32_t static_address, std::uint32_t *entry);
}

// What crash.cpp and crash_report.cpp share.
namespace shim::report {

// A copy of a thread's stack memory, taken when an error happened (the thread has moved on since).
struct StackCopy {
    const std::uint8_t *data = nullptr;
    std::uint32_t base = 0, size = 0;   // the address data[0] was copied from
};

struct Event {
    const EXCEPTION_RECORD *record = nullptr;   // the exception (for an exit without one: made up, see `what`)
    const CONTEXT *context = nullptr;           // the thread's registers at that moment
    DWORD thread_id = 0;
    bool main_thread = false;
    const char *source = "";                    // who reported it ("the game's crash handler", ...)
    const char *what = nullptr;                 // an exit without an exception: what happened ("abort() was called")
    StackCopy stack;                            // errors: the stack copy; fatal: empty (the stack is read live)
    SYSTEMTIME time{};
};

// The report text of a fatal crash (`fatal`) or of one survived error. `cause` gets the one-line verdict.
std::string describe(const Event &e, bool fatal, std::string &cause);

// The minidump: full memory, `comment` as its comment stream. Falls back to smaller dumps if that fails.
bool write_dump(const char *path, const Event &e, const std::string &comment, std::string &note);

// Loads the system's dbghelp.dll and initialises it. At start-up: a crash must not load DLLs (the crashed thread may
// hold the loader lock).
void prepare();

// Shared by both files.
std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *exception_name(DWORD code);
HMODULE shim_module();
DWORD start_tick();
const char *session_stamp();                    // the shim's start, "YYYY-MM-DD_hh-mm-ss"

// The last lines of the game's log (every level) and of the shim's log, oldest first.
std::string recent_game_log();
std::string recent_shim_log();
// The errors file of this session ("" while there is none) and how many errors went into it.
std::string errors_file(int *count);

// Reads memory without faulting (ReadProcessMemory on this process): false if any byte is unreadable.
bool safe_read(std::uintptr_t address, void *out, std::size_t n);

}  // namespace shim::report
