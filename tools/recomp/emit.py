import ppc

class EmitError(Exception):
    pass

SYSCALL_STUB = 0x02000000

def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"

def mask(mb, me):
    m1 = 0xFFFFFFFF >> mb
    m2 = (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return (m1 & m2) if mb <= me else (m1 | m2)

R = [f"ctx.r[{n}]" for n in range(32)]
F = [f"ctx.f[{n}]" for n in range(32)]

def ra0(n):
    return "0u" if n == 0 else R[n]

def cr0(dst):
    return f"wf::cmp_s(ctx, 0, (int32_t){dst}, 0);"

class FunctionEmitter:
    def __init__(self, ctx):
        self.c = ctx
        self.warnings = []

    def call(self, target, frm, nxt, tail):
        out = [] if tail else [f"ctx.lr = {h(nxt)};"]
        slot = self.c.import_slot(target)
        name = self.c.func_name(target)
        if slot is not None:
            out.append(f"wf::call_import(ctx, base, {slot.id}); // {slot.module}::{slot.name}")
        elif SYSCALL_STUB <= target < SYSCALL_STUB + 8:
            out.append("// .syscall stub (nop; blr)")
        elif target == 0:
            out.append(f"wf::bad_indirect_call(ctx, 0, {h(frm)}); // weak symbol")
        elif name:
            out.append(f"{name}(ctx, base);")
            self.callees.add(name)
        else:
            self.warnings.append(f"{frm:08x}: call to {target:08x} which is not a function start")
            out.append(f"wf::call_indirect(ctx, base, {h(target)}, {h(frm)});")
        if tail:
            out.append("return;")
        return out

    def bc_cond(self, i):
        pre, conds = [], []
        if not i.bo & 4:
            pre.append("ctx.ctr--;")
            conds.append("ctx.ctr == 0" if i.bo & 2 else "ctx.ctr != 0")
        if not i.bo & 0x10:
            conds.append(f"ctx.cr[{i.bi}]" if i.bo & 8 else f"!ctx.cr[{i.bi}]")
        return pre, " && ".join(conds)

    def goto(self, i, t):
        return (["PPC_YIELD_CHECK();"] if t <= i.addr else []) + [f"goto loc_{t:08X};"]

    def branch(self, i):
        f = self.f
        nxt = i.addr + 4
        if i.op == "b":
            t = self.c.img.branch_target(i)
            if i.lk:
                return self.call(t, i.addr, nxt, tail=False)
            if t in f.labels and f.start <= t:
                return self.goto(i, t)
            return self.call(t, i.addr, nxt, tail=True)
        if i.op == "bc":
            t = self.c.img.branch_target(i)
            pre, cond = self.bc_cond(i)
            if i.lk:
                body = self.call(t, i.addr, nxt, tail=False)
            elif t in f.labels:
                body = self.goto(i, t)
            else:
                body = self.call(t, i.addr, nxt, tail=True)
            if not cond:
                return pre + body
            return pre + [f"if ({cond}) {{"] + ["\t" + b for b in body] + ["}"]
        if i.op == "bclr":
            pre, cond = self.bc_cond(i)
            if i.lk:
                if cond:
                    raise EmitError("conditional blrl")
                return pre + ["{ uint32_t t = ctx.lr;", f"ctx.lr = {h(nxt)};", f"wf::call_indirect(ctx, base, t, {h(i.addr)}); }}"]
            return pre + ([f"if ({cond}) return;"] if cond else ["return;"])
        if i.op == "bcctr" and i.addr in self.c.img.switch_tables:
            cases = [f"\tcase {h(s)}: goto loc_{s:08X};" for s in self.c.img.switch_tables[i.addr]]
            return ["switch (ctx.ctr) {"] + cases + ["}", f"wf::bad_indirect_call(ctx, ctx.ctr, {h(i.addr)});"]
        if i.op == "bcctr":
            pre, cond = self.bc_cond(i)
            if i.bo & 4 == 0:
                raise EmitError("bcctr decrementing ctr")
            if i.lk:
                body = [f"ctx.lr = {h(nxt)};", f"wf::call_indirect(ctx, base, ctx.ctr, {h(i.addr)});"]
            else:
                body = [f"wf::call_indirect(ctx, base, ctx.ctr, {h(i.addr)});", "return;"]
            if cond:
                return pre + [f"if ({cond}) {{"] + ["\t" + b for b in body] + ["}"]
            return pre + body
        raise EmitError(i.op)

    def insn(self, i):
        op = i.op
        d, a, b, c = i.rd, i.ra, i.rb, i.rc_reg
        rd, ra, rb = R[d], R[a], R[b]
        rs = R[d]
        rc = i.rc

        if op in ("b", "bc", "bclr", "bcctr"):
            return self.branch(i)

        if op == "addi":
            return [f"{rd} = {ra0(a)} + {h(i.simm)};"] if a else [f"{rd} = {h(i.simm)};"]
        if op == "addis":
            v = (i.uimm << 16) & 0xFFFFFFFF
            return [f"{rd} = {ra0(a)} + {h(v)};"] if a else [f"{rd} = {h(v)};"]
        if op in ("addic", "addic."):
            out = [f"{{ uint64_t t = (uint64_t){ra} + {h(i.simm)}; ctx.xer_ca = (uint8_t)(t >> 32); {rd} = (uint32_t)t; }}"]
            return out + ([cr0(rd)] if op == "addic." else [])
        if op == "subfic":
            return [f"{{ uint64_t t = (uint64_t)(uint32_t)~{ra} + {h(i.simm)} + 1; ctx.xer_ca = (uint8_t)(t >> 32); {rd} = (uint32_t)t; }}"]
        if op == "mulli":
            return [f"{rd} = (uint32_t)((int64_t)(int32_t){ra} * {i.simm});"]
        three = {
            "add": f"{ra} + {rb}",
            "subf": f"{rb} - {ra}",
            "neg": f"0u - {ra}",
            "mullw": f"(uint32_t)((int64_t)(int32_t){ra} * (int32_t){rb})",
            "mulhw": f"(uint32_t)(((int64_t)(int32_t){ra} * (int32_t){rb}) >> 32)",
            "mulhwu": f"(uint32_t)(((uint64_t){ra} * {rb}) >> 32)",
            "divw": f"(uint32_t)wf::divw((int32_t){ra}, (int32_t){rb})",
            "divwu": f"wf::divwu({ra}, {rb})",
        }
        if op in three:
            return [f"{rd} = {three[op]};"] + ([cr0(rd)] if rc else [])
        carry = {
            "addc": f"(uint64_t){ra} + {rb}",
            "adde": f"(uint64_t){ra} + {rb} + ctx.xer_ca",
            "addze": f"(uint64_t){ra} + ctx.xer_ca",
            "addme": f"(uint64_t){ra} + ctx.xer_ca + 0xFFFFFFFFu",
            "subfc": f"(uint64_t)(uint32_t)~{ra} + {rb} + 1",
            "subfe": f"(uint64_t)(uint32_t)~{ra} + {rb} + ctx.xer_ca",
            "subfze": f"(uint64_t)(uint32_t)~{ra} + ctx.xer_ca",
            "subfme": f"(uint64_t)(uint32_t)~{ra} + ctx.xer_ca + 0xFFFFFFFFu",
        }
        if op in carry:
            return [f"{{ uint64_t t = {carry[op]}; ctx.xer_ca = (uint8_t)(t >> 32); {rd} = (uint32_t)t; }}"] + ([cr0(rd)] if rc else [])

        logic = {
            "and": f"{rs} & {rb}", "andc": f"{rs} & ~{rb}", "or": f"{rs} | {rb}", "orc": f"{rs} | ~{rb}",
            "xor": f"{rs} ^ {rb}", "nor": f"~({rs} | {rb})", "nand": f"~({rs} & {rb})", "eqv": f"~({rs} ^ {rb})",
            "slw": f"({rb} & 0x20) ? 0u : ({rs} << ({rb} & 31))",
            "srw": f"({rb} & 0x20) ? 0u : ({rs} >> ({rb} & 31))",
            "extsb": f"(uint32_t)(int32_t)(int8_t){rs}", "extsh": f"(uint32_t)(int32_t)(int16_t){rs}",
            "cntlzw": f"{rs} ? (uint32_t)__builtin_clz({rs}) : 32u",
            "sraw": f"wf::sraw(ctx, {rs}, {rb} & 0x3F)",
        }
        if op in logic:
            return [f"{ra} = {logic[op]};"] + ([cr0(ra)] if rc else [])
        if op == "srawi":
            sh = b
            if sh == 0:
                out = [f"{ra} = {rs};", "ctx.xer_ca = 0;"]
            else:
                m = (1 << sh) - 1
                out = [f"ctx.xer_ca = (int32_t){rs} < 0 && ({rs} & {h(m)}) != 0;", f"{ra} = (uint32_t)((int32_t){rs} >> {sh});"]
            return out + ([cr0(ra)] if rc else [])
        imm_logic = {"ori": "|", "xori": "^", "andi.": "&"}
        if op in imm_logic:
            out = [f"{ra} = {rs} {imm_logic[op]} {h(i.uimm)};"]
            return out + ([cr0(ra)] if op == "andi." else [])
        imm_logic_s = {"oris": "|", "xoris": "^", "andis.": "&"}
        if op in imm_logic_s:
            out = [f"{ra} = {rs} {imm_logic_s[op]} {h(i.uimm << 16)};"]
            return out + ([cr0(ra)] if op == "andis." else [])
        if op in ("rlwinm", "rlwimi", "rlwnm"):
            m = mask(c, i.me)
            if op == "rlwnm":
                rot = f"wf::rotl({rs}, {rb})"
            else:
                rot = f"wf::rotl({rs}, {b})" if b else rs
            if op == "rlwimi":
                out = [f"{ra} = ({rot} & {h(m)}) | ({ra} & {h(~m)});"]
            else:
                out = [f"{ra} = {rot} & {h(m)};"]
            return out + ([cr0(ra)] if rc else [])

        if op in ("cmp", "cmpl", "cmpi", "cmpli"):
            if (i.word >> 21) & 1:
                raise EmitError("64-bit compare")
            fn = "cmp_s" if op in ("cmp", "cmpi") else "cmp_u"
            if op == "cmp":
                return [f"wf::cmp_s(ctx, {i.crfd}, (int32_t){ra}, (int32_t){rb});"]
            if op == "cmpl":
                return [f"wf::cmp_u(ctx, {i.crfd}, {ra}, {rb});"]
            if op == "cmpi":
                return [f"wf::cmp_s(ctx, {i.crfd}, (int32_t){ra}, {i.simm});"]
            return [f"wf::cmp_u(ctx, {i.crfd}, {ra}, {h(i.uimm)});"]

        crlogic = {"crand": "{a} & {b}", "cror": "{a} | {b}", "crxor": "{a} ^ {b}", "crnand": "!({a} & {b})",
                   "crnor": "!({a} | {b})", "creqv": "!({a} ^ {b})", "crandc": "{a} & !{b}", "crorc": "{a} | !{b}"}
        if op in crlogic:
            e = crlogic[op].format(a=f"ctx.cr[{a}]", b=f"ctx.cr[{b}]")
            return [f"ctx.cr[{d}] = (uint8_t)({e});"]
        if op == "mcrf":
            return [f"__builtin_memcpy(&ctx.cr[{i.crfd * 4}], &ctx.cr[{i.crfs * 4}], 4);"]
        if op == "mfcr":
            return [f"{rd} = wf::get_cr(ctx);"]
        if op == "mtcrf":
            return [f"wf::set_cr_field(ctx, {fld}, ({rs} >> {28 - 4 * fld}) & 0xF);" for fld in range(8) if i.crm & (0x80 >> fld)]

        if op in ("mfspr", "mtspr"):
            spr = i.spr
            if spr == 8: reg = "ctx.lr"
            elif spr == 9: reg = "ctx.ctr"
            elif spr == 1: reg = None
            elif 912 <= spr <= 919: reg = f"ctx.gqr[{spr - 912}]"
            elif 896 <= spr <= 903: reg = f"ctx.gqr[{spr - 896}]"
            else: raise EmitError(f"{op} spr {spr}")
            if op == "mfspr":
                return [f"{rd} = {reg};"] if reg else [f"{rd} = wf::get_xer(ctx);"]
            return [f"{reg} = {rs};"] if reg else [f"wf::set_xer(ctx, {rs});"]

        mem = self.memop(i)
        if mem is not None:
            return mem

        if op in ("dcbf", "dcbst", "dcbt", "dcbtst", "dcbi", "icbi"):
            return [f"// {op}"]
        if op in ("dcbz", "dcbz_l"):
            return [f"__builtin_memset(base + (({ra0(a)} + {rb}) & ~31u), 0, 32);"]
        if op == "sync":
            return ["__atomic_thread_fence(__ATOMIC_SEQ_CST);"]
        if op in ("isync", "eieio"):
            return ["__atomic_signal_fence(__ATOMIC_SEQ_CST);"]

        if op in ("tw", "twi"):
            to = d
            bv = f"{i.simm}" if op == "twi" else f"(int32_t){rb}"
            bu = h(i.simm) if op == "twi" else rb
            if to == 31:
                return [f"wf::trap(ctx, base, {h(i.addr)});"]
            conds = []
            if to & 16: conds.append(f"(int32_t){ra} < {bv}")
            if to & 8: conds.append(f"(int32_t){ra} > {bv}")
            if to & 4: conds.append(f"{ra} == (uint32_t){bu}")
            if to & 2: conds.append(f"{ra} < (uint32_t){bu}")
            if to & 1: conds.append(f"{ra} > (uint32_t){bu}")
            if not conds:
                return ["// trap never"]
            return [f"if ({' || '.join(conds)}) wf::trap(ctx, base, {h(i.addr)});"]

        fpu = self.fpu(i)
        if fpu is not None:
            return fpu
        raise EmitError(f"unsupported instruction {op}")

    def memop(self, i):
        op = i.op
        d, a, b = i.rd, i.ra, i.rb
        loads = {"lwz": ("ld32({ea})", ""), "lbz": ("ld8({ea})", ""), "lhz": ("ld16({ea})", ""),
                 "lha": ("(uint32_t)(int32_t)(int16_t)wf::ld16(base, {ea})", "raw")}
        stores = {"stw": "st32", "stb": "st8", "sth": "st16"}
        base_op, form = op, "d"
        for suffix, fm in (("ux", "ux"), ("x", "x"), ("u", "u")):
            if op.endswith(suffix) and op[:-len(suffix)] in ("lwz", "lbz", "lhz", "lha", "stw", "stb", "sth",
                                                               "lfs", "lfd", "stfs", "stfd", "psq_l", "psq_st"):
                base_op, form = op[:-len(suffix)], fm
                break
        if base_op not in ("lwz", "lbz", "lhz", "lha", "stw", "stb", "sth", "lfs", "lfd", "stfs", "stfd",
                           "psq_l", "psq_st") and op not in ("lmw", "stmw", "lswi", "stswi", "lwbrx", "stwbrx",
                                                             "lhbrx", "sthbrx", "lwarx", "stwcx.", "stfiwx"):
            return None

        if op in ("lwbrx", "stwbrx", "lhbrx", "sthbrx", "lwarx", "stwcx.", "stfiwx"):
            form = "x"
        disp = i.psq_d if base_op in ("psq_l", "psq_st") and form in ("d", "u") else i.simm
        if form in ("d", "u"):
            ea = f"{ra0(a)} + {h(disp)}" if a else h(disp)
        else:
            ea = f"{ra0(a)} + {R[b]}"
        upd = form in ("u", "ux")
        out = []
        if upd:
            out.append(f"ea = {ea};")
            ea = "ea"

        if base_op in loads:
            expr, kind = loads[base_op]
            if kind == "raw":
                out.append(f"{R[d]} = {expr.format(ea=ea)};")
            else:
                out.append(f"{R[d]} = wf::{expr.format(ea='base, ' + ea)};")
        elif base_op in stores:
            out.append(f"wf::{stores[base_op]}(base, {ea}, {'(uint8_t)' if base_op == 'stb' else '(uint16_t)' if base_op == 'sth' else ''}{R[d]});")
        elif base_op == "lfs":
            out.append(f"wf::lfs(ctx, {d}, wf::ld32(base, {ea}));")
        elif base_op == "lfd":
            out.append(f"{F[d]}.ps0u = wf::ld64(base, {ea});")
        elif base_op == "stfs":
            out.append(f"wf::st32(base, {ea}, wf::stfs_bits(ctx, {d}));")
        elif base_op == "stfd":
            out.append(f"wf::st64(base, {ea}, {F[d]}.ps0u);")
        elif base_op == "psq_l":
            out.append(f"wf::psq_load(ctx, base, {d}, {ea}, {i.psq_w}, {i.psq_i});")
        elif base_op == "psq_st":
            out.append(f"wf::psq_store(ctx, base, {d}, {ea}, {i.psq_w}, {i.psq_i});")
        elif op == "stfiwx":
            out.append(f"wf::st32(base, {ea}, (uint32_t){F[d]}.ps0u);")
        elif op == "lwbrx":
            out.append(f"{R[d]} = __builtin_bswap32(wf::ld32(base, {ea}));")
        elif op == "stwbrx":
            out.append(f"wf::st32(base, {ea}, __builtin_bswap32({R[d]}));")
        elif op == "lhbrx":
            out.append(f"{R[d]} = __builtin_bswap16(wf::ld16(base, {ea}));")
        elif op == "sthbrx":
            out.append(f"wf::st16(base, {ea}, __builtin_bswap16((uint16_t){R[d]}));")
        elif op == "lwarx":
            out.append(f"{R[d]} = wf::lwarx(ctx, base, {ea});")
        elif op == "stwcx.":
            out.append(f"wf::stwcx(ctx, base, {ea}, {R[d]});")
        elif op in ("lmw", "stmw"):
            out = [f"ea = {ea};"]
            for k, reg in enumerate(range(d, 32)):
                if op == "lmw":
                    out.append(f"{R[reg]} = wf::ld32(base, ea + {4 * k}u);")
                else:
                    out.append(f"wf::st32(base, ea + {4 * k}u, {R[reg]});")
        elif op in ("lswi", "stswi"):
            ea = h(0) if a == 0 else R[a]
            out = [f"ea = {ea};"]
            n = b if b else 32
            reg, k = d - 1, 0
            for byte in range(n):
                if k == 0:
                    reg = (reg + 1) % 32
                    if op == "lswi":
                        out.append(f"{R[reg]} = 0;")
                shift = 24 - k
                if op == "lswi":
                    out.append(f"{R[reg]} |= (uint32_t)wf::ld8(base, ea + {byte}u) << {shift};")
                else:
                    out.append(f"wf::st8(base, ea + {byte}u, (uint8_t)({R[reg]} >> {shift}));")
                k = (k + 8) % 32
        else:
            raise EmitError(op)
        if upd:
            out.append(f"{R[a]} = ea;")
        return out

    def fpu(self, i):
        op = i.op
        d, a, b, c = i.rd, i.ra, i.rb, i.rc_reg
        fd, fa, fb, fc = F[d], F[a], F[b], F[c]
        if op.startswith("f") and i.rc and op not in ("fcmpu", "fcmpo"):
            raise EmitError(f"{op}. (Rc=1)")
        bits = {"fmr": f"{fb}.ps0u", "fneg": f"{fb}.ps0u ^ 0x8000000000000000ull",
                "fabs": f"{fb}.ps0u & 0x7FFFFFFFFFFFFFFFull", "fnabs": f"{fb}.ps0u | 0x8000000000000000ull"}
        if op in bits:
            return [f"{fd}.ps0u = {bits[op]};"]
        dbl = {"fadd": f"{fa}.ps0 + {fb}.ps0", "fsub": f"{fa}.ps0 - {fb}.ps0", "fmul": f"{fa}.ps0 * {fc}.ps0",
               "fdiv": f"{fa}.ps0 / {fb}.ps0",
               "fmadd": f"__builtin_fma({fa}.ps0, {fc}.ps0, {fb}.ps0)",
               "fmsub": f"__builtin_fma({fa}.ps0, {fc}.ps0, -{fb}.ps0)",
               "fnmadd": f"-__builtin_fma({fa}.ps0, {fc}.ps0, {fb}.ps0)",
               "fnmsub": f"-__builtin_fma({fa}.ps0, {fc}.ps0, -{fb}.ps0)"}
        if op in dbl:
            return [f"{fd}.ps0 = {dbl[op]};"]
        c25 = f"wf::fp::round25({fc}.ps0)"
        sgl = {"fadds": f"{fa}.ps0 + {fb}.ps0", "fsubs": f"{fa}.ps0 - {fb}.ps0", "fmuls": f"{fa}.ps0 * {c25}",
               "fdivs": f"{fa}.ps0 / {fb}.ps0",
               "fmadds": f"__builtin_fma({fa}.ps0, {c25}, {fb}.ps0)",
               "fmsubs": f"__builtin_fma({fa}.ps0, {c25}, -{fb}.ps0)",
               "fnmadds": f"-__builtin_fma({fa}.ps0, {c25}, {fb}.ps0)",
               "fnmsubs": f"-__builtin_fma({fa}.ps0, {c25}, -{fb}.ps0)",
               "frsp": f"{fb}.ps0"}
        if op in sgl:
            return [f"wf::set_single(ctx, {d}, {sgl[op]});"]
        if op == "fres":
            return [f"{{ double t = wf::fp::fres({fb}.ps0); {fd}.ps0 = t; {fd}.ps1 = t; }}"]
        if op == "frsqrte":
            return [f"{fd}.ps0 = wf::fp::frsqrte({fb}.ps0);"]
        if op == "fctiwz":
            return [f"{fd}.ps0u = wf::fp::fctiwz({fb}.ps0);"]
        if op == "fsel":
            return [f"{{ wf::FPR t = ({fa}.ps0 >= -0.0) ? {fc} : {fb}; {fd} = t; }}"]
        if op in ("fcmpu", "fcmpo"):
            return [f"wf::cmp_f(ctx, {i.crfd}, {fa}.ps0, {fb}.ps0);"]

        def pair(e0, e1):
            return [f"{{ double t0 = {e0}, t1 = {e1}; {fd}.ps0 = t0; {fd}.ps1 = t1; }}"]

        def fl(e):
            return f"(double)(float)({e})"

        if op in ("ps_add", "ps_sub", "ps_div"):
            o = {"ps_add": "+", "ps_sub": "-", "ps_div": "/"}[op]
            return pair(fl(f"{fa}.ps0 {o} {fb}.ps0"), fl(f"{fa}.ps1 {o} {fb}.ps1"))
        if op == "ps_mul":
            return pair(fl(f"{fa}.ps0 * wf::fp::round25({fc}.ps0)"), fl(f"{fa}.ps1 * wf::fp::round25({fc}.ps1)"))
        fused = {"ps_madd": ("", "+"), "ps_msub": ("", "-"), "ps_nmadd": ("-", "+"), "ps_nmsub": ("-", "-")}
        if op in fused:
            neg, sign = fused[op]
            bneg = "-" if sign == "-" else ""
            return pair(fl(f"{neg}__builtin_fma({fa}.ps0, wf::fp::round25({fc}.ps0), {bneg}{fb}.ps0)"),
                        fl(f"{neg}__builtin_fma({fa}.ps1, wf::fp::round25({fc}.ps1), {bneg}{fb}.ps1)"))
        if op in ("ps_madds0", "ps_madds1", "ps_muls0", "ps_muls1"):
            slot = op[-1]
            cs = f"wf::fp::round25({fc}.ps{slot})"
            if op.startswith("ps_madds"):
                return [f"{{ double c = {cs};"] + ["\t" + l for l in pair(fl(f"__builtin_fma({fa}.ps0, c, {fb}.ps0)"),
                                                                          fl(f"__builtin_fma({fa}.ps1, c, {fb}.ps1)"))] + ["}"]
            return [f"{{ double c = {cs};"] + ["\t" + l for l in pair(fl(f"{fa}.ps0 * c"), fl(f"{fa}.ps1 * c"))] + ["}"]
        if op == "ps_sum0":
            return pair(fl(f"{fa}.ps0 + {fb}.ps1"), fl(f"{fc}.ps1"))
        if op == "ps_sum1":
            return pair(fl(f"{fc}.ps0"), fl(f"{fa}.ps0 + {fb}.ps1"))
        if op == "ps_sel":
            return pair(f"({fa}.ps0 >= -0.0) ? {fc}.ps0 : {fb}.ps0", f"({fa}.ps1 >= -0.0) ? {fc}.ps1 : {fb}.ps1")
        merges = {"ps_merge00": (f"{fa}.ps0", f"{fb}.ps0"), "ps_merge01": (f"{fa}.ps0", f"{fb}.ps1"),
                  "ps_merge10": (f"{fa}.ps1", f"{fb}.ps0"), "ps_merge11": (f"{fa}.ps1", f"{fb}.ps1")}
        if op in merges:
            return pair(*merges[op])
        if op == "ps_mr":
            return [f"{fd} = {fb};"] if d != b else ["// ps_mr self"]
        signbits = {"ps_neg": "^ 0x8000000000000000ull", "ps_abs": "& 0x7FFFFFFFFFFFFFFFull", "ps_nabs": "| 0x8000000000000000ull"}
        if op in signbits:
            s = signbits[op]
            return [f"{fd}.ps0u = {fb}.ps0u {s};", f"{fd}.ps1u = {fb}.ps1u {s};"]
        if op == "ps_res":
            return pair(fl(f"wf::fp::fres({fb}.ps0)"), fl(f"wf::fp::fres({fb}.ps1)"))
        if op == "ps_rsqrte":
            return pair(fl(f"wf::fp::frsqrte({fb}.ps0)"), fl(f"wf::fp::frsqrte({fb}.ps1)"))
        if op in ("ps_cmpu0", "ps_cmpo0"):
            return [f"wf::cmp_f(ctx, {i.crfd}, {fa}.ps0, {fb}.ps0);"]
        if op in ("ps_cmpu1", "ps_cmpo1"):
            return [f"wf::cmp_f(ctx, {i.crfd}, {fa}.ps1, {fb}.ps1);"]
        return None

    def function(self, f, name):
        self.f = f
        self.callees = set()
        body = []
        probes = getattr(self.c, "probes", {})
        for a in f.insns:
            i = self.c.img.insn(a)
            if a in f.labels:
                body.append(f"loc_{a:08X}:;")
            if a in probes:
                body.append(f'\twf::probe(ctx, base, {h(a)}, "{probes[a]}");')
            try:
                lines = self.insn(i)
            except EmitError as e:
                raise EmitError(f"{a:08x} ({i.op}): {e}") from None
            body.append(f"\t// {a:08X} {i.op}{'l' if i.op in ('b', 'bc', 'bclr', 'bcctr') and i.lk else ''}")
            body.extend("\t" + l for l in lines)
        if f.fallthrough:
            last = self.c.img.insn(f.insns[-1]) if f.insns else None
            if last is None or not ppc.is_terminator(last):
                body.extend("\t" + l for l in self.call(f.fallthrough, f.insns[-1] if f.insns else f.start, 0, tail=True))
        head = [f"PPC_FUNC({name})", "{", "\t[[maybe_unused]] uint32_t ea;", f"\tPPC_INTERP_ENTRY({h(f.start)});"]
        return head + body + ["}"]
