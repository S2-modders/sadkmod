# Mods

The shim (`bin\wsock32.dll`) loads mods from the game's `mods` folder at start-up. A mod can replace or add
game data files, and it can bring code (`mod.dll`) that patches and hooks the game through `sadkmod`. Like the
shim, mods only run on the **DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`).

This folder holds the repo's own mods:

| Mod | What it does |
|---|---|
| `gamebridge` | Hosting from behind NAT, through the lobby server (`docs/bridge-protocol.md`): hooks the shim's `connect` / `send` / `listen` / `closesocket`, which carry all of TinCat's traffic. `gamebridge.ini`: `[Bridge] ForceBridge` (always host through the bridge), `Port` (the server's bridge port, default 7072). Installed by SAdK-ServerConfig. |
| `assetshare` | What a match's players share, over the game's own file transfer (`docs/s2tftp.md`): the map, and the host's server mods (below). Custom maps in `Documents\SAdK\maps` appear in the map picker (`docs/BINARY_PATCHES.md`, "Map sharing"). `assetshare.ini`: `[AssetShare] AcceptServerMaps`, `AcceptServerMods` (false: refuse those downloads; you can't get ready in a game that needs them). Needed by every player: a host kicks a joiner without it. Installed by SAdK-ServerConfig. |
| `billboards` | The lobby's advertising screens show a plain area of their board instead of the dead web pages. Installed by SAdK-ServerConfig, whose "Disable billboards" is `Enabled` in the mod's `billboards.ini`. `docs/BINARY_PATCHES.md`, "Billboards". |
| `borderless` | The game's fullscreen becomes a borderless window over one monitor (`borderless.ini`: `Monitor`), the picture at the resolution set in the game's options, stretched. The game's own settings are untouched. Instant switching to other windows, no lost device. Windowed mode is unchanged. |
| `nomeshcache` | The game never uses its converted-mesh cache (`%LOCALAPPDATA%\SAdK\*.mshraw`): every model is read from its `.KEX`, so a changed model shows up at once. Loading takes longer, and the lobby town flickers with it (maintainer's test, 2026-10-08; cause unknown, not pursued): a tool for model makers, not for playing. |
| `propreload` | **Test only.** Reloads the property database (`scripts\properties\*.lua`) every time a match is entered, with the game's own functions, to find out whether that is safe between matches (`docs/data-loading.md` §1). Logs the record counts before and after. |
| `foresterfix` | **Server mod**, data only. The forester no longer crashes the game when it plants a tree on volcanic ground: `patterns.lua` by PiotrWieczorek (`foresterfix/README.md`). |
| `navalcombatfix` | **Server mod** (every player of a match needs it). Soldiers cross the water through their own player's harbours, for attacks (also from a colony) and defence, with the game's own soldier counts; a ship picks up soldiers left with nowhere to go behind a harbour that turned neutral (`navalcombatfix/CONCEPT.md`, `docs/navy-and-military.md` §7). `navalcombatfix.ini` switches each rule and tunes the crossing distance and the pickup wait. Not tested in the game yet. |
| `npcmodels` | An NPC record's `bdyprt` value picks the `npc_bodyparts.xml` set, so the server can show the female, MacDoyleJr and MacGabhan NPC models (`!npc` in chat). `docs/BINARY_PATCHES.md`, "NPC model sets". |

## Layout in the game folder

```
<game>\mods\
    <name>\
        data\...        replacement or new data files: same paths as below <game>\data, plain (not encrypted)
        mod.dll         optional code
        *.dll, *.ini    anything else the mod needs (mod.dll finds DLLs next to it)
