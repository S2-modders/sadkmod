# sadkmod — modding *Die Siedler - Aufbruch der Kulturen* from inside the game

A small C++23 library for code that runs inside `SADK.exe`, as a DLL the game loads, such as the bridge
shim (`bridge/wsock32_shim`, built on it). It gives such code:

- **Typed declarations of the game**, generated from the project's Ghidra map (`sourcemap/`): every named
  function as a callable, hookable object with its calling convention, every struct and class with a layout
  checked against Ghidra's offsets, typed vtables, enums, and named globals.
- **Patches** that state the bytes they replace and only apply where those bytes are found.
- **Hooks**: whole functions through MinHook, or single vtable slots.
- **Mirrors of the MSVC 2005 containers** the game passes around (`std::string`, `vector`, `list`).
- **A verify mode** that runs a mod's patch code against a copy of `SADK.exe` without starting the game.
- **The mod host** (`host.hpp`) and the **mod interface** (`mod.hpp`): the shim loads `<game>\mods\*` with it —
  data file overrides and `mod.dll` code. How to write a mod: `mods/README.md`.

Only the **DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`) is described. Every address is
specific to that build, so a mod must check `sadk::exe_is_supported()` before patching or hooking anything.

## Example

```cpp
#include <sadkmod/sadkmod.hpp>
#include <sadkmod/game/sadk_noav/fn/S2CE.hpp>

namespace game = sadk::game;
using Load = sadk::Hook<game::fn::S2CE::CTexture::CreateFromFile>;

// Same signature as the declaration, calling convention included; anything else does not compile.
static bool SADK_THISCALL on_load(game::S2CE::CTexture *tex, void *path, bool single, bool pool, bool full)
{
    auto *name = static_cast<const sadk::msvc::string *>(path);
    sadk::log("loading %.*s", int(name->size), name->data());
    bool ok = Load::original(tex, path, single, pool, full);
    if (ok) sadk::log("  -> %ux%u", tex->width, tex->height);       // typed fields, offsets checked
    return ok;
}

