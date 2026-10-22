// DLSS-G preset guards (ext/fg/dlssg/PresetGuards.h): the structural scan that finds NVIDIA's cached-settings reader and the
// endpoint create / configuration functions in a mapped nvngx_dlssg.dll image. Here against a synthetic image whose create function
// is a real instruction stream (positive case in the 310.9.1.0 shape and in the 310.9.16 shape, every guard broken one at a time,
// the context-argument proof with its negative images, the provider choice). The real DLL is the manual recipe
// fg_preset_guards_dll (needs the DLL path in OPTISCALER_DLSSG_DLL).
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetGuards.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ext::fg::preset::guards;

namespace
{
constexpr uint32_t kImageSize = 0x6000;
constexpr uint32_t kTextVa = 0x1000, kTextSize = 0x3000;
constexpr uint32_t kDataVa = 0x4000, kDataSize = 0x1000; // not executable: the shared configuration global, a cookie, an import
constexpr uint32_t kPdataVa = 0x5000, kPdataSize = 0x1000;

constexpr uint32_t kReader = 0x1100, kReadFn = 0x1200, kCreate = 0x1400, kDefaults = 0x2800, kCopy = 0x2900;
constexpr uint32_t kGlobal = 0x4100, kCookie = 0x4200, kImport = 0x4300;

// where the entry rdx (the context argument) is kept, and where it is reloaded from
enum class Where
{
    R13, Rbx, Rsi, Rdi, R12, R15,
    SlotRsp,  // [rsp+60h]  (the 310.9.16 build)
    SlotRbp,  // the same slot through rbp
    Volatile, // r8: not preserved across calls
    Home,     // [rsp+10h]: the callee-writable home area
    HomeEdge, // [rsp+18h]: still the home area (it ends at rsp+20h)
    Beyond,   // [rsp+frameSub]: just above the frame (the saved registers), not a local slot
    None
};

enum class Clobber
{
    None,
    // writes the saved register (any of them must be refused when the context sits in a register)
    RegMov, RegXor32, RegPop, RegXchg, RegByte, RegLea,
    // writes the saved slot (refused when the context sits in a slot)
    SlotQword, SlotViaRbp, SlotDword, SlotMovups, SlotByte, SlotLeaBelow, SlotLeaRbpBelow, MovRaxRsp, PushRax,
    // review 2 (M2/M3): aliasing stores, boundaries of the slot, frame addresses copied, string and bit-string stores
    AliasStore, SlotLeaAt60, SlotLeaAt64, SlotLeaAt67, SlotDwordAt5D, SlotIndexedStore, SlotLeaIndexed, WriteRbp, StoreRspToMem, MovqXmmRsp,
    LeaRbpIndex,
    // pointer tracking: a register that may hold a frame address (lea of a frame location, a copy of it, arithmetic on it)
    AliasViaMov, AliasViaArithmetic, AliasKilledByLoad, AliasKilledByCall, AliasKeptInCalleeSaved, NegDispUntainted, IndexedOtherBase,
    MovImmRbp, // mov ebp,1: writes rbp without reading it (no modrm byte, nothing else flags it)
    // review 3, M1/L3: the frame-address mark through two-register forms, partial writes, merges, loops, and what it must not lose
    MarkAddSource, MarkPartialAl, MarkPartialAx, MarkPartialMovAx, MarkPartialLoadAl, MarkBranchMerge, MarkLoopCarry, MarkKeptInR15, MarkKeptInRsi,
    MarkLeaFromBase, MarkLeaFromIndex, MarkMov8B, MarkIndexStore, MarkXchg, MarkCmov, StoreViaR12, MarkXchgShort, MarkPartialXor,
    // string and bit-string stores write where a register says: refused in every mode
    StringStore, BtsMem,
    // fix round 3 (L2): a full-width overwrite drops the mark, so a store through the register is no longer a store to the slot (accepted)
    KillMovImm, KillMovImmC7, KillMov8B, KillMov89, KillMov32, KillXorZero, KillSubZero, KillMovzxReg, KillMovzxMem, KillMovsxReg, KillMovsxdMem, KillLea,
    KillMerge,
    // fix round 3 (M1 items 1-3): carriers the mark pass missed. Each lea rax,[rbp-30h] image moves the address and stores through the copy.
    MarkXchgRexB, MarkXchg32RexB, NopNoMark, MarkImul6B, MarkImul69, NoMarkImul6B,
    MarkCmpxchg, MarkCmpxchgRax, NoMarkCmpxchg, MarkShld, MarkShrdCl, MarkMovsxd,
    // neighbours and unrelated writes that must NOT be refused
    NeighborAbove, NeighborBelow, MovupsAbove, LeaAbove, ScratchWrites
};

struct Spec
{
    // each field breaks one guard when changed from its default
    bool readerPrologueOk = true;
    int readCallCount = 1;       // copies of the read call pattern (1 = unique)
    bool afterCallTestOk = true; // the "test al,al" after the reader call
    bool readFnPrologueOk = true;
    int createCallers = 1;       // calls to the read function (1 = unique)
    bool createPrologueOk = true;
    uint32_t frameDisp = 0xFFFFFCA0; // lea rbp,[rsp+disp] (310.9.1.0 has -0x360, 310.9.16 -0x390)
    uint32_t frameSub = 0x460;
    bool statusConstant = true; // mov eax,0BAD0xxxxh somewhere in create: the 32-bit NGX status return
    bool sixthArg = true;       // a load of the 6th (last) stack argument is allowed
    bool seventhArgRbp = false; // [rbp+disp32] that reaches the 7th stack argument
    bool seventhArgRsp = false; // [rsp+disp32] that reaches the 7th stack argument
    bool falsePositiveJnz = true; // a jnz rel32 whose 32-bit displacement equals a 7th-argument rbp displacement (0F 85 .. is a jcc)
    bool defaultsOk = true;
    int defaultsHits = 1;
    bool copyOk = true;
    int copySites = 2;
    uint32_t copySiteGlobal = kGlobal; // global read before the second copy
    bool copySetupOk = true;
    bool pdataOk = true;
    bool copyInsideOtherFunction = false; // the real copy function is a leaf: it has no exception-table entry

    // the context argument
    Where save = Where::R13;
    int reloadFrom = -1;        // -1: reloaded from where it was saved; else a Where value
    bool saveFormMov89 = false; // encode the register save as 89 /r instead of 8B /r
    bool callBeforeSave = false;
    bool rdxWrittenBeforeSave = false; // xor edx,edx before the save: rdx no longer holds the argument
    bool pushBeforeSave = false;       // push rax after the prologue: the frame offsets would shift
    Clobber clobber = Clobber::None;
    bool movRcxOk = true;       // mov rcx,[rip+global] right before the read call
    bool movRcxFromFrame = false; // mov rcx,[rbp+disp32] there instead: same length, not a global

    // control flow around the region between the save and the reload
    bool jumpBackIntoRegion = false;
    bool jumpIntoSetup = false;
    bool jumpToEntry = false;
    bool jumpToSave = false;
    bool jumpMidInstruction = false;
    bool indirectJump = false;
    bool unknownOpcode = false;
    // review 2 (M2/M3)
    bool jumpToCall = false;           // jmp from after the sites to the read call itself
    bool jumpToReload = false;         // jmp from outside to the reload instruction
    bool jumpBackToSaveInside = false; // jz inside the region back to the save
    bool branchBeforeSave = false;     // test eax,eax / jz far, before the save
    bool callIntoFunction = false;     // call whose target is inside create (the middle of the region)
    bool callIntoTail = false;         // call whose target is inside create, after the read call (not in the region)
    int leavingJump = 0;               // 1: jmp out of create to a place that is no function start, 2: to the start of a function (a tail call)
    bool chainedFragment = false;      // a cold fragment in the exception table chained to create
    bool lea12BeforeSave = false;      // lea r12,[rbp-30h] in the entry run, before the save
    bool chainedOdd = false;           // the chained fragment's unwind info has an odd number of codes
    bool hiddenDefaultsCall = false;   // the bytes of "lea rcx,[rbp+40h] / call defaults" inside an immediate (not an instruction)
    bool hiddenCopySetup = false;      // the second copy site's "mov rdx,[rip+g]" is only bytes inside an immediate
    bool chainedPointer = false;       // a cold fragment chained to create through the low-bit pointer form
    // signature proof
    bool wideLoadAtSixth = false;      // movups xmm0,[rbp+...] over the 6th argument: 16 bytes reach the 7th
    bool loadAt14th = false;           // mov rax,[rbp+...] at entry rsp+70h
    bool loadAt15th = false;           // ... at entry rsp+78h (outside the window)
    bool statusDecoyHigh = false;      // mov eax,0BAD10002h only
};

struct Code
{
    std::vector<uint8_t>& im;
    uint32_t at;
    void b(std::initializer_list<int> bytes)
    {
        for (int v : bytes)
            im[at++] = static_cast<uint8_t>(v);
    }
    void i32(int32_t v)
    {
        std::memcpy(&im[at], &v, 4);
        at += 4;
    }
    void rel32(uint32_t target) { i32(static_cast<int32_t>(target) - static_cast<int32_t>(at + 4)); }
    void rip(uint32_t target, int tail = 0) { i32(static_cast<int32_t>(target) - static_cast<int32_t>(at + 4 + static_cast<uint32_t>(tail))); }
};

struct Fake
{
    std::vector<uint8_t> image;
    std::vector<Section> sections;
    uint32_t sites[2] {};
    uint32_t saveAt = 0, reloadAt = 0, movRcxAt = 0, callAt = 0, midRegionAt = 0, epilogueAt = 0, createEnd = 0;
    uint32_t pdataSize = kPdataSize;

