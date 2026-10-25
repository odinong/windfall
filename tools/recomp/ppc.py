from dataclasses import dataclass

PRIMARY = {
    3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu",
}

OP19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
        225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}

OP31 = {
    0: "cmp", 4: "tw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr", 20: "lwarx", 23: "lwzx",
    24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl", 40: "subf", 54: "dcbst", 55: "lwzux",
    60: "andc", 75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 104: "neg", 119: "lbzux",
    124: "nor", 136: "subfe", 138: "adde", 144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx",
    183: "stwux", 200: "subfze", 202: "addze", 215: "stbx", 232: "subfme", 234: "addme",
    235: "mullw", 246: "dcbtst", 247: "stbux", 266: "add", 278: "dcbt", 279: "lhzx", 284: "eqv",
    311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 371: "mftb", 375: "lhaux", 407: "sthx",
    412: "orc", 439: "sthux", 444: "or", 459: "divwu", 467: "mtspr", 470: "dcbi", 476: "nand",
    491: "divw", 512: "mcrxr", 533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw", 567: "lfsux",
    597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux", 661: "stswx", 662: "stwbrx", 663: "stfsx",
    695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx", 792: "sraw",
    824: "srawi", 854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi",
    983: "stfiwx", 1014: "dcbz",
}
OP31_OE = {8: "subfc", 10: "addc", 40: "subf", 104: "neg", 136: "subfe", 138: "adde",
           200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw", 266: "add",
           459: "divwu", 491: "divw"}

OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
        28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
          28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
OP63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
          64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs",
          583: "mffs", 711: "mtfsf"}

OP4_A = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
         15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res",
         25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
OP4_X = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1",
         136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
         624: "ps_merge11", 1014: "dcbz_l"}
OP4_Q = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}

def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)

@dataclass
class Insn:
    addr: int
    word: int
    op: str

    @property
    def rd(self): return (self.word >> 21) & 31
    @property
    def ra(self): return (self.word >> 16) & 31
    @property
    def rb(self): return (self.word >> 11) & 31
    @property
    def rc_reg(self): return (self.word >> 6) & 31
    @property
    def me(self): return (self.word >> 1) & 31
    @property
    def rc(self): return self.word & 1
    @property
    def lk(self): return self.word & 1
    @property
    def aa(self): return (self.word >> 1) & 1
    @property
    def simm(self): return sext(self.word & 0xFFFF, 16)
    @property
    def uimm(self): return self.word & 0xFFFF
    @property
    def crfd(self): return (self.word >> 23) & 7
    @property
    def crfs(self): return (self.word >> 18) & 7
    @property
    def bo(self): return (self.word >> 21) & 31
    @property
    def bi(self): return (self.word >> 16) & 31
    @property
    def spr(self): return ((self.word >> 16) & 31) | (((self.word >> 11) & 31) << 5)
    @property
    def crm(self): return (self.word >> 12) & 0xFF
    @property
    def nb(self): return (self.word >> 11) & 31
    @property
    def psq_w(self): return (self.word >> 15) & 1 if self.op in ("psq_l", "psq_lu", "psq_st", "psq_stu") else (self.word >> 10) & 1
    @property
    def psq_i(self): return (self.word >> 12) & 7 if self.op in ("psq_l", "psq_lu", "psq_st", "psq_stu") else (self.word >> 7) & 7
    @property
    def psq_d(self): return sext(self.word & 0xFFF, 12)

    @property
    def target(self):
        if self.op == "b":
            d = sext(self.word & 0x03FFFFFC, 26)
        else:
            d = sext(self.word & 0xFFFC, 16)
        return d & 0xFFFFFFFF if self.aa else (self.addr + d) & 0xFFFFFFFF

    def __str__(self):
        return f"{self.addr:08x}: {self.word:08x} {self.op}"

def decode(word, addr=0):
    p = word >> 26
    op = PRIMARY.get(p)
    if op:
        if op == "sc" and word != 0x44000002:
            return None
        return Insn(addr, word, op)
    if p == 19:
        op = OP19.get((word >> 1) & 0x3FF)
    elif p == 31:
        op = OP31.get((word >> 1) & 0x3FF)
        if op is None and (word >> 10) & 1:
            op = OP31_OE.get((word >> 1) & 0x1FF)
            if op:
                op += "o"
    elif p == 59:
        op = OP59.get((word >> 1) & 0x1F)
    elif p == 63:
        op = OP63_A.get((word >> 1) & 0x1F) or OP63_X.get((word >> 1) & 0x3FF)
        x = OP63_X.get((word >> 1) & 0x3FF)
        if x:
            op = x
    elif p == 4:
        op = OP4_X.get((word >> 1) & 0x3FF) or OP4_Q.get((word >> 1) & 0x3F) or OP4_A.get((word >> 1) & 0x1F)
    if op is None:
        return None
    return Insn(addr, word, op)

def is_terminator(i):
    if i.op == "b" and not i.lk:
        return True
    if i.op in ("bclr", "bcctr") and not i.lk and (i.bo & 0x14) == 0x14:
        return True
    if i.op == "rfi":
        return True
    return False
