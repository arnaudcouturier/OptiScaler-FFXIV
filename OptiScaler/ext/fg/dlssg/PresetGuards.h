#pragma once
// DLSS-G preset guards: find, in a mapped nvngx_dlssg.dll image, the functions the live preset switch depends on, and refuse
// anything that does not match the exact code shape. Pure over (bytes, sections, exception table): tests/ext/fg/
// fg_preset_guards.cpp runs it on a synthetic image; the manual recipe fg_preset_guards_dll runs it on the real DLL.
//
// What is searched (all names are ours; the shapes were read in 310.9.1.0 and cross-checked on 310.8.0):
//   reader    bool(id, uint* out): NVIDIA's cached-settings lookup. Found by its unique call site
//             "lea rdx,[rsp+68h]; mov dword [rsp+68h],0; mov ecx,10E41DF1h; call reader; test al,al".
//   readFn    EndpointConfiguration::ReadRegkeys(config*, context*): the function holding that call site.
//   create    the endpoint create function: the one unique caller of readFn. It copies the process-wide configuration into a
//             private one for each endpoint:  defaults(rbp+40h)  ...  copy(rbp+40h, [shared global])  at two sites.
// The switch re-runs "defaults + readFn(context)" on each new endpoint's private copy, with `reader` answering the active
// request, so every guard below is about proving those four functions and the data flow between them:
//   * every pattern matches exactly once (or exactly twice for the two copy sites) across the executable sections;
//   * every target is the first byte of a function of the exception table and lies in an executable section;
//   * create's second argument is the context. create is decoded instruction by instruction (PresetX64.h); the context must be kept
//     (a callee-saved register or a frame slot) by the straight-line code at its start, and the very same place must be loaded back
//     into rdx right before the readFn call, with nothing between the two that can write that place and no way into that stretch of
//     code from outside (see ProveContext). No fixed register is assumed: 310.9.1.0 keeps it in r13, 310.9.16 in [rsp+60h];
//   * the global read before the readFn call and before both copy calls is the same global;
//   * the stack-frame constants of create are masked (they differ between builds) but must still look like a frame.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "PresetX64.h"

namespace ext::fg::preset::guards
{
struct Section
{
    uint32_t va;
    uint32_t size;
    bool executable;
};

// A mapped image: bytes[rva] is the byte at that RVA. The exception table (.pdata) is `pdataSize` bytes at `pdataVa`.
struct ImageView
{
    const uint8_t* bytes;
    size_t size;
    const Section* sections;
    size_t sectionCount;
    uint32_t pdataVa;
    uint32_t pdataSize;
};

enum class Why : uint8_t
{
    Ok,
    OutOfImage,
    NoPdata,
    NoReader,
    ReaderNotUnique,
    ReaderPrologue,
    ReadFnPrologue,
    NoCreate,
    CreateNotUnique,
    CreatePrologue,
    CreateDecode,
    CreateContext,
    CreateSignature,
    ReadCallSetup,
    DefaultsNotFound,
    CopySites,
    CopySetup,
    GlobalMismatch,
    NotFunctionStart
};

constexpr const char* WhyText(Why why)
{
    switch (why)
    {
    case Why::Ok:
        return "ok";
    case Why::OutOfImage:
        return "the image is unreadable";
    case Why::NoPdata:
        return "no exception table";
    case Why::NoReader:
        return "the cached-settings reader call was not found";
    case Why::ReaderNotUnique:
        return "the cached-settings reader call is not unique";
    case Why::ReaderPrologue:
        return "the cached-settings reader does not start as expected";
    case Why::ReadFnPrologue:
        return "the settings read function does not start as expected";
    case Why::NoCreate:
        return "the endpoint create function was not found";
    case Why::CreateNotUnique:
        return "the settings read function has more than one caller";
    case Why::CreatePrologue:
        return "the endpoint create function does not start as expected";
    case Why::CreateDecode:
        return "the endpoint create function contains code the safety check cannot follow";
    case Why::CreateContext:
        return "the endpoint create function does not keep its context argument as expected";
    case Why::CreateSignature:
        return "the endpoint create function does not look like a six-argument function returning a 32-bit status";
    case Why::ReadCallSetup:
        return "the settings read call is not set up as expected";
    case Why::DefaultsNotFound:
        return "the configuration defaults function was not found exactly once";
    case Why::CopySites:
        return "the configuration copy function was not found at exactly two call sites";
    case Why::CopySetup:
        return "a configuration copy call is not set up as expected";
    case Why::GlobalMismatch:
        return "the shared configuration is not the same object at every site";
    case Why::NotFunctionStart:
        return "a located address is not the start of a function";
    }
    return "";
}

// RVAs. All zero unless why == Ok.
struct Located
{
    Why why = Why::Ok;
    uint32_t reader = 0;
    uint32_t readFn = 0;
    uint32_t create = 0;
    uint32_t defaults = 0;
    uint32_t copy = 0;
    uint32_t copySites[2] {};  // address after each copy call inside create (the hook compares return addresses)
    uint32_t sharedGlobal = 0; // RVA of the shared configuration pointer
    // Where the proof that create passes its context argument to the settings read function found things (informational; set when why == Ok).
    bool ctxInSlot = false;    // false: kept in the register ctxReg; true: kept in a frame slot at rsp+ctxSlot (after the prologue)
    uint8_t ctxReg = 0;        // x64 register number (3 rbx, 6 rsi, 7 rdi, 12..15 r12..r15)
    int32_t ctxSlot = 0;       // offset from rsp after the prologue
    uint32_t ctxSaveAt = 0;    // RVA of the instruction that keeps rdx
    uint32_t ctxReloadAt = 0;  // RVA of the instruction that puts it back into rdx before the read call
    uint32_t ctxChecked = 0;   // instructions between the two that were checked
    uint32_t createInsns = 0;  // instructions decoded in create
    // When why != Ok and the failure is at a place in the code: its RVA (0 when not applicable).
    uint32_t failAt = 0;
};

namespace detail
{
// pattern bytes; a mask byte of 0 means "any"
template <size_t N> struct Pat
{
    uint8_t b[N];
    uint8_t m[N];
    static constexpr size_t size = N;
};

constexpr uint8_t kAny = 0x100 & 0xFF; // placeholder only; masks are separate arrays

class Scanner
{
  public:
    explicit Scanner(const ImageView& v) : v_(v) {}