    ImageView View() const
    {
        return { image.data(), image.size(), sections.data(), sections.size(), kPdataVa, pdataSize };
    }
};

int RegOf(Where w)
{
    switch (w)
    {
    case Where::R13: return 13;
    case Where::Rbx: return 3;
    case Where::Rsi: return 6;
    case Where::Rdi: return 7;
    case Where::R12: return 12;
    case Where::R15: return 15;
    case Where::Volatile: return 8;
    default: return -1;
    }
}

void EmitSave(Code& c, Where w, const Spec& s, int32_t rbpToRsp)
{
    const int r = RegOf(w);
    if (r >= 0)
    {
        if (s.saveFormMov89) // mov r/m64, rdx
            c.b({ 0x48 | (r >= 8 ? 1 : 0), 0x89, 0xC0 | (2 << 3) | (r & 7) });
        else // mov r64, rdx
            c.b({ 0x48 | (r >= 8 ? 4 : 0), 0x8B, 0xC0 | ((r & 7) << 3) | 2 });
        return;
    }
    switch (w)
    {
    case Where::SlotRsp: c.b({ 0x48, 0x89, 0x54, 0x24, 0x60 }); break;
    case Where::SlotRbp: c.b({ 0x48, 0x89, 0x95 }); c.i32(0x60 - rbpToRsp); break;
    case Where::Home: c.b({ 0x48, 0x89, 0x54, 0x24, 0x10 }); break;
    case Where::HomeEdge: c.b({ 0x48, 0x89, 0x54, 0x24, 0x18 }); break;
    case Where::Beyond: c.b({ 0x48, 0x89, 0x94, 0x24 }); c.i32(static_cast<int32_t>(s.frameSub)); break;
    default: c.b({ 0x90 }); break;
    }
}

void EmitReload(Code& c, Where w, int32_t rbpToRsp, uint32_t frameSub)
{
    const int r = RegOf(w);
    if (r >= 0)
    {
        c.b({ 0x48 | (r >= 8 ? 1 : 0), 0x8B, 0xC0 | (2 << 3) | (r & 7) }); // mov rdx, reg
        return;
    }
    switch (w)
    {
    case Where::SlotRsp: c.b({ 0x48, 0x8B, 0x54, 0x24, 0x60 }); break;
    case Where::SlotRbp: c.b({ 0x48, 0x8B, 0x95 }); c.i32(0x60 - rbpToRsp); break;
    case Where::Home: c.b({ 0x48, 0x8B, 0x54, 0x24, 0x10 }); break;
    case Where::HomeEdge: c.b({ 0x48, 0x8B, 0x54, 0x24, 0x18 }); break;
    case Where::Beyond: c.b({ 0x48, 0x8B, 0x94, 0x24 }); c.i32(static_cast<int32_t>(frameSub)); break;
    default: c.b({ 0x48, 0x8B, 0xD1 }); break; // mov rdx, rcx
    }
}

void EmitClobber(Code& c, Clobber k, Where w, int32_t rbpToRsp)
{
    const int r = RegOf(w) >= 0 ? RegOf(w) : 13;
    switch (k)
    {
    case Clobber::None: break;
    case Clobber::RegMov: c.b({ 0x48 | (r >= 8 ? 1 : 0), 0x89, 0xC0 | (r & 7) }); break; // mov reg, rax
    case Clobber::RegXor32:
        if (r >= 8)
            c.b({ 0x45, 0x33, 0xC0 | ((r & 7) << 3) | (r & 7) });
        else
            c.b({ 0x33, 0xC0 | (r << 3) | r });
        break;
    case Clobber::RegPop:
        if (r >= 8)
            c.b({ 0x41 });
        c.b({ 0x58 + (r & 7) });
        break;
    case Clobber::RegXchg: c.b({ 0x48 | (r >= 8 ? 1 : 0), 0x87, 0xC0 | (r & 7) }); break; // xchg reg, rax
    case Clobber::RegByte: c.b({ 0x40 | (r >= 8 ? 1 : 0), 0x88, 0xC0 | (r & 7) }); break; // mov reg8, al
    case Clobber::RegLea: c.b({ 0x48 | (r >= 8 ? 4 : 0), 0x8D, 0x40 | ((r & 7) << 3), 0x08 }); break; // lea reg,[rax+8]
    case Clobber::SlotQword: c.b({ 0x48, 0x89, 0x44, 0x24, 0x60 }); break;
    case Clobber::SlotViaRbp: c.b({ 0x48, 0x89, 0x85 }); c.i32(0x60 - rbpToRsp); break;
    case Clobber::SlotDword: c.b({ 0x89, 0x44, 0x24, 0x64 }); break;
    case Clobber::SlotMovups: c.b({ 0x0F, 0x11, 0x44, 0x24, 0x58 }); break; // 16 bytes at 58h..68h
    case Clobber::SlotByte: c.b({ 0x88, 0x44, 0x24, 0x67 }); break;
    case Clobber::SlotLeaBelow: c.b({ 0x48, 0x8D, 0x4C, 0x24, 0x50 }); break;
    case Clobber::SlotLeaRbpBelow: c.b({ 0x48, 0x8D, 0x8D }); c.i32(0x50 - rbpToRsp); break;
    case Clobber::MovRaxRsp: c.b({ 0x48, 0x8B, 0xC4 }); break;
    case Clobber::PushRax: c.b({ 0x50 }); break;
    case Clobber::AliasStore:
        c.b({ 0x48, 0x8D, 0x45, 0xD0 }); // lea rax,[rbp-30h]: a frame address above the slot (allowed)
        c.b({ 0x4C, 0x89, 0x60, static_cast<int>(static_cast<uint8_t>(static_cast<int8_t>(0x60 - (rbpToRsp - 0x30)))) }); // mov [rax-70h],r12 = the slot
        break;
    case Clobber::AliasViaMov:
        c.b({ 0x48, 0x8D, 0x45, 0xD0 });                // lea rax,[rbp-30h]
        c.b({ 0x48, 0x89, 0xC2 });                      // mov rdx,rax
        c.b({ 0x4C, 0x89, 0x62, static_cast<int>(static_cast<uint8_t>(static_cast<int8_t>(0x60 - (rbpToRsp - 0x30)))) }); // mov [rdx-70h],r12
        break;
    case Clobber::AliasViaArithmetic:
        c.b({ 0x48, 0x8D, 0x45, 0xD0 });                // lea rax,[rbp-30h]
        c.b({ 0x48, 0x83, 0xE8, static_cast<int>(static_cast<uint8_t>(rbpToRsp - 0x30 - 0x60)) }); // sub rax,70h: now the slot
        c.b({ 0x4C, 0x89, 0x20 });                      // mov [rax],r12
        break;
    case Clobber::AliasKilledByLoad:
        c.b({ 0x48, 0x8D, 0x45, 0xD0 });                // lea rax,[rbp-30h]
        c.b({ 0x48, 0x8B, 0x01 });                      // mov rax,[rcx]: not a frame address any more
        c.b({ 0x4C, 0x89, 0x60, static_cast<int>(static_cast<uint8_t>(static_cast<int8_t>(0x60 - (rbpToRsp - 0x30)))) }); // mov [rax-70h],r12
        break;
    case Clobber::AliasKilledByCall:
        c.b({ 0x48, 0x8D, 0x45, 0xD0 });                // lea rax,[rbp-30h]
        c.b({ 0xFF, 0x15 });
        c.rip(kImport);                                 // call: rax is volatile
        c.b({ 0x4C, 0x89, 0x60, static_cast<int>(static_cast<uint8_t>(static_cast<int8_t>(0x60 - (rbpToRsp - 0x30)))) });
        break;
    case Clobber::AliasKeptInCalleeSaved:
        c.b({ 0x48, 0x8D, 0x5D, 0xD0 });                // lea rbx,[rbp-30h]
        c.b({ 0xFF, 0x15 });
        c.rip(kImport);                                 // call: rbx survives
        c.b({ 0x4C, 0x89, 0x63, static_cast<int>(static_cast<uint8_t>(static_cast<int8_t>(0x60 - (rbpToRsp - 0x30)))) }); // mov [rbx-70h],r12
        break;
    case Clobber::NegDispUntainted: c.b({ 0x4C, 0x89, 0x67, 0xF0 }); break;  // mov [rdi-10h],r12: rdi never held a frame address
    case Clobber::IndexedOtherBase: c.b({ 0x4C, 0x89, 0x24, 0xC1 }); break;  // mov [rcx+rax*8],r12
    case Clobber::MovImmRbp: c.b({ 0xBD, 0x01, 0x00, 0x00, 0x00 }); break;   // mov ebp,1
    case Clobber::MarkAddSource: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x33, 0xC9, 0x48, 0x03, 0xC8, 0x4C, 0x89, 0x61, 0x90 }); break; // lea rax / xor ecx,ecx / add rcx,rax / mov [rcx-70h],r12
    case Clobber::MarkPartialAl: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0xB0, 0x10, 0x4C, 0x89, 0x60, 0x90 }); break;                  // lea rax / mov al,10h / mov [rax-70h],r12
    case Clobber::MarkPartialAx: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x66, 0xB8, 0x34, 0x12, 0x4C, 0x89, 0x60, 0x90 }); break;      // mov ax,1234h
    case Clobber::MarkPartialMovAx: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x66, 0x89, 0xC8, 0x4C, 0x89, 0x60, 0x90 }); break;          // mov ax,cx
    case Clobber::MarkPartialLoadAl: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x8A, 0x01, 0x4C, 0x89, 0x60, 0x90 }); break;               // mov al,[rcx]
    case Clobber::MarkBranchMerge: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x4D, 0x85, 0xC0, 0x74, 0x02, 0x33, 0xC0, 0x4C, 0x89, 0x60, 0x90 }); break; // jz over xor eax,eax
    case Clobber::MarkLoopCarry: c.b({ 0x4C, 0x89, 0x60, 0x90, 0x48, 0x8D, 0x45, 0xD0, 0x74, 0xF6 }); break;                  // store through rax, lea rax, jz back
    case Clobber::MarkKeptInR15:
        c.b({ 0x4C, 0x8D, 0x7D, 0xD0, 0xFF, 0x15 });
        c.rip(kImport);                                                                                                           // lea r15 / call
        c.b({ 0x4D, 0x89, 0x67, 0x90 });                                                                                          // mov [r15-70h],r12
        break;
    case Clobber::MarkKeptInRsi:
        c.b({ 0x48, 0x8D, 0x75, 0xD0, 0xFF, 0x15 });
        c.rip(kImport);
        c.b({ 0x4C, 0x89, 0x66, 0x90 });                                                                                          // mov [rsi-70h],r12
        break;
    case Clobber::MarkLeaFromBase: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x8D, 0x48, 0x08, 0x4C, 0x89, 0x61, 0x90 }); break;     // lea rcx,[rax+8]
    case Clobber::MarkLeaFromIndex: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x8D, 0x0C, 0x02, 0x4C, 0x89, 0x61, 0x90 }); break;    // lea rcx,[rdx+rax]
    case Clobber::MarkMov8B: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x8B, 0xC8, 0x4C, 0x89, 0x61, 0x90 }); break;                 // mov rcx,rax (8B form)
    case Clobber::MarkIndexStore: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x4C, 0x89, 0x24, 0xC1 }); break;                              // mov [rcx+rax*8],r12, rax marked
    case Clobber::MarkXchg: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x87, 0xC1, 0x4C, 0x89, 0x61, 0x90 }); break;                  // xchg rcx,rax
    case Clobber::MarkCmov: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x0F, 0x44, 0xC8, 0x4C, 0x89, 0x61, 0x90 }); break;            // cmovz rcx,rax
    case Clobber::MarkXchgShort: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x91, 0x4C, 0x89, 0x61, 0x90 }); break;                    // xchg rcx,rax (short form)
    case Clobber::MarkPartialXor: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x66, 0x31, 0xC0, 0x4C, 0x89, 0x60, 0x90 }); break;            // xor ax,ax: a partial zero
    case Clobber::StoreViaR12: c.b({ 0x4D, 0x89, 0x6C, 0x24, 0x90 }); break;                                                    // mov [r12-70h],r13
    // lea rax,[rbp-30h] / <a full-width overwrite of rax> / mov [rax-70h],r12 (the slot if rax still held the frame address)
    case Clobber::KillMovImm: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0xB8, 0x10, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x60, 0x90 }); break;                        // mov eax,10h
    case Clobber::KillMovImmC7: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0xC7, 0xC0, 0x10, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x60, 0x90 }); break;          // mov rax,10h (C7)
    case Clobber::KillMov8B: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x8B, 0xC1, 0x4C, 0x89, 0x60, 0x90 }); break;                                    // mov rax,rcx (8B)
    case Clobber::KillMov89: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x89, 0xC8, 0x4C, 0x89, 0x60, 0x90 }); break;                                    // mov rax,rcx (89)
    case Clobber::KillMov32: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x89, 0xC8, 0x4C, 0x89, 0x60, 0x90 }); break;                                          // mov eax,ecx
    case Clobber::KillXorZero: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x33, 0xC0, 0x4C, 0x89, 0x60, 0x90 }); break;                                        // xor eax,eax
    case Clobber::KillSubZero: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x2B, 0xC0, 0x4C, 0x89, 0x60, 0x90 }); break;                                  // sub rax,rax
    case Clobber::KillMovzxReg: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x0F, 0xB6, 0xC1, 0x4C, 0x89, 0x60, 0x90 }); break;                                 // movzx eax,cl
    case Clobber::KillMovzxMem: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x0F, 0xB6, 0x01, 0x4C, 0x89, 0x60, 0x90 }); break;                                 // movzx eax,byte [rcx]
    case Clobber::KillMovsxReg: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x0F, 0xBE, 0xC1, 0x4C, 0x89, 0x60, 0x90 }); break;                                 // movsx eax,cl
    case Clobber::KillMovsxdMem: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x63, 0x01, 0x4C, 0x89, 0x60, 0x90 }); break;                                // movsxd rax,dword [rcx]
    case Clobber::KillLea: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x8D, 0x41, 0x08, 0x4C, 0x89, 0x60, 0x90 }); break;                                // lea rax,[rcx+8] (rcx holds no frame address)
    case Clobber::KillMerge:                                                                                                                   // both paths overwrite rax
        c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x4D, 0x85, 0xC0, 0x74, 0x04, 0x33, 0xC0, 0xEB, 0x05, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x60, 0x90 });
        break;                                                                                                                                 // test r8,r8 / jz L1 / xor eax,eax / jmp L2 / L1: mov eax,1 / L2: store
    case Clobber::MarkXchgRexB: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x49, 0x90, 0x4D, 0x89, 0x60, 0x90 }); break;                                      // xchg rax,r8 (REX.W+B 90) / mov [r8-70h],r12
    case Clobber::MarkXchg32RexB: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x41, 0x90, 0x4D, 0x89, 0x60, 0x90 }); break;                                    // xchg r8d,eax (32-bit form)
    case Clobber::NopNoMark: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x90, 0x4D, 0x89, 0x60, 0x90 }); break;                                               // nop / mov [r8-70h],r12: r8 never held a frame address
    case Clobber::MarkImul6B: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x6B, 0xC8, 0x01, 0x4C, 0x89, 0x61, 0x90 }); break;                              // imul rcx,rax,1 / mov [rcx-70h],r12
    case Clobber::MarkImul69: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x69, 0xC8, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x61, 0x90 }); break;            // imul rcx,rax,1 (imm32)
    case Clobber::NoMarkImul6B: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x6B, 0xC9, 0x01, 0x4C, 0x89, 0x61, 0x90 }); break;                           // imul rcx,rcx,1: rcx never held the address
    case Clobber::MarkCmpxchg:                                                                                                                  // lea rdx / xor eax,eax / xor ecx,ecx / cmpxchg rcx,rdx (equal: rcx = rdx) / store via rcx
        c.b({ 0x48, 0x8D, 0x55, 0xD0, 0x33, 0xC0, 0x33, 0xC9, 0x48, 0x0F, 0xB1, 0xD1, 0x4C, 0x89, 0x61, 0x90 });
        break;
    case Clobber::MarkCmpxchgRax: c.b({ 0x48, 0x8D, 0x4D, 0xD0, 0x48, 0x0F, 0xB1, 0xD1, 0x4C, 0x89, 0x60, 0x90 }); break;                       // lea rcx / cmpxchg rcx,rdx (not equal: rax = rcx) / store via rax
    case Clobber::NoMarkCmpxchg: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x33, 0xC9, 0x48, 0x0F, 0xB1, 0xD1, 0x4C, 0x89, 0x61, 0x90 }); break;            // rdx and rcx never held the address
    case Clobber::MarkShld: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x33, 0xC9, 0x48, 0x0F, 0xA4, 0xC1, 0x01, 0x4C, 0x89, 0x61, 0x90 }); break;          // shld rcx,rax,1 into a zeroed rcx
    case Clobber::MarkShrdCl: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x33, 0xC9, 0x48, 0x0F, 0xAD, 0xC1, 0x4C, 0x89, 0x61, 0x90 }); break;              // shrd rcx,rax,cl
    case Clobber::MarkMovsxd: c.b({ 0x48, 0x8D, 0x45, 0xD0, 0x48, 0x63, 0xC8, 0x4C, 0x89, 0x61, 0x90 }); break;                                // movsxd rcx,eax
    case Clobber::SlotLeaAt60: c.b({ 0x48, 0x8D, 0x4C, 0x24, 0x60 }); break;
    case Clobber::SlotLeaAt64: c.b({ 0x48, 0x8D, 0x4C, 0x24, 0x64 }); break;
    case Clobber::SlotLeaAt67: c.b({ 0x48, 0x8D, 0x4C, 0x24, 0x67 }); break;
    case Clobber::SlotDwordAt5D: c.b({ 0x89, 0x44, 0x24, 0x5D }); break;       // 5Dh..61h: overlaps byte 60h only
    case Clobber::SlotIndexedStore: c.b({ 0x48, 0x89, 0x4C, 0xC4, 0x58 }); break; // mov [rsp+rax*8+58h],rcx
    case Clobber::SlotLeaIndexed: c.b({ 0x48, 0x8D, 0x4C, 0xC4, 0x70 }); break;  // lea rcx,[rsp+rax*8+70h]
    case Clobber::WriteRbp: c.b({ 0x48, 0x89, 0xC5 }); break;                    // mov rbp,rax
    case Clobber::StoreRspToMem: c.b({ 0x48, 0x89, 0x60, 0xD5 }); break;         // mov [rax-2Bh],rsp
    case Clobber::MovqXmmRsp: c.b({ 0x66, 0x48, 0x0F, 0x6E, 0xE4 }); break;      // movq xmm4,rsp
    case Clobber::LeaRbpIndex: c.b({ 0x48, 0x8D, 0x04, 0x29 }); break;           // lea rax,[rcx+rbp*1]
    case Clobber::StringStore: c.b({ 0xF3, 0x48, 0xAB }); break;                  // rep stosq
    case Clobber::BtsMem: c.b({ 0x48, 0x0F, 0xAB, 0x04, 0x24 }); break;          // bts [rsp],rax
    case Clobber::NeighborAbove: c.b({ 0x48, 0x89, 0x44, 0x24, 0x68 }); break;  // qword at 68h..70h
    case Clobber::NeighborBelow: c.b({ 0x89, 0x44, 0x24, 0x5C }); break;        // dword at 5Ch..60h
    case Clobber::MovupsAbove: c.b({ 0x0F, 0x11, 0x44, 0x24, 0x68 }); break;    // 16 bytes at 68h..78h
    case Clobber::LeaAbove: c.b({ 0x48, 0x8D, 0x4C, 0x24, 0x68 }); break;       // lea rcx,[rsp+68h]
    case Clobber::ScratchWrites:
        c.b({ 0x48, 0x89, 0xC1 });                   // mov rcx, rax
        c.b({ 0x45, 0x33, 0xDB });                   // xor r11d, r11d
        c.b({ 0x4C, 0x89, 0x45, 0x18 });             // mov [rbp+18h], r8
        c.b({ 0x0F, 0x29, 0x45, 0x30 });             // movaps [rbp+30h], xmm0 (a far frame location)
        break;
    }
}

