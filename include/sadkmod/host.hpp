// The mod host: what the process that loads mods (the bridge shim) runs.
//
// start_mods() finds <game>\mods\* (folders starting with '_' or '.' are skipped) and reads what each mod changes
// (mod.hpp): a mod.dll's sadkmod_flags(), or the marker files .client / .server / .lobby of a mod without one. A mod
// without any flag, or whose mod.dll lacks sadkmod_init / sadkmod_version / sadkmod_flags, is not loaded. Every other
// mod is activated, in folder-name order:
//   - its data files go into the in-memory index. The file redirect (CreateFileA/W) opens the mod's file for a read
//     of <game>\data\<path> when the index has <path>; the mesh cache %LOCALAPPDATA%\SAdK\<mesh>.mshraw of a modded
//     .KEX goes to %LOCALAPPDATA%\SAdK\mods\ instead; plain (unencrypted) files pass NBase::gDecryptData. Two mods
//     with the same file are a conflict (registry.hpp): the first keeps it, the game shows both and closes.
//   - its mod.dll's sadkmod_init runs. Its hooks run in folder-name order (registry.hpp).
// Call it on the main thread before the game's entry point, after exe_is_supported().
//
// Later, mods can be added (add_mod: a folder outside the scan, such as a downloaded <game>\mods\.temp_<name>),
// activated and deactivated. The game reads its property database (scripts\properties) only at start-up; when the
// active mods' files there change, the host fills it again the next time a match is entered (Scene_CreateGlobal,
// docs/data-loading.md).
//
// Deactivating: the mod's sadkmod_stop runs, the registry takes back its hooks, table slots and patches, its data
// files leave the index, then its mod.dll is freed. Before freeing, every other thread is paused and checked: none
// may be executing in the mod.dll or have an address inside it on its stack (a return address into it, or anything
// else that points there). If one does, the mod.dll stays mapped, inert, and free_pending() tries again. Deactivate
// at a quiet moment (on the main thread between matches); what a mod handed the game outside the registry is
// sadkmod_stop's job.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sadk::host {

struct Summary {
    int mods = 0;          // folders active
    int skipped = 0;       // folders not loaded (no flags, missing exports, mod.dll failed)
    int files = 0;         // data files in the index
    int dlls = 0;          // mod.dll files started
};

Summary start_mods();

struct ModInfo {
    std::string folder;          // folder name; identifies the mod (also its owner name in the registry)
    std::string name;            // the same without ".temp_"
    std::string dir;
    std::uint32_t version = 0;   // sadkmod_version(); 0 without mod.dll
    std::uint32_t flags = 0;
    bool has_dll = false;
    bool active = false;
    bool pending_free = false;   // deactivated, its mod.dll not freed yet (free_pending)
};
std::vector<ModInfo> mods();

// A mod folder outside the scan: read and listed, not active. False if it has no flags or is listed already.
bool add_mod(const char *dir);
bool forget_mod(const char *folder);   // an inactive mod leaves the list (its folder may be deleted then)

// False if it is unknown, active, waiting to be freed, or its mod.dll failed to start (then all it did is undone).
bool activate(const char *folder);

enum class Unload { done, busy, not_found, not_active };
// busy: everything is undone, but the mod.dll is still mapped (a thread was in it); free_pending() retries.
Unload deactivate(const char *folder);
int free_pending();   // returns how many mod.dlls still wait

// MD5 of the mod folder's content: every file, by lowercase path relative to the folder, name and bytes.
std::string content_hash(const char *folder);

// The file redirect on its own, for tests: a path as the game would pass it -> the file to open instead, or
// nullptr (also nullptr for writes into data\). The result stays valid until the next call on the same thread.
const wchar_t *redirect(const wchar_t *path, bool write);

}  // namespace sadk::host