    bool InImage(uint64_t rva, uint64_t n) const { return v_.bytes != nullptr && rva + n <= v_.size; }

    bool Exec(uint64_t rva, uint64_t n) const
    {
        for (size_t i = 0; i < v_.sectionCount; ++i)
        {
            const auto& s = v_.sections[i];
            if (s.executable && rva >= s.va && rva + n <= static_cast<uint64_t>(s.va) + s.size && InImage(rva, n))
                return true;
        }
        return false;
    }

    template <size_t N> bool Match(uint64_t at, const uint8_t (&bytes)[N], const uint8_t (&mask)[N]) const
    {
        if (!InImage(at, N))
            return false;
        for (size_t i = 0; i < N; ++i)
            if (mask[i] && v_.bytes[at + i] != bytes[i])
                return false;
        return true;
    }

    template <size_t N> bool Match(uint64_t at, const uint8_t (&bytes)[N]) const
    {
        return InImage(at, N) && std::memcmp(v_.bytes + at, bytes, N) == 0;
    }

    uint32_t U32(uint64_t at) const
    {
        uint32_t x = 0;
        std::memcpy(&x, v_.bytes + at, 4);
        return x;
    }
    int32_t I32(uint64_t at) const
    {
        int32_t x = 0;
        std::memcpy(&x, v_.bytes + at, 4);
        return x;
    }

    // Calls fn(rva) for every offset of every executable section where the first byte equals `first`.
    template <class Fn> void ForEachExec(Fn&& fn) const
    {
        for (size_t i = 0; i < v_.sectionCount; ++i)
        {
            const auto& s = v_.sections[i];
            if (!s.executable || !InImage(s.va, s.size))
                continue;
            for (uint64_t at = s.va; at < static_cast<uint64_t>(s.va) + s.size; ++at)
                fn(static_cast<uint32_t>(at));
        }
    }

    const ImageView& view() const { return v_; }