Fake Build(const Spec& s)
{
    Fake f;
    f.image.assign(kImageSize, 0xCC);
    f.sections = { { kTextVa, kTextSize, true }, { kDataVa, kDataSize, false }, { kPdataVa, kPdataSize, false } };
    auto& im = f.image;
    const int32_t rbpToRsp = static_cast<int32_t>(s.frameSub) + static_cast<int32_t>(s.frameDisp);

    // reader
    if (s.readerPrologueOk)
        Code { im, kReader }.b({ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x8B, 0xD9, 0xE8 });
    else
        Code { im, kReader }.b({ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xFA, 0x8B, 0xD9, 0xE8 });
    {
        Code c { im, kReader + 20 };
        c.rel32(kReader + 0x80);
    }

    // the read function (EndpointConfiguration::ReadRegkeys): prologue, then the reader call pattern once or more
    if (s.readFnPrologueOk)
        Code { im, kReadFn }.b({ 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40 });
    else
        Code { im, kReadFn }.b({ 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x48 });
    for (int i = 0; i < s.readCallCount; ++i)
    {
        Code c { im, kReadFn + 0x20 + static_cast<uint32_t>(i) * 0x20 };
        c.b({ 0x48, 0x8D, 0x54, 0x24, 0x68, 0xC7, 0x44, 0x24, 0x68, 0, 0, 0, 0, 0xB9, 0xF1, 0x1D, 0xE4, 0x10, 0xE8 });
        c.rel32(kReader);
        c.b({ 0x84, 0xC0 }); // test al,al
        if (!s.afterCallTestOk)
            im[c.at - 2] = 0x85;
    }

    // ---- the endpoint create function: a real instruction stream -------------------------------------------------------------
    Code c { im, kCreate };
    std::vector<uint32_t> toEpilogue; // rel32 fields (of a jmp / jcc) that must reach the epilogue
    if (s.createPrologueOk)
        c.b({ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24 });
    else
        c.b({ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x25 });
    c.i32(static_cast<int32_t>(s.frameDisp));
    c.b({ 0x48, 0x81, 0xEC });
    c.i32(static_cast<int32_t>(s.frameSub));
    c.b({ 0x48, 0x8B, 0x05 });  c.rip(kCookie);        // mov rax,[rip+cookie]
    c.b({ 0x48, 0x33, 0xC4 });                          // xor rax,rsp
    c.b({ 0x48, 0x89, 0x85 });  c.i32(0x350);           // mov [rbp+350h],rax
    c.b({ 0x4C, 0x89, 0x4D, 0x28 });                    // mov [rbp+28h],r9
    if (s.callBeforeSave)
    {
        c.b({ 0xFF, 0x15 });
        c.rip(kImport); // call [rip+import]: rdx is gone after this
    }
    if (s.branchBeforeSave)
    {
        c.b({ 0x85, 0xC0, 0x0F, 0x84 }); // test eax,eax / jz far (to the epilogue): the straight-line run ends here
        toEpilogue.push_back(c.at);
        c.i32(0);
    }
    if (s.lea12BeforeSave)
        c.b({ 0x4C, 0x8D, 0x65, 0xD0 }); // lea r12,[rbp-30h]
    if (s.rdxWrittenBeforeSave)
        c.b({ 0x33, 0xD2 }); // xor edx,edx
    if (s.pushBeforeSave)
        c.b({ 0x50 }); // push rax
    f.saveAt = c.at;
    EmitSave(c, s.save, s, rbpToRsp);
    c.b({ 0x4C, 0x8B, 0xD1 }); // mov r10, rcx
    {
        const int32_t d = static_cast<int32_t>(s.frameDisp);
        if (s.sixthArg)
        {
            c.b({ 0x48, 0x8B, 0x85 });            // mov rax,[rbp+disp32]  -> entry rsp + 30h, the 6th argument
            c.i32(56 - d + 0x30);
        }
        c.b({ 0x48, 0x89, 0x44, 0x24, 0x58 });    // mov [rsp+58h], rax: the neighbour just below the 310.9.16 slot
        if (s.seventhArgRbp)
        {
            c.b({ 0x48, 0x8B, 0x85 });
            c.i32(56 - d + 0x38);                 // entry rsp + 38h, the 7th
        }
        if (s.wideLoadAtSixth)
        {
            c.b({ 0x0F, 0x10, 0x85 }); // movups xmm0,[rbp+disp32] at entry rsp+30h: 16 bytes
            c.i32(56 - d + 0x30);
        }
        if (s.loadAt14th)
        {
            c.b({ 0x48, 0x8B, 0x85 });
            c.i32(56 - d + 0x70);
        }
        if (s.loadAt15th)
        {
            c.b({ 0x48, 0x8B, 0x85 });
            c.i32(56 - d + 0x78);
        }
        if (s.seventhArgRsp)
        {
            c.b({ 0x48, 0x8B, 0x84, 0x24 });
            c.i32(static_cast<int32_t>(s.frameSub) + 56 + 0x38);
        }
    }
    c.b({ 0xFF, 0x15 });
    c.rip(kImport);               // call [rip+import]  (a call between the save and the reload is fine for callee-saved places)
    c.b({ 0x85, 0xC0 });          // test eax,eax
    c.b({ 0x74, 0x0A });          // jz +10 (over the error return)
    if (s.statusDecoyHigh)
        c.b({ 0xB8, 0x02, 0x00, 0xD1, 0xBA }); // mov eax,0BAD10002h: not an NGX status
    else if (s.statusConstant)
        c.b({ 0xB8, 0x02, 0x00, 0xD0, 0xBA }); // mov eax,0BAD0002h
    else
        c.b({ 0xB8, 0x02, 0x00, 0x00, 0x00 }); // a mov eax,2 that is not an NGX status
    c.b({ 0xE9 });
    toEpilogue.push_back(c.at);
    c.i32(0);                     // jmp epilogue
    EmitClobber(c, s.clobber, s.save, rbpToRsp);
    if (s.unknownOpcode)
        c.b({ 0x0F, 0x38, 0x00, 0xC1 }); // pshufb xmm0, xmm1: not in the decoder's table
    if (s.hiddenDefaultsCall)
    {
        // mov rax, imm64 whose bytes are "lea rcx,[rbp+40h] / call defaults" (the fourth byte of the rel32 is the 00 of the next
        // instruction): the pattern is there, but no instruction starts on it
        c.b({ 0x48, 0xB8 });
        const uint32_t at = c.at;
        const int32_t rel = static_cast<int32_t>(kDefaults) - static_cast<int32_t>(at + 9);
        c.b({ 0x48, 0x8D, 0x4D, 0x40, 0xE8, rel & 0xFF, (rel >> 8) & 0xFF, (rel >> 16) & 0xFF });
        c.b({ 0x00, 0xC0 }); // add al,al (its first byte completes the rel32)
    }
    f.midRegionAt = c.at;
    c.b({ 0x4C, 0x89, 0x54, 0x24, 0x68 }); // mov [rsp+68h], r10
    c.b({ 0x0F, 0x10, 0x44, 0x24, 0x68 }); // movups xmm0,[rsp+68h]
    if (s.jumpBackToSaveInside)
    {
        const int rel = static_cast<int>(f.saveAt) - static_cast<int>(c.at + 2);
        CHECK_MSG(rel >= -128, "builder: the region is too long for a rel8 back to the save (%d)", rel);
        c.b({ 0x74, rel & 0xFF }); // jz back to the save
    }
    {
        // jnz rel32 whose displacement, read as a modrm byte pair, looks like [rbp+disp32] of the 7th argument
        int32_t rel = 56 - static_cast<int32_t>(s.frameDisp) + 0x38 + 8;
        if (rel < 0x200)
            rel = 0x200;
        c.b({ 0x0F, 0x85 });
        if (s.falsePositiveJnz)
        {
            c.i32(rel);
            f.epilogueAt = c.at + static_cast<uint32_t>(rel); // rel32 is relative to the end of the instruction
        }
        else
        {
            toEpilogue.push_back(c.at);
            c.i32(0);
            f.epilogueAt = c.at + 0x300;
        }
    }
    // defaults(rbp+0x40)
    for (int i = 0; i < s.defaultsHits; ++i)
    {
        c.b({ 0x48, 0x8D, 0x4D, 0x40, 0xE8 });
        c.rel32(kDefaults);
    }
    // cmp qword [rip+global],0 / jnz site1  (the fast path over the read call)
    c.b({ 0x48, 0x83, 0x3D });
    c.rip(kGlobal, 1);
    c.b({ 0x00 });
    c.b({ 0x0F, 0x85 });
    const uint32_t site1Fix = c.at;
    c.i32(0);
    uint32_t jzFix = 0;
    if (s.jumpIntoSetup)
    {
        c.b({ 0x74, 0x00 }); // jz over the reload straight to "mov rcx,[rip+g]" (patched below)
        jzFix = c.at - 1;
    }
    // the reload, the global read and the call of the read function
    for (int i = 0; i < s.createCallers; ++i)
    {
        f.reloadAt = c.at;
        EmitReload(c, s.reloadFrom < 0 ? s.save : static_cast<Where>(s.reloadFrom), rbpToRsp, s.frameSub);
        f.movRcxAt = c.at;
        if (s.movRcxFromFrame)
        {
            c.b({ 0x48, 0x8B, 0x8D });
            c.i32(0x40);
        }
        else
        {
            if (s.movRcxOk)
                c.b({ 0x48, 0x8B, 0x0D });
            else
                c.b({ 0x48, 0x8B, 0x05 });
            c.rip(kGlobal);
        }
        f.callAt = c.at;
        c.b({ 0xE8 });
        c.rel32(kReadFn);
    }
    if (s.jumpIntoSetup)
    {
        // the 8-bit displacement of the jz above: from its end to the "mov rcx,[rip+g]" (one caller in this case)
        im[jzFix] = static_cast<uint8_t>(f.movRcxAt - (jzFix + 1));
    }
    // copy(rbp+0x40, global) at two sites
    uint32_t site1Start = 0;
    for (int i = 0; i < s.copySites; ++i)
    {
        const uint32_t global = (i == 1) ? s.copySiteGlobal : kGlobal;
        if (i == 1)
            site1Start = c.at;
        if (s.hiddenCopySetup && i == 1)
            c.b({ 0x48, 0xB8, 0x90, 0x48, 0x8B, 0x15 }); // mov rax,imm64 = 90 48 8B 15 <disp32>: the setup bytes, not an instruction
        else if (s.copySetupOk)
            c.b({ 0x48, 0x8B, 0x15 });
        else
            c.b({ 0x48, 0x8B, 0x05 });
        c.rip(global);
        c.b({ 0x48, 0x8D, 0x4D, 0x40, 0xE8 });
        c.rel32(kCopy);
        if (i < 2)
            f.sites[i] = c.at; // the address after the call
    }
    if (s.copySites < 2)
        site1Start = c.at;
    {
        const int32_t rel = static_cast<int32_t>(site1Start) - static_cast<int32_t>(site1Fix + 4);
        std::memcpy(&im[site1Fix], &rel, 4);
    }
    if (s.jumpBackIntoRegion)
    {
        c.b({ 0xE9 });
        c.rel32(f.midRegionAt);
    }
    if (s.jumpToEntry)
    {
        c.b({ 0xE9 });
        c.rel32(kCreate);
    }
    if (s.jumpToCall)
    {
        c.b({ 0xE9 });
        c.rel32(f.callAt);
    }
    if (s.jumpToReload)
    {
        c.b({ 0xE9 });
        c.rel32(f.reloadAt);
    }
    if (s.callIntoFunction)
    {
        c.b({ 0xE8 });
        c.rel32(f.midRegionAt);
    }
    if (s.callIntoTail)
    {
        c.b({ 0xE8 });
        c.rel32(f.sites[0]); // the second copy site: an instruction start after the read call
    }
    if (s.leavingJump != 0)
    {
        c.b({ 0xE9 });
        c.rel32(s.leavingJump == 1 ? kDefaults + 8 : (s.leavingJump == 2 ? kDefaults : kReadFn)); // inside another function / its start / the read function
    }
    if (s.jumpToSave)
    {
        c.b({ 0xE9 });
        c.rel32(f.saveAt);
    }
    if (s.jumpMidInstruction)
    {
        c.b({ 0xE9 });
        c.rel32(f.movRcxAt + 1);
    }
    if (s.indirectJump)
        c.b({ 0xFF, 0xE0 }); // jmp rax
    // pad with nops up to the epilogue
    while (c.at < f.epilogueAt)
        c.b({ 0x90 });
    CHECK_MSG(c.at == f.epilogueAt, "builder: the create body overran the epilogue (%X > %X)", c.at, f.epilogueAt);
    c.b({ 0x48, 0x81, 0xC4 });
    c.i32(static_cast<int32_t>(s.frameSub)); // add rsp, frameSub
    c.b({ 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0xC3 }); // pop r15 r14 r13 r12 rdi rsi rbp; ret
    f.createEnd = c.at;
    for (uint32_t fix : toEpilogue)
    {
        const int32_t rel = static_cast<int32_t>(f.epilogueAt) - static_cast<int32_t>(fix + 4);
        std::memcpy(&im[fix], &rel, 4);
    }

    // defaults and copy functions
    if (s.defaultsOk)
        Code { im, kDefaults }.b({ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x20, 0x41, 0xBB, 0x03, 0, 0, 0 });
    if (s.copyOk)
        Code { im, kCopy }.b({ 0x8B, 0x02, 0x89, 0x01, 0x0F, 0xB7, 0x42, 0x04, 0x66, 0x89, 0x41, 0x04, 0x8B, 0x42, 0x14, 0x89, 0x41, 0x14 });

    // .pdata: begin, end, unwind
    struct Fn
    {
        uint32_t begin, end;
    };
    std::vector<Fn> fns = { { kReader, kReader + 0x100 }, { kReadFn, kReadFn + 0x100 }, { kCreate, f.createEnd }, { kDefaults, kDefaults + 0x40 } };
    if (s.copyInsideOtherFunction)
        fns.push_back({ kCopy - 4, kCopy + 0x40 });
    uint32_t p = kPdataVa;
    for (const auto& fn : fns)
    {
        std::memcpy(&im[p], &fn.begin, 4);
        std::memcpy(&im[p + 4], &fn.end, 4);
        p += 12;
    }
    if (s.chainedFragment)
    {
        // a cold fragment {0x3000, 0x3040} whose unwind info is "chained to {create}": UNWIND_INFO flags = UNW_FLAG_CHAININFO, no codes,
        // then the parent RUNTIME_FUNCTION
        const uint32_t unwind = kPdataVa + 0x800;
        const uint32_t cold[3] = { 0x3000, 0x3040, unwind };
        std::memcpy(&im[p], cold, 12);
        const uint8_t info[4] = { 0x21, 0, static_cast<uint8_t>(s.chainedOdd ? 3 : 0), 0 };
        std::memcpy(&im[unwind], info, 4);
        const uint32_t parent[3] = { kCreate, f.createEnd, 0 };
        std::memcpy(&im[unwind + 4 + (s.chainedOdd ? 8 : 0)], parent, 12); // after the codes, padded to an even count
    }
    if (s.chainedPointer)
    {
        const uint32_t parentAt = kPdataVa + 0x900;
        const uint32_t cold[3] = { 0x3040, 0x3080, parentAt | 1 };
        std::memcpy(&im[p], cold, 12);
        const uint32_t parent[3] = { kCreate, f.createEnd, 0 };
        std::memcpy(&im[parentAt], parent, 12);
    }
    if (!s.pdataOk)
        f.pdataSize = 0; // no exception table
    return f;
}

