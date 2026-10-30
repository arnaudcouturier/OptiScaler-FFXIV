#pragma once
// Anchors for the companion / nameplate native hooks (seam ids companion.*).
//
// OWNER: the companion seams. This is the ONLY resolver file that seam work edits (plus its own tests under tests/ext/ffxiv/resolve/).
// Every anchor below was written AFTER reading its consumer: ext\ffxiv\companion\CompanionNative.cpp (the four Detours and the queue chain),
// CompanionPackets.h (queue snapshot, batch header, Substitute/PointerScope), CompanionVisibilityQueue.h (index buffer, layout slot table,
// vertex ranges) and replay\FfxivNameplate*.h (the same offsets again). The consumer facts each anchor pins are listed per seam; what it
// cannot pin is listed in the "not self-healing" list at the end of this file.
//
// ---- TWO TIERS PER DETOUR SITE (the two-tier shape follows the design of AnchorsJitter.h) --------------------------------------------------
// On an UNKNOWN build a bound Guard field is filled with the LIVE bytes read at the resolved site, so once the resolver replaces the kCurrentBuild reads
// the hooks' byte guard (Install()) compares live bytes with themselves and checks nothing: the anchors are then the ONLY verification. Every
// Detour site (batch16, batch32, enqueue, bindState) therefore has
//   * a LOCATION seam (`batch16`, `batch32`, `enqueue`, `bindState`): found by two or three independent anchors (the strict head among them,
//     which is also what detects a foreign jump over the entry: EntryPatched). Bound to the Rva fields;
//   * a SHAPE seam (`<name>Shape`, a CodeSite, failClosed): exactly ONE guard anchor (fromSeam = the location seam, offset 0) holding the strict
//     prologue pattern, verified at the location's address. The Guard fields are bound to the SHAPE seams, never to the location seams
//     (batchInstructions to batch16Shape; batch32Shape has no field of its own, the table keeps one batch guard). One anchor on purpose: with
//     two a failing piece next to a passing one would read FoundSingleAnchor (usable) and fill the Guard with live bytes. When the strict
//     pattern does not match the shape seam is NotFound, its Guard field stays ZERO (on a KNOWN build too: failClosed, the table's 16-byte guard
//     is only a prefix of the strict pattern) and the hooks' compare of the live bytes against zero refuses to install.
// Hook-site rule for the companion group: a Detour site is usable ONLY when BOTH its location seam and its shape seam are usable (on a known build
// ResolvedBuild::UsableOrTable on both: the location may be filled from the table, the shape never is). There is NO table fallback for a shape.
// batch16 and batch32 are two sites: test batch32Shape directly (batchInstructions is the batch16 guard only). The gate must keep the compare of the
// live bytes against the resolved Guard field. Also: bindState must not be hooked on an unknown build unless `Usable("companion.nativeContextOffset")`
// (an unresolved Offset seam leaves the table default, and nativeContextOffset is dereferenced as a COM pointer, CompanionLayer.cpp), and prefer a
// plain Found (both anchors) for it: only anchor `loadBeforeStateCall` pins the vtable call that proves the object is a D3D11 context.
// enqueue is a leaf: only anchor `callerEpilogue` (a real call target) pins its ENTRY address, the head and the tail shift together. On an unknown
// build the gate should hook enqueue only when that anchor's AnchorResult (SeamResult::anchors, name "callerEpilogue") is Unique, NOT on the sum
// SeamResult::hits (an Unique head plus a two-candidate tail also sums to 3); without it a new first statement before the head would be
// a silent plain Found at entry+k (the shape seam verifies the head at that address, which matches).
//
// Contract: mid-function sites would be CodeSite (the shape seams are CodeSites: they are never a jump
// check), return addresses come from Binding::valueOffset (none needed), the two batch functions are told apart by a caller-side anchor AND by an
// instruction inside the body (never by address order alone) and are listed in Registry::mustDiffer, decoded offsets carry pred::ValueIn,
// every predicate and fromSeam names a companion.* seam only (this worktree is self-contained), and every pattern text is pinned by
// tests/ext/ffxiv/resolve/resolve_companion_pins.cpp independently of GameBuild.h.
//
// Masking rule used here: a pattern masks ONLY rel32 / RIP displacements and call targets the consumer does not read, and the struct
// displacement of the field an Offset seam DECODES (that seam checks the value with ValueIn instead). Every other struct displacement, stack
// slot and register use stays fixed, so a recompiled frame layout or a changed struct does not match silently.
//
// Seams (bound field in brackets):
//   companion.batch16, companion.batch32   the two native UI batch submitters (void(renderer, char mode)); [batch16Rva, batch32Rva]
//   companion.enqueue                      the draw-command enqueue (leaf, int(context, command)); [enqueueRva]
//   companion.bindState                    the draw-state bind (void(context, state, instances)); [bindStateRva]
//   companion.batch16Shape, batch32Shape,  the strict prologue of each Detour site, verified at the location seam's address (CodeSite,
//   enqueueShape, bindStateShape           failClosed); [batchInstructions (batch16Shape), enqueueInstructions, bindStateInstructions]
//   companion.nameplateModulePointer       the UI module singleton ([[g]+8] renderer state, [[g]+8]+8 queue); [nameplateModulePointerRva]
//   companion.layoutSlotTable              the .rdata table layout -> vertex slot (8 bytes per entry); [layoutSlotTableRva]
//   companion.indexBufferMap               helper (no field): the function the batches call with [renderer+0x1e0]; it ties the index
//                                          buffer offsets to the object the consumer reads
//   17 Offset seams                        one per struct offset an instruction carries (see below); [..Offset fields]

