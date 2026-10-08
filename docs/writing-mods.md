# Mods

The bridge shim (`bin\wsock32.dll`) loads mods from the game's `mods` folder at start-up. A mod can replace or add
game data files, and it can bring code (`mod.dll`) that patches and hooks the game through `sadkmod`. Like the
shim, mods only run on the **DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`).

This folder holds the repo's own mods:

| Mod | What it does |
|---|---|
| `billboards` | The lobby's advertising screens show a plain area of their board instead of the dead web pages. Installed by SAdK-ServerConfig ("Disable billboards"). `docs/BINARY_PATCHES.md`, "Billboards". |
| `borderless` | Fullscreen becomes a borderless window over the monitor: the game keeps the resolution set in its options, stretched to the monitor. Instant switching to other windows, no lost device. Windowed mode is unchanged. |
| `nomeshcache` | The game never uses its converted-mesh cache (`%LOCALAPPDATA%\SAdK\*.mshraw`): every model is read from its `.KEX`, so a changed model shows up at once. Loading takes longer. |
| `npcmodels` | An NPC record's `bdyprt` value picks the `npc_bodyparts.xml` set, so the server can show the female, MacDoyleJr and MacGabhan NPC models (`!npc` in chat). `docs/BINARY_PATCHES.md`, "NPC model sets". |

## Layout in the game folder

```
<game>\mods\
    <name>\
        data\...        replacement or new data files: same paths as below <game>\data, plain (not encrypted)
        mod.dll         optional code
        *.dll, *.ini    anything else the mod needs (mod.dll finds DLLs next to it)
```

- Mods are processed in **folder-name order** (case-insensitive). When two mods have the same data file, the later
  one wins; the log names the override.
- A folder whose name starts with `_` or `.` is skipped: rename `billboards` to `_billboards` to switch it off.
- Nothing in the game folder is changed. Deleting a mod's folder undoes it.

## Data files

The shim indexes every file below each `mods\<name>\data` once at start-up and redirects the game's file opens
(`CreateFileA` / `CreateFileW`): reading `<game>\data\<path>` opens the mod's `data\<path>` when the index has it.
Paths are matched case-insensitively, whatever form the game uses (relative or absolute, `\` or `/`).

- **Plain files.** The game's own files are encrypted; mod files are not. The shim lets a file without the
  encryption header through the game's decrypter (`NBase::gDecryptData` S 006e6660, which would otherwise crash on
  purpose). An encrypted file (`tools/sadk_crypt.py encrypt`) works as well.
- **Reads only.** A write into `<game>\data` goes to the game's own file.
- **Meshes.** The game caches converted meshes as `%LOCALAPPDATA%\SAdK\<mesh>.mshraw`. The cache of a mesh a mod
  replaces goes to `%LOCALAPPDATA%\SAdK\mods\` instead, so the game's own cache is never overwritten and removing
  the mod goes back to the original model.
- **New files** are found by name (existence checks and opens go through `CreateFile`). A file the game would only
  find by listing a folder (`FindFirstFile`) is not listed. [TODO: no such case is known yet]
- **Matches.** The game's build checksum covers `data\game` and `data\lobby` → `scripts\**\*.lua` and
  `settings\**\*.xml`. A mod that changes those makes the host's checksum differ, and only players with the same
  mods can play matches together (`!CHECKSUM MISMATCH`). Lobby config, models and textures are not covered.

## mod.dll

A mod's code is a 32-bit Windows DLL named `mod.dll` that exports `sadkmod_init`. With sadkmod (C++23, mingw-w64):

```cpp
#include <sadkmod/sadkmod.hpp>
#include <sadkmod/game/sadk_noav/fn/S2CE.hpp>

namespace game = sadk::game;
using Load = sadk::Hook<game::fn::S2CE::CTexture::CreateFromFile>;

static bool SADK_THISCALL on_load(game::S2CE::CTexture *tex, void *path, bool a, bool b, bool c)
{
    bool ok = Load::original(tex, path, a, b, c);
    if (ok) sadk::log("texture %ux%u", tex->width, tex->height);
    return ok;
}

static bool start()                    // once, on the game's main thread, before the game's own start-up
{
    sadk::log("settings in %s", sadk::mod_dir());
    return Load::install(on_load);     // logged as "S2CE::CTexture::CreateFromFile"
}

SADKMOD_MAIN(start)
```

- **When:** the shim calls `sadkmod_init` before `SADK.exe`'s entry point runs. The game is not initialised yet, so
  `start` installs hooks and patches; the real work happens in them later.
- **Log:** `sadk::log` writes to the shim's log (`bin\wsock32_shim.txt`), prefixed with the mod's name.
- **Hooks:** `sadk::Hook<>` and `sadk::hook_function` go through the shim's one hook registry. Several mods may hook
  the same function: the most recently loaded runs first, and its `original` leads through the earlier ones to the
  game. `sadk::patch` changes bytes directly and checks what it replaces; two mods patching the same bytes do not
  mix (the second finds unexpected bytes and is not applied).
- **Interface:** `sadkmod/include/sadkmod/mod.hpp` (`sadkmod_api`, version 1). `SADKMOD_MAIN` checks the version;
  fields are only ever appended. Without sadkmod, a mod can implement `sadkmod_init(const sadkmod_api *)` itself.
- **Settings:** `sadk::mod_settings()` is the mod's `<name>.ini` next to its `mod.dll` (e.g. `billboards.ini`);
  other files live in `sadk::mod_dir()`.

## Building the repo's mods

```
make            # build/<name>/ for every mod here: mod.dll plus its .ini files (builds sadkmod first)
make verify     # every mod's patches against a DRM-free SADK.exe under Wine (SADK_EXE=<path>)
```

Install: copy `build/<name>` to `<game>\mods\<name>`. A new mod is a folder here with its `.cpp` files (and any
`.ini`, `.txt` or `data/` to ship); `tests/verify_mods.cpp` lists the start functions to verify.

## Status

The host is tested under Wine (`make test` in `sadkmod`): discovery and order, overrides through `CreateFileA` /
`CreateFileW`, writes left alone, the mesh-cache redirect, `mod.dll` loading and two hooks chained on one
function. The mods' patches are verified against `SADK.exe`. **Nothing of the mod system has run in the game
yet** [TODO].