Located Scan(const Spec& s) { return Locate(Build(s).View()); }

// the 310.9.16 shape: frame -390h / 490h, the context kept in [rsp+60h]
Spec Shape31016()
{
    Spec s;
    s.frameDisp = 0xFFFFFC70;
    s.frameSub = 0x490;
    s.save = Where::SlotRsp;
    return s;
}

void FindsEverything()
{
    const Fake f = Build(Spec {});
    const Located l = Locate(f.View());
    CHECK_MSG(l.why == Why::Ok, "why=%s", WhyText(l.why));
    CHECK(l.reader == kReader && l.readFn == kReadFn && l.create == kCreate);
    CHECK(l.defaults == kDefaults && l.copy == kCopy);
    CHECK(l.copySites[0] == f.sites[0] && l.copySites[1] == f.sites[1]);
    CHECK(l.sharedGlobal == kGlobal);
    // the context proof reports what it found
    CHECK(!l.ctxInSlot && l.ctxReg == 13);
    CHECK(l.ctxSaveAt == f.saveAt && l.ctxReloadAt == f.reloadAt);
    CHECK(l.ctxChecked > 5 && l.createInsns > l.ctxChecked);
}

void Finds31016Shape()
{
    const Spec s = Shape31016();
    const Fake f = Build(s);
    const Located l = Locate(f.View());
    CHECK_MSG(l.why == Why::Ok, "why=%s (at %X)", WhyText(l.why), l.failAt);
    CHECK(l.create == kCreate && l.sharedGlobal == kGlobal);
    CHECK(l.ctxInSlot && l.ctxSlot == 0x60);
    CHECK(l.ctxSaveAt == f.saveAt && l.ctxReloadAt == f.reloadAt);
}

