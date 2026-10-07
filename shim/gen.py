"""Build helper: generates the forwarding thunks (thunks.S), the export table (wsock32.def) and the
ordinal/index table (ordinals.h) from exports.txt (ordinal name). Every export jumps through
real_ptrs[i], filled from the system wsock32.dll by ordinal at load; the HOOKED ones are exported from
bridge.cpp instead (extern "C" shim_<name>, stdcall)."""

HOOKED = {"connect": 12, "send": 16, "listen": 8, "closesocket": 4}   # name -> stdcall arg bytes

rows = [line.split() for line in open("exports.txt") if line.strip()]
with open("thunks.S", "w") as s, open("wsock32.def", "w") as d:
    s.write("    .text\n")
    d.write("LIBRARY wsock32.dll\nEXPORTS\n")
    for i, (ordinal, name) in enumerate(rows):
        if name in HOOKED:
            d.write(f"    {name} = shim_{name}@{HOOKED[name]} @{ordinal}\n")
            continue
        s.write(f"    .globl _fwd_{ordinal}\n_fwd_{ordinal}:\n    jmp *_real_ptrs+{4 * i}\n")
        d.write(f"    {name} = fwd_{ordinal} @{ordinal}\n")
with open("ordinals.h", "w") as h:
    h.write(f"#define N_EXPORTS {len(rows)}\nstatic const unsigned short ordinals[N_EXPORTS] = {{"
            + ", ".join(o for o, _ in rows) + "};\n")
    for i, (_o, name) in enumerate(rows):
        h.write(f"#define IDX_{name} {i}\n")