  private:
    const ImageView& v_;
};

struct Function
{
    uint32_t begin, end;
};

inline bool LoadFunctions(const Scanner& sc, std::vector<Function>& out)
{
    const auto& v = sc.view();
    if (v.pdataSize < 12 || !sc.InImage(v.pdataVa, v.pdataSize))
        return false;
    for (uint32_t off = 0; off + 12 <= v.pdataSize; off += 12)
    {
        const uint32_t begin = sc.U32(v.pdataVa + off), end = sc.U32(v.pdataVa + off + 4);
        if (begin == 0 && end == 0)
            break;
        if (end > begin)
            out.push_back({ begin, end });
    }
    return !out.empty();
}

inline const Function* OwnerOf(const std::vector<Function>& fns, uint32_t rva)
{
    for (const auto& f : fns)
        if (f.begin <= rva && rva < f.end)
            return &f;
    return nullptr;
}

inline const Function* StartOf(const std::vector<Function>& fns, uint32_t rva)
{
    for (const auto& f : fns)
        if (f.begin == rva)
            return &f;
    return nullptr;
}
// True when rva lies strictly inside a function of the table (a leaf function has no entry at all, which is fine).
inline bool InsideFunction(const std::vector<Function>& fns, uint32_t rva)
{
    for (const auto& f : fns)
        if (f.begin < rva && rva < f.end)
            return true;
    return false;
}
// ---- the context argument ----------------------------------------------------------------------------------------------------
// Frame of create as the prologue builds it:  lea rbp,[rsp+rbpDisp]  and  sub rsp,size  (after seven pushes). Offsets below are
// "frame offsets": bytes above rsp after the prologue. [rsp+d] is frame offset d, [rbp+d] is d + rbpToRsp.
struct Frame
{
    int64_t rbpDisp;  // negative
    int64_t size;
    int64_t rbpToRsp; // rbp - rsp after the prologue
};

// A place the context can be kept: a register, or an 8-byte slot of the frame.
struct Loc
{
    bool slot = false;
    uint8_t reg = x64::kNoReg;
    int64_t off = 0;
    bool operator==(const Loc& o) const { return slot == o.slot && (slot ? off == o.off : reg == o.reg); }
};

constexpr bool CalleeSaved(uint8_t r)
{
    return r == x64::RBX || r == x64::RSI || r == x64::RDI || (r >= x64::R12 && r <= x64::R15);
}

// Frame offset of an [rsp+disp] / [rbp+disp] operand without an index register.
inline bool FrameOffset(const x64::MemOperand& m, const Frame& f, int64_t& off)
{
    if (!m.present || m.ripRel || m.index != x64::kNoReg)
        return false;
    if (m.base == x64::RSP)
        off = m.disp;
    else if (m.base == x64::RBP)
        off = static_cast<int64_t>(m.disp) + f.rbpToRsp;
    else
        return false;
    return true;
}

// mov r64, rdx  /  mov [frame slot], rdx
inline bool IsSaveOfRdx(const x64::Insn& n, const Frame& f, Loc& loc)
{
    if (n.twoByte || !n.rexW || !n.hasModrm)
        return false;
    if (n.opcode == 0x89 && n.reg == x64::RDX) // mov r/m64, rdx
    {
        if (n.mod == 3)
        {
            loc = Loc { false, n.rm, 0 };
            return true;
        }
        int64_t off = 0;
        if (FrameOffset(n.mem, f, off))
        {
            loc = Loc { true, x64::kNoReg, off };
            return true;
        }
        return false;
    }
    if (n.opcode == 0x8B && n.mod == 3 && n.rm == x64::RDX) // mov r64, rdx
    {
        loc = Loc { false, n.reg, 0 };
        return true;
    }
    return false;
}

// mov rdx, r64  /  mov rdx, [frame slot]
inline bool IsReloadToRdx(const x64::Insn& n, const Frame& f, Loc& loc)
{
    if (n.twoByte || !n.rexW || !n.hasModrm)
        return false;
    if (n.opcode == 0x8B && n.reg == x64::RDX) // mov rdx, r/m64
    {
        if (n.mod == 3)
        {
            loc = Loc { false, n.rm, 0 };
            return true;
        }
        int64_t off = 0;
        if (FrameOffset(n.mem, f, off))
        {
            loc = Loc { true, x64::kNoReg, off };
            return true;
        }
        return false;
    }
    if (n.opcode == 0x89 && n.mod == 3 && n.rm == x64::RDX) // mov rdx, r64 (the other encoding)
    {
        loc = Loc { false, n.reg, 0 };
        return true;
    }
    return false;
}

inline int InsnIndex(const std::vector<x64::Insn>& insns, uint32_t rva)
{
    const auto it = std::lower_bound(insns.begin(), insns.end(), rva, [](const x64::Insn& a, uint32_t v) { return a.at < v; });
    return (it != insns.end() && it->at == rva) ? static_cast<int>(it - insns.begin()) : -1;
}

// Start of a fragment of the exception table that is chained to `create` (a cold part of the same function: code the decode of create
// does not cover); 0 when there is none. Both ways of chaining are read: UNWIND_INFO with UNW_FLAG_CHAININFO (the parent
// RUNTIME_FUNCTION follows the unwind codes) and an unwind pointer with the low bit set.
inline uint32_t ChainedFragmentOf(const Scanner& sc, uint32_t create)
{
    const auto& v = sc.view();
    for (uint32_t off = 0; off + 12 <= v.pdataSize; off += 12)
    {
        const uint32_t begin = sc.U32(v.pdataVa + off), end = sc.U32(v.pdataVa + off + 4), unwind = sc.U32(v.pdataVa + off + 8);
        if (begin == 0 && end == 0)
            break;
        uint32_t parent = 0;
        if (unwind & 1)
        {
            const uint32_t at = unwind & ~3u;
            if (sc.InImage(at, 12))
                parent = sc.U32(at);
        }
        else if (sc.InImage(unwind, 4) && ((v.bytes[unwind] >> 3) & 4) != 0)
        {
            const uint32_t codes = v.bytes[unwind + 2];
            const uint32_t at = unwind + 4 + ((codes + 1) & ~1u) * 2;
            if (sc.InImage(at, 12))
                parent = sc.U32(at);
        }
        if (parent == create && begin != create)
            return begin;
    }
    return 0;
}

struct ContextFacts
{
    Loc loc;
    uint32_t saveAt = 0;
    uint32_t reloadAt = 0;
    uint32_t checked = 0;
    uint32_t globalRead = 0; // RVA of the global that "mov rcx,[rip+g]" loads before the read call
};

// Proves that create hands its own second argument (the context, rdx at entry) to the settings read function:
//  1. the read call is preceded by "mov rcx,[rip+g]" and, before that, "mov rdx, X" (X a register or a frame slot);
//  2. the straight-line code at the start of create (after the prologue, up to its first branch, call or return, and while rdx is
//     untouched) keeps rdx in a place that survives calls (a callee-saved register, or a frame slot at or above rsp+20h): that
//     instruction is the "save", and it must keep it in exactly X;
//  3. nothing between the save and the reload writes X (registers: any write incl. partial writes, pop, xchg, implicit writes;
//     frame slots: any store whose bytes overlap it, through rsp or rbp; also no address of the slot or of anything below it is
//     taken, no copy of rsp/rbp as a value, no push/pop or other move of rsp/rbp);
//  4. control flow: no jump from outside [save, reload] lands in it, no jump lands anywhere in (reload, call] and none lands on the
//     entry run, every direct jump target inside create is an instruction start, there is no indirect jump, no call into create
//     itself, a jump out of create only as a tail call to the start of another function (the real builds have none; a call out
//     returns; Locate also refuses a tail call to the read function, the cached-settings reader, defaults or copy), and no cold
//     fragment of the exception table is chained to create: the only way to the call is through the save and then the reload.
//  A frame slot is also refused when a store in the region goes through a register that may hold a frame address ("marked": a
//  frame lea, its copies, two-register arithmetic / cmov / xchg with a marked source, arithmetic on it; a mark is dropped only by a
//  full-width overwrite or by a call for the volatile registers; computed by a forward may-analysis over the whole region, entry run
//  included, with merges at jump targets and a fixed point for loops), when an index register is marked, and in every slot case for
//  string stores and bit-string stores (they write where a register says); so is a frame address copied or stored. NOT tracked: a
//  frame address stored to memory and read back, an xmm carrier; hand-made code only.
inline Why ProveContext(const std::vector<x64::Insn>& insns, const std::vector<Function>& fns, uint32_t create, const Frame& f, uint32_t chainedFragment, uint32_t readCall, ContextFacts& out, uint32_t& failAt)
{
    using namespace x64;
    const int iCall = InsnIndex(insns, readCall);
    if (iCall < 3 || insns[iCall].flow != Flow::Call)
    {
        failAt = readCall;
        return Why::CreateDecode;
    }
    const Insn& movRcx = insns[iCall - 1];
    const Insn& reload = insns[iCall - 2];
    // 1. mov rcx,[rip+g] / mov rdx, X
    if (movRcx.twoByte || movRcx.opcode != 0x8B || !movRcx.rexW || !movRcx.hasModrm || movRcx.reg != RCX || !movRcx.mem.present || !movRcx.mem.ripRel || movRcx.len != 7)
    {
        failAt = movRcx.at;
        return Why::ReadCallSetup;
    }
    out.globalRead = static_cast<uint32_t>(static_cast<int64_t>(movRcx.at) + movRcx.len + movRcx.mem.disp);
    Loc want;
    if (!IsReloadToRdx(reload, f, want))
    {
        failAt = reload.at;
        return Why::ReadCallSetup;
    }

    // 2. the entry run and the saves in it
    const int iStart = InsnIndex(insns, create + 31); // the instruction after "sub rsp,imm32"
    if (iStart < 0)
    {
        failAt = create + 31;
        return Why::CreateDecode;
    }
    struct Save
    {
        Loc loc;
        int idx;
    };
    std::vector<Save> saves;
    for (int j = iStart; j < iCall; ++j)
    {
        const Insn& n = insns[j];
        Loc loc;
        if (IsSaveOfRdx(n, f, loc))
        {
            const bool survives = loc.slot ? (loc.off >= 0x20 && loc.off + 8 <= f.size) : CalleeSaved(loc.reg);
            if (survives)
                saves.push_back({ loc, j });
        }
        if ((n.gprWrites & (1u << RDX)) != 0)
            break; // rdx no longer holds the argument
        if (n.writesStack || (n.gprWrites & ((1u << RSP) | (1u << RBP))) != 0)
            break; // the frame moves: offsets would not be comparable
        if (n.flow != Flow::Next)
            break; // the end of the straight-line run (a call destroys rdx)
    }
    const Save* save = nullptr;
    for (const auto& s : saves)
        if (s.loc == want)
        {
            save = &s;
            break;
        }
    if (save == nullptr)
    {
        failAt = reload.at;
        return Why::CreateContext;
    }
    const int iSave = save->idx, iReload = iCall - 2;
    if (iSave >= iReload)
    {
        failAt = reload.at;
        return Why::CreateContext;
    }
    const uint32_t saveStart = insns[iSave].at, reloadStart = reload.at, callStart = readCall;
    const uint32_t createEnd = insns.back().at + insns.back().len;

    // a cold fragment chained to create runs code that the decode of create does not cover
    if (chainedFragment != 0)
    {
        failAt = chainedFragment;
        return Why::CreateContext;
    }

    // 4. control flow (two passes: first every target must be an instruction start, then the rules)
    for (const Insn& b : insns)
    {
        if (b.flow == Flow::JumpIndirect)
        {
            failAt = b.at;
            return Why::CreateDecode;
        }
        if ((b.flow == Flow::Jump || b.flow == Flow::CondJump) && b.target >= create && b.target < createEnd && InsnIndex(insns, static_cast<uint32_t>(b.target)) < 0)
        {
            failAt = b.at;
            return Why::CreateDecode;
        }
    }
    for (const Insn& b : insns)
    {
        const bool direct = b.flow == Flow::Jump || b.flow == Flow::CondJump || (b.flow == Flow::Call && b.target != -1);
        if (!direct)
            continue;
        if (b.target < create || b.target >= createEnd)
        {
            // leaving create: a call comes back. A jump out never does: it is allowed only as a tail call to the START of another
            // function (it cannot enter create again, and Locate refuses the starts that lead to the read call: the read function, the
            // cached-settings reader, defaults and copy); a jump to anywhere else could reach them without the reload
            if (b.flow != Flow::Call && (b.target < 0 || StartOf(fns, static_cast<uint32_t>(b.target)) == nullptr))
            {
                failAt = b.at;
                return Why::CreateContext;
            }
            continue;
        }
        if (b.flow == Flow::Call) // a call into create itself would run region code without the save
        {
            failAt = b.at;
            return Why::CreateContext;
        }
        const uint32_t t = static_cast<uint32_t>(b.target);
        const bool fromRegion = b.at >= saveStart && b.at <= reloadStart;
        if ((t >= create && t <= saveStart) || (!fromRegion && t >= saveStart && t <= reloadStart) || (t > reloadStart && t <= callStart))
        {
            failAt = b.at;
            return Why::CreateContext;
        }
    }

    // Registers that may hold a frame address before each instruction, computed from the end of the prologue (so a frame lea in the
    // entry run counts) to the reload: a forward may-analysis, union at jump targets, repeated to a fixed point (loops). A frame
    // location's address taken with lea marks its register; so do mov / lea / two-register add-sub-or-xor-and-adc-sbb-imul-cmov-xchg-xadd
    // with a marked source; arithmetic with an immediate and every other instruction leave the mark alone. A mark is dropped only
    // by a FULL-WIDTH overwrite (a 64/32-bit mov from an unmarked register or from memory or an immediate, movzx/movsx, pop, a zero
    // idiom, lea of unmarked registers) and, for the volatile registers, by a call; 8- and 16-bit writes keep it.
    const size_t regionLen = static_cast<size_t>(iReload - iStart + 1);
    std::vector<uint16_t> marks(regionLen, 0);
    if (want.slot)
    {
        constexpr uint16_t kVolatile = (1u << RAX) | (1u << RCX) | (1u << RDX) | (1u << R8) | (1u << R9) | (1u << R10) | (1u << R11);
        auto step = [&](uint16_t cur, const Insn& n) -> uint16_t
        {
            auto holds = [&](uint8_t r) { return r != kNoReg && (cur & (1u << r)) != 0; };
            const bool narrow = n.opsize16 && !n.rexW; // 16-bit operand: a partial write
            uint16_t next = cur;
            if (n.flow == Flow::Call)
                next = static_cast<uint16_t>(next & ~kVolatile);
            if (n.isLea && n.mem.present)
            {
                const bool frame = !n.mem.ripRel && (n.mem.base == RSP || n.mem.base == RBP);
                const bool derived = frame || holds(n.mem.ripRel ? kNoReg : n.mem.base) || holds(n.mem.index);
                if (derived)
                    next = static_cast<uint16_t>(next | (1u << n.reg));
                else if (!narrow)
                    next = static_cast<uint16_t>(next & ~(1u << n.reg));
                return next;
            }
            if (!n.hasModrm || n.mod != 3)
            {
                // no register source: only the definite full-width overwrites drop a mark
                bool kill = false;
                if (!n.twoByte)
                {
                    if ((n.opcode >= 0xB8 && n.opcode <= 0xBF) || (n.opcode >= 0x58 && n.opcode <= 0x5F) || n.opcode == 0x63)
                        kill = !narrow;
                    else if (n.opcode == 0x8B && n.mod != 3)
                        kill = !narrow;
                }
                else if ((n.opcode == 0xB6 || n.opcode == 0xB7 || n.opcode == 0xBE || n.opcode == 0xBF) && n.mod != 3)
                    kill = !narrow;
                if (kill)
                    next = static_cast<uint16_t>(next & ~n.gprWrites);
                // xchg r, rax (short form 91..97, and 90 with REX.B = xchg r8,rax: the decoder reports both registers in gprWrites; a plain
                // nop / pause has none): both may hold either value. The 32-bit form (41 90) carries too, like every 32-bit copy here.
                if (!n.twoByte && n.opcode >= 0x90 && n.opcode <= 0x97 && (cur & n.gprWrites) != 0)
                    next = static_cast<uint16_t>(next | n.gprWrites);
                return next;
            }
            // register forms (mod == 3): dst and src
            if (!n.twoByte)
            {
                const uint8_t op = n.opcode;
                if (op == 0x89 || op == 0x8B) // mov
                {
                    const uint8_t dst = (op == 0x89) ? n.rm : n.reg, src = (op == 0x89) ? n.reg : n.rm;
                    // rsp and rbp are frame addresses themselves: a copy of either (made before the save too, where nothing marks them)
                    // is marked. Only here: other forms name them as a value legitimately (the stack cookie's "xor rax,rsp").
                    if (holds(src) || src == RSP || src == RBP)
                        next = static_cast<uint16_t>(next | (1u << dst));
                    else if (!narrow)
                        next = static_cast<uint16_t>(next & ~(1u << dst));
                    return next;
                }
                if (op == 0x87 || op == 0x86) // xchg: both registers may now hold either value
                {
                    if (holds(n.reg) || holds(n.rm))
                        next = static_cast<uint16_t>(next | (1u << n.reg) | (1u << n.rm));
                    return next;
                }
                if (op < 0x40 && (op & 7) <= 3 && op < 0x38) // add or adc sbb and sub xor (not cmp)
                {
                    const uint8_t dst = ((op & 2) == 0) ? n.rm : n.reg, src = ((op & 2) == 0) ? n.reg : n.rm;
                    const bool zeroIdiom = (op == 0x31 || op == 0x33 || op == 0x29 || op == 0x2B) && n.reg == n.rm && !narrow;
                    if (zeroIdiom)
                        next = static_cast<uint16_t>(next & ~(1u << dst));
                    else if (holds(src))
                        next = static_cast<uint16_t>(next | (1u << dst));
                    return next;
                }
                if (op == 0x63) // movsxd r, r/m32: a copy of the source's low half
                {
                    if (holds(n.rm))
                        next = static_cast<uint16_t>(next | (1u << n.reg));
                    return next;
                }
                if (op == 0x6B || op == 0x69) // imul r, r/m, imm: the source reaches the destination
                {
                    if (holds(n.rm))
                        next = static_cast<uint16_t>(next | (1u << n.reg));
                    return next;
                }
                if (op == 0xC7 && n.ext == 0) // mov r, imm
                {
                    if (!narrow)
                        next = static_cast<uint16_t>(next & ~(1u << n.rm));
                    return next;
                }
                return next;
            }
            // two-byte register forms
            const uint8_t op = n.opcode;
            if ((op >= 0x40 && op <= 0x4F) || op == 0xAF) // cmovcc, imul r, r/m: the source reaches the destination
            {
                if (holds(n.rm))
                    next = static_cast<uint16_t>(next | (1u << n.reg));
                return next;
            }
            if (op == 0xB1) // cmpxchg r/m, r: on a match the source goes to the destination, otherwise the destination to rax (0F B0, the byte form, moves no address)
            {
                if (holds(n.reg))
                    next = static_cast<uint16_t>(next | (1u << n.rm));
                if (holds(n.rm))
                    next = static_cast<uint16_t>(next | (1u << RAX));
                return next;
            }
            if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) // shld / shrd r/m, r: bits of the source land in the destination
            {
                if (holds(n.reg))
                    next = static_cast<uint16_t>(next | (1u << n.rm));
                return next;
            }
            if (op == 0xC1 || op == 0xC0) // xadd: both registers
            {
                if (holds(n.reg) || holds(n.rm))
                    next = static_cast<uint16_t>(next | (1u << n.reg) | (1u << n.rm));
                return next;
            }
            if ((op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) && !narrow) // movzx / movsx: a small number
                next = static_cast<uint16_t>(next & ~(1u << n.reg));
            return next;
        };
        // forward worklist over the region
        std::vector<int> work;
        std::vector<bool> reached(regionLen, false);
        reached[0] = true;
        work.push_back(iStart);
        while (!work.empty())
        {
            const int j = work.back();
            work.pop_back();
            const Insn& n = insns[j];
            const uint16_t out = step(marks[static_cast<size_t>(j - iStart)], n);
            auto flowTo = [&](int k)
            {
                if (k < iStart || k > iReload)
                    return;
                const size_t at = static_cast<size_t>(k - iStart);
                const uint16_t merged = static_cast<uint16_t>(marks[at] | out);
                if (!reached[at] || merged != marks[at])
                {
                    reached[at] = true;
                    marks[at] = merged;
                    work.push_back(k);
                }
            };
            const bool falls = n.flow == Flow::Next || n.flow == Flow::CondJump || n.flow == Flow::Call;
            if (falls && j + 1 <= iReload)
                flowTo(j + 1);
            if ((n.flow == Flow::Jump || n.flow == Flow::CondJump) && n.target >= create)
            {
                const int k = InsnIndex(insns, static_cast<uint32_t>(n.target));
                if (k >= 0)
                    flowTo(k);
            }
        }
    }
    // 3. nothing in the region writes the saved place
    for (int j = iSave + 1; j <= iReload; ++j)
    {
        const Insn& n = insns[j];
        bool clobbered = false;
        if (!want.slot)
            clobbered = clobbered || (n.gprWrites & (1u << want.reg)) != 0;
        else
        {
            int64_t off = 0;
            if (n.aliasStore) // rep movs/stos and bts/btr/btc write where a register says
                clobbered = true;
            if (n.writesStack || (n.gprWrites & ((1u << RSP) | (1u << RBP))) != 0 || n.readsFrameRegAsValue)
                clobbered = true; // the frame moves, or its address is copied or stored somewhere
            if (n.isLea && n.mem.present && !n.mem.ripRel && (n.mem.base == RSP || n.mem.base == RBP))
                clobbered = clobbered || !FrameOffset(n.mem, f, off) || off < want.off + 8; // the address of the slot, or of something below it, is taken
            if (n.memWrite && n.mem.present && !n.mem.ripRel && (n.mem.base == RSP || n.mem.base == RBP))
                clobbered = clobbered || !FrameOffset(n.mem, f, off) || (off < want.off + 8 && off + n.memSize > want.off);
            else if (n.memWrite && n.mem.present && !n.mem.ripRel)
            {
                // a store through another register: refused when the base or the index may hold a frame address
                const uint16_t held = marks[static_cast<size_t>(j - iStart)];
                clobbered = clobbered || (n.mem.base != kNoReg && (held & (1u << n.mem.base)) != 0) || (n.mem.index != kNoReg && (held & (1u << n.mem.index)) != 0);
            }
        }
        if (clobbered)
        {
            failAt = n.at;
            return Why::CreateContext;
        }
    }