void FrameSizeDoesNotMatter()
{
    // 310.9.1.0 and 310.9.16 differ in the stack frame constants of create
    for (uint32_t disp : { 0xFFFFFCA0u, 0xFFFFFC70u, 0xFFFFFD00u })
        for (uint32_t sub : { 0x460u, 0x490u, 0x300u })
            for (Where w : { Where::R13, Where::SlotRsp })
            {
                // a frame whose rbp lies less than 0x80 above rsp would put the locals addressed through rbp below the slot at
                // rsp+60h (or below rsp): not a frame a real function has, and the slot proof rightly refuses it
                if (w == Where::SlotRsp && static_cast<int32_t>(sub) + static_cast<int32_t>(disp) < 0x80)
                    continue;
                Spec s;
                s.frameDisp = disp;
                s.frameSub = sub;
                s.save = w;
                const Located l = Scan(s);
                CHECK_MSG(l.why == Why::Ok, "disp %X sub %X: %s", disp, sub, WhyText(l.why));
            }
    Spec huge; // not a stack frame: the sub is a 32-bit size far above any real frame
    huge.frameSub = 0x90000;
    CHECK(Scan(huge).why == Why::CreatePrologue);
    Spec positive; // lea rbp,[rsp+positive]
    positive.frameDisp = 0x00000360;
    CHECK(Scan(positive).why == Why::CreatePrologue);
}

