"""Mod-making helpers for Windfall (works on the player's own dump; nothing here contains game data).

  python tools/modkit.py ls <content path> [inner archive path ...]
        list the files in an archive (nested: e.g. Common/Pack/szs_permanent1.pack sea_Room11.szs)
  python tools/modkit.py extract <content path> <inner path> [...] --out <file>
        write one file from inside (nested) archives
  python tools/modkit.py actors <content path> <inner path ...>
        list a room's actors (the room.dzr embedded in a room .bfres, or a .dzr/.dzs file)
  python tools/modkit.py make-examples [--mods <dir>]
        build the example mods into the mods folder (default %APPDATA%\\WWHDRecomp\\mods):
          "Example Title Text"  - nested archive patch: the title screen says "Windfall Mods OK"
          "Example Pot Ring"    - a custom map edit: Windfall Island's pots form a ring around the pier

The game folder comes from WF_GAME_DIR, else the one saved in Windfall's settings.
A mod's content/ mirrors the game's content/; a folder named like an archive patches files inside it.
"""
import argparse
import os
import re
import struct
import sys

APPDATA = os.environ.get("APPDATA", "")

def yaz0_decompress(d):
    if d[:4] != b"Yaz0":
        return d
    size = struct.unpack(">I", d[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = d[i]
        i += 1
        for b in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> b):
                out.append(d[i])
                i += 1
            else:
                b1, b2 = d[i], d[i + 1]
                i += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = d[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)

def sarc_files(d):
    d = yaz0_decompress(d)
    assert d[:4] == b"SARC", "not a SARC archive"
    e = ">" if d[6:8] == b"\xfe\xff" else "<"
    hdr = struct.unpack(e + "H", d[4:6])[0]
    data = struct.unpack(e + "I", d[0xC:0x10])[0]
    n = struct.unpack(e + "H", d[hdr + 6:hdr + 8])[0]
    nodes = [struct.unpack(e + "IIII", d[hdr + 12 + 16 * k:hdr + 28 + 16 * k]) for k in range(n)]
    names = hdr + 12 + 16 * n + 8
    out = {}
    for _, attr, s, t in nodes:
        o = names + (attr & 0xFFFF) * 4
        out[d[o:d.index(b"\0", o)].decode()] = d[data + s:data + t]
    return out

def game_dir():
    if os.environ.get("WF_GAME_DIR"):
        return os.environ["WF_GAME_DIR"]
    ini = os.path.join(APPDATA, "WWHDRecomp", "settings.ini")
    if os.path.exists(ini):
        for line in open(ini, encoding="utf-8"):
            if line.startswith("paths.game"):
                return line.split("=", 1)[1].strip()
    sys.exit("set WF_GAME_DIR to your dump (the folder with code, content, meta)")

def load(content_path, inner=()):
    data = open(os.path.join(game_dir(), "content", content_path), "rb").read()
    for name in inner:
        data = sarc_files(data)[name]
    return data

def find_dzr(blob):
    if blob[:4] != b"FRES":
        return 0
    i = blob.find(b"ACTR")
    for k in range(40):
        start = i - 4 - 12 * k
        cnt = struct.unpack(">I", blob[start:start + 4])[0]
        if 0 < cnt < 64 and k < cnt and all(re.fullmatch(rb"[A-Z0-9]{4}", blob[start + 4 + 12 * j:start + 8 + 12 * j]) for j in range(cnt)):
            return start
    raise SystemExit("no room data found")

def chunks(blob, base):
    cnt = struct.unpack(">I", blob[base:base + 4])[0]
    for j in range(cnt):
        tag, c, off = struct.unpack(">4sII", blob[base + 4 + 12 * j:base + 16 + 12 * j])
        yield tag.decode(), c, base + off

def actor_entries(blob, base, tag="ACTR", size=0x20):
    for t, c, off in chunks(blob, base):
        if t == tag:
            for e in range(c):
                o = off + e * size
                yield o, blob[o:o + 8].split(b"\0")[0].decode("latin1"), struct.unpack(">I", blob[o + 8:o + 12])[0], struct.unpack(">fff", blob[o + 12:o + 24])

def write_mod(mods, folder, ini, files):
    root = os.path.join(mods, folder)
    for rel, data in files.items():
        p = os.path.join(root, "content", *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    open(os.path.join(root, "mod.ini"), "w", encoding="utf-8").write(ini)
    print("wrote", root)

def make_examples(mods):
    msbt = bytearray(load("Common/Pack/permanent_2d_UsEnglish.pack", ["Title_00_msbt.szs", "Title_00.msbt"]))
    old, new = "Press Any Button".encode("utf-16-be"), "Windfall Mods OK".encode("utf-16-be")
    i = msbt.find(old)
    assert i >= 0 and len(old) == len(new)
    msbt[i:i + len(old)] = new
    write_mod(mods, "Example Title Text",
              "name = Example: Title Text\nauthor = Windfall\nversion = 1\ndescription = The title screen says \"Windfall Mods OK\" (a file patched inside a nested archive).\n",
              {"Common/Pack/permanent_2d_UsEnglish.pack/Title_00_msbt.szs/Title_00.msbt": bytes(msbt)})

    bfres = bytearray(load("Common/Pack/szs_permanent1.pack", ["sea_Room11.szs", "Room11.bfres"]))
    base = find_dzr(bfres)
    pots = [o for o, name, _, _ in actor_entries(bfres, base) if name == "kotubo"]
    import math
    cx, cy, cz = 669.0, 105.0, -197306.0 - 260.0
    for k, o in enumerate(pots):
        a = 2 * math.pi * k / len(pots)
        struct.pack_into(">fff", bfres, o + 12, cx + 230 * math.cos(a), cy, cz + 230 * math.sin(a))
    write_mod(mods, "Example Pot Ring",
              "name = Example: Pot Ring (custom map)\nauthor = Windfall\nversion = 1\ndescription = Windfall Island's pots stand in a ring at the pier (an edit of the room's actor list).\n",
              {"Common/Pack/szs_permanent1.pack/sea_Room11.szs/Room11.bfres": bytes(bfres)})
    print(f"moved {len(pots)} pots")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--mods", default=os.environ.get("WF_MODS_DIR", os.path.join(APPDATA, "WWHDRecomp", "mods")))
    a = ap.parse_args()
    if a.cmd == "ls":
        for name, d in sarc_files(load(a.args[0], a.args[1:])).items():
            print(f"{len(d):10}  {name}")
    elif a.cmd == "extract":
        open(a.out, "wb").write(load(a.args[0], a.args[1:]))
    elif a.cmd == "actors":
        blob = load(a.args[0], a.args[1:])
        base = find_dzr(blob)
        for tag in ("ACTR", "SCOB", "TRES"):
            for o, name, prm, (x, y, z) in actor_entries(blob, base, tag, 0x24 if tag == "SCOB" else 0x20):
                print(f"{tag} {name:8} {prm:08x} ({x:9.1f}, {y:8.1f}, {z:9.1f})")
    elif a.cmd == "make-examples":
        make_examples(a.mods)
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()