#include "ResolveTypes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ext::ffxiv::resolve
{
namespace companion_detail
{
// Everything from the entry of a batch function to the load of the index-buffer byte size ([r13+0x560]). Identical in batch16 and batch32;
// the instruction that follows tells them apart (shr rax,1: 16-bit indices, shr rax,2: 32-bit indices). Fixed: the prologue spills (rdx ->
// mode, rcx -> renderer: the consumer's signature), the loads of renderer+0x580 (batch list), +0x588 (batch count) and +0x1e0 (index buffer)
// that the consumer swaps or reads, the `mov r9d,2` and the stack slots. Masked: the rel32 of the call to the index-buffer map.
inline const char* BatchHead()
{
    return "88 54 24 10 48 89 4C 24 08 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 "
           "48 8B 81 80 05 00 00 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 "
           "48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ?? 48 8B D8 48 89 44 24 58 4C 8B F8 "
           "48 89 44 24 38 4C 8B E0 49 8B 85 60 05 00 00 ";
}
inline std::string Batch16Entry() { return std::string(BatchHead()) + "48 D1 E8 48 89 44 24 50 33 C0"; }
inline std::string Batch32Entry() { return std::string(BatchHead()) + "48 C1 E8 02 48 89 44 24 50 33 C0"; }

// The dispatcher (0x63b250) that calls the batch functions: renderer in rcx, mode in dl, and the choice by the index-buffer type
// ([ib+0x3c] == 2 calls the 32-bit batch, anything else the 16-bit one). Two E8 calls: the first one (Rel32 at 20) is batch32, the second (at 27)
// is batch16. The rel32 are the only masked bytes.
inline const char* DispatcherCall()
{
    return "48 8B 83 E0 01 00 00 40 0F B6 D5 48 8B CB 83 78 3C 02 75 07 E8 ?? ?? ?? ?? EB 05 E8 ?? ?? ?? ??";
}

// The tail-call dispatcher stub (0x63b730), the SECOND caller of the 16-bit batch function: the same choice as the dispatcher
// above made with a `je` to the 32-bit batch and a `jmp` (E9, Rel32 at 17) to the 16-bit one. Only the jmp can be decoded (Rel32 reads E8 / E9;
// RipDisp32 refuses the `0F 84` that carries the je). Unique (margin 5), target the 16-bit function. It is what turns "both strict heads broken
// AND the dispatcher's call order inverted" from a silent swap of the two batch seams into a Conflict of batch16.
inline const char* DispatcherStub() { return "48 8B 81 E0 01 00 00 83 78 3C 02 0F 84 ?? ?? ?? ?? E9 ?? ?? ?? ??"; }

// ---- the bytes the Offset seams decode (the displacement of the field is masked, everything around it is fixed) ----
// batch body, first block: mov rax,[rcx+LIST] ; mov r13,rcx ; ... mov eax,[rcx+COUNT] ; xor r8d,r8d ; mov rcx,[rcx+INDEXBUFFER] ; xor edx,edx
inline const char* BatchHeaderLoads()
{
    return "48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??";
}
// batch body, per-layout vertex slot: mov edx,[r13+VERTEXOFFSET] ; add rdx,[r13+VERTEXALLOCATION] ; mov r11d,[r13+VERTEXSTRIDE]
inline const char* BatchVertexSlot()
{
    return "33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78";
}
// batch body: layout = header[FORMAT] & 15 (the consumer's `format & 15`), compared against the cached layout
inline const char* BatchFormat()
{
    return "41 8B 4C 24 ?? 8B C1 83 E0 0F 3B 44 24 44 75 32 8B C1 C1 E8 04 0F B6 C0 3B 44 24 34";
}
// batch body: cmp word [header+EXTENSION],2 (the consumer's extension word)
inline const char* BatchExtension() { return "66 41 83 7C 24 ?? 02 44 8B 54 24 40 40 0F 94 C7 44 3B D3"; }
// batch body: the layout -> slot lookup  lea rcx,[image base] ; movsxd rcx,[rcx+rbx*8+TABLE] ; mov rax,rcx ; add rax,rax ; mov eax,[r13+rax*8+..] ;
// add rax,[r13+..] ; ... lea rax,[rcx+0x50] ; add rax,rax ; mov ecx,[r13+rax*8]. The *8 of the table index (entry size 8), the *16 of the slot
// stride (add rax,rax with scale 8) and the slot*16+0x500 form are fixed; the lea displacement is position dependent and masked.
inline const char* BatchLayoutLookup()
{
    return "48 8D 0D ?? ?? ?? ?? 48 63 8C D9 ?? ?? ?? ?? 48 8B C1 48 03 C0 41 8B 84 C5 ?? ?? ?? ?? 49 03 85 ?? ?? ?? ?? 48 89 45 88 48 8D 41 50 "
           "48 03 C0 41 8B 4C C5 00";
}
// The call from the batch functions to the index-buffer map: r9d = 2, the renderer's +0x1e0 object in rcx (fixed offset), edx = 0, r8d = 0.
inline const char* BatchIndexBufferCall()
{
    return "41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ??";
}
// the index-buffer map (0x21eb50): entry through the first range check
inline const char* IndexMapEntry()
{
    return "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 8B 49 38 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B 40 F6 C1 03 74 0A "
           "41 F6 C1 01 0F 85 ?? ?? ?? ??";
}
// the index-buffer map body: capacity [rcx+CAPACITY], flags [rbx+FLAGS], the flag logic (flags & 0x11, flags & 0x40) and the two CPU pointers
// ([rbx+CPUSTART], [rbx+CPUEND]); masked: the four struct displacements and the rel32 / RIP displacements.
inline const char* IndexMapBody()
{
    return "8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? "
           "8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? "
           "48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??";
}
// bindState (0x22d750): the argument mapping after the prologue (rdx -> r14 state, rcx -> rsi context, r8d -> r12d instances) and the
// `cmp byte [state+FLAG],0` that opens the function; the displacement of the flag is masked, the jump rel32 too.
inline const char* BindStatePrologue()
{
    return "80 7A ?? 00 4C 8B F2 48 89 5C 24 ?? 48 8B F1 48 89 6C 24 ?? 4C 89 64 24 ?? 45 8B E0 4C 89 7C 24 ?? 0F 84 ?? ?? ?? ??";
}
// bindState: the load of the native context ([rsi+CONTEXT]) before the virtual calls through its vtable, twice in the function. The consumer
// dereferences that field as a COM pointer (CompanionLayer.cpp Stamp: GetType / SetPrivateData), so the bytes must prove it is a D3D11 context:
//   A  mov rdi,[rsi+CTX] ; mov edx,ebp ; mov rax,[rdi] ; mov rbx,[rax+0x118]   (vtable slot 35: loaded here, called later through rbx)
//   Call  the whole call site of the vtable call: cmp [rbx+0x3c],2 ; mov edx,0x39 ; mov rcx,[rsi+CTX] ; mov r8d,0x2a ; cmovne r8d,edx ; ... ;
//         mov rax,[rcx] ; ... ; call [rax+0x98]   (vtable slot 19 = ID3D11DeviceContext::IASetIndexBuffer, r8d 0x2a / 0x39 = R32_UINT / R16_UINT).
//         The same text is the location anchor `contextCall` of the bindState seam (the CTX displacement masked in both).
inline const char* BindStateContextA() { return "48 8B BE ?? ?? ?? ?? 8B D5 48 8B 07 48 8B 98 18 01 00 00"; }
inline const char* BindStateContextCall()
{
    return "83 7B 3C 02 BA 39 00 00 00 48 8B 8E ?? ?? ?? ?? 41 B8 2A 00 00 00 44 0F 45 C2 8B 15 ?? ?? ?? ?? 45 33 C9 48 8B 01 "
           "48 8B 54 D3 48 FF 90 98 00 00 00";
}
// enqueue tail: lea rcx,[rdx+0x10] (16-byte entries) ; mov [r9+0x20],rcx ; mov [rdx],eax ; mov [rdx+ENTRYPOINTER],r11 (the command) ; ret
inline const char* EnqueueTail() { return "49 8B 51 20 48 8D 4A 10 49 89 49 20 89 02 4C 89 5A ?? C3"; }
// batch body, the batch list read the consumer's Substitute() rewrites: mov r9,[rbp-0x18] (the list pointer saved at entry) ; add rax,rax (with
// the scale 8: 16-byte entries) ; mov r12,[r9+rax*8+ENTRYPOINTER] (the batch header pointer) ; test r12,r12 ; je ; mov eax,[r12] (the kind, a
// displacement-less read at +0: pinned, not decoded)
inline const char* BatchListEntry() { return "4C 8B 4D E8 48 03 C0 4D 8B 64 C1 ?? 4D 85 E4 0F 84 ?? ?? ?? ?? 41 8B 04 24"; }
} // namespace companion_detail

inline void AddCompanionAnchors(Registry& r)
{
    using namespace companion_detail;

    auto scan = [](const char* name, std::string pattern)
    {
        AnchorSpec a;
        a.name = name;
        a.pattern = std::move(pattern);
        return a;
    };
    // an anchor whose value is a struct offset / immediate read from `at` of the match (Offset seams)
    auto field = [](const char* name, std::string pattern, std::int32_t at, std::uint8_t width, std::uint32_t lo, std::uint32_t hi)
    {
        AnchorSpec a;
        a.name = name;
        a.pattern = std::move(pattern);
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::Field, at, 0, 0, width };
        a.predicates = { pred::ValueIn(lo, hi) };
        return a;
    };
    // a field anchor that must lie in the function of another seam. Where the pattern occurs in two functions (batch16 and batch32) the raw
    // scan has two hits and only SameFunctionAs picks this function's one: the margin gate (which excludes only the surviving candidates)
    // would count the sibling function's copy as "another site", so minMargin is 0 here and the predicates are the disambiguator.
    auto fieldIn = [&field](const char* name, std::string pattern, std::int32_t at, std::uint8_t width, std::uint32_t lo, std::uint32_t hi,
                            const char* functionSeam, bool twoFunctions)
    {
        AnchorSpec a = field(name, std::move(pattern), at, width, lo, hi);
        a.predicates.push_back(pred::SameFunctionAs(functionSeam));
        if (twoFunctions)
            a.minMargin = 0;
        return a;
    };
    // A SHAPE seam: one guard anchor, never scanned: the strict pattern is verified at value(location) + 0. failClosed: never filled from the table.
    auto shapeSeam = [](const char* name, const char* locationSeam, std::string strict)
    {
        AnchorSpec a;
        a.name = "shape";
        a.pattern = std::move(strict);
        a.fromSeam = locationSeam;
        SeamSpec s;
        s.id = std::string("companion.") + name;
        s.kind = SeamKind::CodeSite;
        s.anchors.push_back(std::move(a));
        s.failClosed = true;
        return s;
    };
    auto offsetSeam = [&r](const char* name, std::vector<AnchorSpec> anchors)
    {
        const std::string id = std::string("companion.") + name;
        r.seams.push_back({ id, SeamKind::Offset, std::move(anchors) });
        r.bindings.push_back({ id, id, 0 });
    };

    // ======================================================================================================================================
    // companion.batch16 / companion.batch32: Detours over the two native UI batch submitters, void(renderer rcx, char mode dl).
    //
    // Consumer (CompanionNative.cpp OnBatch16/OnBatch32, Invoke, VisibilityScope): hooks the ENTRY, swaps the batch list pointer at
    // renderer+0x580 (PointerScope) while the original runs, reads count at +0x588, vertex allocation +0x570, vertex stride/offset +0x500/+0x504,
    // the index buffer at +0x1e0, and passes the index width (2 for batch16, 4 for batch32) to the visibility code.
    //
    // Anchors (both seams, two independent ways, neither is the prologue alone; batch16 has a third, the tail-call stub):
    //   entryToIndexShift  the whole head from the entry (entryAnchor: a foreign jump over the entry reads as EntryPatched) through the load of
    //                      the index byte size and the shift that makes the index count: shr 1 is batch16, shr 2 is batch32. It pins the
    //                      Detour's instruction boundaries (the first 9 bytes are two whole instructions), the (renderer, mode) spills, the
    //                      three renderer loads the hook relies on and the index width. The two functions differ in exactly the shift, so the
    //                      pattern says which is which; margin against the sibling is 8 / 9 fixed bytes.
    //   dispatcherCall     the caller: the dispatcher picks batch32 when the index buffer type is 2 and batch16 otherwise, and passes (renderer,
    //                      mode). Rel32 target of the first call = batch32, of the second = batch16. Independent of the callee bytes.
    //   dispatcherStub     (batch16 only) the second caller of batch16, a tail-call stub with the same choice: `je batch32 ; jmp batch16`, the jmp
    //                      decoded. batch32 has no second caller (its je cannot be decoded). Unique, margin 5.
    // Same address order is NOT used anywhere: Registry::mustDiffer makes a swap or a collapse a Conflict.
    // The 16-byte prologue alone is NOT an anchor: it hits 2 sites and the nearest other function differs in one byte (margin 1).
    // ======================================================================================================================================
    {
        AnchorSpec l16 = scan("entryToIndexShift", Batch16Entry());
        l16.entryAnchor = true;
        AnchorSpec c16 = scan("dispatcherCall", DispatcherCall());
        c16.valueFrom = ValueFrom::Target;
        c16.decode = { DecodeKind::Rel32, 27, 0, 0, 0 };
        c16.predicates = { pred::TargetIn(RegionKind::Code) };
        AnchorSpec stub16 = scan("dispatcherStub", DispatcherStub());
        stub16.valueFrom = ValueFrom::Target;
        stub16.decode = { DecodeKind::Rel32, 17, 0, 0, 0 };
        stub16.predicates = { pred::TargetIn(RegionKind::Code) };
        r.seams.push_back({ "companion.batch16", SeamKind::Function, { l16, c16, stub16 } });

        AnchorSpec l32 = scan("entryToIndexShift", Batch32Entry());
        l32.entryAnchor = true;
        AnchorSpec c32 = scan("dispatcherCall", DispatcherCall());
        c32.valueFrom = ValueFrom::Target;
        c32.decode = { DecodeKind::Rel32, 20, 0, 0, 0 };
        c32.predicates = { pred::TargetIn(RegionKind::Code) };
        r.seams.push_back({ "companion.batch32", SeamKind::Function, { l32, c32 } });

        r.mustDiffer.push_back({ "companion.batch16", "companion.batch32" });
        r.bindings.push_back({ "companion.batch16Rva", "companion.batch16", 0 });
        r.bindings.push_back({ "companion.batch32Rva", "companion.batch32", 0 });
        // The SHAPE seams: the strict head (the same text as the location seams' entryToIndexShift anchor) verified at the location's address.
        r.seams.push_back(shapeSeam("batch16Shape", "companion.batch16", Batch16Entry()));
        r.seams.push_back(shapeSeam("batch32Shape", "companion.batch32", Batch32Entry()));
        // One Guard field for both sites (the table keeps one array: both functions start with the same 16 bytes). It is read at batch16, through
        // the shape seam; batch32Shape has no field, the gate tests it directly.
        r.bindings.push_back({ "companion.batchInstructions", "companion.batch16Shape", 0 });
    }

    // ======================================================================================================================================
    // companion.enqueue: Detour over the draw-command enqueue int(context rcx, command rdx), a LEAF function (no .pdata entry).
    //
    // Consumer (OnEnqueue): Layer::EmitVisibilityCommand(command) with the command pointer, then the original. The command goes into a queue of
    // 16-byte entries {key, command}.
    //
    // Anchors:
    //   entry            the old 16-byte guard as an entry anchor (movsxd rax,[rcx+0xc] ; mov r11,rdx ; lea r8,[rax+rax*2] ; mov eax,[rcx+8] ;
    //                    first bytes of add dword [..],-1). Two whole instructions (7 bytes) are inside it, so a Detour copies whole
    //                    instructions. Unique, margin 9. The ONLY anchor with recorded bytes in the previous build.
    //   entryWriteTail   the tail of the same function: lea rcx,[rdx+0x10] ; mov [r9+0x20],rcx ; mov [rdx],eax ; mov [rdx+8],r11 ; ret, with the
    //                    reset of the block counter (mov dword [r9+0x28],0x3ff) in front. It pins that rdx (the command) is stored in the entry
    //                    at +8 and that an entry is 16 bytes. The entry is 0x9b bytes back (valueOffset: leaf functions have no .pdata).
    //   callerEpilogue   the CALLER side: the tail of a wrapper that calls enqueue (mov rdx,rdi ; mov [rdi+0x30],r14 ; mov rcx,rsi ;
    //                    call enqueue ; the epilogue). The Rel32 target IS the entry, read from a real call instruction, so it is the only anchor
    //                    that does not move with the function's own bytes. Without it a leaf whose head and tail shift together (a prefix before
    //                    the head, a changed head and length) gave a usable single-anchor or a silent plain Found at entry+k, inside the function.
    //                    Unique at 0x23c0d2 (margin 6, 38 fixed bytes), target TargetIn(Code).
    // ======================================================================================================================================
    {
        AnchorSpec e1 = scan("entry", "48 63 41 0C 4C 8B DA 4C 8D 04 40 8B 41 08 42 83");
        e1.entryAnchor = true;
        AnchorSpec e2 = scan("entryWriteTail", "41 C7 41 28 FF 03 00 00 49 8B 51 20 48 8D 4A 10 49 89 49 20 89 02 4C 89 5A 08 C3");
        e2.valueOffset = -0x9b; // tail at 0x240c1b, entry at 0x240b80
        AnchorSpec e3 = scan("callerEpilogue", "48 8B D7 4C 89 77 30 48 8B CE E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 6C 24 38 48 8B 74 24 40 48 8B 7C 24 48 "
                                               "48 83 C4 20 41 5E C3");
        e3.valueFrom = ValueFrom::Target;
        e3.decode = { DecodeKind::Rel32, 10, 0, 0, 0 };
        e3.predicates = { pred::TargetIn(RegionKind::Code) };
        r.seams.push_back({ "companion.enqueue", SeamKind::Function, { e1, e2, e3 } });
        r.bindings.push_back({ "companion.enqueueRva", "companion.enqueue", 0 });
        // the shape: the 16-byte head at the entry (the same text as anchor `entry`; whole instructions in its first 7 bytes)
        r.seams.push_back(shapeSeam("enqueueShape", "companion.enqueue", e1.pattern));
        r.bindings.push_back({ "companion.enqueueInstructions", "companion.enqueueShape", 0 });
    }

    // ======================================================================================================================================
    // companion.bindState: Detour over the draw-state bind void(context rcx, state rdx, UINT instances r8d).
    //
    // Consumer (OnBindState): calls the ORIGINAL first, then Layer::BindVisibilityCommand(context, state); it reads context+0x17b8 (native context
    // pointer) and treats state-0x20 as the command that was enqueued (VisibilityQueue::Bind); drawStateFlagOffset 0x49 is the flag byte.
    //
    // Anchors:
    //   entry            the old 16-byte guard (push rsi ; push r14 ; sub rsp,0x48 ; cmp byte [rdx+0x49],0 ; mov r14,rdx ; first byte of the
    //                    next instruction). Whole instructions in the first 8 bytes. Unique, margin 4.
    //   argumentMapping  the entry plus the spills that show the argument mapping: rdx -> r14 (state), rcx -> rsi (context), r8d -> r12d
    //                    (instances); the function start comes from .pdata (EnclosingFunction), so it also checks the unwind data.
    //   contextCall      a body anchor 0x164 bytes in: the second load of the native context and the virtual call through it, found through
    //                    .pdata. Independent of the prologue bytes.
    // ======================================================================================================================================
    {
        AnchorSpec s1 = scan("entry", "40 56 41 56 48 83 EC 48 80 7A 49 00 4C 8B F2 48");
        s1.entryAnchor = true;
        AnchorSpec s2 = scan("argumentMapping",
                             "40 56 41 56 48 83 EC 48 80 7A 49 00 4C 8B F2 48 89 5C 24 70 48 8B F1 48 89 6C 24 78 4C 89 64 24 38 45 8B E0 4C 89 7C 24 30 "
                             "0F 84 ?? ?? ?? ??");
        s2.valueFrom = ValueFrom::EnclosingFunction;
        AnchorSpec s3 = scan("contextCall", BindStateContextCall());
        s3.valueFrom = ValueFrom::EnclosingFunction;
        r.seams.push_back({ "companion.bindState", SeamKind::Function, { s1, s2, s3 } });
        r.bindings.push_back({ "companion.bindStateRva", "companion.bindState", 0 });
        // the shape: the entry plus the argument mapping (anchor `argumentMapping`, rel32 masked); the Guard is its first 16 bytes
        r.seams.push_back(shapeSeam("bindStateShape", "companion.bindState", s2.pattern));
        r.bindings.push_back({ "companion.bindStateInstructions", "companion.bindStateShape", 0 });
    }

    // ======================================================================================================================================
    // companion.nameplateModulePointer: the UI module singleton, a pointer global in the bss tail of .data. MEDIUM confidence (it was medium in
    // the table too: reference-chain evidence, no instruction guard).
    //
    // Consumer (CompanionNative.cpp Queue(), FfxivNameplatePackets.h): module = [base+RVA]; renderer = [module+8]; queue = [renderer+8].
    //
    // Evidence (honest limit): three code sites, each a distinct piece of UI code, load the global and walk exactly that chain [[g]+8]+8 and
    // then use fields of the queue-sized object (+0x80, +0x84, +0x90). Nothing in the executable says that this object is the NamePlate
    // command queue the consumer snapshots (queue +0x50/+0x68/+0x6c, 64-entry blocks): that last link stays a runtime check (the consumer
    // range-checks every read and the first chain log line shows the values). All three anchors must give the same global for the seam to be
    // Found; any other global with the same chain shape has at most 2 sites, none with these bytes. Each pattern is a complete short sequence
    // and carries the displacement of the global as the only mask.
    // ======================================================================================================================================
    const char* const chainSites[3][2] = {
        { "chainKeySet", "48 8B 05 ?? ?? ?? ?? 0F B6 91 EF 01 00 00 48 8B 48 08 48 8B 41 08 89 90 90 00 00 00" },
        { "chainKeyReset", "48 8B 05 ?? ?? ?? ?? 48 8B 48 08 48 8B 41 08 C7 80 90 00 00 00 00 00 00 00" },
        { "chainSecondary", "48 8B 05 ?? ?? ?? ?? 48 8B 58 08 0F 85 ?? ?? ?? ?? 48 8B 4B 08" },
    };
    {
        std::vector<AnchorSpec> anchors;
        for (const auto& site : chainSites)
        {
            AnchorSpec a = scan(site[0], site[1]);
            a.valueFrom = ValueFrom::Target;
            a.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
            a.predicates = { pred::TargetIn(RegionKind::Bss) };
            anchors.push_back(std::move(a));
        }
        r.seams.push_back({ "companion.nameplateModulePointer", SeamKind::Global, std::move(anchors) });
        r.bindings.push_back({ "companion.nameplateModulePointerRva", "companion.nameplateModulePointer", 0 });
    }
    // The two +8 of that chain (module -> renderer state, state -> queue): the byte is fixed in the three patterns (that is the pin), the
    // Offset seams only read it back, so a different offset could not have matched the pointer anchors in the first place.
    {
        const std::int32_t moduleAt[3] = { 17, 10, 10 }, queueAt[3] = { 21, 14, 20 };
        std::vector<AnchorSpec> module, queue;
        for (int i = 0; i < 3; ++i)
        {
            module.push_back(field(chainSites[i][0], chainSites[i][1], moduleAt[i], 1, 1, 0x7f));
            queue.push_back(field(chainSites[i][0], chainSites[i][1], queueAt[i], 1, 1, 0x7f));
        }
        offsetSeam("moduleStatePointerOffset", std::move(module));
        offsetSeam("stateQueuePointerOffset", std::move(queue));
    }

    // ======================================================================================================================================
    // companion.layoutSlotTable: the .rdata table that maps a batch layout (0..8) to a vertex slot (0..2), read by the consumer as
    // int at [imageBase + RVA + layout*8] (CompanionVisibilityQueue.h Begin).
    //
    // Anchors:
    //   inBatchA / inBatchB   the lookup in the two batch functions: movsxd rcx,[rcx+rbx*8+TABLE] with rcx = the image base; the disp32 IS the
    //                         table RVA (Field decode). expectedHits 2, one anchor per function (both must agree). It pins the table entry
    //                         size (*8) and the vertex slot stride (*16). The lea that makes the image base is position dependent and masked:
    //                         that rcx really is the image base is NOT proven by the bytes.
    //   content               the 72-byte table itself in read-only data (nine entries, low dwords 0 0 0 0 1 2 2 1 1, high dwords 0 0 0 0 5 4
    //                         3 2 1): unique in .rdata. The only anchor with recorded previous-build bytes.
    // ======================================================================================================================================
    {
        AnchorSpec a = field("inBatchA", BatchLayoutLookup(), 11, 4, 0x100000, 0x6000000);
        a.expectedHits = 2;
        a.pickIndex = 0;
        AnchorSpec b = field("inBatchB", BatchLayoutLookup(), 11, 4, 0x100000, 0x6000000);
        b.expectedHits = 2;
        b.pickIndex = 1;
        AnchorSpec c = scan("content",
                            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
                            "01 00 00 00 05 00 00 00 02 00 00 00 04 00 00 00 02 00 00 00 03 00 00 00 01 00 00 00 02 00 00 00 01 00 00 00 01 00 00 00");
        c.searchIn = SearchIn::ReadOnlyData;
        r.seams.push_back({ "companion.layoutSlotTable", SeamKind::Table, { a, b, c } });
        r.bindings.push_back({ "companion.layoutSlotTableRva", "companion.layoutSlotTable", 0 });
    }

    // ======================================================================================================================================
    // companion.indexBufferMap (helper, no GameBuild field): the function both batches call as f(renderer[+0x1e0], 0, 0, 2). It returns the CPU
    // pointer of an index buffer and is where the consumer's index-buffer layout comes from (CompanionVisibilityQueue.h Emit: flags, capacity,
    // CPU start / end). Because the batch functions pass the renderer's +0x1e0 object to it, the offsets decoded from it belong to the object the
    // consumer reads at renderer+0x1e0.
    //
    // Anchors: entry (the first 57 bytes, entryAnchor), the call from batch16 and from batch32 (Rel32 target, one anchor each), and the
    // body with the flag logic (flags & 0x11, flags & 0x40 select CPU start or CPU end), found through .pdata.
    // ======================================================================================================================================
    {
        AnchorSpec entry = scan("entry", IndexMapEntry());
        entry.entryAnchor = true;
        AnchorSpec callA = scan("calledFromBatchA", BatchIndexBufferCall());
        callA.expectedHits = 2;
        callA.pickIndex = 0;
        callA.valueFrom = ValueFrom::Target;
        callA.decode = { DecodeKind::Rel32, 28, 0, 0, 0 };
        callA.predicates = { pred::TargetIn(RegionKind::Code) };
        AnchorSpec callB = callA;
        callB.name = "calledFromBatchB";
        callB.pickIndex = 1;
        AnchorSpec body = scan("flagLogic", IndexMapBody());
        body.valueFrom = ValueFrom::EnclosingFunction;
        r.seams.push_back({ "companion.indexBufferMap", SeamKind::Function, { entry, callA, callB, body } });
    }

    // ======================================================================================================================================
    // Offset seams: the struct offsets an instruction carries. Each reads the displacement from the instruction the consumer's access mirrors
    // and checks it with ValueIn. Two anchors where the same read exists twice (batch16 and batch32, bindState twice, the CPU start pointer
    // twice in the index map); they must agree. Anchors tied to a function seam use SameFunctionAs, so they cannot be satisfied by a copy
    // of the pattern elsewhere.
    // ======================================================================================================================================
    {
        // renderer (the batch functions' rcx)
        auto both = [&](const char* seamName, std::string pattern, std::int32_t at, std::uint8_t width, std::uint32_t lo, std::uint32_t hi)
        {
            offsetSeam(seamName, { fieldIn("inBatch16", pattern, at, width, lo, hi, "companion.batch16", true),
                                   fieldIn("inBatch32", pattern, at, width, lo, hi, "companion.batch32", true) });
        };
        both("rendererBatchListOffset", BatchHeaderLoads(), 3, 4, 0x40, 0x4000);        // mov rax,[rcx+0x580]
        both("rendererBatchCountOffset", BatchHeaderLoads(), 22, 4, 0x40, 0x4000);      // mov eax,[rcx+0x588]
        both("rendererIndexBufferOffset", BatchHeaderLoads(), 32, 4, 0x40, 0x4000);     // mov rcx,[rcx+0x1e0]
        both("rendererVertexOffsetOffset", BatchVertexSlot(), 5, 4, 0x40, 0x4000);      // mov edx,[r13+0x504]
        both("rendererVertexAllocationOffset", BatchVertexSlot(), 12, 4, 0x40, 0x4000); // add rdx,[r13+0x570]
        both("rendererVertexStrideOffset", BatchVertexSlot(), 19, 4, 0x40, 0x4000);     // mov r11d,[r13+0x500]
        // batch header (the 64-byte command header, r12 in the batch functions)
        both("batchFormatOffset", BatchFormat(), 4, 1, 1, 0x3c);       // mov ecx,[r12+0x30] ; and eax,0xf (4-byte read of the 64-byte header: max 0x3c)
        both("batchExtensionOffset", BatchExtension(), 5, 1, 1, 0x3e); // cmp word [r12+4],2 (2-byte read of the 64-byte header: max 0x3e)
    }
    {
        // bindState
        offsetSeam("drawStateFlagOffset", { fieldIn("firstCompare", BindStatePrologue(), 2, 1, 1, 0x7f, "companion.bindState", false) });
        offsetSeam("nativeContextOffset", { fieldIn("loadBeforeVirtualCall", BindStateContextA(), 3, 4, 0x100, 0x10000, "companion.bindState", false),
                                            fieldIn("loadBeforeStateCall", BindStateContextCall(), 12, 4, 0x100, 0x10000, "companion.bindState", false) });
    }
    {
        // index buffer, from the index-buffer map
        auto in = [&](const char* name, std::int32_t at)
        { return fieldIn(name, IndexMapBody(), at, 1, 1, 0x7f, "companion.indexBufferMap", false); };
        offsetSeam("indexBufferCapacityOffset", { in("capacityLoad", 2) });  // mov ecx,[rcx+0x38]
        offsetSeam("indexBufferFlagsOffset", { in("flagsLoad", 28) });       // mov ecx,[rbx+0x40]
        offsetSeam("indexBufferCpuStartOffset", { in("startCompare", 61), in("startReturn", 78) }); // cmp qword [rbx+0x60],0 ; mov rax,[rbx+0x60]
        offsetSeam("indexBufferCpuEndOffset", { in("endReturn", 93) });      // mov rax,[rbx+0x68]
    }
    {
        // the queue entry {key, pointer}: the pointer is the second qword. Read in both batch functions (the list entries the consumer swaps) and
        // written by enqueue (the command queue); all three must say the same.
        AnchorSpec store = field("commandStore", EnqueueTail(), 17, 1, 8, 8); // ValueEquals(8): the consumer indexes entry[1] of {key, pointer}
        store.predicates.push_back(pred::After("companion.enqueue", 0x200));
        offsetSeam("entryPointerOffset", { fieldIn("listEntryInBatch16", BatchListEntry(), 11, 1, 8, 8, "companion.batch16", true),
                                           fieldIn("listEntryInBatch32", BatchListEntry(), 11, 1, 8, 8, "companion.batch32", true), store });
    }

    // ======================================================================================================================================
    // NOT SELF-HEALING (no instruction of this group carries them, or no unique carrier with a proven role was found; they keep the table value
    // on an unknown build and the resolver logs them): queue block layout (queueHeadOffset 0x50, queueRemainingOffset 0x68, queueBlocksOffset 0x6c,
    // queueStridesOffset 0x34, queueNextBlockOffset 0x3f0: 440 functions use a +0x3f0 field, none was tied to this queue), batch header
    // (batchKindOffset 0x00 is a displacement-less read, fixed in the list-entry pattern; batchWrapperOffset 0x10 and batchCountOffset 0x20 /
    // batchVerticesOffset 0x28 are carried by `mov r8,[r12+0x10]` and `lea r9,[r12+0x20]` in the batch functions but the roles of those fields
    // are unproven there), resourceWrapperOffset 0x68, draw command (drawCommandTypeOffset 0x00, baseVertexOffset 0x08, startIndexOffset 0x14,
    // indexCountOffset 0x18: read by the executor of the draw commands, which no anchor of this group reaches), drawStateCommandBackPointerOffset
    // 0x20 (the state-0x20 = command relation is not carried by bindState), rendererQueuesOffset 0x590 (no consumer reads it).
    // The Constants (queue count/stride, command size, batch entry/header sizes, slot stride, layout entry size, drawCommandType) are never bound;
    // the entry size 16 is pinned by the enqueue tail, the table entry size 8 and the slot stride 16 by the layout lookup.
    //
    // KNOWN LIMITS (found in review, deliberately not widened; tests/ext/ffxiv/resolve/resolve_companion_flips.cpp pins the mask of every
    // anchor, so a change of any of them is an edit on purpose):
    //   * fixed rel8 branches whose target lies outside the anchor span (BatchFormat `75 32`, IndexMapBody `74 72` / `74 23`, IndexMapEntry `74 0A`)
    //     and frame-slot displacements fixed in the Offset anchors: a recompile that moves them fails closed (the table default stays) and costs the
    //     healing of the 8 + 5 offsets those patterns serve.
    //   * foreign hooks: only E9 / EB / FF 25 over an entry read as EntryPatched on the location seams. Every other shape (mov rax,imm64 ; jmp rax,
    //     push ; ret, a leading int3) reads as NotFound or as a usable location, but the SHAPE seam of each Detour site refuses any foreign byte of
    //     the strict prologue, so the Guard stays zero and the site is not usable (hook-site rule above). indexBufferMap is a helper with no hook.
    //   * layout lookup: `lea rcx,[rip+X]` (rcx = the image base) is masked and not provable by the bytes, and its two vertex displacements
    //     ([r13+rax*8+0x504], [r13+0x570]) are masked and never compared with the BatchVertexSlot decode (flips test class L). If the table RVA
    //     were wrong the consumer rejects it (slot / stride checks): availability only. `content` (72 bytes in .rdata) is the real pin.
    //   * pseudo-evidence: moduleStatePointerOffset / stateQueuePointerOffset decode a byte the pointer patterns fix (always 8: ValueIn cannot
    //     reject, they stay Found when the pointer seam is NotFound); drawStateFlagOffset has no consumer in ext\; the two indexBufferCpuStartOffset
    //     anchors are two reads of one pattern. Count them as constants in the "healed" tally.
    //   * the batch header offsets heal one by one (format, extension) while kind, wrapper, count and vertices keep table values: if the header was
    //     reshuffled the siblings are stale (the consumer's Supported() rejects most such headers). The gate treats the header group
    //     all-or-nothing (kOffsetGroups in Gate.h).
    //   * CompanionPackets.h (UiModuleGlobalRva, VertexSlotTableRva) read ActiveBuild(), so a resolved layoutSlotTableRva /
    //     nameplateModulePointerRva is used there. The nameplate pointer stays MEDIUM confidence (reference-chain evidence).
    //   * batch32 has one caller anchor (its `je rel32` cannot be decoded): with both strict heads broken and the dispatcher's call order inverted,
    //     batch16 is a Conflict but batch32's location is the 16-bit function's address; only batch32Shape (the strict 32-bit head) refuses it.
    // ======================================================================================================================================
}
} // namespace ext::ffxiv::resolve
