import sys

import capstone

import rpx

RELOC_NAMES = {1: "ADDR32", 4: "@l", 5: "@h", 6: "@ha", 10: "REL24"}

class Disasm:
    def __init__(self, path):
        self.r = rpx.load(path)
        self.text = next(s for s in self.r.sections if s.name == ".text")
        self.cs = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
        self.rel_at = {}
        for x in self.r.relocs:
            self.rel_at.setdefault(x.offset & ~3, []).append(x)
        self.imports = {i.addr: f"{i.module}::{i.name}" for i in self.r.imports}

    def word(self, addr):
        s = self.r.section_at(addr)
        o = addr - s.addr
        return int.from_bytes(s.data[o:o + 4], "big")

    def line(self, addr):
        o = addr - self.text.addr
        raw = self.text.data[o:o + 4]
        ins = next(self.cs.disasm(raw, addr), None)
        txt = f"{ins.mnemonic:8} {ins.op_str}" if ins else f".word 0x{raw.hex()}"
        notes = []
        for x in self.rel_at.get(addr, []):
            tgt = self.r.symbol_value(x)
            notes.append(f"{RELOC_NAMES.get(x.type, x.type)} -> {self.imports.get(tgt, hex(tgt))}")
        return f"{addr:08x}: {raw.hex()}  {txt}" + (f"    ; {', '.join(notes)}" if notes else "")

    def show(self, addr, count):
        for a in range(addr, addr + 4 * count, 4):
            print(self.line(a))

def main():
    d = Disasm(sys.argv[1])
    if sys.argv[2] == "--bctr":
        n = int(sys.argv[3])
        found = 0
        for a in range(d.text.addr, d.text.addr + d.text.size, 4):
            if d.word(a) == 0x4E800420:
                print(f"--- bctr #{found} at {a:08x}")
                d.show(a - 4 * 10, 12)
                found += 1
                if found == n:
                    break
    else:
        d.show(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 32)

if __name__ == "__main__":
    main()
