# wsock32 shim

A proxy `wsock32.dll` that loads mods into the game and reports its crashes. Mods: `docs/writing-mods.md`; the
library both are built on: `README.md`.

`tincat3.dll` imports all of its sockets from `WSOCK32.dll` by ordinal; `SADK.exe` uses `WS2_32.dll`
directly. `WSOCK32.dll` is not a KnownDLL, so Windows loads a copy from the game folder before the system one
(live 2026-10-07: loaded, all exports resolved).

- **Forwarding:** every export is a 6-byte thunk, `JMP DWORD PTR [real_ptrs + 4*i]`, to the system `wsock32.dll`'s
  export of the same ordinal (`gen.py` generates `thunks.S`, `wsock32.def` and `ordinals.h` from `exports.txt`).
  Mods may hook these exports: they carry exactly TinCat's sockets (the `gamebridge` mod hooks `connect`,
  `send`, `listen`, `closesocket`). Mods get the shim's module handle as `sadkmod_api::host_module`.
- **Mod host:** DllMain only opens the log (`wsock32_shim.txt` next to the game exe; mods log there too), loads the
  real `wsock32.dll` and puts a jump on `SADK.exe`'s entry point (the shim is loaded with the process: `SADK.exe`
  imports `tincat3.dll`, which imports `WSOCK32`). The jump runs once on the main thread before any game code: it
  restores the entry point, checks the exe, starts the crash report and loads the mods (`sadk::host::start_mods`),
  then jumps to the real entry point with the stack as the loader left it, so no frame of the shim stays below the
  game's.
