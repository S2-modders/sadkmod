# wsock32 shim

A proxy `wsock32.dll` that loads mods into the game. Mods: `mods/README.md`; the library both are built on:
`sadkmod/README.md`.

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
  restores the entry point, checks the exe and loads the mods (`sadk::host::start_mods`), then calls the real entry
  point.
- **Requires the DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`). On any other build the shim stays a
  pure pass-through and loads no mods.
- Everything else lives in mods: hosting through the lobby server is `mods/gamebridge`, map sharing
  `mods/assetshare`.
- Build (Linux, mingw-w64, Python 3): `make` -> `wsock32.dll` (builds `sadkmod` first). `make test` loads it into a
  test program under Wine: forwarding works, and it stays inactive outside `SADK.exe`.
- Install: copy it into the game's `bin` folder, next to `SADK.exe` (SAdK-ServerConfig does this, with the mods).
  Remove it to undo.
