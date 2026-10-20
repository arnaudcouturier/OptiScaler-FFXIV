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
    None
};

enum class Clobber
{
    None,
    // writes the saved register (any of them must be refused when the context sits in a register)
    RegMov, RegXor32, RegPop, RegXchg, RegByte, RegLea,
    // writes the saved slot (refused when the context sits in a slot)
    SlotQword, SlotViaRbp, SlotDword, SlotMovups, SlotByte, SlotLeaBelow, SlotLeaRbpBelow, MovRaxRsp, PushRax,
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

    // control flow around the region between the save and the reload
    bool jumpBackIntoRegion = false;
    bool jumpIntoSetup = false;
    bool jumpToEntry = false;
    bool jumpToSave = false;
    bool jumpMidInstruction = false;
    bool indirectJump = false;
    bool unknownOpcode = false;
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
    default: c.b({ 0x90 }); break;
    }
}

void EmitReload(Code& c, Where w, int32_t rbpToRsp)
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
    if (s.statusConstant)
        c.b({ 0xB8, 0x02, 0x00, 0xD0, 0xBA }); // mov eax,0BAD0002h
    else
        c.b({ 0xB8, 0x02, 0x00, 0x00, 0x00 }); // a mov eax,2 that is not an NGX status
    c.b({ 0xE9 });
    toEpilogue.push_back(c.at);
    c.i32(0);                     // jmp epilogue
    EmitClobber(c, s.clobber, s.save, rbpToRsp);
    if (s.unknownOpcode)
        c.b({ 0x0F, 0x38, 0x00, 0xC1 }); // pshufb xmm0, xmm1: not in the decoder's table
    f.midRegionAt = c.at;
    c.b({ 0x4C, 0x89, 0x54, 0x24, 0x68 }); // mov [rsp+68h], r10
    c.b({ 0x0F, 0x10, 0x44, 0x24, 0x68 }); // movups xmm0,[rsp+68h]
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
        EmitReload(c, s.reloadFrom < 0 ? s.save : static_cast<Where>(s.reloadFrom), rbpToRsp);
        f.movRcxAt = c.at;
        if (s.movRcxOk)
            c.b({ 0x48, 0x8B, 0x0D });
        else
            c.b({ 0x48, 0x8B, 0x05 });
        c.rip(kGlobal);
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
        if (s.copySetupOk)
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
