import os
import struct
import sys

import analyze
import emit
import ppc

def D(op, rt, ra, imm): return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)
def X(xo, rt, ra, rb, rc=0, op=31): return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc
def XO(xo, rt, ra, rb, rc=0): return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc
def M(op, rs, ra, sh, mb, me, rc=0): return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc
def A(op, xo, d, a, b, c=0): return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1)
def XL(xo, bt, ba, bb): return (19 << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1)
def PSQ(op, fd, ra, w, i, d): return (op << 26) | (fd << 21) | (ra << 16) | (w << 15) | (i << 12) | (d & 0xFFF)
def BC(bo, bi, disp): return (16 << 26) | (bo << 21) | (bi << 16) | (disp & 0xFFFC)

addi = lambda d, a, v: D(14, d, a, v)
addic = lambda d, a, v: D(12, d, a, v)
subfic = lambda d, a, v: D(8, d, a, v)
mulli = lambda d, a, v: D(7, d, a, v)
cmpwi = lambda crf, a, v: D(11, crf << 2, a, v)
cmplwi = lambda crf, a, v: D(10, crf << 2, a, v)
andi_ = lambda a, s, v: D(28, s, a, v)
lhau = lambda d, a, v: D(43, d, a, v)
lfs = lambda d, a, v: D(48, d, a, v)
stfs = lambda s, a, v: D(52, s, a, v)
lfd = lambda d, a, v: D(50, d, a, v)
lmw = lambda d, a, v: D(46, d, a, v)
stmw = lambda s, a, v: D(47, s, a, v)

f2u = lambda f: struct.unpack(">I", struct.pack(">f", f))[0]
d2u = lambda d: struct.unpack(">Q", struct.pack(">d", d))[0]