void EachGuardFailsOnItsOwn()
{
    auto expect = [](const char* name, const Spec& s, Why why)
    {
        const Located l = Scan(s);
        CHECK_MSG(l.why == why, "%s: got %s", name, WhyText(l.why));
        CHECK_MSG(l.create == 0 && l.reader == 0, "%s: located something anyway", name);
    };
    { Spec s; s.readerPrologueOk = false; expect("reader prologue", s, Why::ReaderPrologue); }
    { Spec s; s.readCallCount = 0; expect("no read call", s, Why::NoReader); }
    { Spec s; s.readCallCount = 2; expect("two read calls", s, Why::ReaderNotUnique); }
    { Spec s; s.afterCallTestOk = false; expect("test al,al missing", s, Why::NoReader); }
    { Spec s; s.readFnPrologueOk = false; expect("read function prologue", s, Why::ReadFnPrologue); }
    { Spec s; s.createCallers = 0; expect("no create", s, Why::NoCreate); }
    { Spec s; s.createCallers = 2; expect("two callers of the read function", s, Why::CreateNotUnique); }
    { Spec s; s.createPrologueOk = false; expect("create prologue", s, Why::CreatePrologue); }
    { Spec s; s.statusConstant = false; expect("no 32-bit status constant", s, Why::CreateSignature); }
    { Spec s; s.seventhArgRbp = true; expect("7th argument through rbp", s, Why::CreateSignature); }
    { Spec s; s.seventhArgRsp = true; expect("7th argument through rsp", s, Why::CreateSignature); }
    { Spec s; s.seventhArgRbp = true; s.frameDisp = 0xFFFFFC70; expect("7th argument, other frame size", s, Why::CreateSignature); }
    { Spec s = Shape31016(); s.seventhArgRbp = true; expect("7th argument, 310.9.16 shape", s, Why::CreateSignature); }
    { Spec s; s.movRcxOk = false; expect("mov rcx,[rip+g] before the read call", s, Why::ReadCallSetup); }
    { Spec s; s.movRcxFromFrame = true; expect("rcx loaded from the frame, not from a global", s, Why::ReadCallSetup); }
    { Spec s; s.defaultsOk = false; expect("defaults body", s, Why::DefaultsNotFound); }
    { Spec s; s.defaultsHits = 2; expect("two defaults calls", s, Why::DefaultsNotFound); }
    { Spec s; s.copyOk = false; expect("copy body", s, Why::CopySites); }
    { Spec s; s.copySites = 1; expect("one copy site", s, Why::CopySites); }
    { Spec s; s.copySites = 0; expect("no copy site", s, Why::CopySites); }
    { Spec s; s.copySetupOk = false; expect("copy setup", s, Why::CopySetup); }
    { Spec s; s.copySiteGlobal = kGlobal + 8; expect("copy reads another global", s, Why::GlobalMismatch); }
    { Spec s; s.pdataOk = false; expect("no exception table", s, Why::NoPdata); }
    { Spec s; s.copyInsideOtherFunction = true; expect("copy target is inside another function", s, Why::NotFunctionStart); }
}

void SignatureFacts()
{
    // the 6th argument (the last one the hook forwards) and no stack argument at all are both fine
    Spec with6;
    with6.sixthArg = true;
    CHECK(Scan(with6).why == Why::Ok);
    Spec without;
    without.sixthArg = false;
    CHECK(Scan(without).why == Why::Ok);
    // the other frame size (310.9.16), with a 6th argument load: still fine
    Spec other;
    other.frameDisp = 0xFFFFFC70;
    CHECK(Scan(other).why == Why::Ok);
}

void JccDisplacementIsNotAnArgument()
{
    // A jnz rel32 is 0F 85 <rel32>: the byte 85 looks like a modrm byte [rbp+disp32] and the displacement is 0x419 here, the
    // number of the 7th-argument slot of the 310.9.16 frame. The real 310.9.16 create has exactly this instruction (jnz at 36A80).
    // An unaligned byte scan refuses it; the decoded scan must not.
    for (Where w : { Where::R13, Where::SlotRsp })
    {
        for (uint32_t disp : { 0xFFFFFCA0u, 0xFFFFFC70u })
        {
            Spec s;
            s.frameDisp = disp;
            s.save = w;
            s.falsePositiveJnz = true;
            const Fake f = Build(s);
            // the instruction is really there: find 0F 85 followed by the 7th-argument displacement
            const int32_t want = 56 - static_cast<int32_t>(disp) + 0x38 + 8;
            bool present = false;
            for (uint32_t at = kCreate; at + 6 < f.createEnd; ++at)
            {
                int32_t v;
                std::memcpy(&v, &f.image[at + 2], 4);
                if (f.image[at] == 0x0F && f.image[at + 1] == 0x85 && v == want)
                    present = true;
            }
            CHECK(present);
            CHECK_MSG(Locate(f.View()).why == Why::Ok, "disp %X: %s", disp, WhyText(Locate(f.View()).why));
        }
    }
}

// ---- the context argument ---------------------------------------------------------------------------------------------------
void ContextProofAccepts()
{
    // every callee-saved register, both encodings of the save, both frame shapes
    for (Where w : { Where::R13, Where::Rbx, Where::Rsi, Where::Rdi, Where::R12, Where::R15 })
        for (bool form89 : { false, true })
            for (bool shape31016 : { false, true })
            {
                Spec s = shape31016 ? Shape31016() : Spec {};
                s.save = w;
                s.saveFormMov89 = form89;
                const Fake f = Build(s);
                const Located l = Locate(f.View());
                CHECK_MSG(l.why == Why::Ok, "reg %d form89 %d: %s", RegOf(w), form89, WhyText(l.why));
                CHECK_MSG(!l.ctxInSlot && l.ctxReg == RegOf(w), "reg %d reported as %d", RegOf(w), l.ctxReg);
            }
    // the slot through rsp or through rbp, in any mix
    for (Where saved : { Where::SlotRsp, Where::SlotRbp })
        for (Where reloaded : { Where::SlotRsp, Where::SlotRbp })
            for (bool shape31016 : { false, true })
            {
                Spec s = shape31016 ? Shape31016() : Spec {};
                s.save = saved;
                s.reloadFrom = static_cast<int>(reloaded);
                const Located l = Scan(s);
                CHECK_MSG(l.why == Why::Ok, "slot save %d reload %d: %s", static_cast<int>(saved), static_cast<int>(reloaded), WhyText(l.why));
                CHECK(l.ctxInSlot && l.ctxSlot == 0x60);
            }
}

void ContextProofHarmlessWrites()
{
    // writes that touch neither the saved register nor the saved slot are not clobbers
    for (Clobber k : { Clobber::ScratchWrites, Clobber::NeighborAbove, Clobber::NeighborBelow, Clobber::MovupsAbove, Clobber::LeaAbove, Clobber::MovRaxRsp, Clobber::PushRax })
    {
        Spec reg;
        reg.clobber = k;
        if (k != Clobber::PushRax)
            CHECK_MSG(Scan(reg).why == Why::Ok, "register case, clobber %d: %s", static_cast<int>(k), WhyText(Scan(reg).why));
        Spec slot = Shape31016();
        slot.clobber = k;
        // a plain read of rsp / a push moves or copies the frame address: refused for a slot, harmless for a register
        const bool refusedForSlot = (k == Clobber::MovRaxRsp || k == Clobber::PushRax);
        CHECK_MSG((Scan(slot).why == Why::Ok) == !refusedForSlot, "slot case, clobber %d: %s", static_cast<int>(k), WhyText(Scan(slot).why));
    }
    // a different register than the one holding the context may be written freely
    for (Clobber k : { Clobber::RegMov, Clobber::RegXor32, Clobber::RegXchg, Clobber::RegByte, Clobber::RegLea })
    {
        Spec s = Shape31016(); // the context is in the slot; the clobber helper writes r13 here
        s.clobber = k;
        CHECK_MSG(Scan(s).why == Why::Ok, "r13 written while the context sits in a slot, clobber %d: %s", static_cast<int>(k), WhyText(Scan(s).why));
    }
}

void ContextProofRefuses()
{
    auto expect = [](const char* name, const Spec& s, Why why)
    {
        const Located l = Scan(s);
        CHECK_MSG(l.why == why, "%s: got %s", name, WhyText(l.why));
        CHECK_MSG(l.create == 0 && l.reader == 0, "%s: located something anyway", name);
    };
    // never saved: the reload reads a register nobody filled
    { Spec s; s.save = Where::None; s.reloadFrom = static_cast<int>(Where::R13); expect("context never saved (register)", s, Why::CreateContext); }
    { Spec s = Shape31016(); s.save = Where::None; s.reloadFrom = static_cast<int>(Where::SlotRsp); expect("context never saved (slot)", s, Why::CreateContext); }
    // saved in one place, a different one reloaded
    { Spec s; s.save = Where::R13; s.reloadFrom = static_cast<int>(Where::R12); expect("saved r13, reloaded r12", s, Why::CreateContext); }
    { Spec s; s.save = Where::Rbx; s.reloadFrom = static_cast<int>(Where::R13); expect("saved rbx, reloaded r13", s, Why::CreateContext); }
    { Spec s; s.save = Where::R13; s.reloadFrom = static_cast<int>(Where::SlotRsp); expect("saved r13, reloaded from the slot", s, Why::CreateContext); }
    { Spec s = Shape31016(); s.save = Where::SlotRsp; s.reloadFrom = static_cast<int>(Where::Home); expect("saved in the slot, reloaded from another slot", s, Why::CreateContext); }
    { Spec s = Shape31016(); s.save = Where::SlotRsp; s.reloadFrom = static_cast<int>(Where::R13); expect("saved in the slot, reloaded from r13", s, Why::CreateContext); }
    // saved after a call: rdx no longer holds the argument
    { Spec s; s.callBeforeSave = true; expect("saved after a call (register)", s, Why::CreateContext); }
    { Spec s = Shape31016(); s.callBeforeSave = true; expect("saved after a call (slot)", s, Why::CreateContext); }
    // rdx overwritten before it is kept, or the frame moved before a slot save
    { Spec s; s.rdxWrittenBeforeSave = true; expect("rdx written before the save (register)", s, Why::CreateContext); }
    { Spec s = Shape31016(); s.rdxWrittenBeforeSave = true; expect("rdx written before the save (slot)", s, Why::CreateContext); }
    { Spec s = Shape31016(); s.pushBeforeSave = true; expect("push before a slot save", s, Why::CreateContext); }
    // places that do not survive: a volatile register, the callee's home area
    { Spec s; s.save = Where::Volatile; expect("saved in a volatile register", s, Why::CreateContext); }
    { Spec s; s.save = Where::Home; expect("saved in the home area", s, Why::CreateContext); }
    { Spec s; s.save = Where::Beyond; expect("saved just above the frame", s, Why::CreateContext); }
    {   // the same with the locals far above the slot, so that taking their address is not what refuses it
        Spec s;
        s.save = Where::Beyond;
        s.frameDisp = 0xFFFFFFF0;
        expect("saved just above the frame (locals above it)", s, Why::CreateContext);
    }
    // the saved register written between the save and the reload, in every way
    for (Where w : { Where::R13, Where::Rbx, Where::Rsi, Where::Rdi, Where::R12, Where::R15 })
        for (Clobber k : { Clobber::RegMov, Clobber::RegXor32, Clobber::RegPop, Clobber::RegXchg, Clobber::RegByte, Clobber::RegLea })
        {
            Spec s;
            s.save = w;
            s.clobber = k;
            const std::string name = "register " + std::to_string(RegOf(w)) + " written, kind " + std::to_string(static_cast<int>(k));
            expect(name.c_str(), s, Why::CreateContext);
        }
    // the saved slot written, through rsp, through the rbp alias, partially, with a vector store, one byte; its address taken
    for (Where w : { Where::SlotRsp, Where::SlotRbp })
        for (Clobber k : { Clobber::SlotQword, Clobber::SlotViaRbp, Clobber::SlotDword, Clobber::SlotMovups, Clobber::SlotByte, Clobber::SlotLeaBelow,
                           Clobber::SlotLeaRbpBelow, Clobber::MovRaxRsp, Clobber::PushRax })
        {
            Spec s = Shape31016();
            s.save = w;
            s.clobber = k;
            const std::string name = "slot " + std::to_string(static_cast<int>(w)) + " clobbered, kind " + std::to_string(static_cast<int>(k));
            expect(name.c_str(), s, Why::CreateContext);
        }
}

