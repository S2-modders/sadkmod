// The crash report in the real game (../../game_crash_test.sh): crashes SADK.exe in the way crashtest.ini names,
// early in its start-up: in a hook on CApplicationEx::ParseCommandLine S 00403ac0, the first thing
// CApplicationEx::Initialize S 004075d0 does, inside WinMain's crash handler (docs/sim/app-shell.md §3.1, §3.12) and
// before any graphics. Not a mod to install: a test.
//   [crash] kind =
//     survive    a fault in this mod that its own handler survives, twice; then the game ends normally
//     mod        a fault in this mod's code
//     native     the game's own code faults with no mod code on the stack: the hook leaves a bad pointer in
//                CApplicationEx +0x64 (the -log_path string), which Initialize hands to Log_SetDirectory right after
//     called     this mod calls a game function that faults (Generic_StdString_cstr_at4 S 0072b260 on a null object)
//     thread     a thread of this mod faults (no handler of the game's covers it)
//     abort      this mod calls the C runtime's _abort S 006fdfcc
//     purecall   this mod calls the C runtime's __purecall S 006f371b
#include <sadkmod/sadkmod.hpp>
#include <sadkmod/game/sadk_noav/fn/CApplicationEx.hpp>
#include <sadkmod/game/sadk_noav/fn/_global.hpp>

#include <string>

namespace fn = sadk::game::fn;
using Parse = sadk::Hook<fn::CApplicationEx::ParseCommandLine>;

// movl $1, (%eax) with EAX = 0: an access violation, 6 bytes long (C7 00 01 00 00 00)
#define FAULT() asm volatile("xorl %%eax, %%eax\n\t.byte 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00" ::: "eax", "memory")

static std::string kind;
static volatile LONG recovering;

static LONG CALLBACK skip_fault(EXCEPTION_POINTERS *ep)
{
    auto *ip = reinterpret_cast<unsigned char *>(static_cast<ULONG_PTR>(ep->ContextRecord->Eip));
    if (recovering && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ip[0] == 0xC7 && ip[1] == 0x00) {
        ep->ContextRecord->Eip += 6;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static __attribute__((noinline)) void survive()
{
    void *h = AddVectoredExceptionHandler(0, skip_fault);
    recovering = 1;
    FAULT();
    recovering = 0;
    RemoveVectoredExceptionHandler(h);
}

static DWORD WINAPI crashing_thread(void *)
{
    FAULT();
    return 0;
}

static bool SADK_THISCALL parse(sadk::game::CApplicationEx *app, char *cmd)
{
    bool ok = Parse::original(app, cmd);
    sadk::log("crashtest: %s", kind.c_str());
    // a level the game does not log (g_LogLevel is 1): the crash report keeps it, safely formatted, once with a count
    for (int i = 0; i < 2; i++)
        fn::Log_Printf(3, const_cast<char *>("crashtest: level %d, %s, %.2f, %S, %s"), 3, "text", 1.5, L"wide",
                       reinterpret_cast<char *>(0x10));
    if (kind == "survive") {
        survive();
        survive();
        Sleep(800);   // the report thread writes an error once it is 300 ms old
        ExitProcess(0);
    }
    if (kind == "mod") FAULT();
    if (kind == "native") {
        auto *path = sadk::at<sadk::msvc::string>(app, 0x64);   // logPath: its text "at" address 0x10
        path->capacity = 0x20;
        path->size = 5;
        path->heap_text = reinterpret_cast<char *>(0x10);
    }
    if (kind == "called") reinterpret_cast<char *(SADK_FASTCALL *)(void *)>(sadk::resolve(sadk::Module::sadk, 0x0072b260))(nullptr);
    if (kind == "thread") {
        CloseHandle(CreateThread(nullptr, 0, crashing_thread, nullptr, 0, nullptr));
        Sleep(INFINITE);
    }
    if (kind == "abort") fn::_abort();
    if (kind == "purecall") reinterpret_cast<void (*)()>(fn::__purecall.get())();
    return ok;
}

static bool start()
{
    kind = sadk::mod_settings().get("crash", "kind", "");
    return Parse::install(parse, "crashtest: ParseCommandLine");
}

SADKMOD_MAIN(start, 1, SADKMOD_CLIENT)
