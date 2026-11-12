// A mod.dll stand-in for crash_test.cpp: code in <root>\mods\crashmod\ that faults. Plain Win32, no sadkmod.
#include <windows.h>

// movl $1, (%eax) with EAX = 0: an access violation in this DLL's code, 6 bytes long (C7 00 01 00 00 00)
#define FAULT() asm volatile("xorl %%eax, %%eax\n\t.byte 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00" ::: "eax", "memory")

static volatile LONG recovering;

// What tells the crash report whether a mod.dll keeps frame pointers (its prologue); not a leaf, so it has one when
// built with -fno-omit-frame-pointer. The "old" build (crash_mod_old.dll) is made without.
extern "C" __declspec(dllexport) bool sadkmod_init(const void *) { return GetTickCount() != 0; }

// The mod survives its own fault: a handler after the shim's skips the instruction.
static LONG CALLBACK skip_fault(EXCEPTION_POINTERS *ep)
{
    auto *ip = reinterpret_cast<unsigned char *>(static_cast<ULONG_PTR>(ep->ContextRecord->Eip));
    if (recovering && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ip[0] == 0xC7 && ip[1] == 0x00) {
        ep->ContextRecord->Eip += 6;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(dllexport) void crashmod_survive()
{
    void *h = AddVectoredExceptionHandler(0, skip_fault);
    recovering = 1;
    FAULT();
    recovering = 0;
    RemoveVectoredExceptionHandler(h);
}

extern "C" __declspec(dllexport) __attribute__((noinline)) void crashmod_crash() { FAULT(); }

extern "C" __declspec(dllexport) __attribute__((noinline)) void crashmod_call(void (*f)()) { f(); asm volatile("" ::: "memory"); }

static DWORD WINAPI crashing_thread(void *)
{
    crashmod_crash();
    return 0;
}

extern "C" __declspec(dllexport) void crashmod_thread_crash()
{
    CloseHandle(CreateThread(nullptr, 0, crashing_thread, nullptr, 0, nullptr));
    Sleep(INFINITE);
}
