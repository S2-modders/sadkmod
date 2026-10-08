# Mods

The bridge shim (`bin\wsock32.dll`) loads mods from the game's `mods` folder at start-up. A mod can replace or add
game data files, and it can bring code (`mod.dll`) that patches and hooks the game through `sadkmod`. Like the
shim, mods only run on the **DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`).

This folder holds the repo's own mods:

| Mod | What it does |
|---|---|
| `billboards` | The lobby's advertising screens show a plain area of their board instead of the dead web pages. Installed by SAdK-ServerConfig, whose "Disable billboards" is `Enabled` in the mod's `billboards.ini`. `docs/BINARY_PATCHES.md`, "Billboards". |
| `borderless` | The game's fullscreen becomes a borderless window over one monitor (`borderless.ini`: `Monitor`), the picture at the resolution set in the game's options, stretched. The game's own settings are untouched. Instant switching to other windows, no lost device. Windowed mode is unchanged. |
| `nomeshcache` | The game never uses its converted-mesh cache (`%LOCALAPPDATA%\SAdK\*.mshraw`): every model is read from its `.KEX`, so a changed model shows up at once. Loading takes longer, and the lobby town flickers with it (maintainer's test, 2026-10-08; cause unknown, not pursued): a tool for model makers, not for playing. |
| `propreload` | **Test only.** Reloads the property database (`scripts\properties\*.lua`) every time a match is entered, with the game's own functions, to find out whether that is safe between matches (`docs/data-loading.md` §1). Logs the record counts before and after. |
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

SADKMOD_MAIN(start, 1, SADKMOD_CLIENT)   // the mod's version, and what it changes
```

- **When:** the shim calls `sadkmod_init` before `SADK.exe`'s entry point runs. The game is not initialised yet, so
  `start` installs hooks and patches; the real work happens in them later.
- **Log:** `sadk::log` writes to the shim's log (`bin\wsock32_shim.txt`), prefixed with the mod's name.
- **Hooks:** `sadk::Hook<>` and `sadk::hook_function` go through the shim's one hook registry. Several mods may hook
  the same function: they run in load order, the mod whose folder name sorts first runs first, and its `original`
  leads through the later ones to the game, however late each mod installs its hook (`10_first`, `20_second`: the
  folder name decides). The shim's own hooks always come last, next to the game's code. Table slots
  (`sadk::hook_slot`) are ordered the same way. `sadk::patch` changes bytes directly and checks what it replaces; two mods patching the same bytes do not
  mix (the second finds unexpected bytes and is not applied).
- **Version and flags:** `SADKMOD_MAIN(start, version, flags)` also exports `sadkmod_version()` (the mod's own version,
  an integer) and `sadkmod_flags()`: what the mod changes, combinable — `SADKMOD_CLIENT` (only this player's game),
  `SADKMOD_SERVER` (matches: every player of a match needs it alike), `SADKMOD_LOBBY` (the lobby village). A mod
  without `mod.dll` names its flags with empty files `.client`, `.server`, `.lobby` in its folder. A mod without
  any flag is not loaded. [TODO: the host does not read the flags yet; server-mod transfer is being built]
- **Unloading:** `SADKMOD_STOP(stop)` exports `sadkmod_stop()`, called before a mod is unloaded: stop the mod's
  threads, take back what it gave the game. Hooks, table slots and patches made through sadkmod are recorded under
  the mod's name and taken back by the host; `write_memory` and a mod's own MinHook are not.
- **Interface:** `sadkmod/include/sadkmod/mod.hpp` (`sadkmod_api`, version 2). `SADKMOD_MAIN` checks the version;
  fields are only ever appended. Without sadkmod, a mod can implement the exports itself.
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
function. The mods' patches are verified against `SADK.exe`. In the game (maintainer's test, 2026-10-08) the mod
system and the repo's mods work.