CASES = [
    ("addic carry out", {"r3": 0xFFFFFFFF}, [addic(4, 3, 1)], {"r4": 0, "ca": 1}),
    ("addic -1 sets carry", {"r3": 5}, [addic(4, 3, -1)], {"r4": 4, "ca": 1}),
    ("subfic borrow", {"r3": 5}, [subfic(4, 3, 3)], {"r4": 0xFFFFFFFE, "ca": 0}),
    ("subfic no borrow", {"r3": 3}, [subfic(4, 3, 5)], {"r4": 2, "ca": 1}),
    ("64-bit add via addc/adde", {"r3": 1, "r4": 0xFFFFFFFF, "r5": 1, "r8": 0},
     [XO(10, 6, 4, 5), XO(138, 7, 3, 8)], {"r6": 0, "r7": 2, "ca": 0}),
    ("addze adds carry", {"r3": 7, "ca": 1}, [XO(202, 4, 3, 0)], {"r4": 8, "ca": 0}),
    ("subfze", {"r3": 0, "ca": 1}, [XO(200, 4, 3, 0)], {"r4": 0, "ca": 1}),
    ("srawi negative, ones shifted out", {"r3": 0xFFFFFFF5}, [X(824, 3, 4, 2)], {"r4": 0xFFFFFFFD, "ca": 1}),
    ("srawi negative, zeros shifted out", {"r3": 0xFFFFFFF4}, [X(824, 3, 4, 2)], {"r4": 0xFFFFFFFD, "ca": 0}),
    ("srawi positive", {"r3": 11}, [X(824, 3, 4, 2)], {"r4": 2, "ca": 0}),
    ("sraw shift >= 32", {"r3": 0x80000000, "r5": 40}, [X(792, 3, 4, 5)], {"r4": 0xFFFFFFFF, "ca": 1}),
    ("slw shift 32 gives 0", {"r3": 1, "r5": 32}, [X(24, 3, 4, 5)], {"r4": 0}),
    ("slw shift 31", {"r3": 1, "r5": 31}, [X(24, 3, 4, 5)], {"r4": 0x80000000}),
    ("srw shift 63 gives 0", {"r3": 0xFFFFFFFF, "r5": 63}, [X(536, 3, 4, 5)], {"r4": 0}),
    ("rlwinm wrap-around mask", {"r3": 0xFFFFFFFF}, [M(21, 3, 4, 0, 30, 1)], {"r4": 0xC0000003}),
    ("rlwinm rotate", {"r3": 0x12345678}, [M(21, 3, 4, 8, 0, 31)], {"r4": 0x34567812}),
    ("rlwimi insert", {"r3": 0xF, "r4": 0xAAAAAAAA}, [M(20, 3, 4, 4, 24, 27)], {"r4": 0xAAAAAAFA}),
    ("rlwnm", {"r3": 0x80000001, "r5": 33}, [M(23, 3, 4, 5, 0, 31)], {"r4": 0x00000003}),
    ("cntlzw 0", {"r3": 0}, [X(26, 3, 4, 0)], {"r4": 32}),
    ("cntlzw 1", {"r3": 1}, [X(26, 3, 4, 0)], {"r4": 31}),
    ("divw INT_MIN/-1", {"r3": 0x80000000, "r4": 0xFFFFFFFF}, [XO(491, 5, 3, 4)], {"r5": 0xFFFFFFFF}),
    ("divw by zero, negative", {"r3": 0xFFFFFFF0, "r4": 0}, [XO(491, 5, 3, 4)], {"r5": 0xFFFFFFFF}),
    ("divw by zero, positive", {"r3": 16, "r4": 0}, [XO(491, 5, 3, 4)], {"r5": 0}),
    ("divw truncates toward zero", {"r3": 0xFFFFFFF9, "r4": 2}, [XO(491, 5, 3, 4)], {"r5": 0xFFFFFFFD}),
    ("divwu by zero", {"r3": 16, "r4": 0}, [XO(459, 5, 3, 4)], {"r5": 0}),
    ("mulhw signed", {"r3": 0xFFFFFFFE, "r4": 3}, [XO(75, 5, 3, 4)], {"r5": 0xFFFFFFFF}),
    ("mulhwu unsigned", {"r3": 0xFFFFFFFE, "r4": 3}, [XO(11, 5, 3, 4)], {"r5": 2}),
    ("mulli negative", {"r3": 7}, [mulli(4, 3, -3)], {"r4": 0xFFFFFFEB}),
    ("neg INT_MIN", {"r3": 0x80000000}, [XO(104, 4, 3, 0)], {"r4": 0x80000000}),
    ("extsb", {"r3": 0x1280}, [X(954, 3, 4, 0)], {"r4": 0xFFFFFF80}),
    ("cmpw signed vs cmplw unsigned", {"r3": 0xFFFFFFFF, "r4": 1},
     [X(0, 0, 3, 4), X(32, 1 << 2, 3, 4)], {"cr0": 1, "cr1": 0, "cr2": 0, "cr4": 0, "cr5": 1, "cr6": 0}),
    ("cmplwi", {"r3": 0x8000}, [cmplwi(7, 3, 0x7FFF)], {"cr28": 0, "cr29": 1, "cr30": 0}),
    ("or. sets cr0.eq", {"r3": 0}, [X(444, 3, 4, 3, rc=1)], {"cr0": 0, "cr1": 0, "cr2": 1}),
    ("andi. always records", {"r3": 0xF0}, [andi_(4, 3, 0x0F)], {"r4": 0, "cr2": 1}),
    ("crxor clears", {"cr6": 1}, [XL(193, 6, 6, 6)], {"cr6": 0}),
    ("creqv sets", {"cr6": 0}, [XL(289, 6, 6, 6)], {"cr6": 1}),
    ("mfcr packs bits", {"cr0": 1, "cr2": 1, "cr31": 1}, [X(19, 5, 0, 0)], {"r5": 0xA0000001}),
    ("lswi 7 bytes", {"r3": 0x100, "mem8@0x100": list(b"ABCDEFG")}, [X(597, 5, 3, 7)],
     {"r5": 0x41424344, "r6": 0x45464700}),
    ("stswi 6 bytes", {"r3": 0x140, "r5": 0x31323334, "r6": 0x35363738}, [X(725, 5, 3, 6)],
     {"mem8@0x140": list(b"123456"), "mem8@0x146": [0]}),
    ("lmw/stmw", {"r3": 0x180, "r29": 1, "r30": 2, "r31": 3},
     [stmw(29, 3, 0), addi(29, 0, 0), addi(30, 0, 0), addi(31, 0, 0), lmw(29, 3, 4)],
     {"r29": 2, "r30": 3, "r31": 0}),
    ("lhau sign-extends and updates", {"r3": 0x1FE, "mem16@0x200": 0x8001}, [lhau(4, 3, 2)], {"r4": 0xFFFF8001, "r3": 0x200}),
    ("lwbrx", {"r3": 0x220, "mem32@0x220": 0x11223344}, [X(534, 4, 0, 3)], {"r4": 0x44332211}),
    ("stwcx. after lwarx succeeds", {"r3": 0x240, "r5": 99, "mem32@0x240": 7},
     [X(20, 4, 0, 3), X(150, 5, 0, 3, rc=1)], {"r4": 7, "cr2": 1, "mem32@0x240": 99}),
    ("stwcx. to another address fails", {"r3": 0x240, "r6": 0x244, "r5": 99, "mem32@0x244": 5},
     [X(20, 4, 0, 3), X(150, 5, 0, 6, rc=1)], {"cr2": 0, "mem32@0x244": 5}),
    ("bdnz loop", {"r3": 0, "ctr": 5}, [addi(3, 3, 1), BC(16, 0, -4)], {"r3": 5, "ctr": 0}),
    ("lfs/stfs keep signalling NaN bits", {"r3": 0x260, "mem32@0x260": 0x7F800001},
     [lfs(1, 3, 0), stfs(1, 3, 4)], {"mem32@0x264": 0x7F800001}),
    ("lfs fills both slots", {"r3": 0x260, "mem32@0x260": f2u(1.5)}, [lfs(1, 3, 0)], {"f1.ps0": 1.5, "f1.ps1": 1.5}),
    ("frsp rounds to single, copies to ps1", {"f2.ps0": 1.0 / 3.0}, [X(12, 1, 0, 2, op=63)],
     {"f1.ps0": struct.unpack(">f", struct.pack(">f", 1.0 / 3.0))[0], "f1.ps1": struct.unpack(">f", struct.pack(">f", 1.0 / 3.0))[0]}),
    ("fctiwz -2.7", {"f2.ps0": -2.7}, [X(15, 1, 0, 2, op=63)], {"f1.ps0": ("bits", 0xFFF80000FFFFFFFE)}),
    ("fctiwz saturates", {"f2.ps0": 3e9}, [X(15, 1, 0, 2, op=63)], {"f1.ps0": ("bits", 0xFFF800007FFFFFFF)}),
    ("stfiwx stores low word", {"r3": 0x280, "f1.ps0": ("bits", 0xFFF80000FFFFFFFE)}, [X(983, 1, 0, 3)],
     {"mem32@0x280": 0xFFFFFFFE}),
    ("fsel -0.0 picks frC", {"f2.ps0": -0.0, "f3.ps0": 7.0, "f4.ps0": 9.0}, [A(63, 23, 1, 2, 4, 3)], {"f1.ps0": 7.0}),
    ("fsel NaN picks frB", {"f2.ps0": float("nan"), "f3.ps0": 7.0, "f4.ps0": 9.0}, [A(63, 23, 1, 2, 4, 3)], {"f1.ps0": 9.0}),
    ("fmadd is fused", {"f2.ps0": 1 + 2.0 ** -30, "f3.ps0": 1 - 2.0 ** -30, "f4.ps0": -1.0},
     [A(63, 29, 1, 2, 4, 3)], {"f1.ps0": -(2.0 ** -60)}),
    ("fmadds", {"f2.ps0": 2.0, "f3.ps0": 3.0, "f4.ps0": 1.0}, [A(59, 29, 1, 2, 4, 3)], {"f1.ps0": 7.0, "f1.ps1": 7.0}),
    ("fneg only touches ps0 sign", {"f2.ps0": 2.0, "f1.ps1": 5.0}, [X(40, 1, 0, 2, op=63)], {"f1.ps0": -2.0, "f1.ps1": 5.0}),
    ("fcmpu NaN is unordered", {"f2.ps0": float("nan"), "f3.ps0": 1.0}, [X(0, 1 << 2, 2, 3, op=63)],
     {"cr4": 0, "cr5": 0, "cr6": 0, "cr7": 1}),
    ("psq_l u8 scale 2", {"r3": 0x2A0, "gqr2": (2 << 24) | (4 << 16), "mem8@0x2A0": [8, 12]},
     [PSQ(56, 1, 3, 0, 2, 0)], {"f1.ps0": 2.0, "f1.ps1": 3.0}),
    ("psq_l w=1 float sets ps1=1", {"r3": 0x2C0, "gqr0": 0, "mem32@0x2C0": f2u(4.5)},
     [PSQ(56, 1, 3, 1, 0, 0)], {"f1.ps0": 4.5, "f1.ps1": 1.0}),
    ("psq_st s16 scale 1", {"r3": 0x2E0, "gqr3": (1 << 8) | 7, "f1.ps0": 1.5, "f1.ps1": -3.25},
     [PSQ(60, 1, 3, 0, 3, 0)], {"mem16@0x2E0": 3, "mem16@0x2E2": 0xFFFA}),
    ("psq_st u8 clamps", {"r3": 0x300, "gqr3": 4, "f1.ps0": 300.0, "f1.ps1": -5.0},
     [PSQ(60, 1, 3, 0, 3, 0)], {"mem8@0x300": [255, 0]}),
    ("ps_sum0", {"f2.ps0": 1.0, "f2.ps1": 2.0, "f3.ps0": 10.0, "f3.ps1": 20.0, "f4.ps0": 100.0, "f4.ps1": 200.0},
     [A(4, 10, 1, 2, 3, 4)], {"f1.ps0": 21.0, "f1.ps1": 200.0}),
    ("ps_merge10 aliasing frD=frA", {"f1.ps0": 1.0, "f1.ps1": 2.0, "f3.ps0": 3.0, "f3.ps1": 4.0},
     [X(592, 1, 1, 3, op=4)], {"f1.ps0": 2.0, "f1.ps1": 3.0}),
    ("ps_madds0", {"f2.ps0": 1.0, "f2.ps1": 2.0, "f3.ps0": 3.0, "f3.ps1": 4.0, "f4.ps0": 10.0, "f4.ps1": 20.0},
     [A(4, 14, 1, 2, 4, 3)], {"f1.ps0": 13.0, "f1.ps1": 26.0}),
    ("ps_neg", {"f2.ps0": 1.0, "f2.ps1": -2.0}, [X(40, 1, 0, 2, op=4)], {"f1.ps0": -1.0, "f1.ps1": 2.0}),
]

