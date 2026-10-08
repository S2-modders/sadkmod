// The mod host: what the process that loads mods (the bridge shim) runs once at start-up.
//
// start_mods() finds <game>\mods\*, builds the in-memory index of every mod's data files, installs the file
// redirect (CreateFileA/W: a read of <game>\data\<path> opens the mod's file when the index has <path>; the mesh
// cache %LOCALAPPDATA%\SAdK\<mesh>.mshraw of a modded .KEX goes to %LOCALAPPDATA%\SAdK\mods\ instead, so the
// game's own cache is never overwritten), lets plain (unencrypted) files through NBase::gDecryptData, then loads
// every mod.dll and calls its sadkmod_init. Call it on the main thread before the game's entry point, after
// exe_is_supported().
#pragma once
#include <cstddef>

namespace sadk::host {

struct Summary {
    int mods = 0;          // folders taken into account
    int files = 0;         // data files in the index (a file two mods have is a conflict: the first keeps it)
    int dlls = 0;          // mod.dll files that loaded and initialised
    int dll_failures = 0;
};

Summary start_mods();

// The file redirect on its own, for tests: a path as the game would pass it -> the file to open instead, or
// nullptr (also nullptr for writes into data\). The result stays valid until the next call on the same thread.
const wchar_t *redirect(const wchar_t *path, bool write);

// Builds the index for an explicit game root (tests); start_mods() calls it with game_root().
Summary build_index(const char *root);

}  // namespace sadk::host
