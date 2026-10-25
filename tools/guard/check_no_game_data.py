"""Refuse to commit or package anything that is (or is derived from) Nintendo game data.

Usage:
  check_no_game_data.py --staged          check files staged in git (pre-commit hook)
  check_no_game_data.py --dir <folder>    check every file in a folder (packaging)
  check_no_game_data.py <file> ...        check specific files

Exits 1 and lists offenders if anything looks like game data.
"""
import argparse, hashlib, os, subprocess, sys

BLOCKED_EXT = {
    ".rpx", ".rpl", ".app", ".h3", ".tik", ".tmd", ".cert", ".wud", ".wux", ".iso",
    ".szs", ".sarc", ".pack", ".bfstm", ".bfsar", ".bfwav", ".aw", ".gsh", ".sharcfb",
    ".bfres", ".bflyt", ".bflim", ".bdt", ".baglenv", ".asn", ".aaf", ".bfma", ".btsnd",
    ".h264", ".arc", ".rarc", ".yaz0",
}
BLOCKED_DIRS = {"generated", "extracted", "game", "content", "meta", "code", "shadercache"}
MAGICS = [b"Yaz0", b"SARC", b"FSTM", b"FSAR", b"FWAV", b"FRES", b"FLYT", b"FLIM", b"Gfx2", b"WUX0", b"RARC"]
KNOWN_SHA1 = {
    "bdcbc4f815ae4c9b0a1f4a16238f96c197b38239",
    "1e9c969e672956cc68246b3db0a2f4966c19f569",
}
BLOCKED_NAMES = {"windfall_game.dll", "windfall_game.lib", "windfall_game.pdb", "recomp_table.cpp", "recomp_imports.cpp", "recomp_module.cpp"}
MAX_BYTES = 64 * 1024 * 1024

def problems(path, rel, allow_big=()):
    out = []
    parts = {p.lower() for p in rel.replace("\\", "/").split("/")[:-1]}
    if parts & BLOCKED_DIRS:
        out.append(f"inside a blocked directory ({', '.join(sorted(parts & BLOCKED_DIRS))})")
    name = os.path.basename(rel).lower()
    if name in BLOCKED_NAMES or (name.startswith("recomp_") and name[7:10].isdigit()):
        out.append("recompiled game code")
    ext = os.path.splitext(rel)[1].lower()
    if ext in BLOCKED_EXT:
        out.append(f"blocked extension {ext}")
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError:
        return out
    if any(head.startswith(m) for m in MAGICS):
        out.append(f"game file magic {head[:4]!r}")
    if head[:4] == b"\x7fELF" and len(head) > 7 and head[7] == 0xCA:
        out.append("Wii U RPX/RPL executable")
    if size > MAX_BYTES and os.path.basename(rel).lower() not in allow_big:
        out.append(f"suspiciously large ({size // (1024 * 1024)} MiB)")
    if size <= MAX_BYTES * 4:
        h = hashlib.sha1()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if h.hexdigest() in KNOWN_SHA1:
            out.append("matches a known game file hash")
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--staged", action="store_true")
    ap.add_argument("--dir")
    ap.add_argument("--allow-big", action="append", default=[], help="file name allowed to exceed the size limit")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()

    items = []
    if a.staged:
        names = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
                               capture_output=True, check=True).stdout.decode().split("\0")
        items += [(n, n) for n in names if n]
    if a.dir:
        for root, dirs, files in os.walk(a.dir):
            dirs[:] = [d for d in dirs if d != ".git"]
            for fn in files:
                p = os.path.join(root, fn)
                items.append((p, os.path.relpath(p, a.dir)))
    items += [(f, f) for f in a.files]

    bad = 0
    allow = {n.lower() for n in a.allow_big}
    for path, rel in items:
        for why in problems(path, rel, allow):
            print(f"GAME DATA GUARD: {rel}: {why}", file=sys.stderr)
            bad += 1
    if bad:
        print("Refusing: game files (or files derived from them) must never be committed or packaged.", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