class FakeImg:
    def __init__(self, words, base):
        self.base = base
        self.words = words
        self.switch_tables = {}

    def insn(self, a):
        i = ppc.decode(self.words[(a - self.base) // 4], a)
        if i is None:
            raise ValueError(f"test word {self.words[(a - self.base) // 4]:08x} does not decode")
        return i

    def branch_target(self, i):
        return i.target

class FakeCtx:
    def __init__(self, img):
        self.img = img

    def func_name(self, a):
        return None

    def import_slot(self, a):
        return None

def cval(v):
    if isinstance(v, tuple):
        return f"0x{v[1]:X}ull"
    if v != v:
        return "NAN"
    return repr(float(v))

def build(out):
    lines = ['// Generated by tools/recomp/test_semantics.py', '#include <cmath>', '#include <cstdio>', '#include <cstdlib>',
             '#include <vector>', '#include "wf/ppc.h"', '',
             'namespace wf { PPCFunc** g_funcTable;',
             'void bad_indirect_call(PPCContext&, uint32_t, uint32_t) { std::abort(); }',
             'void yield(PPCContext&, uint8_t*) {}', 'void trap(PPCContext&, uint8_t*, uint32_t) { std::abort(); }',
             'void call_import(PPCContext&, uint8_t*, uint32_t) { std::abort(); }',
             'void indirect_miss(PPCContext&, uint8_t*, uint32_t, uint32_t) { std::abort(); }',
             'namespace interp { bool g_active = false; bool entry(PPCContext&, uint8_t*, uint32_t) { return false; } } }', '',
             'static int g_fail = 0;',
             'static void check(const char* t, const char* what, uint64_t got, uint64_t want) {',
             '\tif (got != want) { std::printf("FAIL %s: %s = %llX, want %llX\\n", t, what, (unsigned long long)got, (unsigned long long)want); g_fail++; } }',
             'static void checkf(const char* t, const char* what, double got, double want) {',
             '\tif (!(got == want || (std::isnan(got) && std::isnan(want)))) { std::printf("FAIL %s: %s = %.17g, want %.17g\\n", t, what, got, want); g_fail++; } }',
             '']
    runners = []
    for n, (name, setup, words, expect) in enumerate(CASES):
        base = 0x02000000 + n * 0x100
        img = FakeImg(words, base)
        f = analyze.Function(base)
        f.insns = [base + 4 * k for k in range(len(words))]
        for a in f.insns:
            i = img.insn(a)
            if i.op in ("b", "bc") and not i.lk:
                f.labels.add(i.target)
        em = emit.FunctionEmitter(FakeCtx(img))
        lines += em.function(f, f"test_{n}")
        lines.append("")

        body = ["\tstd::vector<uint8_t> mem(0x10000); uint8_t* base = mem.data();",
                "\twf::PPCContext ctx{}; ctx.yield_request = 0;"]
        def setv(k, v):
            if k.startswith("mem"):
                kind, addr = k.split("@")
                addr = int(addr, 16)
                if kind == "mem8":
                    for o, b in enumerate(v):
                        body.append(f"\twf::st8(base, {addr + o}, {b});")
                else:
                    body.append(f"\twf::st{kind[3:]}(base, {addr}, 0x{v:X}u);")
            elif k.startswith("f"):
                reg, slot = k.split(".")
                if isinstance(v, tuple):
                    body.append(f"\tctx.f[{reg[1:]}].{slot}u = 0x{v[1]:X}ull;")
                else:
                    body.append(f"\tctx.f[{reg[1:]}].{slot} = {cval(v)};")
            elif k.startswith("r"):
                body.append(f"\tctx.r[{k[1:]}] = 0x{v:X}u;")
            elif k == "ca":
                body.append(f"\tctx.xer_ca = {v};")
            elif k == "ctr":
                body.append(f"\tctx.ctr = {v};")
            elif k.startswith("gqr"):
                body.append(f"\tctx.gqr[{k[3:]}] = 0x{v:X}u;")
            elif k.startswith("cr"):
                body.append(f"\tctx.cr[{k[2:]}] = {v};")
        for k, v in setup.items():
            setv(k, v)
        body.append(f"\ttest_{n}(ctx, base);")
        t = name.replace('"', "'")
        for k, v in expect.items():
            if k.startswith("mem"):
                kind, addr = k.split("@")
                addr = int(addr, 16)
                if kind == "mem8":
                    for o, b in enumerate(v):
                        body.append(f'\tcheck("{t}", "{kind}@{addr + o:#x}", wf::ld8(base, {addr + o}), {b});')
                else:
                    body.append(f'\tcheck("{t}", "{k}", wf::ld{kind[3:]}(base, {addr}), 0x{v:X}u);')
            elif k.startswith("f"):
                reg, slot = k.split(".")
                if isinstance(v, tuple):
                    body.append(f'\tcheck("{t}", "{k}", ctx.f[{reg[1:]}].{slot}u, 0x{v[1]:X}ull);')
                else:
                    body.append(f'\tcheckf("{t}", "{k}", ctx.f[{reg[1:]}].{slot}, {cval(v)});')
            elif k.startswith("r"):
                body.append(f'\tcheck("{t}", "{k}", ctx.r[{k[1:]}], 0x{v:X}u);')
            elif k == "ca":
                body.append(f'\tcheck("{t}", "ca", ctx.xer_ca, {v});')
            elif k == "ctr":
                body.append(f'\tcheck("{t}", "ctr", ctx.ctr, {v});')
            elif k.startswith("cr"):
                body.append(f'\tcheck("{t}", "{k}", ctx.cr[{k[2:]}], {v});')
        lines.append(f"static void run_{n}() {{")
        lines += body
        lines.append("}")
        runners.append(f"run_{n}();")
    lines.append("int main() {")
    lines += ["\t" + r for r in runners]
    lines.append(f'\tstd::printf("%d of {len(CASES)} cases failed\\n", g_fail ? g_fail : 0);')
    lines.append("\treturn g_fail ? 1 : 0;")
    lines.append("}")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"wrote {len(CASES)} cases to {out}")

if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build(sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "generated", "cputest.cpp"))
