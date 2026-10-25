from array import array
from bisect import bisect_right
from dataclasses import dataclass, field
import sys

import ppc
import rpx

@dataclass
class Function:
    start: int
    insns: list = field(default_factory=list)
    labels: set = field(default_factory=set)
    fallthrough: int = 0
    tail_targets: set = field(default_factory=set)

class Image:
    def __init__(self, r: rpx.Rpx):
        self.r = r
        self.text = next(s for s in r.sections if s.name == ".text")
        self.lo, self.hi = self.text.addr, self.text.addr + self.text.size
        w = array("I", self.text.data)
        if sys.byteorder == "little":
            w.byteswap()
        self.words = w
        self._dec = {}
        self.imports = {i.addr: i for i in r.imports}
        self.rel24 = {}
        self.code_ptrs = set()
        code_hi_lo = {}
        for x in r.relocs:
            v = r.symbol_value(x)
            if x.type == rpx.R_PPC_REL24:
                self.rel24[x.offset] = v
            elif x.type == rpx.R_PPC_ADDR32 and self.in_text(v):
                self.code_ptrs.add(v)
            elif x.type in (rpx.R_PPC_ADDR16_LO, rpx.R_PPC_ADDR16_HA) and self.in_text(v) and self.in_text(x.offset & ~3):
                code_hi_lo.setdefault(v, []).append(x.offset & ~3)
        self.switch_tables = self.find_switch_tables(code_hi_lo)
        table_bases = {slots[0] for slots in self.switch_tables.values()}
        self.code_ptrs |= set(code_hi_lo) - table_bases

    def find_switch_tables(self, code_hi_lo):
        tables = {}
        for tbl, refs in code_hi_lo.items():
            bctr = tbl - 4
            if not self.in_text(bctr) or self.word(bctr) != 0x4E800420:
                continue
            if not all(bctr - 40 <= ref < bctr for ref in refs):
                continue
            count = None
            for k in range(1, 16):
                cmp_at = bctr - 4 * k
                w = self.word(cmp_at)
                if (w >> 26) == 10:
                    n = w & 0xFFFF
                    for b_at in range(cmp_at + 4, bctr, 4):
                        bi = self.insn(b_at)
                        if bi and bi.op == "bc":
                            if bi.bi % 4 == 1 and bi.bo & 8:
                                count = n + 1
                            elif bi.bi % 4 == 0 and not bi.bo & 8:
                                count = n
                            break
                    break
            slots = []
            a = tbl
            while self.in_text(a) and (self.word(a) >> 26) == 18 and not (self.word(a) & 3) and (count is None or len(slots) < count):
                slots.append(a)
                a += 4
            if count is not None and len(slots) != count:
                raise ValueError(f"switch at {bctr:08x}: bound says {count} cases, found {len(slots)} branch slots")
            if slots:
                tables[bctr] = slots
        return tables

    def in_text(self, a):
        return self.lo <= a < self.hi

    def word(self, a):
        return self.words[(a - self.lo) >> 2]

    def insn(self, a):
        i = self._dec.get(a)
        if i is None and a not in self._dec:
            i = ppc.decode(self.word(a), a)
            self._dec[a] = i
        return i

    def branch_target(self, i):
        if i.op == "b" and i.addr in self.rel24:
            return self.rel24[i.addr]
        return i.target

def discover(img: Image, extra_seeds=(), log=print):
    seeds = {img.r.entry} | set(extra_seeds)
    for a in range(img.lo, img.hi, 4):
        w = img.word(a)
        if (w >> 26) == 18 and (w & 1):
            i = img.insn(a)
            t = img.branch_target(i)
            if img.in_text(t):
                seeds.add(t)
    seeds |= img.code_ptrs
    seeds = {s for s in seeds if img.in_text(s) and img.insn(s) is not None}
    log(f"seeds: {len(seeds)} (code pointers {len(img.code_ptrs)})")

    log(f"switch tables: {len(img.switch_tables)}")
    strong = set(seeds)
    starts = set(seeds)
    rounds = 0
    while True:
        rounds += 1
        funcs, found, bad = trace_all(img, sorted(starts), strong)
        starts -= bad
        pending = found - starts
        if pending:
            log(f"round {rounds}: {len(funcs)} functions, {len(pending)} new tail-call targets")
            starts |= pending
            continue
        gaps = sweep_gaps(img, funcs) - starts
        if not gaps:
            break
        log(f"round {rounds}: {len(funcs)} functions, {len(gaps)} unreached code regions swept")
        starts |= gaps
    if bad:
        log(f"dropped {len(bad)} swept regions that run into invalid instructions (data)")
    return funcs

def trace_all(img, starts, strong):
    funcs, found, bad = {}, set(), set()
    for idx, s in enumerate(starts):
        end = starts[idx + 1] if idx + 1 < len(starts) else img.hi
        f, tails = trace(img, s, end)
        if f is None:
            if s in strong:
                raise ValueError(f"seed {s:08x} runs into an invalid instruction")
            bad.add(s)
            continue
        funcs[s] = f
        found |= tails
    return funcs, found, bad

def sweep_gaps(img, funcs):
    covered = bytearray((img.hi - img.lo) >> 2)
    for f in funcs.values():
        for a in f.insns:
            covered[(a - img.lo) >> 2] = 1
    gaps = set()
    a = img.lo
    while a < img.hi:
        if covered[(a - img.lo) >> 2]:
            a += 4
            continue
        w = img.word(a)
        if w == 0 or w == 0x60000000 or img.insn(a) is None:
            a += 4
            continue
        gaps.add(a)
        while a < img.hi and not covered[(a - img.lo) >> 2]:
            a += 4
    return gaps

def trace(img: Image, start, end):
    f = Function(start)
    seen = set()
    work = [start]
    found = set()
    ok = True
    while work:
        a = work.pop()
        while start <= a < end and a not in seen:
            i = img.insn(a)
            if i is None:
                ok = False
                break
            seen.add(a)
            if i.op == "b" and not i.lk:
                t = img.branch_target(i)
                if start <= t < end:
                    f.labels.add(t)
                    work.append(t)
                else:
                    f.tail_targets.add(t)
                    if img.in_text(t):
                        found.add(t)
                break
            if i.op == "bcctr" and a in img.switch_tables:
                for slot in img.switch_tables[a]:
                    if not (start <= slot < end):
                        return None, set()
                    f.labels.add(slot)
                    work.append(slot)
                break
            if i.op == "bc" and not i.lk:
                t = img.branch_target(i)
                if start <= t < end:
                    f.labels.add(t)
                    work.append(t)
                else:
                    f.tail_targets.add(t)
                    if img.in_text(t):
                        found.add(t)
            if ppc.is_terminator(i):
                break
            a += 4
        else:
            if a == end and a not in seen:
                f.fallthrough = end
    if not ok:
        return None, set()
    f.insns = sorted(seen)
    return f, found