```

- Mods are processed in **folder-name order** (case-insensitive); number prefixes (`10_x`, `20_y`) set the order.
- **Conflicts stop the game.** Two mods may not replace the same data file, and two patches may not change the same
  bytes of the game's code. The first mod keeps it; for the second the game shows a message naming both mods and
  the file or address, and closes. Switch one of them off and start again.
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
  (`sadk::hook_slot`) are ordered the same way.
- **Game events instead of hooks:** `sadk::events::on_frame`, `on_match_enter` (before a match's world is built),
  `on_match_leave`, `on_session_end`, `on_net_event` (every event of the match network). The host hooks the game
  once and calls each mod in load order; a mod's subscriptions end when it is unloaded. `sadk::events::post(fn)`
  runs `fn` on the game's main thread at the next frame: the safe way to call game functions from a thread of your
  own. (`sadkmod/include/sadkmod/events.hpp` names the game functions behind each event.)
- **Small helpers:** `sadk::ui::room_message(text)` writes a line into the pre-game room's chat;
  `sadk::msvc::owned_string` is a game string whose text lives on the game's heap (for strings the game fills in);
  `sadk::mods::list()` / `activate` / `deactivate` / … manage other mods (`sadkmod/include/sadkmod/mods.hpp`).
- **One call inside one function:** `sadk::patch_calls(fn::Where, fn::What, (void *)detour, "label", 1)` redirects
  every call from `Where` to `What` (here: exactly one), found in the game binary (`sadkmod/include/sadkmod/calls.hpp`);
  `sadk::calls(fn::Where, fn::What)` lists them. No call-site address to look up by hand.
- **Patches:** `sadk::patch` changes bytes directly and checks what it replaces. One patch per address: a patch over
  bytes another one already changed is a conflict (see "Layout in the game folder").
- **Version and flags:** `SADKMOD_MAIN(start, version, flags)` also exports `sadkmod_version()` (the mod's own version,
  an integer) and `sadkmod_flags()`: what the mod changes, combinable — `SADKMOD_CLIENT` (only this player's game),
  `SADKMOD_SERVER` (matches: every player of a match needs it alike), `SADKMOD_LOBBY` (the lobby village). A mod
  without `mod.dll` names its flags with empty files `.client`, `.server`, `.lobby` in its folder. A mod without
  any flag, or a `mod.dll` without `sadkmod_version` / `sadkmod_flags` (built for an older sadkmod), is not loaded;
  the log says why. Every other installed mod is loaded at start-up, whatever its flags.
- **Loading and unloading later:** the host can also add a mod folder outside the scan (a downloaded
  `mods\.temp_<name>`, which keeps `<name>.ini`), activate it and deactivate any mod again (`sadkmod/include/sadkmod/host.hpp`;
  for mods, the same through `sadkmod_api`). A mod activated later starts after the game's start-up: its hooks
  only catch what runs from then on. When the active mods' property scripts (`data\game\scripts\properties`)
  change, the host fills the game's property database again the next time a match is entered
  (`docs/data-loading.md`).
- **Unloading:** `SADKMOD_STOP(stop)` exports `sadkmod_stop()`, called before a mod is unloaded: stop the mod's
  threads, take back what it gave the game. Hooks, table slots and patches made through sadkmod are recorded under
  the mod's folder name and taken back by the host; `write_memory` and a mod's own MinHook are not. The `mod.dll` is
  freed only when no thread runs in it or has an address inside it on its stack; until then it stays mapped,
  without any hook or patch.
- **Interface:** `sadkmod/include/sadkmod/mod.hpp` (`sadkmod_api`, version 2). `SADKMOD_MAIN` checks the version;
  fields are only ever appended. Without sadkmod, a mod can implement the exports itself.
- **Settings:** `sadk::mod_settings()` is the mod's `<name>.ini` next to its `mod.dll` (e.g. `billboards.ini`);
  other files live in `sadk::mod_dir()`.

## Server mods in a match (assetshare)

A server mod changes what happens in a match, so every player of a match needs the same server mods. The host's
set counts, and the assetshare mod makes it so:

1. **Joining.** When the host's game information arrives, the joiner fetches the host's manifest: its active server
   mods with name, version and content hash. No manifest within 15 s (a host without assetshare, a vanilla game):
   the host runs no server mods, so the joiner's own are switched off for this game. A host that runs server mods
   kicks a joiner that has not asked for the manifest within 20 s ("This game needs the assetshare mod"): without
   assetshare it would play with different rules. A host without server mods kicks nobody.
2. **Your own mods first.** Your server mods that the host does not run, or runs in another version or content, are
   switched off for this game. Only then is anything activated, so nothing conflicts.
3. **The host's mods.** Where you have the same mod (name, version and content hash), your copy is used. Every other
   one is downloaded into `<game>\mods\.temp_<name>`, checked against the host's hash and activated. A mod whose
   download is refused (`AcceptServerMods = false`), fails, or does not match keeps you from getting ready.
4. **In the room.** Lines in the pre-game room's chat say what happens: the mods switched off, each download and its
   progress, and the result ("Server mods for this game: rules 3 (downloaded), balance 1 (yours)."). "Ready" is
   refused until all of it is done.
5. **Afterwards.** When the network session ends (the match is left, the room is left, a kick), the downloaded mods
   are switched off and deleted and your own come back on. A `.temp_` folder left by a crash is deleted at the next
   start.

A map goes with the files of the same name next to it: `<map>.bin` (the map's environment data,
`CEnviromentMgr::LoadMapEnvData`), `<map>.lua` (its script) and `<map>_<name>.lua` (its cutscene scripts). What is
transferred is one archive per mod or map file, compressed with LZMS when both sides run Windows 8 or later
(the Windows Compression API; stored otherwise), sent in 4 KB blocks whatever the host's connection type. The game's
build checksum, which a joiner sends before anything can be transferred, leaves out server mods' files on both sides
(`GameData_ComputeBuildChecksum`; `sadkmod_api::redirect_server_mods`): server mods are matched by name, version and
hash instead. [TODO: not yet run in the game]

## Building the repo's mods

```
make            # build/<name>/ for every mod here: mod.dll plus its .ini files (builds sadkmod first)
make verify     # every mod's patches against a DRM-free SADK.exe under Wine (SADK_EXE=<path>)
make test       # assetshare's archive format under Wine (Wine has no LZMS: only the stored form runs there)
```

Install: copy `build/<name>` to `<game>\mods\<name>`. A new mod is a folder here with its `.cpp` files (and any
`.ini`, `.txt` or `data/` to ship); `tests/verify_mods.cpp` lists the start functions to verify.

## Status

The host is tested under Wine (`make test` in `sadkmod`): discovery, flags (from `mod.dll` and marker files; none:
not loaded), data-file conflicts (also for a mod activated later), overrides through `CreateFileA` / `CreateFileW`,
writes left alone, the mesh-cache redirect, `mod.dll` loading, hooks chained on one function in load order,
deactivating and activating mods, a `mod.dll` unloaded while another thread is inside its hook (freed once the thread
has left), a downloaded `.temp_` mod, the content hash. The mods' patches are verified against `SADK.exe`. In the
game (maintainer's test, 2026-10-08) the mod system and the repo's mods work, and the property database can be
filled again between offline matches (`propreload`). Loading and unloading mods in the running game is [TODO].