// ---- review 2 (M2/M3): the region rules and their boundaries -----------------------------------------------------------------------
void RegionRules()
{
    auto scan = [](Spec s, Where w, Clobber k)
    {
        s.save = w;
        s.clobber = k;
        return Scan(s);
    };
    const Clobber slotRefused[] = { Clobber::AliasStore, Clobber::SlotLeaAt60, Clobber::SlotLeaAt64, Clobber::SlotLeaAt67, Clobber::SlotDwordAt5D,
                                    Clobber::SlotIndexedStore, Clobber::SlotLeaIndexed, Clobber::WriteRbp, Clobber::StoreRspToMem,
                                    Clobber::MovqXmmRsp, Clobber::LeaRbpIndex, Clobber::StringStore, Clobber::BtsMem };
    for (Clobber k : slotRefused)
        for (Where w : { Where::SlotRsp, Where::SlotRbp })
        {
            const Located l = scan(Shape31016(), w, k);
            CHECK_MSG(l.why == Why::CreateContext, "slot %d, clobber %d: %s", static_cast<int>(w), static_cast<int>(k), WhyText(l.why));
        }
    // in the 310.9.1.0 frame as well
    for (Clobber k : { Clobber::AliasStore, Clobber::SlotLeaAt60, Clobber::SlotDwordAt5D, Clobber::SlotIndexedStore })
        CHECK_MSG(scan(Spec {}, Where::SlotRsp, k).why == Why::CreateContext, "310.9.1.0 frame, clobber %d", static_cast<int>(k));
    // a register holds the context: only the string and bit-string stores are refused (they write where a register says); frame addresses,
    // slot writes and the like are irrelevant to a register
    for (Clobber k : { Clobber::AliasStore, Clobber::SlotLeaAt60, Clobber::SlotLeaAt64, Clobber::SlotDwordAt5D, Clobber::SlotIndexedStore,
                       Clobber::SlotLeaIndexed, Clobber::WriteRbp, Clobber::StoreRspToMem, Clobber::MovqXmmRsp, Clobber::LeaRbpIndex })
        CHECK_MSG(scan(Spec {}, Where::R13, k).why == Why::Ok, "register case, clobber %d: %s", static_cast<int>(k), WhyText(scan(Spec {}, Where::R13, k).why));
    // string and bit-string stores write where a register says, which matters for a frame slot only (the registers they change are
    // caught as writes): accepted for a context kept in a callee-saved register that they do not change
    for (Clobber k : { Clobber::StringStore, Clobber::BtsMem })
    {
        CHECK_MSG(scan(Spec {}, Where::R13, k).why == Why::Ok, "register case, store %d", static_cast<int>(k));
        CHECK_MSG(scan(Shape31016(), Where::SlotRsp, k).why == Why::CreateContext, "slot case, store %d", static_cast<int>(k));
    }
    // pointer tracking: a frame address copied or computed on keeps pointing into the frame; overwritten or lost in a call it does not
    for (Clobber k : { Clobber::AliasViaMov, Clobber::AliasViaArithmetic, Clobber::AliasKeptInCalleeSaved })
        CHECK_MSG(scan(Shape31016(), Where::SlotRsp, k).why == Why::CreateContext, "alias clobber %d", static_cast<int>(k));
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::IndexedOtherBase).why == Why::Ok); // neither base nor index holds a frame address
    // review 3, M1/L3: the mark follows two-register forms, survives partial writes, merges and loops, and a pre-save lea
    for (Clobber k : { Clobber::MarkAddSource, Clobber::MarkPartialAl, Clobber::MarkPartialAx, Clobber::MarkPartialMovAx, Clobber::MarkPartialLoadAl,
                       Clobber::MarkBranchMerge, Clobber::MarkLoopCarry, Clobber::MarkKeptInR15, Clobber::MarkKeptInRsi, Clobber::MarkLeaFromBase,
                       Clobber::MarkLeaFromIndex, Clobber::MarkMov8B, Clobber::MarkIndexStore, Clobber::MarkXchg, Clobber::MarkCmov,
                       Clobber::MarkXchgShort, Clobber::MarkPartialXor, Clobber::MarkXchgRexB, Clobber::MarkXchg32RexB,
                       Clobber::MarkImul6B, Clobber::MarkImul69, Clobber::MarkCmpxchg, Clobber::MarkCmpxchgRax, Clobber::MarkShld, Clobber::MarkShrdCl,
                       Clobber::MarkMovsxd })
    {
        const Located l = scan(Shape31016(), Where::SlotRsp, k);
        CHECK_MSG(l.why == Why::CreateContext, "mark image %d: %s", static_cast<int>(k), WhyText(l.why));
        CHECK_MSG(scan(Spec {}, Where::R13, k).why == Why::Ok, "register mode, image %d: %s", static_cast<int>(k), WhyText(scan(Spec {}, Where::R13, k).why));
    }
    {
        Spec s = Shape31016();
        s.lea12BeforeSave = true;
        s.clobber = Clobber::StoreViaR12;
        CHECK_MSG(Scan(s).why == Why::CreateContext, "a frame address built before the save and used in the region: %s", WhyText(Scan(s).why));
        Spec t = Shape31016();
        t.clobber = Clobber::StoreViaR12; // without the lea, r12 holds no frame address
        CHECK_MSG(Scan(t).why == Why::Ok, "the same store without the lea: %s", WhyText(Scan(t).why));
    }
    // a plain nop is not an exchange: r8 never held the address (`49 90` is xchg rax,r8, `41 90` the 32-bit form, which this model treats
    // like every 32-bit copy: the mark goes with it, conservatively)
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::NopNoMark).why == Why::Ok);
    CHECK(scan(Spec {}, Where::R13, Clobber::NopNoMark).why == Why::Ok);
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::NoMarkImul6B).why == Why::Ok); // a three-operand imul of an unmarked register marks nothing
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::NoMarkCmpxchg).why == Why::Ok); // a cmpxchg between unmarked registers marks nothing
    CHECK(scan(Spec {}, Where::R13, Clobber::NoMarkCmpxchg).why == Why::Ok);
    // fix round 3 (L2): the accepting direction. Each image takes the frame address into rax, overwrites rax at full width and stores through it:
    // not a store to the slot any more. A mutant that never drops a mark refuses these (a silent "restart needed" on a real build).
    // (pop is not here: any pop in the region is refused as a stack write, so it can never be the kill that saves an image.)
    for (Clobber k : { Clobber::KillMovImm, Clobber::KillMovImmC7, Clobber::KillMov8B, Clobber::KillMov89, Clobber::KillMov32, Clobber::KillXorZero,
                       Clobber::KillSubZero, Clobber::KillMovzxReg, Clobber::KillMovzxMem, Clobber::KillMovsxReg, Clobber::KillMovsxdMem, Clobber::KillLea,
                       Clobber::KillMerge })
    {
        CHECK_MSG(scan(Shape31016(), Where::SlotRsp, k).why == Why::Ok, "slot mode, full-width kill %d: %s", static_cast<int>(k), WhyText(scan(Shape31016(), Where::SlotRsp, k).why));
        CHECK_MSG(scan(Spec {}, Where::R13, k).why == Why::Ok, "register mode, full-width kill %d", static_cast<int>(k));
    }
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::MovImmRbp).why == Why::CreateContext);
    CHECK(scan(Spec {}, Where::R13, Clobber::MovImmRbp).why == Why::Ok);
    CHECK(scan(Spec {}, Where::R13, Clobber::IndexedOtherBase).why == Why::Ok);
    for (Clobber k : { Clobber::AliasKilledByLoad, Clobber::AliasKilledByCall, Clobber::NegDispUntainted })
        CHECK_MSG(scan(Shape31016(), Where::SlotRsp, k).why == Why::Ok, "not an alias, clobber %d: %s", static_cast<int>(k),
                  WhyText(scan(Shape31016(), Where::SlotRsp, k).why));
    // the stores above the slot and the lea at its end stay allowed (the real function has them)
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::NeighborAbove).why == Why::Ok);
    CHECK(scan(Shape31016(), Where::SlotRsp, Clobber::LeaAbove).why == Why::Ok);
    // a save in the last 8 bytes of the home area is still in the home area
    { Spec s; s.save = Where::HomeEdge; CHECK(Scan(s).why == Why::CreateContext); }
}

