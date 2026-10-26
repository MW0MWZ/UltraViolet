// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
#include "cpu/ultrasparc/disasm.h"

#include <cstdarg>
#include <cstdio>

namespace uv::usparc {

namespace {

constexpr const char* kRegs[32] = {"%g0", "%g1", "%g2", "%g3", "%g4", "%g5", "%g6", "%g7", "%o0", "%o1", "%o2",
                                   "%o3", "%o4", "%o5", "%sp", "%o7", "%l0", "%l1", "%l2", "%l3", "%l4", "%l5",
                                   "%l6", "%l7", "%i0", "%i1", "%i2", "%i3", "%i4", "%i5", "%fp", "%i7"};
constexpr const char* kIcc[16] = {"n", "e", "le", "l", "leu", "cs", "neg", "vs",
                                  "a", "ne", "g", "ge", "gu", "cc", "pos", "vc"};
constexpr const char* kFcc[16] = {"n", "ne", "lg", "ul", "l", "ug", "g", "u",
                                  "a", "e", "ue", "ge", "uge", "le", "ule", "o"};
constexpr const char* kRcond[8] = {nullptr, "z", "lez", "lz", nullptr, "nz", "gz", "gez"};
constexpr const char* kPr[32] = {"%tpc", "%tnpc", "%tstate", "%tt", "%tick", "%tba", "%pstate", "%tl",
                                 "%pil", "%cwp", "%cansave", "%canrestore", "%cleanwin", "%otherwin", "%wstate",
                                 "%fq", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "%ver"};
constexpr const char* kAsr[32] = {"%y", nullptr, "%ccr", "%asi", "%tick", "%pc", "%fprs", nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                  "%pcr", "%pic", "%dcr", "%gsr", "%set_softint", "%clear_softint", "%softint",
                                  "%tick_cmpr", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

std::string fmt(const char* f, ...) UV_PRINTF(1, 2);
std::string fmt(const char* f, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

s64 sext(u64 v, unsigned bits) {
    const u64 m = u64{1} << (bits - 1);
    v &= (m << 1) - 1;
    return static_cast<s64>((v ^ m) - m);
}

std::string hex(s64 v) { return v < 0 ? fmt("-0x%llx", static_cast<unsigned long long>(-v)) : fmt("0x%llx", static_cast<unsigned long long>(v)); }

// Second operand: a register or a signed immediate.
std::string op2(u32 insn, unsigned bits = 13) {
    if ((insn >> 13) & 1) return hex(sext(insn, bits));
    return kRegs[insn & 31];
}

// Effective address [rs1 + rs2] or [rs1 + simm13].
std::string address(u32 insn) {
    const unsigned rs1 = (insn >> 14) & 31;
    if ((insn >> 13) & 1) {
        const s64 imm = sext(insn, 13);
        if (rs1 == 0) return fmt("[%s]", hex(imm).c_str());
        if (imm == 0) return fmt("[%s]", kRegs[rs1]);
        return imm < 0 ? fmt("[%s - 0x%llx]", kRegs[rs1], static_cast<unsigned long long>(-imm))
                       : fmt("[%s + 0x%llx]", kRegs[rs1], static_cast<unsigned long long>(imm));
    }
    const unsigned rs2 = insn & 31;
    if (rs2 == 0) return fmt("[%s]", kRegs[rs1]);
    return fmt("[%s + %s]", kRegs[rs1], kRegs[rs2]);
}

std::string asi_suffix(u32 insn) {
    if ((insn >> 13) & 1) return " %asi";
    return fmt(" 0x%02x", (insn >> 5) & 0xFF);
}

std::string freg(unsigned r, bool dbl) {
    if (!dbl) return fmt("%%f%u", r);
    return fmt("%%f%u", (r & 0x1E) | ((r & 1) << 5));
}

std::string target(u64 pc, s64 disp) { return fmt("0x%llx", static_cast<unsigned long long>(pc + static_cast<u64>(disp))); }

std::string format2(u32 insn, u64 pc) {
    const unsigned op2v = (insn >> 22) & 7;
    const bool a = (insn >> 29) & 1;
    const unsigned cond = (insn >> 25) & 15;
    const unsigned rd = (insn >> 25) & 31;
    switch (op2v) {
        case 0: return fmt("illtrap 0x%x", insn & 0x3FFFFF);
        case 1: {
            const unsigned cc = (insn >> 20) & 3;
            const bool p = (insn >> 19) & 1;
            return fmt("b%s%s%s %s, %s", kIcc[cond], a ? ",a" : "", p ? ",pt" : ",pn", cc == 2 ? "%xcc" : cc == 0 ? "%icc" : "%?",
                       target(pc, sext(insn, 19) * 4).c_str());
        }
        case 2: return fmt("b%s%s %s", kIcc[cond], a ? ",a" : "", target(pc, sext(insn, 22) * 4).c_str());
        case 3: {
            const char* rc = kRcond[(insn >> 25) & 7];
            if (!rc || ((insn >> 28) & 1)) return fmt(".word 0x%08x", insn);
            const u64 d16 = (((insn >> 20) & 3) << 14) | (insn & 0x3FFF);
            return fmt("br%s%s%s %s, %s", rc, a ? ",a" : "", ((insn >> 19) & 1) ? ",pt" : ",pn", kRegs[(insn >> 14) & 31],
                       target(pc, sext(d16, 16) * 4).c_str());
        }
        case 4:
            if (rd == 0 && (insn & 0x3FFFFF) == 0) return "nop";
            return fmt("sethi %%hi(0x%x), %s", (insn & 0x3FFFFF) << 10, kRegs[rd]);
        case 5:
            return fmt("fb%s%s%s %%fcc%u, %s", kFcc[cond], a ? ",a" : "", ((insn >> 19) & 1) ? ",pt" : ",pn", (insn >> 20) & 3,
                       target(pc, sext(insn, 19) * 4).c_str());
        case 6: return fmt("fb%s%s %s", kFcc[cond], a ? ",a" : "", target(pc, sext(insn, 22) * 4).c_str());
        default: return fmt(".word 0x%08x", insn);
    }
}

std::string format3_alu(u32 insn) {
    static constexpr const char* kNames[64] = {
        "add", "and", "or", "xor", "sub", "andn", "orn", "xnor", "addc", "mulx", "umul", "smul", "subc", "udivx",
        "udiv", "sdiv", "addcc", "andcc", "orcc", "xorcc", "subcc", "andncc", "orncc", "xnorcc", "addccc", nullptr,
        "umulcc", "smulcc", "subccc", nullptr, "udivcc", "sdivcc", "taddcc", "tsubcc", "taddcctv", "tsubcctv",
        "mulscc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "sdivx", nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "jmpl", "return", nullptr, "flush",
        "save", "restore", nullptr, nullptr};
    const unsigned op3 = (insn >> 19) & 0x3F;
    const unsigned rd = (insn >> 25) & 31, rs1 = (insn >> 14) & 31;
    const bool imm = (insn >> 13) & 1;
    switch (op3) {
        case 0x25:
        case 0x26:
        case 0x27: {
            static constexpr const char* kSh[3] = {"sll", "srl", "sra"};
            const bool x = (insn >> 12) & 1;
            const std::string n = imm ? fmt("%u", insn & (x ? 63 : 31)) : kRegs[insn & 31];
            return fmt("%s%s %s, %s, %s", kSh[op3 - 0x25], x ? "x" : "", kRegs[rs1], n.c_str(), kRegs[rd]);
        }
        case 0x28:
            if (rs1 == 15 && rd == 0) return imm ? fmt("membar 0x%x", insn & 0x7F) : "stbar";
            if (kAsr[rs1]) return fmt("rd %s, %s", kAsr[rs1], kRegs[rd]);
            return fmt("rd %%asr%u, %s", rs1, kRegs[rd]);
        case 0x2A:
            if (kPr[rs1]) return fmt("rdpr %s, %s", kPr[rs1], kRegs[rd]);
            return fmt(".word 0x%08x", insn);
        case 0x2B: return "flushw";
        case 0x2C: {
            const bool cc2 = (insn >> 18) & 1;
            const unsigned cc = (insn >> 11) & 3;
            const unsigned c = (insn >> 14) & 15;
            const std::string src = imm ? hex(sext(insn, 11)) : kRegs[insn & 31];
            if (cc2) return fmt("mov%s %s, %s, %s", kIcc[c], cc == 2 ? "%xcc" : "%icc", src.c_str(), kRegs[rd]);
            return fmt("mov%s %%fcc%u, %s, %s", kFcc[c], cc, src.c_str(), kRegs[rd]);
        }
        case 0x2E: return fmt("popc %s, %s", op2(insn).c_str(), kRegs[rd]);
        case 0x2F: {
            const char* rc = kRcond[(insn >> 10) & 7];
            if (!rc) return fmt(".word 0x%08x", insn);
            return fmt("movr%s %s, %s, %s", rc, kRegs[rs1], op2(insn, 10).c_str(), kRegs[rd]);
        }
        case 0x30:
            if (rd == 15 && rs1 == 0 && imm) return "sir";
            if (kAsr[rd]) return fmt("wr %s, %s, %s", kRegs[rs1], op2(insn).c_str(), kAsr[rd]);
            return fmt("wr %s, %s, %%asr%u", kRegs[rs1], op2(insn).c_str(), rd);
        case 0x31: return rd == 0 ? "saved" : rd == 1 ? "restored" : fmt(".word 0x%08x", insn);
        case 0x32:
            if (kPr[rd]) return fmt("wrpr %s, %s, %s", kRegs[rs1], op2(insn).c_str(), kPr[rd]);
            return fmt(".word 0x%08x", insn);
        case 0x34:
        case 0x35: return fmt("fpop%u 0x%03x, %%f%u, %%f%u, %%f%u", op3 - 0x33, (insn >> 5) & 0x1FF, rs1, insn & 31, rd);
        case 0x36: return fmt("impdep1 0x%03x, %%f%u, %%f%u, %%f%u", (insn >> 5) & 0x1FF, rs1, insn & 31, rd);
        case 0x37: return fmt(".word 0x%08x", insn);
        case 0x38: {
            const std::string addr = address(insn);
            const std::string inner = addr.substr(1, addr.size() - 2);
            if (rd == 0 && rs1 == 31 && imm && (insn & 0x1FFF) == 8) return "ret";
            if (rd == 0 && rs1 == 15 && imm && (insn & 0x1FFF) == 8) return "retl";
            if (rd == 0) return fmt("jmp %s", inner.c_str());
            return fmt("jmpl %s, %s", inner.c_str(), kRegs[rd]);
        }
        case 0x39: {
            const std::string addr = address(insn);
            return fmt("return %s", addr.substr(1, addr.size() - 2).c_str());
        }
        case 0x3A: {
            const unsigned cc = (insn >> 11) & 3;
            const std::string n = imm ? fmt("0x%x", insn & 0x7F) : kRegs[insn & 31];
            if (rs1 == 0) return fmt("t%s %s, %s", kIcc[(insn >> 25) & 15], cc == 2 ? "%xcc" : "%icc", n.c_str());
            return fmt("t%s %s, %s + %s", kIcc[(insn >> 25) & 15], cc == 2 ? "%xcc" : "%icc", kRegs[rs1], n.c_str());
        }
        case 0x3B: return fmt("flush %s", address(insn).c_str());
        case 0x3C:
        case 0x3D:
            if (rd == 0 && rs1 == 0 && !imm && (insn & 31) == 0) return op3 == 0x3C ? "save" : "restore";
            break;
        case 0x3E: return rd == 0 ? "done" : rd == 1 ? "retry" : fmt(".word 0x%08x", insn);
        default: break;
    }
    if (!kNames[op3]) return fmt(".word 0x%08x", insn);
    if (op3 == 0x02 && rs1 == 0) return fmt("mov %s, %s", op2(insn).c_str(), kRegs[rd]);
    if (op3 == 0x14 && rd == 0) return fmt("cmp %s, %s", kRegs[rs1], op2(insn).c_str());
    return fmt("%s %s, %s, %s", kNames[op3], kRegs[rs1], op2(insn).c_str(), kRegs[rd]);
}

std::string format3_mem(u32 insn) {
    static constexpr const char* kNames[64] = {
        "lduw", "ldub", "lduh", "ldd", "stw", "stb", "sth", "std", "ldsw", "ldsb", "ldsh", "ldx", nullptr, "ldstub",
        "stx", "swap", "lduwa", "lduba", "lduha", "ldda", "stwa", "stba", "stha", "stda", "ldswa", "ldsba", "ldsha",
        "ldxa", nullptr, "ldstuba", "stxa", "swapa", "ld", nullptr, "ldq", "ldd", "st", nullptr, "stq", "std", nullptr,
        nullptr, nullptr, nullptr, nullptr, "prefetch", nullptr, nullptr, "lda", nullptr, "ldqa", "ldda", "sta",
        nullptr, "stqa", "stda", nullptr, nullptr, nullptr, nullptr, "casa", "prefetcha", "casxa", nullptr};
    const unsigned op3 = (insn >> 19) & 0x3F;
    const unsigned rd = (insn >> 25) & 31;
    const bool alt = op3 & 0x10;
    const bool fp = op3 & 0x20;
    if (op3 == 0x21) return rd == 1 ? fmt("ldx %s, %%fsr", address(insn).c_str()) : fmt("ld %s, %%fsr", address(insn).c_str());
    if (op3 == 0x25) return rd == 1 ? fmt("stx %%fsr, %s", address(insn).c_str()) : fmt("st %%fsr, %s", address(insn).c_str());
    if (op3 == 0x3C || op3 == 0x3E) {
        const std::string a = (insn >> 13) & 1 ? " %asi" : fmt(" 0x%02x", (insn >> 5) & 0xFF);
        return fmt("%s [%s]%s, %s, %s", kNames[op3], kRegs[(insn >> 14) & 31], a.c_str(), kRegs[insn & 31], kRegs[rd]);
    }
    if (!kNames[op3]) return fmt(".word 0x%08x", insn);
    if (op3 == 0x2D || op3 == 0x3D) return fmt("%s %s%s, %u", kNames[op3], address(insn).c_str(), alt ? asi_suffix(insn).c_str() : "", rd);
    const bool store = fp ? ((op3 & 0xF) >= 4 && (op3 & 0xF) <= 7) : ((op3 & 0xF) >= 4 && (op3 & 0xF) <= 7) || (op3 & 0xF) == 0xE;
    std::string r;
    if (fp) r = freg(rd, (op3 & 3) == 3 || (op3 & 3) == 2);
    else r = kRegs[rd];
    const std::string a = address(insn) + (alt ? asi_suffix(insn) : "");
    if (store) return fmt("%s %s, %s", kNames[op3], r.c_str(), a.c_str());
    return fmt("%s %s, %s", kNames[op3], a.c_str(), r.c_str());
}

}  // namespace

std::string disassemble(u32 insn, u64 pc) {
    switch (insn >> 30) {
        case 0: return format2(insn, pc);
        case 1: return fmt("call %s", target(pc, sext(insn, 30) * 4).c_str());
        case 2: return format3_alu(insn);
        default: return format3_mem(insn);
    }
}

}  // namespace uv::usparc