void start()   // e.g. from a thread started in DllMain
{
    sadk::log_open("C:\\mymod.txt");
    if (!sadk::exe_is_supported()) return;
    Load::install(on_load);                       // the log names it: S2CE::CTexture::CreateFromFile
    // a byte patch: expected bytes, replacement, a name for the log
    sadk::patch(0x007e6240, {'a', 'd', '0'}, {'#', 'd', '0'}, "billboard literal");
}
```

Calling a game function is calling its declaration: `game::fn::_malloc(64)`,
`game::fn::NComm_Manager::SendPlayerReadyEvent(mgr, false)`.

## Parts

| Header | What |
|---|---|
| `core.hpp` | `Fn<module, address, pointer type>`, `Var<…>`, `Addr<…>`, `resolve()`, calling convention macros (`SADK_THISCALL`, `SADK_STDCALL`, `SADK_CDECL`, `SADK_FASTCALL`) |
| `hook.hpp` | `Hook<F>::install` / `::original` (MinHook; the log label defaults to the function's name), `hook_slot` for vtables and other function tables; one registry per process, in which a second hook on the same function chains onto the first |
| `patch.hpp` | `patch`, `patch_call` (the original call target as an address or its declaration), `Bytes` with `call_to` / `jmp_to` / `nops` |
| `msvc.hpp` | `msvc::string` (`small`, `borrow`, `view`), `msvc::vector<T>`, `msvc::list<T>` |
| `runtime.hpp` | `log`, `game_root` / `game_path`, `file_md5`, `exe_is_supported`, `Ini`, `mod_settings()`, `proc<T>(dll, name)`, `game_malloc` / `game_free` |
| `verify.hpp` | `map_image`, `verify_with`, `verify_counts` |
| `mod.hpp` | the mod interface: `sadkmod_api`, `SADKMOD_MAIN`, `mod_name()` / `mod_dir()`; in a mod, `log` and hooks go to the host |
| `host.hpp` | the mod host: `start_mods()` (index, `CreateFile` redirect, plain-file decrypt pass-through, mesh-cache redirect, `mod.dll` loading) |
| `game/sadk_noav/…` | generated: `types.hpp`, `fn/<namespace>.hpp`, `vars.hpp`, `module.hpp`, `all.hpp` |
| `game/tincat3/…` | the same for `tincat3.dll` (namespace `sadk::tincat`) |

## Generated declarations

`gen/gen_game_headers.py` reads `sourcemap/<binary>/types.json.gz` and `sourcemap.json.gz` and writes the
headers into `build/include/sadkmod/game/` (not committed; `make` regenerates them when the sourcemap
changes). Namespaces: `sadk::game` for types, `sadk::game::fn` for functions, `sadk::game::var` for globals,
each followed by the Ghidra namespace or category. For example `S2CE::CTexture::CreateFromFile` becomes
`sadk::game::fn::S2CE::CTexture::CreateFromFile`, and the struct is `sadk::game::S2CE::CTexture`.

- **Callable** (`Fn<…>`): named functions whose signature was set by hand or imported in Ghidra, with a
  known calling convention and no struct returned by value. A custom-storage `this` in ECX with stack
  parameters counts as thiscall. Other named functions are **address only** (`Addr<…>`), and the comment
  above each one says why. Exception funclets are left out.
- **Layouts**: every field is placed at Ghidra's offset with explicit padding, and each offset and each size
  is `static_assert`ed, so a layout GCC would place differently does not compile. Unknown field types become
  raw bytes; bit fields are raw bytes with the bits listed in the comment.
- **vtables**: a slot whose Ghidra comment names a callable function gets that function's pointer type, so
  `tex->vftable->CreateFromFile(tex, …)` is typed.
- **Names**: characters that are not valid in C++ become `_`; `operator==` becomes `operator_eq`. Names that are C++
  keywords or macros of the Windows / C runtime headers get a trailing `_`; the macro list comes from the
  compiler, so the headers can be included after `<windows.h>`. Duplicate names in one namespace get
  `_<address>`. The Ghidra category `std` becomes `std_`.
- The signatures are only as good as the Ghidra map. Most names and types come from the mapping run (`[ai]`
  comments with `conf=known / inferred / guess`). Check a function in Ghidra before relying on it.

Every function and global keeps its Ghidra signature and first plate comment as a comment, and carries its Ghidra
name as `.name` (e.g. `fn::S2CE::CTexture::CreateFromFile.name` is `"S2CE::CTexture::CreateFromFile"`), which
`Hook<F>::install` uses as the log label.

`sourcemap/ExportTypesJson.java` (run inside Ghidra, headless) turns a sourcemap's `types.gdt` into
`types.json.gz`; it has to be rerun whenever the sourcemap is exported again.

## ABI rules: GCC code next to MSVC 2005 code

| What | Same in both? |
|---|---|
| struct fields, pointers, `bool` (1 byte) | yes; the generated layouts are checked |
| `this` in ECX, callee-cleaned stack (`SADK_THISCALL`) | yes; the calling convention is part of the type |
| vtables of single inheritance, as structs of function pointers | yes; that is how the declarations model them |
| C++ `virtual` classes, destructors, RTTI, exceptions | **no**: never mirror a game class with `virtual`; mods build with `-fno-exceptions -fno-rtti` |
| `std::string`, `std::vector`, … | **no**: use `sadk::msvc::…` for anything that crosses into the game |
| heap | **separate**: memory the game frees comes from `game_malloc`; never free game memory with our `free` / `delete` |
| C runtime (`FILE *`, `errno`, …) | **separate**: the game's `FILE` is `sadk::game::mbstring_h::FILE`, and only the game's own functions may use it |

## Verify mode

```cpp
static sadk::MappedImage image;
sadk::map_image("SADK.exe", image);
sadk::verify_with(sadk::Module::sadk, &image);
apply_my_patches();                      // unchanged code: patch() only compares, hooks only check the target
sadk::verify_with(sadk::Module::sadk, nullptr);
auto n = sadk::verify_counts();          // n.matched, n.mismatched; details in the log
```

`bridge/wsock32_shim/tests/verify_patches.cpp` does this for every patch of the shim (`make verify` there).

## Building

Linux, mingw-w64 (`i686-w64-mingw32-g++`, GCC 13 or later), Python 3:

```
make            # build/libsadkmod.a and the generated headers
make test       # self-test under Wine; with SADK_EXE=<DRM-free SADK.exe> also against the binary
```

A mod compiles with `-std=c++23 -fno-exceptions -fno-rtti -I<sadkmod>/include -I<sadkmod>/build/include`
and links `<sadkmod>/build/libsadkmod.a` (add `-static-libgcc -static-libstdc++` for a DLL without
runtime dependencies).

## Status

- Library: the self-test passes under Wine (`make test`: string layouts, MinHook through `Hook<>`, table
  slots, patches, verify mode against the DRM-free `SADK.exe`). All generated headers compile with every
  layout check passing.
- Mod host: `make test` also runs `tests/test_host.cpp` from a scratch game folder (two mods overriding the same
  file, a switched-off mod, `CreateFileA`/`W` redirects, writes left alone, the mesh-cache redirect, a test
  `mod.dll` chaining a hook onto the host's).
- The bridge shim on sadkmod: its 9 map-sharing patches verified against `SADK.exe`; the mods' 5 patches
  verified in `mods/`. Loading and pass-through were tested under Wine. **Not yet tested in the running game**
  `[TODO]`.

## Third-party

`third_party/minhook/`: [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-clause,
`LICENSE.txt`), commit in `VERSION.txt`. Only the 32-bit sources are built. The generated headers contain
names, types and addresses only, no game code.