void ControlFlowBoundaries()
{
    for (bool slot : { false, true })
    {
        auto base = [&]() { return slot ? Shape31016() : Spec {}; };
        { Spec s = base(); s.jumpToCall = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a jump to the read call itself (slot %d)", slot); }
        { Spec s = base(); s.jumpToReload = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a jump from outside to the reload (slot %d)", slot); }
        { Spec s = base(); s.jumpBackToSaveInside = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a jump back to the save from inside (slot %d)", slot); }
        { Spec s = base(); s.branchBeforeSave = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a branch before the save (slot %d)", slot); }
        { Spec s = base(); s.callIntoFunction = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a call into the function (slot %d)", slot); }
        { Spec s = base(); s.callIntoTail = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a call into the tail of the function (slot %d)", slot); }
        { Spec s = base(); s.leavingJump = 1; CHECK_MSG(Scan(s).why == Why::CreateContext, "a jump out of the function to a non-function (slot %d)", slot); }
        { Spec s = base(); s.leavingJump = 2; CHECK_MSG(Scan(s).why == Why::CreateContext, "a jump to the start of another function (slot %d)", slot); }
        { Spec s = base(); s.leavingJump = 3; CHECK_MSG(Scan(s).why == Why::CreateContext, "a jump to the settings read function itself (slot %d)", slot); }
        { Spec s = base(); s.chainedFragment = true; s.chainedOdd = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a chained fragment with an odd number of unwind codes (slot %d)", slot); }
        { Spec s = base(); s.chainedFragment = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a cold fragment chained to create (slot %d)", slot); }
        { Spec s = base(); s.chainedPointer = true; CHECK_MSG(Scan(s).why == Why::CreateContext, "a cold fragment chained through a pointer (slot %d)", slot); }
    }
    // the fragment is reported by its place
    Spec s;
    s.leavingJump = 1;
    const Located l = Scan(s);
    CHECK(l.failAt >= kCreate && l.failAt < kCreate + 0x800);
}

void SiteScansUseInstructions()
{
    // the bytes of "lea rcx,[rbp+40h] / call defaults" inside an immediate are not a call: the defaults function is still found once
    for (bool slot : { false, true })
    {
        Spec s = slot ? Shape31016() : Spec {};
        s.hiddenDefaultsCall = true;
        const Located l = Scan(s);
        CHECK_MSG(l.why == Why::Ok && l.defaults == kDefaults, "hidden pattern (slot %d): %s", slot, WhyText(l.why));
    }
}

void HiddenSetup()
{
    // the bytes of "mov rdx,[rip+g]" inside an immediate before the second copy are not the setup of that call
    for (bool slot : { false, true })
    {
        Spec s = slot ? Shape31016() : Spec {};
        s.hiddenCopySetup = true;
        CHECK_MSG(Scan(s).why == Why::CopySetup, "hidden copy setup (slot %d): %s", slot, WhyText(Scan(s).why));
    }
}

void SignatureBoundaries()
{
    for (bool slot : { false, true })
    {
        auto base = [&]() { return slot ? Shape31016() : Spec {}; };
        { Spec s = base(); s.wideLoadAtSixth = true; CHECK_MSG(Scan(s).why == Why::CreateSignature, "a 16-byte load over the 6th and 7th argument (slot %d)", slot); }
        { Spec s = base(); s.loadAt14th = true; CHECK_MSG(Scan(s).why == Why::CreateSignature, "a load at the 14th argument (slot %d)", slot); }
        { Spec s = base(); s.loadAt15th = true; CHECK_MSG(Scan(s).why == Why::Ok, "a load past the window (slot %d): %s", slot, WhyText(Scan(s).why)); }
        { Spec s = base(); s.statusDecoyHigh = true; CHECK_MSG(Scan(s).why == Why::CreateSignature, "0BAD1xxxx is not a status (slot %d)", slot); }
    }
}

void ControlFlowRules()
{
    auto expect = [](const char* name, const Spec& s, Why why)
    {
        const Located l = Scan(s);
        CHECK_MSG(l.why == why, "%s: got %s", name, WhyText(l.why));
    };
    for (bool slot : { false, true })
    {
        auto base = [&]() { return slot ? Shape31016() : Spec {}; };
        { Spec s = base(); s.jumpBackIntoRegion = true; expect("a jump from outside back into the region", s, Why::CreateContext); }
        { Spec s = base(); s.jumpIntoSetup = true; expect("a jump over the reload into the argument setup", s, Why::CreateContext); }
        { Spec s = base(); s.jumpToEntry = true; expect("a jump to the function entry", s, Why::CreateContext); }
        { Spec s = base(); s.jumpToSave = true; expect("a jump back to the save", s, Why::CreateContext); }
        { Spec s = base(); s.jumpMidInstruction = true; expect("a jump into the middle of an instruction", s, Why::CreateDecode); }
        { Spec s = base(); s.indirectJump = true; expect("an indirect jump", s, Why::CreateDecode); }
        { Spec s = base(); s.unknownOpcode = true; expect("an opcode the decoder does not know", s, Why::CreateDecode); }
    }
    // a failure at a place in the code reports where
    Spec u;
    u.unknownOpcode = true;
    const Located l = Scan(u);
    CHECK(l.why == Why::CreateDecode && l.failAt >= kCreate && l.failAt < kCreate + 0x400);
}

void ImageBounds()
{
    // an image view that is too small for the patterns never reads outside it
    Fake f = Build(Spec {});
    ImageView v = f.View();
    v.size = 0x1500; // cuts the image inside create
    const Located l = Locate(v);
    CHECK(l.why != Why::Ok);
    ImageView empty { nullptr, 0, nullptr, 0, 0, 0 };
    CHECK(Locate(empty).why != Why::Ok);
    // a create whose end lies outside the image
    Fake g = Build(Spec {});
    const uint32_t bad = 0x7000;
    std::memcpy(&g.image[kPdataVa + 2 * 12 + 4], &bad, 4); // create's end
    CHECK(Locate(g.View()).why != Why::Ok);
}

// ---- the provider choice ---------------------------------------------------------------------------------------------------
void ProviderChoice()
{
    auto cand = [](const char* path, bool loaded, bool passes) { return Candidate { path, loaded, passes }; };
    {
        std::vector<Candidate> c;
        CHECK(Choose(c).kind == ChoiceKind::None);
    }
    {
        std::vector<Candidate> c { cand("C:/Games/x/OptiScaler/nvngx_dlssg.dll", true, true) };
        const auto r = Choose(c);
        CHECK(r.kind == ChoiceKind::One && r.index == 0);
    }
    {   // a failing or unloaded module is never chosen
        std::vector<Candidate> c { cand("a/nvngx_dlssg.dll", true, false), cand("b/nvngx_dlssg.dll", false, true) };
        CHECK(Choose(c).kind == ChoiceKind::None);
    }
    {   // two pass, one is the driver's own copy: the game's copy wins
        std::vector<Candidate> c { cand("C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_x\\nvngx_dlssg.dll", true, true),
                                    cand("C:\\Games\\FFXIV\\OptiScaler\\nvngx_dlssg.dll", true, true) };
        const auto r = Choose(c);
        CHECK(r.kind == ChoiceKind::One && r.index == 1);
    }
    {   // two game copies: ambiguous, nothing is hooked
        std::vector<Candidate> c { cand("C:\\Games\\a\\nvngx_dlssg.dll", true, true), cand("C:\\Games\\b\\nvngx_dlssg.dll", true, true) };
        CHECK(Choose(c).kind == ChoiceKind::Ambiguous);
    }
    {   // only driver copies pass and there are two: ambiguous too
        std::vector<Candidate> c { cand("C:\\Windows\\System32\\DriverStore\\a\\nvngx_dlssg.dll", true, true),
                                    cand("C:\\Windows\\System32\\DriverStore\\b\\nvngx_dlssg.dll", true, true) };
        CHECK(Choose(c).kind == ChoiceKind::Ambiguous);
    }
    {   // a single driver copy is accepted (it is the one in use)
        std::vector<Candidate> c { cand("C:\\Windows\\System32\\DriverStore\\a\\nvngx_dlssg.dll", true, true) };
        CHECK(Choose(c).kind == ChoiceKind::One);
    }
}

void KnownHashes()
{
    CHECK(KnownHashLabel("ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82") != nullptr);
    CHECK(KnownHashLabel("FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82") != nullptr); // case does not matter
    CHECK(KnownHashLabel("0000000000000000000000000000000000000000000000000000000000000000") == nullptr);
    CHECK(KnownHashLabel("") == nullptr);
    // the maintainer's build, checked in full on the real DLL (manual recipe fg_preset_guards_dll)
    const char* label31016 = KnownHashLabel("8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7");
    CHECK(label31016 != nullptr && std::string(label31016).find("310.9.16") != std::string::npos);
    const char* labelStore = KnownHashLabel("cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72");
    CHECK(labelStore != nullptr && std::string(labelStore).find("model-store") != std::string::npos);
    // version gate: 310.8 and 310.9 families only
    CHECK(VersionAccepted(310, 9) && VersionAccepted(310, 8));
    CHECK(!VersionAccepted(310, 7) && !VersionAccepted(310, 10) && !VersionAccepted(311, 9) && !VersionAccepted(0, 0));
}
} // namespace

int main()
{
    FindsEverything();
    Finds31016Shape();
    FrameSizeDoesNotMatter();
    EachGuardFailsOnItsOwn();
    SignatureFacts();
    JccDisplacementIsNotAnArgument();
    ContextProofAccepts();
    ContextProofHarmlessWrites();
    ContextProofRefuses();
    ControlFlowRules();
    RegionRules();
    ControlFlowBoundaries();
    SiteScansUseInstructions();
    HiddenSetup();
    SignatureBoundaries();
    ImageBounds();
    ProviderChoice();
    KnownHashes();
    if (g_failures)
    {
        std::printf("fg_preset_guards: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_guards: synthetic images (310.9.1.0 and 310.9.16 shapes) located, the context proof accepts and refuses as expected, every guard fails closed on its own, provider choice, hash and version gates passed");
    return 0;
}