    out.loc = want;
    out.saveAt = saveStart;
    out.reloadAt = reloadStart;
    out.checked = static_cast<uint32_t>(iReload - iSave - 1);
    return Why::Ok;
}

// The hook forwards six pointer arguments and returns a 32-bit value (an NGX status). Proven here from the decoded code of create:
//  * no memory operand reaches a stack argument after the sixth: with the frame the prologue showed, the argument slots are at
//    entry-rsp + 28h (5th), 30h (6th), 38h (7th) ...; an access that touches the window of the 7th..14th slot is refused;
//  * the status is a 32-bit value: `mov eax, 0BAD0xxxxh` occurs in create (the error returns).
inline bool LooksLikeSixArgumentStatusFunction(const std::vector<x64::Insn>& insns, uint32_t create, const Frame& f)
{
    using namespace x64;
    bool seventh = false, status = false;
    for (const Insn& n : insns)
    {
        if (n.at < create + 31)
            continue;
        if (n.mem.present && !n.mem.ripRel && n.mem.index == kNoReg && (n.mem.base == RBP || n.mem.base == RSP))
        {
            const int64_t e = (n.mem.base == RBP) ? static_cast<int64_t>(n.mem.disp) + f.rbpDisp - 56 : static_cast<int64_t>(n.mem.disp) - f.size - 56;
            const int64_t bytes = n.memBytes != 0 ? n.memBytes : 1;
            if (e < 0x78 && e + bytes > 0x38)
                seventh = true;
        }
        if (!n.twoByte && n.opcode == 0xB8 && !n.rexW && n.immLen == 4 && n.gprWrites == (1u << RAX) && (static_cast<uint32_t>(n.imm) >> 16) == 0xBAD0)
            status = true;
    }
    return !seventh && status;
}} // namespace detail