- **Crash report:** the shim's own, always loaded with it (below).
- **Requires the DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`). On any other build the shim stays a
  pure pass-through: no mods, no crash report.
- Everything else lives in mods: hosting through the lobby server is the `gamebridge` mod, map sharing
  `assetshare` (both in sadklobby).
- Build (Linux, mingw-w64, Python 3): `make` -> `wsock32.dll` (builds `sadkmod` first; about 1.1 MB, most of it the
  table of the game's function names). `make test` loads it into a test program under Wine (forwarding works, and it
  stays inactive outside `SADK.exe`), then runs the crash report outside the game; `make test-game` runs it in the
  real game (below).
- Install: copy it into the game's `bin` folder, next to `SADK.exe` (SAdK-ServerConfig does this, with the mods).
  Remove it to undo.

## Crash report

`crash.hpp` (handlers and files), `crash_report.cpp` (analysis), `symbols.cpp` (names).

**What it catches.** The game catches a crash on its main thread in `WinMain` with the filter
`Crash_HandleException` S 006e63d0 and writes a small dump (stacks only) and a log of bare addresses, the same two
file names every time (sadk-resurrected `docs/sim/app-shell.md` §3.12). The shim takes that filter over and also
catches what it never sees:

| Path | How |
|---|---|
| a crash on the main thread inside `WinMain` | hook on `Crash_HandleException`; it returns 1, so `WinMain`'s handler (S 00407f3a) still logs `EXCEPTION OCCURED` with the call-stack text at `g_CrashReport` +4, which now holds the cause and the report's path |
| a crash on another thread, or before `WinMain` | the shim's `SetUnhandledExceptionFilter`; a hook on that function keeps it against later callers (the C runtime's start-up sets its own) |
| `__invoke_watson` S 006f25e2, `___report_gsfailure` S 007001bc | they call `UnhandledExceptionFilter` directly, which runs the shim's filter |
| `_abort` S 006fdfcc, `__purecall` S 006f371b, `__amsg_exit` S 006f99ed | hooks; the report is written before the C runtime's message box |

**Files.** In the game's dump folder (`g_CrashDirectory` S 00882808: `-log_path`, or `<My Documents>\SAdK\dumps`,
also before the game has set it): `crash_<time>.zip` with `crashlog_<time>.txt`, `crashdump_<time>.dmp`, the
session's errors file if there is one, `Logfile_0.txt` and `wsock32_shim.txt`. The loose files are deleted once
the ZIP is complete; if packing fails they stay. The newest 5 crashes and 10 errors files are kept. The dump is a
full-memory minidump (with memory info, handles, thread times, unloaded modules; smaller kinds if the system's
DbgHelp refuses) carrying the crash log as its comment stream. The shim loads the system's `dbghelp.dll` by its
path: the game's own copy in `bin` (6.5, 2005) cannot write all of it. Measured under Wine with the game in its
start-up: 173 MB dump, 57 MB packed, 3 s for both. [TODO] the size and time in a running match on Windows.

**The cause** (the report's first lines), from the walked call stack:
- the faulting code is a mod's (`<game>\mods\<folder>\…`, its `mod.dll` or a helper DLL): that mod;
- bytes a mod patched (the registry's records): that mod;
- the shim's code: the mod loader;
- in a system DLL: decided by the first frame in the game's, a mod's or the shim's code above it;
- the game's code: a mod frame above it that called the game directly makes it "probably" that mod; a mod frame
  that only passed the call on through its hook (the frame below it is the hooked function, or the next detour on
  it) does not count, so the shim's own frame hook under every frame does not either. Otherwise the game itself.
The report lists the mods and every change to the game's code by owner, so a native crash with mods active can
still be narrowed down by switching them off.

**The call stack.** The game was built with frame pointer omission and has no symbols, so DbgHelp's walker loses
frames. The shim walks it itself: a function of the Ghidra map keeps EBP as its frame pointer only if it starts with
`PUSH EBP; MOV EBP,ESP` and the crash lies past that (a system DLL's function: by its exports). sadkmod's code keeps
frames (`-fno-omit-frame-pointer` in `pch.mk`) except in leaf functions; a mod built before that is recognised by the
prologue of its exported `sadkmod_init`, and its frame pointer is then followed only where a direct call proves the
step. Without a frame the next return address above ESP is taken: first
one whose direct call provably leads to the function, else the nearest whose call could have. A frame is marked `?`
when it was found without such evidence (perhaps left over from an earlier call); `?` frames never decide the cause.
Names come from the Ghidra map (`gen_symbols.py`: every function of `SADK.exe` and `tincat3.dll` with its code
ranges, built into the shim compressed, about 0.5 MB), a mod's frames as `mods\<folder>\mod.dll+0x…`, a system
DLL's by its exports.

**Errors the game survives.** A vectored exception handler sees every exception first. One with error severity
raised in the game's, the shim's or a mod's code (a C++ throw by its type information) is copied with its stack;
once it is 300 ms old and no fatal crash claimed it, the report thread writes it with cause and call stack to
`errors_<start of the session>.txt`. Each place is written once and only counted after that; the counts are summed up
when the game closes or crashes. The file is made at the first error. Exceptions that system DLLs raise and catch
themselves are left out.

**The game's log.** A hook on `Log_Printf` S 006e5950 keeps its last 200 lines at every level (the game writes only
level 1 to `Logfile_0.txt`), formatted by the shim: a level the game never formatted must not crash it now, so
strings are read without faulting (`(bad pointer)`) and every other value on its own. A repeated line is counted.

**Threads.** Everything is written by a thread of the shim's own, started with the report, while the crashed
thread waits (a stack overflow leaves it almost no stack). A second thread that crashes meanwhile waits for the
first report.

**Tests.** `make test`: a test program plays the game, a DLL in `mods\crashmod\` a mod (`tests/crash_test.cpp`,
`tests/crash_mod.cpp`): an error the mod survives twice (one record, "2 times"), a crash in the mod, in the program's
own code, in the program's code called by the mod, in a thread of the mod, and the crash and the call again with the
mod built without frame pointers (`crash_mod_old.dll`); each checked for its cause, the ZIP's content and the dump
(`tests/crash_check.py`). `make test-game GAME=<install>`: the real `SADK.exe` under Wine and
xvfb, from a folder of links to the install (with a stand-in `PhysXLoader.dll`), crashed by `tests/game/crashtest`
early in its start-up in seven ways (survived error, mod, native, called by the mod, thread, `abort`, pure virtual
call; `tests/game_crash_check.py`) [PROVEN under Wine, 2026-11-12: all seven, causes and names as expected].
[TODO] crashes in a running match on Windows.
