// The small x86-64 decoder behind the DLSS-G preset guards (ext/fg/dlssg/PresetX64.h): instruction lengths, control flow, the registers
// and memory an instruction writes. Every row is an instruction that occurs in NVIDIA's endpoint create function (310.9.1.0,
// 310.9.16) or a form the proof depends on; anything outside the table must be refused, not guessed.
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetX64.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace ext::fg::preset::x64;

namespace
{
constexpr uint32_t Bit(unsigned r) { return 1u << r; }

struct Row
{
    const char* what;
    std::vector<uint8_t> bytes;
    unsigned len;
    Flow flow;
    uint32_t writes;  // register mask
    bool memWrite;
    unsigned memSize; // when memWrite
};

bool Dec(const std::vector<uint8_t>& bytes, Insn& n, uint32_t at = 0x1000)
{
    std::vector<uint8_t> padded = bytes;
    padded.resize(bytes.size() + 16, 0xCC); // decoding must stop at the instruction, not run into the neighbours
    return Decode(padded.data(), bytes.size(), at, n);
}

void Table()
{
    const std::vector<Row> rows = {
        // the prologue of create
        { "mov [rsp+18h],rbx", { 0x48, 0x89, 0x5C, 0x24, 0x18 }, 5, Flow::Next, 0, true, 8 },
        { "push rbp", { 0x55 }, 1, Flow::Next, 0, false, 0 },
        { "push r12", { 0x41, 0x54 }, 2, Flow::Next, 0, false, 0 },
        { "lea rbp,[rsp-390h]", { 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFC, 0xFF, 0xFF }, 8, Flow::Next, Bit(RBP), false, 0 },
        { "sub rsp,490h", { 0x48, 0x81, 0xEC, 0x90, 0x04, 0x00, 0x00 }, 7, Flow::Next, Bit(RSP), false, 0 },
        { "mov rax,[rip+x]", { 0x48, 0x8B, 0x05, 0xDA, 0x5C, 0x6E, 0x00 }, 7, Flow::Next, Bit(RAX), false, 0 },
        { "xor rax,rsp", { 0x48, 0x33, 0xC4 }, 3, Flow::Next, Bit(RAX), false, 0 },
        { "mov [rbp+380h],rax", { 0x48, 0x89, 0x85, 0x80, 0x03, 0x00, 0x00 }, 7, Flow::Next, 0, true, 8 },
        { "mov [rbp+28h],r9", { 0x4C, 0x89, 0x4D, 0x28 }, 4, Flow::Next, 0, true, 8 },
        // the context
        { "mov r13,rdx", { 0x4C, 0x8B, 0xEA }, 3, Flow::Next, Bit(R13), false, 0 },
        { "mov r8,rdx", { 0x4C, 0x8B, 0xC2 }, 3, Flow::Next, Bit(R8), false, 0 },
        { "mov [rsp+60h],rdx", { 0x48, 0x89, 0x54, 0x24, 0x60 }, 5, Flow::Next, 0, true, 8 },
        { "mov rdx,[rsp+60h]", { 0x48, 0x8B, 0x54, 0x24, 0x60 }, 5, Flow::Next, Bit(RDX), false, 0 },
        { "mov rdx,r13", { 0x49, 0x8B, 0xD5 }, 3, Flow::Next, Bit(RDX), false, 0 },
        { "mov rcx,[rip+g]", { 0x48, 0x8B, 0x0D, 0x45, 0x40, 0x63, 0x00 }, 7, Flow::Next, Bit(RCX), false, 0 },
        { "mov rdi,rcx", { 0x48, 0x8B, 0xF9 }, 3, Flow::Next, Bit(RDI), false, 0 },
        { "mov rbx,[rbp+3F8h]", { 0x48, 0x8B, 0x9D, 0xF8, 0x03, 0x00, 0x00 }, 7, Flow::Next, Bit(RBX), false, 0 },
        { "xor r12d,r12d", { 0x45, 0x33, 0xE4 }, 3, Flow::Next, Bit(R12), false, 0 },
        { "mov [rcx+18h],rdx", { 0x48, 0x89, 0x51, 0x18 }, 4, Flow::Next, 0, true, 8 },
        { "mov rax,[rdx]", { 0x48, 0x8B, 0x02 }, 3, Flow::Next, Bit(RAX), false, 0 },
        { "lea rdx,[rcx+20h]", { 0x48, 0x8D, 0x51, 0x20 }, 4, Flow::Next, Bit(RDX), false, 0 },
        // calls, jumps, returns
        { "call [rip+x]", { 0xFF, 0x15, 0xAA, 0xD0, 0x05, 0x00 }, 6, Flow::Call, 0, false, 0 },
        { "call rel32", { 0xE8, 0xD0, 0x12, 0x00, 0x00 }, 5, Flow::Call, 0, false, 0 },
        { "test eax,eax", { 0x85, 0xC0 }, 2, Flow::Next, 0, false, 0 },
        { "jz rel8", { 0x74, 0x0A }, 2, Flow::CondJump, 0, false, 0 },
        { "jnz rel32", { 0x0F, 0x85, 0x19, 0x04, 0x00, 0x00 }, 6, Flow::CondJump, 0, false, 0 },
        { "jmp rel32", { 0xE9, 0x30, 0x0B, 0x00, 0x00 }, 5, Flow::Jump, 0, false, 0 },
        { "jmp rel8", { 0xEB, 0x02 }, 2, Flow::Jump, 0, false, 0 },
        { "ret", { 0xC3 }, 1, Flow::Ret, 0, false, 0 },
        { "int3", { 0xCC }, 1, Flow::Stop, 0, false, 0 },
        // constants and compares
        { "mov eax,0BAD00002h", { 0xB8, 0x02, 0x00, 0xD0, 0xBA }, 5, Flow::Next, Bit(RAX), false, 0 },
        { "cmp eax,1B0h", { 0x3D, 0xB0, 0x01, 0x00, 0x00 }, 5, Flow::Next, 0, false, 0 },
        { "setae al", { 0x0F, 0x93, 0xC0 }, 3, Flow::Next, Bit(RAX), false, 0 },
        { "mov [rdi+28h],al", { 0x88, 0x47, 0x28 }, 3, Flow::Next, 0, true, 1 },
        { "mov byte [rdi+29h],r12b", { 0x44, 0x88, 0x67, 0x29 }, 4, Flow::Next, 0, true, 1 },
        { "cmp ecx,0BAD00000h", { 0x81, 0xF9, 0x00, 0x00, 0xD0, 0xBA }, 6, Flow::Next, 0, false, 0 },
        { "and ecx,0FFF00000h", { 0x81, 0xE1, 0x00, 0x00, 0xF0, 0xFF }, 6, Flow::Next, Bit(RCX), false, 0 },
        { "cmp qword [rip+g],0", { 0x48, 0x83, 0x3D, 0x10, 0x00, 0x00, 0x00, 0x00 }, 8, Flow::Next, 0, false, 0 },
        { "cmp byte [rdi+29h],r12b", { 0x44, 0x38, 0x67, 0x29 }, 4, Flow::Next, 0, false, 0 },
        { "mov byte [rbp+38h],1", { 0xC6, 0x45, 0x38, 0x01 }, 4, Flow::Next, 0, true, 1 },
        { "mov dword [rsi+8],1", { 0xC7, 0x46, 0x08, 0x01, 0x00, 0x00, 0x00 }, 7, Flow::Next, 0, true, 4 },
        // vector moves and conversions
        { "movups xmm0,[rsp+68h]", { 0x0F, 0x10, 0x44, 0x24, 0x68 }, 5, Flow::Next, 0, false, 0 },
        { "movups [rax],xmm0", { 0x0F, 0x11, 0x00 }, 3, Flow::Next, 0, true, 16 },
        { "xorps xmm0,xmm0", { 0x0F, 0x57, 0xC0 }, 3, Flow::Next, 0, false, 0 },
        { "movdqu xmm0,[rax]", { 0xF3, 0x0F, 0x6F, 0x00 }, 4, Flow::Next, 0, false, 0 },
        { "movdqu [rax],xmm0", { 0xF3, 0x0F, 0x7F, 0x00 }, 4, Flow::Next, 0, true, 16 },
        { "cvtsi2sd xmm0,eax", { 0xF2, 0x0F, 0x2A, 0xC0 }, 4, Flow::Next, 0, false, 0 },
        { "cvttsd2si eax,xmm0", { 0xF2, 0x0F, 0x2C, 0xC0 }, 4, Flow::Next, Bit(RAX), false, 0 },
        { "movd eax,xmm0", { 0x66, 0x0F, 0x7E, 0xC0 }, 4, Flow::Next, Bit(RAX), false, 0 },
        { "movq xmm0,xmm1", { 0xF3, 0x0F, 0x7E, 0xC1 }, 4, Flow::Next, 0, false, 0 },
        { "movzx eax,byte [rdi+12h]", { 0x0F, 0xB6, 0x47, 0x12 }, 4, Flow::Next, Bit(RAX), false, 0 },
        { "movsxd rax,ecx", { 0x48, 0x63, 0xC1 }, 3, Flow::Next, Bit(RAX), false, 0 },
        // locked operations and padding
        { "lock xadd [rbx+0Ch],eax", { 0xF0, 0x0F, 0xC1, 0x43, 0x0C }, 5, Flow::Next, Bit(RAX), true, 4 },
        { "lock inc dword [rax]", { 0xF0, 0xFF, 0x00 }, 3, Flow::Next, 0, true, 4 },
        { "nop dword [rax]", { 0x0F, 0x1F, 0x00 }, 3, Flow::Next, 0, false, 0 },
        { "nop dword [rax+rax]", { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, 5, Flow::Next, 0, false, 0 },
        { "66 nop", { 0x66, 0x90 }, 2, Flow::Next, 0, false, 0 },
        { "nop", { 0x90 }, 1, Flow::Next, 0, false, 0 },
        // the epilogue
        { "add rsp,490h", { 0x48, 0x81, 0xC4, 0x90, 0x04, 0x00, 0x00 }, 7, Flow::Next, Bit(RSP), false, 0 },
        { "pop r15", { 0x41, 0x5F }, 2, Flow::Next, Bit(R15), false, 0 },
        { "pop rbp", { 0x5D }, 1, Flow::Next, Bit(RBP), false, 0 },
        { "mov rbx,[rsp+0B0h]", { 0x48, 0x8B, 0x9C, 0x24, 0xB0, 0x00, 0x00, 0x00 }, 8, Flow::Next, Bit(RBX), false, 0 },
        // implicit writes
        { "cdq", { 0x99 }, 1, Flow::Next, Bit(RDX), false, 0 },
        { "idiv ecx", { 0xF7, 0xF9 }, 2, Flow::Next, Bit(RAX) | Bit(RDX), false, 0 },
        { "neg rax", { 0x48, 0xF7, 0xD8 }, 3, Flow::Next, Bit(RAX), false, 0 },
        { "test dword [rax],1", { 0xF7, 0x00, 0x01, 0x00, 0x00, 0x00 }, 6, Flow::Next, 0, false, 0 },
        { "xchg r13,rax", { 0x49, 0x87, 0xC5 }, 3, Flow::Next, Bit(R13) | Bit(RAX), false, 0 },
        { "xchg rcx,rax (short form)", { 0x48, 0x91 }, 2, Flow::Next, Bit(RCX) | Bit(RAX), false, 0 },
        { "xchg r9,rax (short form)", { 0x49, 0x91 }, 2, Flow::Next, Bit(R9) | Bit(RAX), false, 0 },
        { "mov r13b,al", { 0x41, 0x88, 0xC5 }, 3, Flow::Next, Bit(R13), false, 0 },
        { "mov sil,al", { 0x40, 0x88, 0xC6 }, 3, Flow::Next, Bit(RSI), false, 0 },
        { "mov ah,al (no rex: ah is part of rax)", { 0x88, 0xC4 }, 2, Flow::Next, Bit(RAX), false, 0 },
        { "mov r11,imm64", { 0x49, 0xBB, 1, 2, 3, 4, 5, 6, 7, 8 }, 10, Flow::Next, Bit(R11), false, 0 },
        { "shl rax,5", { 0x48, 0xC1, 0xE0, 0x05 }, 4, Flow::Next, Bit(RAX), false, 0 },
        { "rep stosq", { 0xF3, 0x48, 0xAB }, 3, Flow::Next, Bit(RAX) | Bit(RCX) | Bit(RSI) | Bit(RDI), false, 0 },
        { "cmovnz eax,ecx", { 0x0F, 0x45, 0xC1 }, 3, Flow::Next, Bit(RAX), false, 0 },
        { "bswap r9d", { 0x41, 0x0F, 0xC9 }, 3, Flow::Next, Bit(R9), false, 0 },
        // review 2, M1: REX.B on 90 is xchg r8,rax; four prefixes are fine
        { "xchg r8,rax (41 90)", { 0x41, 0x90 }, 2, Flow::Next, Bit(RAX) | Bit(R8), false, 0 },
        { "four operand-size prefixes and a nop", { 0x66, 0x66, 0x66, 0x66, 0x90 }, 5, Flow::Next, 0, false, 0 },
        { "cld", { 0xFC }, 1, Flow::Next, 0, false, 0 },
        { "call [rsp+8] reads 8 bytes", { 0xFF, 0x54, 0x24, 0x08 }, 4, Flow::Call, 0, false, 0 },
    };
    for (const Row& r : rows)
    {
        Insn n;
        const bool ok = Dec(r.bytes, n);
        CHECK_MSG(ok, "%s: refused", r.what);
        if (!ok)
            continue;
        CHECK_MSG(n.len == r.len, "%s: length %u, wanted %u", r.what, n.len, r.len);
        CHECK_MSG(n.flow == r.flow, "%s: flow %d, wanted %d", r.what, static_cast<int>(n.flow), static_cast<int>(r.flow));
        // push/pop/sub/add on rsp are reported through writesStack; compare the register mask without rsp for those
        CHECK_MSG(n.gprWrites == r.writes, "%s: writes %04X, wanted %04X", r.what, n.gprWrites, r.writes);
        CHECK_MSG(n.memWrite == r.memWrite, "%s: memWrite %d, wanted %d", r.what, n.memWrite, r.memWrite);
        if (r.memWrite)
            CHECK_MSG(n.memSize == r.memSize, "%s: store size %u, wanted %u", r.what, n.memSize, r.memSize);
    }
}

void OperandDetails()
{
    Insn n;
    CHECK(Dec({ 0x48, 0x89, 0x54, 0x24, 0x60 }, n)); // mov [rsp+60h],rdx
    CHECK(n.opcode == 0x89 && n.rexW && n.hasModrm && n.reg == RDX && n.mem.present && n.mem.base == RSP && n.mem.index == kNoReg && n.mem.disp == 0x60);

    CHECK(Dec({ 0x4C, 0x8B, 0xEA }, n)); // mov r13,rdx
    CHECK(n.mod == 3 && n.reg == R13 && n.rm == RDX && n.rexW);

    CHECK(Dec({ 0x48, 0x8B, 0x84, 0x24, 0x38, 0x04, 0x00, 0x00 }, n)); // mov rax,[rsp+438h]
    CHECK(n.mem.base == RSP && n.mem.disp == 0x438 && n.memBytes == 8);

    CHECK(Dec({ 0x48, 0x8D, 0x4D, 0x40 }, n)); // lea rcx,[rbp+40h]
    CHECK(n.isLea && n.mem.base == RBP && n.mem.disp == 0x40 && n.memBytes == 1);

    CHECK(Dec({ 0x48, 0x8B, 0x0D, 0x45, 0x40, 0x63, 0x00 }, n)); // mov rcx,[rip+634045h]
    CHECK(n.mem.ripRel && n.mem.base == kNoReg && n.mem.disp == 0x634045);

    CHECK(Dec({ 0x4A, 0x8B, 0x04, 0xE9 }, n)); // mov rax,[rcx+r13*8]
    CHECK(n.mem.base == RCX && n.mem.index == R13 && n.mem.scale == 3);

    CHECK(Dec({ 0x48, 0x8B, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00 }, n)); // mov rax,[10h] (no base, no index)
    CHECK(n.mem.base == kNoReg && n.mem.index == kNoReg && !n.mem.ripRel && n.mem.disp == 0x10);

    // direct targets are absolute RVAs
    CHECK(Dec({ 0x0F, 0x85, 0x19, 0x04, 0x00, 0x00 }, n, 0x2000));
    CHECK(n.target == 0x2000 + 6 + 0x419);
    CHECK(Dec({ 0xEB, 0xFE }, n, 0x2000));
    CHECK(n.target == 0x2000);
    CHECK(Dec({ 0xE8, 0xFB, 0xFF, 0xFF, 0xFF }, n, 0x2000));
    CHECK(n.flow == Flow::Call && n.target == 0x2000);
    CHECK(Dec({ 0xFF, 0x15, 0, 0, 0, 0 }, n, 0x2000));
    CHECK(n.flow == Flow::Call && n.target == -1);

    // rsp / rbp copied as a value
    CHECK(Dec({ 0x48, 0x8B, 0xC4 }, n)); // mov rax,rsp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x48, 0x89, 0xE8 }, n)); // mov rax,rbp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x48, 0x8B, 0x04, 0x24 }, n)); // mov rax,[rsp]: an address, not a value
    CHECK(!n.readsFrameRegAsValue);
    CHECK(Dec({ 0x0F, 0x10, 0xE5 }, n)); // movups xmm4,xmm5: vector registers, not rsp/rbp
    CHECK(!n.readsFrameRegAsValue);
    CHECK(Dec({ 0x0F, 0x28, 0xEC }, n)); // movaps xmm5,xmm4
    CHECK(!n.readsFrameRegAsValue);

    // writes to rsp / push / pop are reported
    CHECK(Dec({ 0x50 }, n));
    CHECK(n.writesStack);
    CHECK(Dec({ 0x41, 0x5D }, n)); // pop r13
    CHECK(n.writesStack && n.gprWrites == Bit(R13));
    CHECK(Dec({ 0x48, 0x83, 0xEC, 0x28 }, n)); // sub rsp,28h
    CHECK(n.writesStack);
    CHECK(Dec({ 0xC9 }, n)); // leave
    CHECK(n.writesStack && (n.gprWrites & Bit(RBP)) && (n.gprWrites & Bit(RSP)));

    // immediates are sign-extended
    CHECK(Dec({ 0x48, 0x83, 0xEC, 0xF8 }, n));
    CHECK(n.imm == -8 && n.immLen == 1);
    CHECK(Dec({ 0x66, 0xB8, 0x34, 0x12 }, n)); // mov ax,1234h: a 16-bit immediate
    CHECK(n.len == 4 && n.immLen == 2 && n.imm == 0x1234);
}

void SixteenBitOperandSizeAndFrameValues()
{
    Insn n;
    // review 2, M1: 66 changes the length of push imm and of branches, and REX.W overrides it: none of that is guessed
    CHECK(!Dec({ 0x66, 0x68, 0x34, 0x12 }, n));                      // push imm16
    CHECK(!Dec({ 0x66, 0x6A, 0x01 }, n));                            // push imm8 with a 16-bit operand
    CHECK(!Dec({ 0x66, 0x48, 0x05, 0x78, 0x56, 0x34, 0x12 }, n));    // add rax,imm32 under 66 + REX.W
    CHECK(Dec({ 0x66, 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8 }, n) && n.len == 11); // mov rax,imm64: REX.W with 66 is still fine (the immediate is 64 bits)
    CHECK(Dec({ 0xFF, 0x54, 0x24, 0x08 }, n) && n.memBytes == 8);       // call [rsp+8] reads a pointer
    CHECK(!Dec({ 0x66, 0xE8, 0x01, 0x00, 0x00, 0x00 }, n));          // call with 66
    CHECK(!Dec({ 0x66, 0xE9, 0x01, 0x00, 0x00, 0x00 }, n));          // jmp with 66
    CHECK(!Dec({ 0x66, 0xEB, 0x01 }, n));
    CHECK(!Dec({ 0x66, 0x74, 0x01 }, n));                            // jz with 66
    CHECK(!Dec({ 0x66, 0x0F, 0x85, 0x01, 0x00, 0x00, 0x00 }, n));    // jnz rel32 with 66
    CHECK(!Dec({ 0x66, 0xC3 }, n));                                  // retw
    CHECK(Dec({ 0x66, 0xB8, 0x34, 0x12 }, n) && n.len == 4);          // mov ax,imm16 is a plain 16-bit form
    CHECK(Dec({ 0x66, 0x90 }, n) && n.len == 2);
    CHECK(Dec({ 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 }, n) && n.len == 6); // nop word [rax+rax]

    // rsp / rbp as a register operand in a MEMORY form: the frame address is stored somewhere
    CHECK(Dec({ 0x48, 0x89, 0x60, 0xD5 }, n));                       // mov [rax-2Bh],rsp
    CHECK(n.readsFrameRegAsValue && n.memWrite);
    CHECK(Dec({ 0x48, 0x89, 0x68, 0x08 }, n));                       // mov [rax+8],rbp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x88, 0x60, 0x08 }, n));                             // mov [rax+8],ah (high byte, no REX): not rsp
    CHECK(!n.readsFrameRegAsValue);
    CHECK(Dec({ 0x40, 0x88, 0x68, 0x08 }, n));                       // mov [rax+8],bpl (REX): the low byte of rbp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x66, 0x48, 0x0F, 0x6E, 0xE4 }, n));                 // movq xmm4,rsp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x66, 0x48, 0x0F, 0x6E, 0xE5 }, n));                 // movq xmm4,rbp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0xF2, 0x48, 0x0F, 0x2A, 0xC4 }, n));                 // cvtsi2sd xmm0,rsp
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x66, 0x0F, 0xC4, 0xC5, 0x01 }, n));                 // pinsrw xmm0,ebp,1
    CHECK(n.readsFrameRegAsValue);
    CHECK(Dec({ 0x66, 0x0F, 0x6E, 0xC1 }, n));                       // movd xmm0,ecx: nothing to flag
    CHECK(!n.readsFrameRegAsValue);
    CHECK(Dec({ 0x48, 0x8D, 0x04, 0x29 }, n));                       // lea rax,[rcx+rbp*1]: rbp as the index
    CHECK(n.isLea && n.readsFrameRegAsValue);
    CHECK(Dec({ 0x48, 0x8D, 0x45, 0xD0 }, n));                       // lea rax,[rbp-30h]: an address, handled by the proof
    CHECK(n.isLea && !n.readsFrameRegAsValue);

    // stores whose address the operand does not give
    CHECK(Dec({ 0xF3, 0x48, 0xAB }, n) && n.aliasStore);            // rep stosq
    CHECK(Dec({ 0xA4 }, n) && n.aliasStore);                         // movsb
    CHECK(Dec({ 0xF3, 0x48, 0xA5 }, n) && n.aliasStore);            // rep movsq
    CHECK(Dec({ 0x48, 0x0F, 0xAB, 0x04, 0x24 }, n) && n.aliasStore); // bts [rsp],rax
    CHECK(Dec({ 0x0F, 0xB3, 0x00 }, n) && n.aliasStore);            // btr [rax],eax
    CHECK(Dec({ 0x0F, 0xBB, 0x00 }, n) && n.aliasStore);            // btc [rax],eax
    CHECK(Dec({ 0x0F, 0xAB, 0xC1 }, n) && !n.aliasStore);           // bts ecx,eax: a register
    CHECK(Dec({ 0x0F, 0xBA, 0x28, 0x05 }, n) && !n.aliasStore);     // bts dword [rax],5: the bit offset is inside the operand
    CHECK(Dec({ 0xAC }, n) && !n.aliasStore);                        // lodsb reads
    CHECK(Dec({ 0xA6 }, n) && !n.aliasStore);                        // cmpsb reads
    CHECK(Dec({ 0x48, 0x89, 0x01 }, n) && !n.aliasStore);           // an ordinary store
    CHECK(!Dec({ 0xFD }, n));                                        // std: a downward string store would reach below
}