inline Located Locate(const ImageView& view)
{
    using namespace detail;
    Located fail;
    auto no = [&](Why why)
    {
        fail.why = why;
        return fail;
    };

    if (view.bytes == nullptr || view.size < 0x100 || view.sections == nullptr || view.sectionCount == 0)
        return no(Why::OutOfImage);
    Scanner sc(view);
    std::vector<Function> fns;
    if (!LoadFunctions(sc, fns))
        return no(Why::NoPdata);

    // ---- 1. the cached-settings reader, by its unique call site ----------------------------------------------------------
    // lea rdx,[rsp+68h] / mov dword [rsp+68h],0 / mov ecx,10E41DF1h / call rel32 / test al,al
    constexpr uint8_t kReadCall[] = { 0x48, 0x8D, 0x54, 0x24, 0x68, 0xC7, 0x44, 0x24, 0x68, 0, 0, 0, 0, 0xB9, 0xF1, 0x1D, 0xE4, 0x10, 0xE8 };
    constexpr uint8_t kReaderPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x8B, 0xD9, 0xE8 };
    uint32_t readerCallSite = 0;
    int readerHits = 0;
    uint32_t readerTarget = 0;
    sc.ForEachExec(
        [&](uint32_t at)
        {
            if (!sc.Match(at, kReadCall) || !sc.InImage(at + 25, 1))
                return;
            const uint8_t* p = view.bytes + at;
            if (p[23] != 0x84 || p[24] != 0xC0)
                return;
            ++readerHits;
            readerCallSite = at;
            readerTarget = static_cast<uint32_t>(static_cast<int64_t>(at) + 23 + sc.I32(at + 19));
        });
    if (readerHits == 0)
        return no(Why::NoReader);
    if (readerHits > 1)
        return no(Why::ReaderNotUnique);
    if (!sc.Exec(readerTarget, sizeof(kReaderPrologue)) || !sc.Match(readerTarget, kReaderPrologue))
        return no(Why::ReaderPrologue);
    if (StartOf(fns, readerTarget) == nullptr)
        return no(Why::NotFunctionStart);

    // ---- 2. the read function: owner of the call site ---------------------------------------------------------------------
    const Function* readOwner = OwnerOf(fns, readerCallSite);
    constexpr uint8_t kReadFnPrologue[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40 };
    if (readOwner == nullptr || !sc.Match(readOwner->begin, kReadFnPrologue))
        return no(Why::ReadFnPrologue);
    const uint32_t readFn = readOwner->begin;

    // ---- 3. create: the unique caller of the read function ----------------------------------------------------------------
    uint32_t readCallInCreate = 0;
    int callers = 0;
    sc.ForEachExec(
        [&](uint32_t at)
        {
            if (view.bytes[at] != 0xE8 || !sc.InImage(at, 5))
                return;
            if (static_cast<int64_t>(at) + 5 + sc.I32(at + 1) != static_cast<int64_t>(readFn))
                return;
            ++callers;
            readCallInCreate = at;
        });
    if (callers == 0)
        return no(Why::NoCreate);
    if (callers > 1)
        return no(Why::CreateNotUnique);
    const Function* createOwner = OwnerOf(fns, readCallInCreate);
    if (createOwner == nullptr)
        return no(Why::NoCreate);
    const uint32_t create = createOwner->begin;
    const uint32_t createEnd = createOwner->end;
    if (createEnd - create > 0x10000)
        return no(Why::CreatePrologue);

    // push-heavy prologue, then lea rbp,[rsp-N] (N small, so the two high bytes are FF FF), sub rsp,M (M < 64 KiB)
    constexpr uint8_t kCreateHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24 };
    constexpr uint8_t kSubRsp[] = { 0x48, 0x81, 0xEC };
    if (!sc.Match(create, kCreateHead) || !sc.InImage(create + 31, 1))
        return no(Why::CreatePrologue);
    {
        const uint8_t* p = view.bytes + create;
        if (p[22] != 0xFF || p[23] != 0xFF) // high bytes of the negative displacement
            return no(Why::CreatePrologue);
        if (!sc.Match(create + 24, kSubRsp) || p[29] != 0 || p[30] != 0)
            return no(Why::CreatePrologue);
    }

    // ---- decode create, then prove its context argument and its signature ----------------------------------------------------
    if (createEnd > view.size || !sc.Exec(create, createEnd - create))
        return no(Why::CreatePrologue);
    Frame frame {};
    frame.rbpDisp = sc.I32(create + 20);
    frame.size = static_cast<int64_t>(sc.U32(create + 27));
    frame.rbpToRsp = frame.size + frame.rbpDisp;
    std::vector<x64::Insn> insns;
    insns.reserve((createEnd - create) / 3 + 16);
    for (uint32_t at = create; at < createEnd;)
    {
        x64::Insn n;
        if (!x64::DecodeAt(view.bytes, view.size, at, createEnd, n))
        {
            fail.failAt = at;
            return no(Why::CreateDecode);
        }
        insns.push_back(n);
        at += n.len;
    }
    ContextFacts ctx;
    {
        uint32_t failAt = 0;
        const Why proof = ProveContext(insns, fns, create, frame, ChainedFragmentOf(sc, create), readCallInCreate, ctx, failAt);
        if (proof != Why::Ok)
        {
            fail.failAt = failAt;
            return no(proof);
        }
    }
    if (!LooksLikeSixArgumentStatusFunction(insns, create, frame))
        return no(Why::CreateSignature);
    const int64_t globalRead = ctx.globalRead;
    // ---- 4. defaults and copy, inside create -----------------------------------------------------------------------------
    constexpr uint8_t kLeaRcxRbp40Call[] = { 0x48, 0x8D, 0x4D, 0x40, 0xE8 };
    constexpr uint8_t kDefaultsStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x20, 0x41, 0xBB, 0x03, 0, 0, 0 };
    constexpr uint8_t kCopyStart[] = { 0x8B, 0x02, 0x89, 0x01, 0x0F, 0xB7, 0x42, 0x04, 0x66, 0x89, 0x41, 0x04, 0x8B, 0x42, 0x14, 0x89, 0x41, 0x14 };
    constexpr uint8_t kMovRdxRip[] = { 0x48, 0x8B, 0x15 };
    uint32_t defaults = 0, copy = 0;
    int defaultsHits = 0, copyHits = 0;
    bool copyDiffers = false;
    uint32_t sites[2] {};
    Why late = Why::Ok;
    for (uint32_t at = create; at + 9 <= createEnd; ++at)
    {
        if (!sc.Match(at, kLeaRcxRbp40Call) || InsnIndex(insns, at) < 0)
            continue;
        const int64_t target64 = static_cast<int64_t>(at) + 9 + sc.I32(at + 5);
        if (target64 < 0 || target64 > static_cast<int64_t>(view.size))
            continue;
        const uint32_t target = static_cast<uint32_t>(target64);
        if (!sc.Exec(target, sizeof(kDefaultsStart)))
            continue;
        if (sc.Match(target, kDefaultsStart))
        {
            ++defaultsHits;
            defaults = target;
        }
        else if (sc.Match(target, kCopyStart))
        {
            if (copyHits > 0 && copy != target)
                copyDiffers = true;
            if (copyHits < 2)
            {
                // mov rdx,[rip+global] right before "lea rcx,[rbp+40h]"
                if (at < create + 7 || !sc.Match(at - 7, kMovRdxRip) || InsnIndex(insns, at - 7) < 0)
                    late = Why::CopySetup;
                else if (static_cast<int64_t>(at) - 7 + 7 + sc.I32(at - 4) != globalRead)
                    late = Why::GlobalMismatch;
                sites[copyHits] = at + 9;
            }
            ++copyHits;
            copy = target;
        }
    }
    if (defaultsHits != 1)
        return no(Why::DefaultsNotFound);
    if (copyHits != 2 || copyDiffers)
        return no(Why::CopySites);
    if (late != Why::Ok)
        return no(late);
    // defaults is a normal function; copy is a leaf (no exception-table entry) but must not sit inside another function
    if (StartOf(fns, defaults) == nullptr || InsideFunction(fns, copy))
        return no(Why::NotFunctionStart);
    // a tail call out of create (allowed by the proof of the context) must not go to a function that leads to the read call
    for (const x64::Insn& b : insns)
    {
        if ((b.flow != x64::Flow::Jump && b.flow != x64::Flow::CondJump) || (b.target >= create && b.target < createEnd))
            continue;
        const uint32_t t = static_cast<uint32_t>(b.target);
        if (t == readerTarget || t == readFn || t == defaults || t == copy)
        {
            fail.failAt = b.at;
            return no(Why::CreateContext);
        }
    }

    Located ok;
    ok.why = Why::Ok;
    ok.reader = readerTarget;
    ok.readFn = readFn;
    ok.create = create;
    ok.defaults = defaults;
    ok.copy = copy;
    ok.copySites[0] = sites[0];
    ok.copySites[1] = sites[1];
    ok.sharedGlobal = static_cast<uint32_t>(globalRead);
    ok.ctxInSlot = ctx.loc.slot;
    ok.ctxReg = ctx.loc.slot ? 0 : ctx.loc.reg;
    ok.ctxSlot = ctx.loc.slot ? static_cast<int32_t>(ctx.loc.off) : 0;
    ok.ctxSaveAt = ctx.saveAt;
    ok.ctxReloadAt = ctx.reloadAt;
    ok.ctxChecked = ctx.checked;
    ok.createInsns = static_cast<uint32_t>(insns.size());
    return ok;
}

