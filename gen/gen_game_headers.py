"""
Generates sadkmod's C++ declarations of the game from the project's Ghidra map (sourcemap/<binary>/):
types.json.gz (every struct, union, enum, typedef and function type) and sourcemap.json.gz (every named function
with its signature and calling convention, labels, typed globals, comments).

Stated reason (HARNESS.md §3; approved by the maintainer 2026-10-07): turns the Ghidra map into compile-time C++
declarations for mods. Neither MCP can produce source code; this reads only the committed sourcemap, never the
game binary.

Output (one tree per binary, namespace in brackets):
  <out>/sadkmod/game/sadk_noav/module.hpp    [sadk::game]          module, md5 of the binary
  <out>/sadkmod/game/sadk_noav/types.hpp     [sadk::game]          structs, unions, enums, typedefs, funcdefs
  <out>/sadkmod/game/sadk_noav/fn/<ns>.hpp   [sadk::game::fn]      functions, one header per top-level namespace
  <out>/sadkmod/game/sadk_noav/vars.hpp      [sadk::game::var]     named, typed globals
  <out>/sadkmod/game/sadk_noav/all.hpp       everything
  <out>/sadkmod/game/tincat3/...             [sadk::tincat, ::fn, ::var]
  <out>/sadkmod/game.hpp                     SADK.exe types + module constants (md5)
The struct layouts are emitted with explicit padding and checked with static_assert against Ghidra's offsets and
sizes, so a layout GCC would place differently fails the build instead of reading the wrong field.

What gets a callable declaration (Fn<...>): named functions whose signature was set by hand or imported
(sig_source USER_DEFINED / IMPORTED), with a known calling convention, no struct returned by value, and
(custom storage) `this` in ECX with all other parameters on the stack. Everything else that is named gets an
address only (Addr<...>), with the reason in a comment. Exception funclets (Unwind@, Catch_All@, LIBRARY:msvc-eh)
are skipped.

Names: Ghidra names are kept where they are C++ identifiers; other characters become '_' (`operator==` ->
`operator_eq`); names that clash with C++ keywords or common Windows macros get a trailing '_'; duplicates in one
namespace get `_<address>`. The Ghidra category `std` becomes `std_` (a nested `std` would hide ::std).

Names that are macros in the Windows / C runtime headers (the list comes from the compiler, see the Makefile:
`g++ -dM -E` over <windows.h> and the standard headers) also get a trailing '_', so the declarations can be
included after <windows.h>.

Usage: python3 gen_game_headers.py <sourcemap dir> <out include dir> [--macros <file of #define lines>]
"""
import gzip
import json
import os
import re
import sys
from collections import defaultdict

KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t
class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default
delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable
namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires
return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try
typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq
NULL TRUE FALSE IN OUT OPTIONAL CONST VOID DELETE ERROR interface near far small hyper pascal cdecl min max
CALLBACK WINAPI APIENTRY PASCAL FAR NEAR STRICT INFINITE MAX_PATH ABSOLUTE RELATIVE TRANSPARENT OPAQUE
DOMAIN OVERFLOW UNDERFLOW TLOSS PLOSS SING EOF BUFSIZ errno stdin stdout stderr assert offsetof
GetObject CreateWindow CreateFont DrawText LoadImage SendMessage PostMessage GetMessage CreateFile DeleteFile
CopyFile MoveFile LoadLibrary GetModuleHandle FindResource SetCurrentDirectory GetCurrentDirectory
RGB GetRValue GetGValue GetBValue IGNORE DIFFERENCE BLACKNESS WHITENESS SRCCOPY ALTERNATE WINDING""".split())

OPERATORS = [("operator==", "operator_eq"), ("operator!=", "operator_ne"), ("operator<=", "operator_le"),
             ("operator>=", "operator_ge"), ("operator<<", "operator_shl"), ("operator>>", "operator_shr"),
             ("operator<", "operator_lt"), ("operator>", "operator_gt"), ("operator+=", "operator_add_assign"),
             ("operator-=", "operator_sub_assign"), ("operator*=", "operator_mul_assign"),
             ("operator/=", "operator_div_assign"), ("operator=", "operator_assign"), ("operator+", "operator_add"),
             ("operator-", "operator_sub"), ("operator*", "operator_mul"), ("operator/", "operator_div"),
             ("operator[]", "operator_index"), ("operator()", "operator_call"), ("operator->", "operator_arrow"),
             ("operator!", "operator_not"), ("operator new", "operator_new"), ("operator delete", "operator_delete")]

# Ghidra built-in types: C++ spelling, size, alignment
BUILTIN = {
    "void": ("void", 0, 1), "bool": ("bool", 1, 1), "char": ("char", 1, 1), "schar": ("signed char", 1, 1),
    "uchar": ("unsigned char", 1, 1), "byte": ("::std::uint8_t", 1, 1), "sbyte": ("::std::int8_t", 1, 1),
    "undefined": ("::std::uint8_t", 1, 1), "undefined1": ("::std::uint8_t", 1, 1),
    "undefined2": ("::std::uint16_t", 2, 2), "undefined4": ("::std::uint32_t", 4, 4),
    "undefined8": ("::std::uint64_t", 8, 8),
    "short": ("::std::int16_t", 2, 2), "ushort": ("::std::uint16_t", 2, 2), "word": ("::std::uint16_t", 2, 2),
    "sword": ("::std::int16_t", 2, 2),
    "int": ("::std::int32_t", 4, 4), "uint": ("::std::uint32_t", 4, 4), "long": ("::std::int32_t", 4, 4),
    "ulong": ("::std::uint32_t", 4, 4), "dword": ("::std::uint32_t", 4, 4), "sdword": ("::std::int32_t", 4, 4),
    "uint3": ("::std::uint8_t[3]", 3, 1), "int3": ("::std::uint8_t[3]", 3, 1),
    "longlong": ("::std::int64_t", 8, 8), "ulonglong": ("::std::uint64_t", 8, 8), "qword": ("::std::uint64_t", 8, 8),
    "sqword": ("::std::int64_t", 8, 8),
    "float": ("float", 4, 4), "double": ("double", 8, 8), "float10": ("::std::uint8_t[10]", 10, 1),
    "wchar_t": ("wchar_t", 2, 2), "wchar16": ("char16_t", 2, 2), "wchar32": ("char32_t", 4, 4),
    "pointer": ("void *", 4, 4), "pointer32": ("void *", 4, 4), "addr": ("void *", 4, 4),
    "string": ("char", 1, 1), "TerminatedCString": ("char", 1, 1), "unicode": ("wchar_t", 2, 2),
    "TerminatedUnicode": ("wchar_t", 2, 2), "ImageBaseOffset32": ("::std::uint32_t", 4, 4),
    "GUID": ("::std::uint8_t[16]", 16, 4),
}
# Ghidra types that sadkmod implements by hand (msvc.hpp)
HANDWRITTEN = {"/std/string": ("::sadk::msvc::string", 28, 4)}

CC_ATTR = {"__thiscall": "SADK_THISCALL", "__stdcall": "SADK_STDCALL", "__cdecl": "SADK_CDECL",
           "__fastcall": "SADK_FASTCALL"}


MACROS = set()   # macro names of the Windows/CRT headers mods include (filled by --macros)


def ident(name, reserved=()):
    for op, rep in OPERATORS:
        if name == op or name.startswith(op + "_"):
            name = rep + name[len(op):]
            break
    name = name.replace("~", "dtor_")
    name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    name = re.sub(r"_+", "_", name) if "__" in name and not name.startswith("__") else name
    if not name or name[0].isdigit():
        name = "_" + name
    while name in KEYWORDS or name in MACROS or name in reserved:
        name += "_"
    return name


def comment(text, limit=300):
    if not text:
        return ""
    t = " ".join(str(text).split())
    if len(t) > limit:
        t = t[:limit - 3] + "..."
    return t.rstrip("\\")


class Program:
    def __init__(self, folder, root_ns, module, out_name):
        self.root_ns, self.module, self.out_name = root_ns, module, out_name
        self.types = {t["path"]: t for t in json.load(gzip.open(os.path.join(folder, "types.json.gz")))["types"]}
        self.map = json.load(gzip.open(os.path.join(folder, "sourcemap.json.gz")))
        self.md5 = self.map["program"]["md5"]
        self.funcs_by_addr = {f["entry"]: f for f in self.map["functions"]}
        self.plates = {c["addr"]: c["text"] for c in self.map["comments"] if c["kind"] == "plate"}
        self._name_types()

    # ---- type names -------------------------------------------------------------------------------------------
    def _name_types(self):
        """C++ namespace + name for every type path, collision-free."""
        cats = defaultdict(set)          # category path -> type names in it
        for p in self.types:
            cat, name = p.rsplit("/", 1)
            cats[cat].add(name)
        self.ns_of_cat = {}

        def cat_ns(cat):
            if cat in self.ns_of_cat:
                return self.ns_of_cat[cat]
            if cat == "":
                r = []
            else:
                parent, comp = cat.rsplit("/", 1)
                pn = cat_ns(parent)
                c = ident(comp)
                if c == "std":
                    c = "std_"
                if comp in cats.get(parent, ()) or c in ("fn", "var"):   # a type of the same name in the parent
                    c += "_"
                r = pn + [c]
            self.ns_of_cat[cat] = r
            return r

        self.cxx = {}                    # path -> (namespace list, name)
        used = defaultdict(set)
        for p in sorted(self.types):
            if p in HANDWRITTEN:
                continue
            cat, name = p.rsplit("/", 1)
            ns = cat_ns(cat)
            n = ident(name)
            while n in used[tuple(ns)]:
                n += "_"
            used[tuple(ns)].add(n)
            self.cxx[p] = (ns, n)

    def qual(self, path):
        if path in HANDWRITTEN:
            return HANDWRITTEN[path][0]
        ns, n = self.cxx[path]
        return "::" + "::".join(["sadk", *self.root_ns, *ns, n])

    # ---- type expressions -------------------------------------------------------------------------------------
    def parse(self, tpath):
        """'/a/B *[4]' -> (base path, pointer depth, array dims). Ghidra puts pointer stars before array dims."""
        dims = []
        m = re.search(r"((\[\d+\])+)$", tpath)
        if m:
            dims = [int(x) for x in re.findall(r"\[(\d+)\]", m.group(1))]
            tpath = tpath[:m.start()]
        depth = 0
        while tpath.endswith(" *") or tpath.endswith("*"):
            tpath = tpath[:-2] if tpath.endswith(" *") else tpath[:-1]
            depth += 1
        return tpath.strip(), depth, dims

    def base_info(self, base):
        """(c++ spelling, size, align, kind) of a base path, or None if unknown."""
        if base in HANDWRITTEN:
            s, size, al = HANDWRITTEN[base]
            return s, size, al, "struct"
        if base.count("/") == 1 and base[1:] in BUILTIN:
            s, size, al = BUILTIN[base[1:]]
            return s, size, al, "builtin"
        t = self.types.get(base)
        if not t:
            return None
        k = t["kind"]
        if k in ("struct", "union"):
            return self.qual(base), t["size"], self.align_of(base), k
        if k == "enum":
            return self.qual(base), t["size"], min(max(t["size"], 1), 8), k
        if k == "typedef":
            inner = self.type_info(t["base"])
            if inner is None:
                return None
            return self.qual(base), inner[1], inner[2], "typedef"
        if k == "funcdef":
            return self.qual(base), 0, 1, "funcdef"
        return None

    def type_info(self, tpath):
        """(c++ declarator template with {} for the name, size, align) or None."""
        base, depth, dims = self.parse(tpath)
        if depth:
            info = self.base_info(base)
            if info is None or info[3] == "builtin" and info[0].endswith("]"):
                s = "void"
            else:
                s = info[0]
            decl = s + " " + "*" * depth + "{}"
            size, al = 4, 4
        else:
            info = self.base_info(base)
            if info is None or (info[1] == 0 and info[0] != "void" and info[3] != "funcdef"):
                return None
            s, size, al, kind = info
            if s.endswith("]"):                      # builtin arrays like uint8_t[3]
                arr = s[s.index("["):]
                decl = s[:s.index("[")] + " {}" + arr
            else:
                decl = s + " {}"
        for d in dims:
            size *= d
        if dims:
            decl = decl + "".join(f"[{d}]" for d in dims)
        return decl, size, al

    def align_of(self, path, _seen=None):
        t = self.types[path]
        if t.get("packed"):
            return max(t.get("align", 1), 1)
        _seen = _seen or set()
        if path in _seen:
            return 4
        _seen.add(path)
        al = 1
        for f in t.get("fields", []):
            base, depth, _ = self.parse(f["type"])
            if depth:
                al = max(al, 4)
                continue
            if base in self.types and self.types[base]["kind"] in ("struct", "union"):
                al = max(al, self.align_of(base, _seen))
            else:
                info = self.base_info(base)
                al = max(al, info[2] if info else 1)
        return al

    # ---- function types ---------------------------------------------------------------------------------------
    def class_this(self, ns):
        path = "/" + ns.replace("::", "/")
        if path in self.types and self.types[path]["kind"] == "struct":
            return self.qual(path) + " *"
        return "void *"

    def param_type(self, tpath):
        if tpath in (None, "/undefined"):
            return "::std::uint32_t"
        info = self.type_info(tpath)
        if info is None:
            return "::std::uint32_t" if self.parse(tpath)[1] == 0 else "void *"
        decl = info[0].format("").strip()
        if "[" in decl:                                  # arrays decay
            decl = decl[:decl.index("[")].strip() + " *"
        return decl

    def callable_reason(self, f):
        """None if f gets an Fn<> declaration, else why not."""
        if f["sig_source"] not in ("USER_DEFINED", "IMPORTED"):
            return f"signature not confirmed (sig_source {f['sig_source']})"
        if f["cc"] not in CC_ATTR:
            return f"calling convention {f['cc']}"
        if f.get("custom_storage"):
            ps = f["params"]
            if f["cc"] != "__thiscall" or not ps or not ps[0].get("storage", "").startswith("register:00000004:"):
                return "custom parameter storage"
            if any(not p.get("storage", "").startswith("Stack[") for p in ps[1:]):
                return "custom parameter storage"
        r = self.parse(f["ret"] or "/void")
        if not r[1] and r[0] in self.types and self.types[r[0]]["kind"] in ("struct", "union"):
            return "returns a struct by value"
        return None

    def fn_pointer(self, f, cc=None, this=None):
        cc = cc or f["cc"]
        ret = "void" if f["ret"] == "/void" else self.param_type(f["ret"])
        args = []
        params = f["params"]
        if f.get("custom_storage"):
            args.append(self.param_type(params[0]["type"]))
            params = params[1:]
        elif cc == "__thiscall":
            args.append(this or self.class_this(f["ns"]))
        args += [self.param_type(p["type"]) for p in params]
        if f.get("varargs"):
            args.append("...")
        return f"{ret} ({CC_ATTR[cc]} *)({', '.join(args)})"

    # ---- emit: types ------------------------------------------------------------------------------------------
    def emit_types(self, out):
        w = out.append
        root = "::".join(["sadk", *self.root_ns])
        w("// Generated by sadkmod/gen/gen_game_headers.py from sourcemap/" + self.out_name + " - do not edit.\n")
        w("#pragma once\n#include <cstddef>\n#include <cstdint>\n#include <sadkmod/core.hpp>\n#include <sadkmod/msvc.hpp>\n\n")
        w("#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Winvalid-offsetof\"\n\n")
        comps = [p for p, t in self.types.items() if t["kind"] in ("struct", "union") and p in self.cxx]
        enums = [p for p, t in self.types.items() if t["kind"] == "enum" and p in self.cxx]
        tdefs = [p for p, t in self.types.items() if t["kind"] == "typedef" and p in self.cxx]
        fdefs = [p for p, t in self.types.items() if t["kind"] == "funcdef" and p in self.cxx]

        def block(path, body):
            ns, _ = self.cxx[path]
            w(f"namespace {'::'.join([root, *ns])} {{\n{body}}}\n")

        for p in sorted(comps):
            block(p, f"{self.types[p]['kind']} {self.cxx[p][1]};\n")
        w("\n")
        for p in sorted(enums):
            t = self.types[p]
            vals = t.get("values", {})
            size = t["size"] if t["size"] in (1, 2, 4, 8) else 4
            signed = any(v < 0 for v in vals.values())
            ut = f"::std::{'' if signed else 'u'}int{size * 8}_t"
            body = (f"// {comment(t.get('desc'))}\n" if t.get("desc") else "") + f"enum class {self.cxx[p][1]} : {ut} {{\n"
            used = set()
            for n, v in sorted(vals.items(), key=lambda kv: (kv[1], kv[0])):
                i = ident(n)
                while i in used:
                    i += "_"
                used.add(i)
                if not signed:
                    v &= (1 << (size * 8)) - 1
                body += f"    {i} = {v if v >= 0 else f'({v})'},\n"
            block(p, body + "};\n")
        w("\n")
        # typedefs and function types refer to each other by name: emit both in dependency order (structs, unions
        # and enums are declared already)
        aliases = set(tdefs) | set(fdefs)
        done = set()

        def refs(p):
            t = self.types[p]
            paths = [t["base"]] if t["kind"] == "typedef" else [t["ret"]] + [a["type"] for a in t.get("params", [])]
            return [b for b in (self.parse(x or "/void")[0] for x in paths) if b in aliases and b != p]

        def emit_alias(p, stack=()):
            if p in done or p in stack:
                return
            for q in refs(p):
                emit_alias(q, stack + (p,))
            t = self.types[p]
            if t["kind"] == "funcdef":
                cc = t.get("cc") if t.get("cc") in CC_ATTR else "__cdecl"
                ret = "void" if t["ret"] == "/void" else self.param_type(t["ret"])
                args = [self.param_type(a["type"]) for a in t.get("params", [])]
                if t.get("varargs"):
                    args.append("...")
                note = "" if t.get("cc") in CC_ATTR else f"  // calling convention {t.get('cc')}: assumed cdecl"
                block(p, f"typedef {ret} ({CC_ATTR[cc]} {self.cxx[p][1]})({', '.join(args)});{note}\n")
            else:
                info = self.type_info(t["base"])
                decl = info[0].format(self.cxx[p][1]) if info else f"::std::uint8_t {self.cxx[p][1]}[{max(t['size'], 1)}]"
                block(p, f"typedef {decl};\n")
            done.add(p)

        for p in sorted(aliases):
            emit_alias(p)
        w("\n")
        # composites in by-value dependency order
        deps = {}
        for p in comps:
            d = set()
            for f in self.types[p].get("fields", []):
                b, depth, _ = self.parse(f["type"])
                if depth:
                    continue
                while b in self.types and self.types[b]["kind"] == "typedef":
                    b2, depth2, _ = self.parse(self.types[b]["base"])
                    if depth2:
                        b = None
                        break
                    b = b2
                if b in self.types and self.types[b]["kind"] in ("struct", "union") and b in self.cxx and b != p:
                    d.add(b)
            deps[p] = d
        order, state = [], {}

        def visit(p):
            if state.get(p) == 2:
                return
            if state.get(p) == 1:
                return                                   # by-value cycle: impossible in a real layout, ignore
            state[p] = 1
            for q in sorted(deps[p]):
                visit(q)
            state[p] = 2
            order.append(p)

        for p in sorted(comps):
            visit(p)
        self.vtable_funcs = 0
        for p in order:
            self.emit_composite(p, block)
        w("\n#pragma GCC diagnostic pop\n")

    def field_decl(self, f, name, struct_path):
        """Declaration of one field, typed function pointer for vtable slots."""
        if f["type"] == "/void *" and re.search(r"(vftable|vtbl|_vtable)", struct_path, re.I):
            m = re.match(r"\s*([0-9a-fA-F]{8})\b", f.get("comment") or "")
            fn = self.funcs_by_addr.get(m.group(1).lower()) if m else None
            if fn and fn.get("name") and self.callable_reason(fn) is None:
                self.vtable_funcs += 1
                return self.fn_pointer(fn).replace("*)(", "*" + name + ")(", 1)
        info = self.type_info(f["type"])
        if info is None or info[1] != f["len"]:
            return f"::std::uint8_t {name}[{f['len']}]", (f" [{f['type']}]" if info is None or f['type'] else "")
        return info[0].format(name)

    def emit_composite(self, p, block):
        t = self.types[p]
        kind, size = t["kind"], t["size"]
        name = self.cxx[p][1]
        fields = t.get("fields", [])
        body, checks = [], []
        reserved = set()
        al_struct = self.align_of(p)
        packed = False
        pos = 0
        used = set()
        bitfield_end = -1
        for f in sorted(fields, key=lambda f: f["off"]) if kind == "struct" else fields:
            off, ln = f["off"], f["len"]
            if ln == 0:
                body.append(f"    // +0x{off:x} {f.get('name')}: zero-length field omitted")
                continue
            if "bits" in f:
                if off < bitfield_end:
                    body[-1] += f"; {f.get('name')}:{f['bits']}@{f['bit_off']}"
                    continue
                if kind == "struct" and off > pos:
                    body.append(f"    ::std::uint8_t _gap_{pos:x}[{off - pos}];")
                body.append(f"    ::std::uint8_t _bits_{off:x}[{ln}];  // bit fields: {f.get('name')}:{f['bits']}@{f['bit_off']}")
                bitfield_end = off + ln
                pos = max(pos, off + ln)
                continue
            if kind == "struct" and off < pos:
                body.append(f"    // +0x{off:x} {f.get('name')}: overlaps the previous field, omitted")
                continue
            if kind == "struct" and off > pos:
                body.append(f"    ::std::uint8_t _gap_{pos:x}[{off - pos}];")
            fname = ident(f.get("name") or f"field_{off:x}", reserved)
            while fname in used:
                fname += "_"
            used.add(fname)
            decl = self.field_decl(f, fname, p)
            note = ""
            if isinstance(decl, tuple):
                decl, note = decl
                note = f" raw bytes{note}"
            base, depth, _ = self.parse(f["type"])
            fal = 4 if depth else (self.type_info(f["type"]) or (0, 0, 1))[2]
            if note:
                fal = 1
            if kind == "struct" and fal and off % fal:
                packed = True
            c = comment(f.get("comment"), 200)
            body.append(f"    {decl};  // +0x{off:x}{note}{(' ' + c) if c else ''}")
            checks.append(f"static_assert(offsetof({name}, {fname}) == 0x{off:x});")
            pos = max(pos, off + ln) if kind == "struct" else pos
        if kind == "struct":
            if size > pos:
                body.append(f"    ::std::uint8_t _tail_{pos:x}[{size - pos}];")
            if size % max(al_struct, 1):
                packed = True
        else:
            body.append(f"    ::std::uint8_t _raw[{max(size, 1)}];")
            if size % max(al_struct, 1):
                packed = True
        if not fields and kind == "struct" and size <= pos:
            body.append(f"    ::std::uint8_t _raw[{max(size, 1)}];")
        head = f"// {comment(t.get('desc'))}\n" if t.get("desc") else ""
        pk = " __attribute__((packed))" if packed or t.get("packed") and t.get("align", 1) == 1 else ""
        text = head + f"{kind}{pk} {name} {{\n" + "\n".join(body) + "\n};\n"
        text += f"static_assert(sizeof({name}) == {max(size, 1)});\n"
        if kind == "struct":
            text += "\n".join(checks) + ("\n" if checks else "")
        block(p, text)

    # ---- emit: functions --------------------------------------------------------------------------------------
    def functions(self):
        """namespace path (list) -> [(name, line)]"""
        mod = f"::sadk::Module::{self.module}"
        by_ns = defaultdict(list)
        for f in self.map["functions"]:
            n = f.get("name")
            if not n or n.startswith(("Unwind@", "Catch_All@", "Catch@")) or "LIBRARY:msvc-eh" in f.get("tags", []):
                continue
            by_ns[f["ns"]].append(f)
        child_ns = defaultdict(set)
        for ns in list(by_ns):
            parts = ns.split("::") if ns else []
            for i in range(len(parts)):
                child_ns["::".join(parts[:i])].add(parts[i])
        result = defaultdict(list)
        for ns, fs in by_ns.items():
            parts = [("std_" if p == "std" else ident(p)) for p in (ns.split("::") if ns else [])]
            counts = defaultdict(int)
            for f in fs:
                counts[ident(f["name"])] += 1
            reserved = {("std_" if c == "std" else ident(c)) for c in child_ns.get(ns, ())}
            used = set()
            for f in sorted(fs, key=lambda f: f["entry"]):
                base = ident(f["name"], reserved)
                n = base if counts[ident(f["name"])] == 1 else f"{base}_{f['entry']}"
                while n in used:
                    n += "_"
                used.add(n)
                addr = f"0x{f['entry']}"
                plate = comment(self.plates.get(f["entry"]), 240)
                why = self.callable_reason(f)
                doc = f"// {f['sig']}" + (f"  [{f['cc']}]" if f["cc"] else "")
                if plate:
                    doc += f"\n// {plate}"
                label = cstr(f"{f['ns']}::{f['name']}" if f["ns"] else f["name"])
                body = f.get("body") or []   # [start, end) ranges of its code (sadk::calls)
                if body:
                    nums = ", ".join(f"0x{v}" for r in body for v in r)
                    label += f", ::sadk::Body<{len(body)}>{{{{{nums}}}}}"
                if why is None:
                    line = f"{doc}\ninline constexpr ::sadk::Fn<{mod}, {addr}, {self.fn_pointer(f)}, {label}> {n}{{}};"
                else:
                    line = f"{doc}\n// address only: {why}\ninline constexpr ::sadk::Addr<{mod}, {addr}, {label}> {n}{{}};"
                result[tuple(parts)].append(line)
        return result

    def globals(self):
        mod = f"::sadk::Module::{self.module}"
        data = {d["addr"]: d for d in self.map["data"]}
        out = defaultdict(list)
        used = defaultdict(set)
        for l in self.map["labels"]:
            d = data.get(l["addr"])
            if not d or re.search(r"_[0-9a-f]{8}$", l["name"]) or l["name"].startswith(("switchD", "caseD", "DAT_")):
                continue
            info = self.type_info(d["type"])
            if info is None:
                continue
            t = info[0].format("").strip()
            if "(" in t:
                continue
            ns = tuple(("std_" if p == "std" else ident(p)) for p in (l["ns"].split("::") if l["ns"] else []))
            n = ident(l["name"])
            while n in used[ns]:
                n += "_"
            used[ns].add(n)
            label = cstr(f"{l['ns']}::{l['name']}" if l["ns"] else l["name"])
            out[ns].append(f"inline constexpr ::sadk::Var<{mod}, 0x{l['addr']}, {t}, {label}> {n}{{}};  // {d['type']}")
        return out


def cstr(text):
    """A C++ string literal of `text` (ASCII; anything else as an escape)."""
    out = []
    for ch in text:
        if ch in '\\"':
            out.append("\\" + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(f"\\x{ord(ch) & 0xff:02x}\"\"")
    return '"' + "".join(out) + '"'


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = open(path).read() if os.path.exists(path) else None
    if old != text:
        with open(path, "w") as fh:
            fh.write(text)


def generate(sourcemap_dir, out_dir):
    progs = [Program(os.path.join(sourcemap_dir, "sadk_noav.exe"), ["game"], "sadk", "sadk_noav.exe"),
             Program(os.path.join(sourcemap_dir, "tincat3.dll"), ["tincat"], "tincat3", "tincat3.dll")]
    stats = []
    for prog, folder in zip(progs, ["sadk_noav", "tincat3"]):
        base = os.path.join(out_dir, "sadkmod", "game", folder)
        root = "::".join(["sadk", *prog.root_ns])
        out = []
        prog.emit_types(out)
        types_text = "".join(out)
        types_text = types_text.replace("#include <sadkmod/msvc.hpp>\n", "#include <sadkmod/msvc.hpp>\n#include \"module.hpp\"\n", 1)
        write(os.path.join(base, "module.hpp"),
              "// Generated by sadkmod/gen/gen_game_headers.py - do not edit.\n#pragma once\n#include <sadkmod/core.hpp>\n"
              f"namespace {root} {{\n"
              f"inline constexpr ::sadk::Module module = ::sadk::Module::{prog.module};\n"
              f"inline constexpr char md5[] = \"{prog.md5}\";  // the binary these declarations describe\n}}\n")
        write(os.path.join(base, "types.hpp"), types_text)
        fns = prog.functions()
        groups = defaultdict(list)
        for ns, lines in fns.items():
            groups[ns[0] if ns else "_global"].append((ns, lines))
        headers = []
        n_fn = n_addr = 0
        for g, items in sorted(groups.items()):
            text = ["// Generated by sadkmod/gen/gen_game_headers.py - do not edit.\n#pragma once\n#include \"../types.hpp\"\n\n"]
            for ns, lines in sorted(items):
                text.append(f"namespace {'::'.join([root, 'fn', *ns])} {{\n\n" + "\n\n".join(lines) + "\n\n}\n\n")
                n_fn += sum(1 for l in lines if "::sadk::Fn<" in l)
                n_addr += sum(1 for l in lines if "::sadk::Addr<" in l)
            fname = f"fn/{g}.hpp"
            write(os.path.join(base, fname), "".join(text))
            headers.append(fname)
        gl = prog.globals()
        text = ["// Generated by sadkmod/gen/gen_game_headers.py - do not edit.\n#pragma once\n#include \"types.hpp\"\n\n"]
        for ns, lines in sorted(gl.items()):
            text.append(f"namespace {'::'.join([root, 'var', *ns])} {{\n" + "\n".join(lines) + "\n}\n")
        write(os.path.join(base, "vars.hpp"), "".join(text))
        write(os.path.join(base, "all.hpp"), "// Generated - do not edit.\n#pragma once\n#include \"types.hpp\"\n#include \"vars.hpp\"\n"
              + "".join(f"#include \"{h}\"\n" for h in headers))
        stats.append(f"{prog.out_name}: {len(prog.types)} types ({prog.vtable_funcs} typed vtable slots), "
                     f"{n_fn} callable functions, {n_addr} address-only, {sum(len(v) for v in gl.values())} globals, "
                     f"{len(headers)} function headers")
    write(os.path.join(out_dir, "sadkmod", "game.hpp"),
          "// Generated - do not edit. SADK.exe types; functions: <sadkmod/game/sadk_noav/fn/<namespace>.hpp>.\n"
          "#pragma once\n#include \"game/sadk_noav/types.hpp\"\n")
    print("\n".join(stats))


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--macros" in args:
        i = args.index("--macros")
        for line in open(args[i + 1]):
            m = re.match(r"#define\s+([A-Za-z_][A-Za-z0-9_]*)", line)
            if m:
                MACROS.add(m.group(1))
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        sys.exit(2)
    generate(*args)