void RefusesWhatItDoesNotKnow()
{
    Insn n;
    // truncated
    CHECK(!Dec({ 0x48, 0x89 }, n));
    CHECK(!Dec({ 0xE8, 0x01 }, n));
    CHECK(!Dec({ 0x48, 0x81, 0xEC, 0x90 }, n));
    CHECK(!Dec({ 0x48, 0x8B, 0x84, 0x24, 0x38 }, n));
    CHECK(!Dec({}, n));
    // out of the table: VEX / EVEX, x87, the 0F 38 / 0F 3A maps, moffs, far control transfers, segment loads, string compares by port, indirect far
    CHECK(!Dec({ 0xC5, 0xF8, 0x77 }, n));                   // vzeroupper
    CHECK(!Dec({ 0xC4, 0xE2, 0x7D, 0x00, 0xC1 }, n));       // vex
    CHECK(!Dec({ 0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1 }, n)); // evex
    CHECK(!Dec({ 0xD9, 0x45, 0x00 }, n));                   // fld dword [rbp]
    CHECK(!Dec({ 0x0F, 0x38, 0x00, 0xC1 }, n));             // pshufb
    CHECK(!Dec({ 0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08 }, n)); // palignr
    CHECK(!Dec({ 0xA1, 1, 2, 3, 4, 5, 6, 7, 8 }, n));       // mov eax,moffs
    CHECK(!Dec({ 0xCD, 0x29 }, n));                         // int 29h
    CHECK(!Dec({ 0xEA, 0, 0, 0, 0, 0, 0 }, n));             // far jmp
    CHECK(!Dec({ 0xFF, 0x2D, 0, 0, 0, 0 }, n));             // jmp far [rip]
    CHECK(!Dec({ 0x8E, 0xD8 }, n));                         // mov ds,eax
    CHECK(!Dec({ 0x0F, 0x05 }, n));                         // syscall
    CHECK(!Dec({ 0x67, 0x8B, 0x00 }, n));                   // address-size override
    CHECK(!Dec({ 0x64, 0x48, 0x8B, 0x04, 0x25, 0x30, 0, 0, 0 }, n)); // gs/fs override
    CHECK(!Dec({ 0x0F, 0xC7, 0x08 }, n));                   // cmpxchg8b
    CHECK(!Dec({ 0x66, 0x0F, 0xF7, 0xC1 }, n));             // maskmovdqu (writes memory through rdi)
    CHECK(!Dec({ 0xC6, 0xF8, 0x00 }, n));                   // xabort (C6 /7)
    CHECK(!Dec({ 0x0F, 0xFF, 0xC0 }, n));                   // ud0
    CHECK(!Dec({ 0x0F, 0xAE, 0x00 }, n));                   // fxsave (memory form of 0F AE)
    // indirect jmp is decoded as such (the guards refuse it)
    CHECK(Dec({ 0xFF, 0xE0 }, n));
    CHECK(n.flow == Flow::JumpIndirect);
    // the instruction length never exceeds the bytes given
    CHECK(!Dec({ 0x66, 0x66, 0x66, 0x66, 0x66, 0x90 }, n)); // five prefixes (four are accepted, see the table)
}
} // namespace

int main()
{
    Table();
    OperandDetails();
    SixteenBitOperandSizeAndFrameValues();
    RefusesWhatItDoesNotKnow();
    if (g_failures)
    {
        std::printf("fg_preset_x64: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_x64: instruction lengths, control flow, register and memory effects pinned; unknown forms refused");
    return 0;
}