// ---- which module to hook ----------------------------------------------------------------------------------------------------
struct Candidate
{
    std::string path;
    bool loaded; // still mapped with the identity we recorded
    bool passes; // every guard above passed on it
};

enum class ChoiceKind : uint8_t
{
    None,
    One,
    Ambiguous
};

struct Choice
{
    ChoiceKind kind = ChoiceKind::None;
    size_t index = 0;
};

inline bool IsDriverCopy(const std::string& path)
{
    std::string lower(path);
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("\\driverstore\\") != std::string::npos || lower.find("/driverstore/") != std::string::npos;
}

// One module that passed. When two pass, the driver's own copy (loaded for its version check) loses to the game's copy; two
// that are equal candidates are ambiguous and nothing is hooked.
inline Choice Choose(const std::vector<Candidate>& candidates)
{
    std::vector<size_t> eligible;
    for (size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].loaded && candidates[i].passes)
            eligible.push_back(i);
    if (eligible.empty())
        return {};
    if (eligible.size() == 1)
        return { ChoiceKind::One, eligible[0] };

    std::vector<size_t> game;
    for (size_t i : eligible)
        if (!IsDriverCopy(candidates[i].path))
            game.push_back(i);
    if (game.size() == 1)
        return { ChoiceKind::One, game[0] };
    return { ChoiceKind::Ambiguous, 0 };
}

