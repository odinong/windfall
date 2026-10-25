from dataclasses import dataclass, field
import struct
import zlib

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHF_RPL_ZLIB = 0x08000000

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_RPL_EXPORTS = 0x80000001
SHT_RPL_IMPORTS = 0x80000002
SHT_RPL_CRCS = 0x80000003
SHT_RPL_FILEINFO = 0x80000004

R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
R_PPC_REL14 = 11
R_PPC_DTPMOD32 = 68
R_PPC_DTPREL32 = 78
R_PPC_GHS_REL16_HA = 251
R_PPC_GHS_REL16_HI = 252
R_PPC_GHS_REL16_LO = 253

@dataclass
class Section:
    index: int
    name: str
    type: int
    flags: int
    addr: int
    size: int
    link: int
    info: int
    data: bytes

    @property
    def is_code(self):
        return self.type == SHT_PROGBITS and self.flags & SHF_EXECINSTR and not self.is_import

    @property
    def is_import(self):
        return self.type == SHT_RPL_IMPORTS

    def contains(self, addr):
        return self.addr <= addr < self.addr + self.size

@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int
    bind: int
    shndx: int

@dataclass
class Import:
    module: str
    name: str
    addr: int
    is_func: bool

@dataclass
class Reloc:
    offset: int
    type: int
    sym: int
    addend: int
    section: int

@dataclass
class Rpx:
    entry: int
    sections: list
    symbols: list
    imports: list
    relocs: list
    fileinfo: dict = field(default_factory=dict)

    def section_at(self, addr):
        for s in self.sections:
            if s.flags & SHF_ALLOC and s.contains(addr):
                return s
        return None

    @property
    def code_sections(self):
        return [s for s in self.sections if s.is_code]

    def symbol_value(self, r):
        s = self.symbols[r.sym]
        return (s.value + r.addend) & 0xFFFFFFFF

_FILEINFO_FIELDS = ["magic_version", "textSize", "textAlign", "dataSize", "dataAlign", "loadSize", "loadAlign",
                    "tempSize", "trampAdjust", "sdaBase", "sda2Base", "stackSize", "stringsOffset", "flags",
                    "heapSize", "tagOffset", "minVersion", "compressionLevel", "trampAddition", "fileInfoPad",
                    "cafeSdkVersion", "cafeSdkRevision"]

def load(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"\x7fELF" or raw[5] != 2 or raw[7] != 0xCA:
        raise ValueError(f"{path}: not a big-endian Cafe RPX/RPL")
    (_, machine, _, entry, _, shoff, _, _, _, _, shentsize, shnum, shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", raw, 16)
    if machine != 20:
        raise ValueError(f"{path}: e_machine {machine} is not PowerPC")

    hdrs = [struct.unpack_from(">10I", raw, shoff + i * shentsize) for i in range(shnum)]
    datas = []
    for (name, typ, flags, addr, off, size, *_rest) in hdrs:
        d = b""
        if typ != SHT_NOBITS and size:
            d = raw[off:off + size]
            if flags & SHF_RPL_ZLIB:
                expect = struct.unpack_from(">I", d, 0)[0]
                d = zlib.decompress(d[4:])
                assert len(d) == expect, "zlib size mismatch"
        datas.append(d)

    shstr = datas[shstrndx]
    cstr = lambda b, o: b[o:b.index(b"\0", o)].decode(errors="replace")
    sections = []
    for i, ((name, typ, flags, addr, off, size, link, info, _, _), d) in enumerate(zip(hdrs, datas)):
        msize = size if typ == SHT_NOBITS else len(d)
        sections.append(Section(i, cstr(shstr, name) if typ else "", typ, flags, addr, msize, link, info, d))

    symtab = next(s for s in sections if s.type == SHT_SYMTAB)
    strtab = sections[symtab.link].data
    symbols = []
    for o in range(0, len(symtab.data), 16):
        n, v, sz, inf, _, shndx = struct.unpack_from(">IIIBBH", symtab.data, o)
        symbols.append(Symbol(cstr(strtab, n), v, sz, inf & 0xF, inf >> 4, shndx))

    imports = []
    for s in symbols:
        if 0 < s.shndx < len(sections) and sections[s.shndx].is_import and s.name and not s.name.startswith("."):
            sec = sections[s.shndx]
            module = sec.name.split("_", 1)[1]
            if module.endswith(".rpl"):
                module = module[:-4]
            imports.append(Import(module, s.name, s.value, sec.name.startswith(".fimport")))

    relocs = []
    for s in sections:
        if s.type != SHT_RELA:
            continue
        for o in range(0, len(s.data), 12):
            off, info, add = struct.unpack_from(">IIi", s.data, o)
            relocs.append(Reloc(off, info & 0xFF, info >> 8, add, s.info))

    fileinfo = {}
    fi = next((s for s in sections if s.type == SHT_RPL_FILEINFO), None)
    if fi:
        fileinfo = dict(zip(_FILEINFO_FIELDS, struct.unpack_from(">" + "I" * len(_FILEINFO_FIELDS), fi.data, 0)))

    return Rpx(entry, sections, symbols, imports, relocs, fileinfo)
