from dataclasses import dataclass

import rpx

FUNC_THUNK_BASE = 0x02910000
DATA_IMPORT_BASE = 0x02920000
DATA_IMPORT_SLOT = 0x800
CODE_END = 0x02930000

@dataclass
class ImportSlot:
    id: int
    module: str
    name: str
    is_func: bool
    guest: int

def assign_imports(r: rpx.Rpx):
    slots = []
    by_addr = {}
    nf = nd = 0
    for imp in sorted(r.imports, key=lambda i: (not i.is_func, i.module, i.name)):
        if imp.is_func:
            guest = FUNC_THUNK_BASE + 8 * nf
            nf += 1
        else:
            guest = DATA_IMPORT_BASE + DATA_IMPORT_SLOT * nd
            nd += 1
        s = ImportSlot(len(slots), imp.module, imp.name, imp.is_func, guest)
        slots.append(s)
        by_addr[imp.addr] = s
    assert FUNC_THUNK_BASE + 8 * nf <= DATA_IMPORT_BASE and DATA_IMPORT_BASE + DATA_IMPORT_SLOT * nd <= CODE_END
    return slots, by_addr

def symbol_address(r: rpx.Rpx, imports_by_addr, reloc):
    sym = r.symbols[reloc.sym]
    base = sym.value
    if base in imports_by_addr:
        base = imports_by_addr[base].guest
    return (base + reloc.addend) & 0xFFFFFFFF

def relocate_words(r: rpx.Rpx, section, words, imports_by_addr):
    mismatches = 0
    for x in r.relocs:
        if x.section != section.index or x.type == rpx.R_PPC_REL24:
            continue
        v = symbol_address(r, imports_by_addr, x)
        is_import = r.symbols[x.sym].value in imports_by_addr
        idx = (x.offset - section.addr) >> 2
        old = words[idx]
        if x.type == rpx.R_PPC_ADDR32:
            assert x.offset & 3 == 0
            new = v
        elif x.type in (rpx.R_PPC_ADDR16_LO, rpx.R_PPC_ADDR16_HI, rpx.R_PPC_ADDR16_HA):
            half = {rpx.R_PPC_ADDR16_LO: v & 0xFFFF, rpx.R_PPC_ADDR16_HI: v >> 16,
                    rpx.R_PPC_ADDR16_HA: ((v + 0x8000) >> 16) & 0xFFFF}[x.type]
            if x.offset & 2:
                new = (old & 0xFFFF0000) | half
            else:
                new = (old & 0x0000FFFF) | (half << 16)
        else:
            raise NotImplementedError(f"relocation type {x.type} at {x.offset:08x}")
        if new != old and not is_import:
            mismatches += 1
        words[idx] = new
    return mismatches