// ---- file identity -------------------------------------------------------------------------------------------------------------
// SHA-256 of runtimes whose code shape was checked. The hash is informational (logged); the structural guards decide.
inline const char* KnownHashLabel(std::string_view sha256Hex)
{
    struct Known
    {
        const char* hex;
        const char* label;
    };
    static constexpr Known kKnown[] = {
        { "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82", "NVIDIA DLSS-G 310.9.1.0, code shape read in full with a decompiler" },
        { "5d5cbf14d2727d47f93fd10bf77bd91708ae122482a6f86fd564971641ebd47b", "NVIDIA DLSS-G 310.8.0, structural scan only" },
        { "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7", "NVIDIA DLSS-G 310.9.16 (Streamline 2.14.16), code shape read in full with a decompiler" },
        { "cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72", "NVIDIA DLSS-G 310.9.16 model-store build (ProgramData/NVIDIA/NGX), accepted by the guards; the live change worked with it in game" },
    };
    if (sha256Hex.size() != 64)
        return nullptr;
    std::string lower(sha256Hex);
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& k : kKnown)
        if (lower == k.hex)
            return k.label;
    return nullptr;
}

// The families whose code shape was checked: 310.8 and 310.9. Anything else is refused without looking further.
constexpr bool VersionAccepted(unsigned major, unsigned minor) { return major == 310 && (minor == 8 || minor == 9); }
} // namespace ext::fg::preset::guards
