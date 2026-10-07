# Sourcemap — import the project's Ghidra work into your own Ghidra

Everything this project has named and typed in the game's binaries, in a form you can apply to your own
copy. No game code or bytes are included: only names, types, signatures and comments, keyed by address.

| Folder | Binary it fits | MD5 |
|---|---|---|
| `sadk_noav.exe/` | `bin\SADK.exe`, the DRM-free (SecuROM-free) build | `d4832bc5103c14f5445471af29b8d778` |
| `tincat3.dll/` | `bin\tincat3.dll` | `8addb5da4b82956cdb67984181bb04e0` |

Each folder holds:

- **`types.gdt`** — a Ghidra data type archive with every type the project defined: classes, structs,
  vtable structs, enums and function definitions. It can also be opened on its own in the Data Type
  Manager (*Open File Archive*).
- **`types.json.gz`** — the same types as plain JSON (struct fields with offsets, enums, typedefs, function
  types), for tools that cannot read a Ghidra archive; `sadkmod` generates its C++ declarations from it.
- **`sourcemap.json.gz`** — the namespaces and classes, every named function with its signature,
  calling convention and tags, the project's own labels, typed globals, and all comments.

## Applying it

1. Import the binary into a Ghidra project (Ghidra 11 or later, x86 32-bit, default image base) and
   let auto-analysis finish.
2. Open the Script Manager, add this folder to the script directories (*Manage Script Directories*), and
   run **`ApplySourcemap.java`** (category *SAdK*). Pick the matching folder, e.g. `sourcemap/sadk_noav.exe`.
3. The script checks the program's MD5 against the map first and refuses a different binary. Applying
   takes a few minutes for `SADK.exe`. It prints a count per section and lists the first failures, if any.

Headless:

```
analyzeHeadless <project dir> <project> -import SADK.exe \
  -scriptPath <repo>/sourcemap -postScript ApplySourcemap.java <repo>/sourcemap/sadk_noav.exe
```

The script overwrites names, signatures and comments at the mapped addresses. Run it on a fresh import,
not on a project with your own work in it.

Verified on fresh imports with Ghidra 12.1: everything applies without failures, and re-exporting the
result gives the same function names, namespaces and signatures as the original. One difference is
Ghidra's own behaviour: about 450 functions have no name in the original, only a low-confidence
`guess_…` label at their entry, and Ghidra promotes such a label to the function's name.

## What the annotations mean

- Plate and decompiler comments starting with `[ai]` come from the project's mapping run. They state
  their evidence and a confidence (`conf=known / inferred / guess`); treat `guess` as a hint.
- Function tags: `AI` marks a name set by the mapping run; `LIBRARY:<lib>` marks recognized library code
  (MSVC runtime, STL, etc.); `CUSTOM_THIS` marks a function whose `this` needs custom storage.
- Address-level narrative documentation lives in `docs/message-catalog.md` (network protocol),
  `docs/types.md` (structs and classes) and `docs/SOURCEMAP.md`.

## Regenerating

`mapping/scripts/ExportSourcemap.java` writes `types.gdt` and `sourcemap.json.gz` from the project's Ghidra database (via the
Ghidra MCP: `run_ghidra_script ExportSourcemap.java` with `program=sadk_noav.exe`, then `tincat3.dll`).
Then `ExportTypesJson.java` (in this folder) writes `types.json.gz` from `types.gdt`. It runs inside Ghidra:
through the MCP, or headless with any small PE as the throwaway import:

```
analyzeHeadless <empty dir> tmp -import <any small .dll> -noanalysis -deleteProject \
  -scriptPath <repo>/sourcemap -postScript ExportTypesJson.java <repo>/sourcemap/sadk_noav.exe
```
