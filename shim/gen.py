"""Build helper: generates the forwarding thunks (thunks.S) and the export table (wsock32.def) from
exports.txt (ordinal name). Every export jumps through real[i], filled from the system wsock32.dll by
ordinal at load; `listen` is exported from shim.c instead."""
import sys

rows = [line.split() for line in open("exports.txt") if line.strip()]
with open("thunks.S", "w") as s, open("wsock32.def", "w") as d:
    s.write("    .text\n")
    d.write("LIBRARY wsock32.dll\nEXPORTS\n")
    for i, (ordinal, name) in enumerate(rows):
        if name == "listen":
            d.write(f"    listen = shim_listen@8 @{ordinal}\n")
            continue
        s.write(f"    .globl _fwd_{ordinal}\n_fwd_{ordinal}:\n    jmp *_real_ptrs+{4 * i}\n")
        d.write(f"    {name} = fwd_{ordinal} @{ordinal}\n")
with open("ordinals.h", "w") as h:
    h.write(f"#define N_EXPORTS {len(rows)}\nstatic const unsigned short ordinals[N_EXPORTS] = {{"
            + ", ".join(o for o, _ in rows) + "};\n")
    h.write(f"#define LISTEN_INDEX {[n for _, n in rows].index('listen')}\n")
