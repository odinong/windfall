"""Reverse-engineering helper for cking.rpx (stripped): cross-references between strings, data and code.

  python xref.py str <regex>         strings matching regex, with the functions that reference them
  python xref.py refs <addr>         functions referencing a data address (lis/addi pairs, pointers)
  python xref.py callers <addr>      functions that call addr
  python xref.py callees <func>      calls made by the function containing func (in order)
  python xref.py func <addr>         start/end of the function containing addr
  python xref.py dis <addr> [n]      disassemble the function containing addr (or n instructions)

The function index (from the recompiler's discovery) is cached in generated/functions.json.
Set WF_RPX to the path of cking.rpx (default: the WF_GAME_DIR dump).
"""
import bisect
import json
import os
import re
import sys

import analyze
import layout
import rpx

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CACHE = os.path.join(ROOT, "generated", "functions.json")

def rpx_path():
    if os.environ.get("WF_RPX"):
        return os.environ["WF_RPX"]
    game = os.environ.get("WF_GAME_DIR", r"D:\WWHD\THE LEGEND OF ZELDA The Wind Waker HD [Game] [0005000010143500]")
    return os.path.join(game, "code", "cking.rpx")

class Xref:
    def __init__(self):
        self.r = rpx.load(rpx_path())
        self.img = analyze.Image(self.r)
        self.imports = {i.addr: f"{i.module}::{i.name}" for i in self.r.imports}
        if os.path.exists(CACHE):
            self.funcs = {int(k): v for k, v in json.load(open(CACHE)).items()}
        else:
            slots, imports_by_addr = layout.assign_imports(self.r)
            layout.relocate_words(self.r, self.img.text, self.img.words, imports_by_addr)
            self.img._dec.clear()
            found = analyze.discover(self.img, log=lambda *a: None)
            self.funcs = {s: (f.insns[-1] + 4 if f.insns else s + 4) for s, f in found.items()}
            os.makedirs(os.path.dirname(CACHE), exist_ok=True)
            json.dump({str(k): v for k, v in self.funcs.items()}, open(CACHE, "w"))
        self.starts = sorted(self.funcs)
        self.data_refs = {}
        self.calls = {}
        for x in self.r.relocs:
            v = self.r.symbol_value(x)
            site = x.offset & ~3
            if x.type == rpx.R_PPC_REL24:
                self.calls[site] = v
            elif x.type in (rpx.R_PPC_ADDR16_LO, rpx.R_PPC_ADDR16_HA, rpx.R_PPC_ADDR32):
                self.data_refs.setdefault(v, set()).add(site)
        lo = self.img.lo
        for i, w in enumerate(self.img.words):
            if (w >> 26) == 18 and (w & 3) == 1:
                site = lo + 4 * i
                if site not in self.calls:
                    li = w & 0x03FFFFFC
                    if li & 0x02000000:
                        li -= 0x04000000
                    self.calls[site] = (site + li) & 0xFFFFFFFF

    def func_of(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        s = self.starts[i]
        return s if addr < self.funcs[s] else None

    def name(self, addr):
        return self.imports.get(addr, f"{addr:08X}")

    def strings(self, pattern):
        rx = re.compile(pattern.encode())
        for s in self.r.sections:
            if s.name not in (".rodata", ".data"):
                continue
            d = bytes(s.data)
            for m in re.finditer(rb"[\x20-\x7e\x80-\xff]{3,}\x00", d):
                if rx.search(m.group(0)):
                    yield s.addr + m.start(), m.group(0)[:-1].decode("utf-8", "replace")

    def referencing_funcs(self, addr, span=1):
        out = set()
        for a in range(addr, addr + span):
            for site in self.data_refs.get(a, ()):
                f = self.func_of(site)
                if f is not None:
                    out.add(f)
        return sorted(out)

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, arg = sys.argv[1], sys.argv[2]
    x = Xref()
    if cmd == "str":
        for addr, s in x.strings(arg):
            fs = x.referencing_funcs(addr)
            print(f"{addr:08X} {s!r:60.60} <- {' '.join(f'{f:08X}' for f in fs) or '-'}")
    elif cmd == "refs":
        a = int(arg, 16)
        print(" ".join(f"{f:08X}" for f in x.referencing_funcs(a)))
    elif cmd == "callers":
        a = int(arg, 16)
        sites = sorted(s for s, t in x.calls.items() if t == a)
        for s in sites:
            print(f"{s:08X} in {x.func_of(s) or 0:08X}")
    elif cmd == "callees":
        f = x.func_of(int(arg, 16))
        for s in sorted(s for s in x.calls if f <= s < x.funcs[f]):
            print(f"{s:08X} -> {x.name(x.calls[s])}")
    elif cmd == "func":
        f = x.func_of(int(arg, 16))
        print(f"{f:08X}-{x.funcs[f]:08X}" if f is not None else "not in a function")
    elif cmd == "dis":
        import disasm
        d = disasm.Disasm(rpx_path())
        a = int(arg, 16)
        if len(sys.argv) > 3:
            start, end = a, a + 4 * int(sys.argv[3])
        else:
            start = x.func_of(a)
            end = x.funcs[start]
        for p in range(start, end, 4):
            print(d.line(p))
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()
