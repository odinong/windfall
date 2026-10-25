import collections
import sys

import rpx

def main(path):
    r = rpx.load(path)
    print(f"entry 0x{r.entry:08x}")
    for s in r.sections:
        if s.type and s.flags & rpx.SHF_ALLOC:
            print(f"  {s.name:24} 0x{s.addr:08x} {s.size:9} {'code' if s.is_code else ''}")
    code = sum(s.size for s in r.code_sections)
    print(f"code: {code} bytes, {code // 4} instructions")
    for k in ("sdaBase", "sda2Base", "stackSize", "cafeSdkVersion"):
        print(f"  {k} = 0x{r.fileinfo.get(k, 0):08x}")
    mods = collections.Counter(i.module for i in r.imports)
    print(f"imports: {len(r.imports)} from {len(mods)} modules: " + ", ".join(f"{m}({n})" for m, n in sorted(mods.items())))
    print("relocs:", dict(sorted(collections.Counter(x.type for x in r.relocs).items())))

if __name__ == "__main__":
    main(sys.argv[1])
