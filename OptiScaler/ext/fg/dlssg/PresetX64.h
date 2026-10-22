#pragma once
// A small x86-64 instruction decoder for the DLSS-G preset guards (PresetGuards.h). It is NOT a disassembler: it answers only the
// questions the guards ask about the code of NVIDIA's endpoint create function:
//   * how long is the instruction (so a linear walk lands on real instruction starts),
//   * where can control go (fall through, jump, conditional jump, call, return, stop),
//   * which general-purpose registers does it write, and does it store to memory (which address, how many bytes),
//   * for the few forms the proof reads: the modrm fields, the memory operand and the immediate.
// Anything outside the table below is refused (Decode returns false) instead of guessed: the guards fail closed on a build that uses
// an instruction this table does not know. Pure, no Windows headers: tests/ext/fg/fg_preset_x64.cpp pins the table.
//
// Not decoded on purpose (all refused): VEX/EVEX (C4, C5, 62), x87 (D8-DF), 0F 38 / 0F 3A maps, moffs moves, port I/O, far
// control transfers, segment loads, string compares with unknown semantics beyond the register effect, address-size prefixes.

#include <cstddef>
#include <cstdint>

namespace ext::fg::preset::x64
{
enum Reg : uint8_t
{
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};
constexpr uint8_t kNoReg = 0xFF;

enum class Flow : uint8_t
{
    Next,         // falls through to the next instruction
    Jump,         // unconditional direct jump (target set)
    CondJump,     // conditional direct jump: target set, also falls through
    Call,         // call (direct: target set, indirect: target -1); returns to the next instruction
    Ret,          // return
    Stop,         // int3, ud2, hlt: control does not continue
    JumpIndirect, // jmp r/m: the destination is not known
};

struct MemOperand
{
    bool present = false;
    bool ripRel = false;
    uint8_t base = kNoReg;  // 0..15
    uint8_t index = kNoReg; // 0..15 (never rsp)
    uint8_t scale = 0;
    int32_t disp = 0;
};

struct Insn
{
    uint32_t at = 0;  // RVA of the first byte (set by the caller of Decode)
    uint8_t len = 0;
    Flow flow = Flow::Next;
    int64_t target = -1; // for Jump / CondJump / direct Call: at + len + rel
    bool twoByte = false;
    uint8_t opcode = 0; // the opcode byte (after 0F for two-byte opcodes)
    bool rexW = false;
    bool opsize16 = false;
    bool hasRex = false;
    bool hasModrm = false;
    uint8_t mod = 0;
    uint8_t reg = 0; // modrm.reg with REX.R applied (0..15)
    uint8_t ext = 0; // modrm.reg raw (0..7): the opcode extension of group opcodes
    uint8_t rm = 0;  // modrm.rm with REX.B applied; meaningful as a register only when mod == 3
    MemOperand mem;  // set when the modrm byte names memory (present == true)
    int64_t imm = 0;
    uint8_t immLen = 0;
    uint16_t gprWrites = 0; // bit n = register n may be written (explicit or implicit; partial writes count)
    bool memWrite = false;  // stores to `mem`
    uint8_t memSize = 0;    // bytes stored (a conservative upper bound for SSE stores)
    uint8_t memBytes = 0;   // bytes a memory operand touches, load or store (conservative; 1 for lea, which touches nothing)
    bool isLea = false;
    bool aliasStore = false; // writes memory at an address the operand does not name: rep movs/stos (rdi), bts/btr/btc with a register bit offset
    bool writesStack = false; // push, pop, leave, call/ret excluded (they balance); also set for any write to rsp
    bool readsFrameRegAsValue = false; // rsp or rbp used as a register operand (not as an addressing base)
};

namespace detail
{
constexpr uint16_t Bit(unsigned r) { return static_cast<uint16_t>(1u << r); }
} // namespace detail

// Decodes one instruction at p (at most `avail` bytes readable). Returns false for anything not in the table or truncated.
inline bool Decode(const uint8_t* p, size_t avail, uint32_t at, Insn& in)
{
    using detail::Bit;
    Insn out;
    out.at = at;
    size_t i = 0;
    bool repF3 = false, repF2 = false;

    // legacy prefixes (at most 4): operand size, rep, lock, and the ignored segment overrides (cs/ds/es/ss)
    for (;; ++i)
    {
        if (i >= avail || i > 4) // at most four prefixes
            return false;
        const uint8_t b = p[i];
        if (b == 0x66)
            out.opsize16 = true;
        else if (b == 0xF3)
            repF3 = true;
        else if (b == 0xF2)
            repF2 = true;
        else if (b == 0xF0 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36)
        {
        }
        else
            break;
    }
    uint8_t rex = 0;
    if ((p[i] & 0xF0) == 0x40)
    {
        rex = p[i++];
        out.hasRex = true;
        out.rexW = (rex & 8) != 0;
        if (i >= avail)
            return false;
    }
    const bool rexR = (rex & 4) != 0, rexX = (rex & 2) != 0, rexB = (rex & 1) != 0;
    uint8_t op = p[i++];

    bool modrm = false;   // the opcode has a modrm byte
    int immBytes = 0;     // immediate size, -1 = operand size (2 with 66, else 4)
    bool is8 = false;     // 8-bit operand form
    bool wReg = false;    // writes the register named by modrm.reg
    bool wRm = false;     // writes modrm.rm (a register when mod == 3, memory otherwise)
    bool wRmSse = false;  // SSE store: writes memory when mod != 3; with mod == 3 it writes an XMM register (no GPR)
    uint16_t implicitW = 0;
    bool regInOpcode = false; // low 3 bits of the opcode name a register (push/pop/mov imm/bswap/xchg)
    bool regWrittenByOpcode = false;

    auto fail = [&]() { return false; };

    if (op == 0x0F)
    {
        if (i >= avail)
            return false;
        out.twoByte = true;
        op = p[i++];
        out.opcode = op;
        if (op == 0x38 || op == 0x3A)
            return fail();
        if (op >= 0x80 && op <= 0x8F) // jcc rel32
        {
            out.flow = Flow::CondJump;
            immBytes = 4;
        }
        else if (op == 0x05 || op == 0x34 || op == 0x35 || op == 0x01 || op == 0x00 || op == 0x02 || op == 0x03 || op == 0x06 || op == 0x07 ||
                 op == 0x08 || op == 0x09 || op == 0x30 || op == 0x32 || op == 0x33 || op == 0xA0 || op == 0xA1 || op == 0xA8 || op == 0xA9 ||
                 op == 0xC7 || op == 0xB2 || op == 0xB4 || op == 0xB5)
            return fail();
        else if (op == 0x0B) // ud2
            out.flow = Flow::Stop;
        else if (op == 0x31) // rdtsc
            implicitW = Bit(RAX) | Bit(RDX);
        else if (op == 0xA2) // cpuid
            implicitW = Bit(RAX) | Bit(RBX) | Bit(RCX) | Bit(RDX);
        else if (op >= 0xC8 && op <= 0xCF) // bswap
        {
            regInOpcode = true;
            regWrittenByOpcode = true;
        }
        else
        {
            modrm = true;
            switch (op)
            {
            case 0x0D: case 0x18: case 0x19: case 0x1C: case 0x1D: case 0x1E: case 0x1F: // prefetch, hint nops (0F 1E only as endbr, below)
                break;
            case 0x10: case 0x12: case 0x14: case 0x15: case 0x16: // movups/movsd/movlps/unpck/movhps loads
                break;
            case 0x11: case 0x13: case 0x17: case 0x29: case 0x2B: case 0x7F: case 0xD6: case 0xE7: // SSE stores
                wRmSse = true;
                break;
            case 0x28: case 0x2A: case 0x2E: case 0x2F: // movaps load, cvtsi2ss, comiss
                break;
            case 0x2C: case 0x2D: // cvttss2si / cvtss2si: GPR destination
                wReg = true;
                break;
            case 0x50: case 0xD7: // movmskps, pmovmskb: GPR destination
                wReg = true;
                break;
            case 0x7E: // movd/movq r/m, xmm  (66 0F 7E) ; movq xmm, xmm/m64 (F3 0F 7E)
                if (repF2 || (repF3 && out.opsize16)) // not a defined form: nothing is guessed
                    return fail();
                if (repF3) // F3 0F 7E loads an XMM register
                    break;
                wRm = true;
                break;
            case 0x70: case 0x71: case 0x72: case 0x73: case 0xC2: case 0xC4: case 0xC6: // imm8 forms (no GPR effect)
                immBytes = 1;
                break;
            case 0xC5: // pextrw reg, xmm, imm8
                immBytes = 1;
                wReg = true;
                break;
            case 0xA3: // bt
                break;
            case 0xAB: case 0xB3: case 0xBB: // bts, btr, btc
                wRm = true;
                break;
            case 0xA4: case 0xAC: // shld/shrd imm8
                immBytes = 1;
                wRm = true;
                break;
            case 0xA5: case 0xAD: // shld/shrd cl
                wRm = true;
                break;
            case 0xAE: // fences only (mod == 3, /5 /6 /7); anything with memory is refused
                break;
            case 0xAF: case 0xB6: case 0xB7: case 0xB8: case 0xBC: case 0xBD: case 0xBE: case 0xBF: // imul, movzx, popcnt, bsf, bsr, movsx
                wReg = true;
                break;
            case 0xB0: case 0xB1: // cmpxchg
                wRm = true;
                is8 = (op == 0xB0);
                implicitW = Bit(RAX);
                break;
            case 0xBA: // grp8 rm, imm8
                immBytes = 1;
                break;
            case 0xC0: case 0xC1: // xadd
                wRm = true;
                wReg = true;
                is8 = (op == 0xC0);
                break;
            case 0xC3: // movnti
                wRm = true;
                break;
            default:
                if ((op >= 0x40 && op <= 0x4F)) // cmovcc
                    wReg = true;
                else if (op >= 0x90 && op <= 0x9F) // setcc
                {
                    wRm = true;
                    is8 = true;
                }
                else if ((op >= 0x51 && op <= 0x6F) || (op >= 0x74 && op <= 0x76) || op == 0x7C || op == 0x7D || (op >= 0xD0 && op <= 0xFE))
                {
                    // SSE/MMX arithmetic, compares, unpacks, converts: XMM/MM destination only
                    if (op == 0xF7) // maskmovdqu writes memory through rdi
                        return fail();
                }
                else
                    return fail();
                break;
            }
        }
    }
    else
    {
        out.opcode = op;
        if (op < 0x40)
        {
            const uint8_t lo = op & 7;
            if (lo >= 6)
                return fail();
            const bool cmp = op >= 0x38;
            if (lo <= 3)
            {
                modrm = true;
                is8 = (lo & 1) == 0;
                if (!cmp)
                {
                    if (lo < 2)
                        wRm = true;
                    else
                        wReg = true;
                }
            }
            else if (lo == 4)
            {
                immBytes = 1;
                if (!cmp)
                    implicitW = Bit(RAX);
            }
            else
            {
                immBytes = -1;
                if (!cmp)
                    implicitW = Bit(RAX);
            }
        }
        else if (op >= 0x50 && op <= 0x5F)
        {
            regInOpcode = true;
            out.writesStack = true;
            regWrittenByOpcode = (op >= 0x58); // pop writes its register
        }
        else if (op >= 0x70 && op <= 0x7F)
        {
            out.flow = Flow::CondJump;
            immBytes = 1;
        }
        else if (op >= 0x91 && op <= 0x97) // xchg rax, r
        {
            regInOpcode = true;
            regWrittenByOpcode = true;
            implicitW = Bit(RAX);
        }
        else if (op >= 0xB0 && op <= 0xB7) // mov r8, imm8
        {
            regInOpcode = true;
            regWrittenByOpcode = true;
            is8 = true;
            immBytes = 1;
        }
        else if (op >= 0xB8 && op <= 0xBF) // mov r, imm32/imm64
        {
            regInOpcode = true;
            regWrittenByOpcode = true;
            immBytes = out.rexW ? 8 : -1;
        }
        else
        {
            switch (op)
            {
            case 0x63: // movsxd
                modrm = true;
                wReg = true;
                break;
            case 0x68:
                immBytes = 4;
                out.writesStack = true;
                break;
            case 0x6A:
                immBytes = 1;
                out.writesStack = true;
                break;
            case 0x69:
                modrm = true;
                wReg = true;
                immBytes = -1;
                break;
            case 0x6B:
                modrm = true;
                wReg = true;
                immBytes = 1;
                break;
            case 0x80: case 0x81: case 0x83:
                modrm = true;
                is8 = (op == 0x80);
                immBytes = (op == 0x81) ? -1 : 1;
                break; // writes decided by modrm.reg below (/7 = cmp)
            case 0x84: case 0x85: // test
                modrm = true;
                is8 = (op == 0x84);
                break;
            case 0x86: case 0x87: // xchg r/m, r
                modrm = true;
                is8 = (op == 0x86);
                wRm = true;
                wReg = true;
                break;
            case 0x88:
                modrm = true;
                is8 = true;
                wRm = true;
                break;
            case 0x89:
                modrm = true;
                wRm = true;
                break;
            case 0x8A:
                modrm = true;
                is8 = true;
                wReg = true;
                break;
            case 0x8B:
                modrm = true;
                wReg = true;
                break;
            case 0x8D:
                modrm = true;
                wReg = true;
                out.isLea = true;
                break;
            case 0x8F: // pop r/m (only /0)
                modrm = true;
                wRm = true;
                out.writesStack = true;
                break;
            case 0x90: // nop / pause; with REX.B it is xchg r8,rax
                implicitW = rexB ? (Bit(RAX) | Bit(R8)) : 0;
                break;
            case 0x98:
                implicitW = Bit(RAX);
                break;
            case 0x99:
                implicitW = Bit(RDX);
                break;
            case 0xFD: // std: a downward string store could reach below its operand
                return fail();
            case 0x9B: case 0x9C: case 0x9D: case 0xF5: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC:
                if (op == 0x9C || op == 0x9D)
                    out.writesStack = true;
                break;
            case 0x9E: // sahf
                break;
            case 0x9F: // lahf writes ah
                implicitW = Bit(RAX);
                break;
            case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
                implicitW = Bit(RAX) | Bit(RCX) | Bit(RSI) | Bit(RDI);
                out.aliasStore = (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB); // movs and stos write through rdi
                break;
            case 0xA8:
                immBytes = 1;
                break;
            case 0xA9:
                immBytes = -1;
                break;
            case 0xC0: case 0xC1: // shifts by imm8
                modrm = true;
                is8 = (op == 0xC0);
                immBytes = 1;
                wRm = true;
                break;
            case 0xC2:
                out.flow = Flow::Ret;
                immBytes = 2;
                break;
            case 0xC3:
                out.flow = Flow::Ret;
                break;
            case 0xC6: case 0xC7: // mov r/m, imm (only /0)
                modrm = true;
                is8 = (op == 0xC6);
                immBytes = is8 ? 1 : -1;
                wRm = true;
                break;
            case 0xC9: // leave
                implicitW = Bit(RSP) | Bit(RBP);
                out.writesStack = true;
                break;
            case 0xCC:
                out.flow = Flow::Stop;
                break;
            case 0xD0: case 0xD1: case 0xD2: case 0xD3: // shifts by 1 / cl
                modrm = true;
                is8 = (op == 0xD0 || op == 0xD2);
                wRm = true;
                break;
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: // loopne/loope/loop/jrcxz
                out.flow = Flow::CondJump;
                immBytes = 1;
                implicitW = (op == 0xE3) ? 0 : Bit(RCX);
                break;
            case 0xE8:
                out.flow = Flow::Call;
                immBytes = 4;
                break;
            case 0xE9:
                out.flow = Flow::Jump;
                immBytes = 4;
                break;
            case 0xEB:
                out.flow = Flow::Jump;
                immBytes = 1;
                break;
            case 0xF4:
                out.flow = Flow::Stop;
                break;
            case 0xF6: case 0xF7: // grp3
                modrm = true;
                is8 = (op == 0xF6);
                break; // details below
            case 0xFE: case 0xFF: // grp4 / grp5
                modrm = true;
                is8 = (op == 0xFE);
                break; // details below
            default:
                return fail();
            }
        }
    }

    // ---- modrm, displacement ----------------------------------------------------------------------------------------------
    if (modrm)
    {
        if (i >= avail)
            return false;
        const uint8_t m = p[i++];
        out.hasModrm = true;
        out.mod = m >> 6;
        out.ext = (m >> 3) & 7;
        out.reg = static_cast<uint8_t>(out.ext | (rexR ? 8 : 0));
        const uint8_t rmf = m & 7;
        if (out.mod == 3)
            out.rm = static_cast<uint8_t>(rmf | (rexB ? 8 : 0));
        else
        {
            out.mem.present = true;
            int dispBytes = 0;
            if (rmf == 4)
            {
                if (i >= avail)
                    return false;
                const uint8_t sib = p[i++];
                out.mem.scale = sib >> 6;
                const uint8_t idx = static_cast<uint8_t>(((sib >> 3) & 7) | (rexX ? 8 : 0));
                if (idx != 4)
                    out.mem.index = idx;
                const uint8_t b = sib & 7;
                if (b == 5 && out.mod == 0)
                    dispBytes = 4;
                else
                    out.mem.base = static_cast<uint8_t>(b | (rexB ? 8 : 0));
            }
            else if (rmf == 5 && out.mod == 0)
            {
                out.mem.ripRel = true;
                dispBytes = 4;
            }
            else
                out.mem.base = static_cast<uint8_t>(rmf | (rexB ? 8 : 0));
            if (out.mod == 1)
                dispBytes = 1;
            else if (out.mod == 2)
                dispBytes = 4;
            if (i + static_cast<size_t>(dispBytes) > avail)
                return false;
            if (dispBytes == 1)
                out.mem.disp = static_cast<int8_t>(p[i]);
            else if (dispBytes == 4)
                out.mem.disp = static_cast<int32_t>(static_cast<uint32_t>(p[i]) | static_cast<uint32_t>(p[i + 1]) << 8 | static_cast<uint32_t>(p[i + 2]) << 16 |
                                                    static_cast<uint32_t>(p[i + 3]) << 24);
            i += static_cast<size_t>(dispBytes);
        }
    }

    // ---- group opcodes: the modrm.reg extension decides the effect --------------------------------------------------------------
    if (!out.twoByte)
    {
        switch (op)
        {
        case 0x80: case 0x81: case 0x83:
            wRm = (out.ext != 7); // /7 = cmp
            break;
        case 0xC6: case 0xC7:
            if (out.ext != 0)
                return fail();
            break;
        case 0x8F:
            if (out.ext != 0)
                return fail();
            break;
        case 0xF6: case 0xF7:
            if (out.ext == 0 || out.ext == 1)
                immBytes = is8 ? 1 : -1;
            else if (out.ext == 2 || out.ext == 3)
                wRm = true;
            else
                implicitW = Bit(RAX) | Bit(RDX); // mul, imul, div, idiv
            break;
        case 0xFE:
            if (out.ext > 1)
                return fail();
            wRm = true;
            break;
        case 0xFF:
            switch (out.ext)
            {
            case 0: case 1:
                wRm = true;
                break;
            case 2:
                out.flow = Flow::Call;
                break;
            case 4:
                out.flow = Flow::JumpIndirect;
                break;
            case 6: // push r/m
                out.writesStack = true;
                break;
            default: // far call / far jmp / invalid
                return fail();
            }
            break;
        default:
            break;
        }
    }
    else
    {
        if (op == 0xBA)
        {
            if (out.ext < 4)
                return fail();
            wRm = (out.ext != 4); // bt does not write
        }
        else if (op == 0xAE)
        {
            if (out.mod != 3 || out.ext < 5)
                return fail();
        }
        else if (op == 0x1E)
        {
            // endbr32 / endbr64 (F3 0F 1E FB / FA) are the only forms: the others include rdssp, which writes a register
            if (!(out.mod == 3 && out.ext == 7 && (out.rm == 2 || out.rm == 3)))
                return fail();
        }
        else if (op == 0xAB || op == 0xB3 || op == 0xBB)
            out.aliasStore = (out.mod != 3); // bts/btr/btc r/m, reg: the bit offset in the register picks the address
        else if (op == 0x71 || op == 0x72 || op == 0x73)
        {
            if (out.mod != 3)
                return fail();
        }
    }

    // A 66 prefix changes the length of push imm and of the branches, and REX.W overrides it on the other immediates: none of that is
    // guessed, the combination is refused (a compiler never emits it).
    if (out.opsize16)
    {
        if (!out.twoByte && (op == 0x68 || op == 0x6A))
            return fail();
        if (out.flow != Flow::Next && out.flow != Flow::Stop)
            return fail();
        if (out.rexW && immBytes == -1)
            return fail();
    }

    // ---- immediate --------------------------------------------------------------------------------------------------------------
    int immLen = immBytes;
    if (immLen == -1)
        immLen = out.opsize16 ? 2 : 4;
    if (immLen > 0)
    {
        if (i + static_cast<size_t>(immLen) > avail)
            return false;
        int64_t v = 0;
        for (int k = immLen - 1; k >= 0; --k)
            v = (v << 8) | p[i + static_cast<size_t>(k)];
        // sign-extend the 1/2/4-byte forms
        if (immLen == 1)
            v = static_cast<int8_t>(v);
        else if (immLen == 2)
            v = static_cast<int16_t>(v);
        else if (immLen == 4)
            v = static_cast<int32_t>(v);
        out.imm = v;
        out.immLen = static_cast<uint8_t>(immLen);
        i += static_cast<size_t>(immLen);
    }
    out.len = static_cast<uint8_t>(i);

    // ---- control flow -----------------------------------------------------------------------------------------------------------
    if ((out.flow == Flow::Jump || out.flow == Flow::CondJump || (out.flow == Flow::Call && !out.hasModrm)) && immLen > 0)
        out.target = static_cast<int64_t>(out.at) + out.len + out.imm;

    // ---- register / memory effects --------------------------------------------------------------------------------------------
    const unsigned width = is8 ? 1u : (out.rexW ? 8u : (out.opsize16 ? 2u : 4u));
    auto byteParent = [&](uint8_t r) -> uint8_t { return (!out.hasRex && r >= 4 && r < 8) ? static_cast<uint8_t>(r - 4) : r; };
    auto markReg = [&](uint8_t r, bool byteForm)
    {
        const uint8_t n = byteForm ? byteParent(r) : r;
        out.gprWrites = static_cast<uint16_t>(out.gprWrites | Bit(n));
        if (n == RSP)
            out.writesStack = true;
    };
    out.gprWrites = static_cast<uint16_t>(out.gprWrites | implicitW);
    if (implicitW & Bit(RSP))
        out.writesStack = true;
    if (regInOpcode)
    {
        const uint8_t r = static_cast<uint8_t>((op & 7) | (rexB ? 8 : 0));
        if (regWrittenByOpcode)
            markReg(r, is8);
    }
    if (wReg)
        markReg(out.reg, is8);
    if (wRm)
    {
        if (out.mod == 3)
            markReg(out.rm, is8);
        else
        {
            out.memWrite = true;
            out.memSize = static_cast<uint8_t>(out.twoByte && (op == 0x7E || op == 0xC3) ? (out.rexW ? 8 : 4) : width);
        }
    }
    if (wRmSse && out.mod != 3)
    {
        out.memWrite = true;
        out.memSize = 16;
    }
    if (out.mem.present)
    {
        const bool sse = out.twoByte && ((op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2F) || (op >= 0x50 && op <= 0x7F) || op == 0xC2 ||
                                         (op >= 0xC4 && op <= 0xC6) || (op >= 0xD0 && op <= 0xFE));
        out.memBytes = out.isLea ? 1 : (sse ? 16 : static_cast<uint8_t>(width));
        if (!out.twoByte && ((op == 0xFF && (out.ext == 2 || out.ext == 4 || out.ext == 6)) || op == 0x8F))
            out.memBytes = 8; // call/jmp/push [mem] and pop [mem] move a pointer
    }
    if (out.hasModrm)
    {
        // rsp / rbp named as a plain register operand (a value, not an address), in a register form AND when the other operand is
        // memory (a store of the frame address): which fields are general-purpose registers?
        bool gprRm = !out.twoByte, gprReg = !out.twoByte;
        if (!out.twoByte)
        {
            switch (op) // group opcodes: modrm.reg is an opcode extension
            {
            case 0x80: case 0x81: case 0x83: case 0xC0: case 0xC1: case 0xC6: case 0xC7: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            case 0xF6: case 0xF7: case 0xFE: case 0xFF: case 0x8F:
                gprReg = false;
                break;
            default:
                break;
            }
        }
        else
        {
            const bool intOp = (op >= 0x40 && op <= 0x4F) || (op >= 0x90 && op <= 0x9F) || (op >= 0xA3 && op <= 0xBF && op != 0xA2 && op != 0xAE) || op == 0xC0 || op == 0xC1 || op == 0xC3;
            // rm is a general-purpose register for the integer ops, 66 0F 7E (movd/movq r/m, xmm), 0F 6E (movd/movq xmm, r/m),
            // 0F 2A (cvtsi2ss/sd) and 0F C4 (pinsrw)
            gprRm = intOp || (op == 0x7E && !repF3) || op == 0x6E || op == 0x2A || op == 0xC4;
            gprReg = (intOp && !(op >= 0x90 && op <= 0x9F) && op != 0xBA) || op == 0x2C || op == 0x2D || op == 0x50 || op == 0xD7 || op == 0xC5;
        }
        // without a REX prefix the byte registers 4..7 are ah, ch, dh, bh, not rsp/rbp/rsi/rdi
        auto isFrame = [&](uint8_t r) { return (r == RSP || r == RBP) && !(is8 && !out.hasRex); };
        if ((out.mod == 3 && gprRm && isFrame(out.rm)) || (gprReg && isFrame(out.reg)))
            out.readsFrameRegAsValue = true;
    }
    if (out.mem.present && !out.mem.ripRel && (out.mem.index == RSP || out.mem.index == RBP))
        out.readsFrameRegAsValue = true;
    in = out;
    return true;
}

// Decodes at an RVA of a mapped image: never reads at or past `limit` or past the image.
inline bool DecodeAt(const uint8_t* bytes, size_t imageSize, uint32_t at, uint32_t limit, Insn& in)
{
    if (bytes == nullptr || at >= imageSize || at >= limit)
        return false;
    size_t avail = static_cast<size_t>(limit) - at;
    if (avail > 15)
        avail = 15;
    if (static_cast<size_t>(at) + avail > imageSize)
        avail = imageSize - at;
    return Decode(bytes + at, avail, at, in);
}
} // namespace ext::fg::preset::x64