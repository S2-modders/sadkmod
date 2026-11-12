// The crash report outside the game (crash.hpp): this program plays the game, <root>\mods\crashmod\mod.dll
// (crash_mod.cpp) a mod. Run from <root>\bin\ (crash_test.sh); the report goes to <root>\dumps\.
//   crash_test.exe clean    the mod survives a fault twice, then the program ends normally
//   crash_test.exe mod      the same, then the mod's code crashes
//   crash_test.exe native   this program's own code crashes (the game's code, no mod on the stack)
//   crash_test.exe called   the mod calls this program's code, which crashes
//   crash_test.exe thread   a thread of the mod crashes (Windows' last-chance filter)
#include "../crash.hpp"

#include <cstdio>
#include <cstring>

#define FAULT() asm volatile("xorl %%eax, %%eax\n\t.byte 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00" ::: "eax", "memory")

extern "C" __attribute__((noinline)) void game_code_crashes() { FAULT(); }

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "clean";
    std::string log = std::string(sadk::game_root()) + "\\bin\\wsock32_shim.txt";
    sadk::log_open(log.c_str());
    shim::crash::start(false);
    HMODULE mod = LoadLibraryA((std::string(sadk::game_root()) + "\\mods\\crashmod\\mod.dll").c_str());
    if (!mod) {
        std::printf("crash_test: no mod.dll (error %lu)\n", GetLastError());
        return 2;
    }
    auto survive = reinterpret_cast<void (*)()>(GetProcAddress(mod, "crashmod_survive"));
    auto crash = reinterpret_cast<void (*)()>(GetProcAddress(mod, "crashmod_crash"));
    auto call = reinterpret_cast<void (*)(void (*)())>(reinterpret_cast<void *>(GetProcAddress(mod, "crashmod_call")));
    auto thread = reinterpret_cast<void (*)()>(GetProcAddress(mod, "crashmod_thread_crash"));
    if (!std::strcmp(what, "clean") || !std::strcmp(what, "mod")) {
        survive();
        survive();
        Sleep(800);   // the report thread writes an error once it is 300 ms old
    }
    if (!std::strcmp(what, "mod")) crash();
    if (!std::strcmp(what, "native")) game_code_crashes();
    if (!std::strcmp(what, "called")) call(game_code_crashes);
    if (!std::strcmp(what, "thread")) thread();
    shim::crash::stop();   // in the shim DllMain does this
    std::printf("crash_test: %s ended normally\n", what);
    return 0;
}
