#pragma once
// Anchors of P2 group 3: the source-level jitter fix (generator, camera getter, producer call, camera head, phases, strength, device
// size), the globals those blocks load and the struct offsets / constants their instructions carry. Seam ids are `jitterSource.*`.
//
// OWNER: the P2 "jitter" writer. This is the ONLY resolver file that writer edits (plus its own tests under tests/ext/ffxiv/resolve/).
//
// Consumers (read before writing the anchors): FfxivJitterSourceHooks.cpp / FfxivJitterSourceDetours.h / FfxivJitterSourceCore.h /
// FfxivJitterSourceAnchors.h. The hooks: a Detour on the generator (rewrites its phase count and divides its x/y outputs by the native
// strength, ONLY for the call at generatorCallReturnRva whose x pointer is the jitter object the camera getter returned to the NGX packet
// producer) and a Detour on the camera getter (observe only, ONLY for the call at producerReturnRva). Nothing else is written to the game.
//
// ---- TWO TIERS PER HOOKED BLOCK (the safety design) -------------------------------------------------------------------------------
// On an UNKNOWN build a bound Guard field is filled with the LIVE bytes read at the resolved site, so once P3 replaces the kCurrentBuild
// reads the hooks' byte guard (VerifyAnchors) compares live bytes with themselves and checks nothing. The anchors are then the ONLY
// verification, so every block has
//   * a LOCATION seam (`generator`, `cameraGetter`, `cameraHead`, `producerCall`, `phases`, `strength`, `deviceSize`): found by loose
//     anchors (two independent ones where two exist), bound to the Rva fields. A Function seam whose entry already starts with a jump
//     (a foreign hook) is EntryPatched there and is never filled from the table, exactly like the old byte guards;
//   * a SHAPE seam (`<name>Shape`, a CodeSite): exactly ONE guard anchor (fromSeam = the location seam) holding the strict pattern, every
//     instruction and displacement the consumer relies on FIXED, only RIP displacements, call targets and a TLS slot immediate masked. The
//     Guard fields are bound to the SHAPE seam, never to the location seam. One anchor on purpose: with two anchors a failing piece next
//     to a passing one reads FoundSingleAnchor, which is usable, which would fill the Guard field with live bytes (the safety gap again).
//     When the strict pattern does not match, the shape seam is NotFound, its Guard field stays ZERO and the hooks' byte compare (live
//     bytes against zero) refuses to install. deviceSize is its own shape (its only anchor is the strict 20 bytes).
//     EVERY shape seam carries SeamSpec::failClosed (pinned per seam in resolve_jitter_anchors): on a KNOWN build the resolver would
//     otherwise fill the Guard field of a seam that did not match from the table, and the table's Guard array is only a PREFIX of the strict
//     pattern (generator 16 of 306 bytes, strength 157 of 204, producerCall 13 of 111): the hooks would install over code that differs from
//     the audited file. With failClosed a shape that does not match is never filled, on any build (resolve_jitter_known: the review's PoC
//     rows and every fixed byte of every shape, on a known build).
// P3 CONTRACT: a hook site is usable only when BOTH its location seam and its shape seam are `Usable` (on EVERY build: a shape is never
// filled from the table, so UsableOrTable adds nothing for it and must not be used for the sites below); P3 must keep the compare of the
// live bytes against the resolved Guard field (VerifyAnchors). Also required: the three globals (`settingsPointer`, `rendererPointer`,
// `devicePointer`: the hooks read memory through them and VerifyAnchors does not look at them; on an unknown build a Conflict leaves the
// field zero, on a known build it is filled from the table: see the gate below), and the
// constant seams `strengthFloor` and `ndcSignMask` (they check the live constants that GameBuild holds as strengthFloorBits and ndcSignMaskBits,
// which the jitter fix reads; the gate requires them to be usable) and `cameraGlobal` (must equal the quality group's
// jitter.cameraGlobalRva, which the jitter probe and the world tie read: the gate compares the two values).
//
// ---- WHAT EACH SHAPE PINS (the consumer's assumptions) and what no anchor can pin ------------------------------------------------
//   generatorShape      the WHOLE generator (306 bytes, 286 fixed): the Detour needs the instruction boundaries of the prologue
//                       (mov rax,rsp / mov [rax+8],rbx / mov [rax+0x10],rsi / push rdi / sub rsp,0x70), the argument roles (rsi=rdx=x,
//                       rdi=r8=y, r9d=frame counter, div [rsp+0xa0] = the fifth argument = the phase count) and the outputs (x-0.5 stored
//                       through rsi, y-0.5 through rdi, ret). The outputs sit at the function's tail, and one contiguous pattern is the only
//                       way to pin both ends in ONE anchor, so the Halton loops in between are pinned too (a recompile of the loops costs
//                       the hook, never safety). Cannot pin: the 0.5 / 1.0 / 3.0 constants behind the masked RIP displacements (the core's
//                       NormalizeHalton checks |x|,|y| <= 0.501 at run time).
//   cameraGetterShape   the whole 8-byte leaf: mov rax,[rcx+0x278] ; ret.
//   producerCallShape   the three global loads of the NGX packet producer and `mov rcx,[rax+0x3028]` (the world camera = the getter's
//                       argument), the eight instructions between it and the call (none writes rcx: the camera reaches the call
//                       unchanged), the E8 whose target is the getter, and `mov rcx,[rax]` of the returned object. Masked: the RIP
//                       displacements, the call target and the immediate of `mov r13d,imm32` (a thread-local slot offset the linker assigns;
//                       the consumer never reads it). Cannot pin: that this producer is the WORLD-scene one beyond "its camera is
//                       [camera global + 0x3028]" (the table never pinned more).
//   cameraHeadShape     the camera update's prologue and frame, the stack cookie, the settings load and the mode byte (+0x54) compared
//                       with 2 (the DLSS-mode test the plan relies on). Masked: the two RIP displacements and the displacement of the final
//                       `jne` (the hooks do not read it; it counted 15 unpinned bytes behind the shape).
//   phasesShape         the jitter object pointer ([rdi+0x278]), the renderer scene size read as one qword (+0x428: width low, height
//                       high), `mov edx,-1`, the DLSS-mode branch (r8b,2), the device output width, the call that computes the other modes'
//                       phase count (target masked) and the generator's argument setup: r9d=[rbx+0x10] (counter), r8=rbx+4 (y), the fifth
//                       argument edx at [rsp+0x20], rcx=rsp+0x30 (the opaque argument), rdx=rbx (x = the jitter object the world tie
//                       compares). NOTE: `lea rcx,[rsp+0x30]` is the opaque FIRST argument; the x pointer the world tie compares is
//                       `mov rdx,rbx`. Both are fixed.
//   strengthShape       the generator call (E8 whose target is the generator, return address = site + 5), the extents (+0x700/+0x704),
//                       the settings amount (+0x78) and scale (+0x4c), the whole strength expression, the doubling, the NDC division, the
//                       y sign flip, the stores of the NDC pair at [rsp+0x38]/[rsp+0x3c] and `mov [rcx+8],rax` (the NDC pair lands at the
//                       jitter object + 8, where the observe hook reads it). Longer than the old 157-byte guard so that the NDC store is
//                       pinned. Cannot pin: the values of the .rdata constants behind masked displacements except the two seams below.
//   deviceSize          the device global load and the output size as the pair (+0x8c width, +0x90 height). Two byte-identical template
//                       instances exist (0xd3f73 and 0xd4013): expectedHits 2 and pickIndex 0 (the table names the first). The consumer only
//                       needs one reader of the pair, so a swap of the two would be harmless; a third copy makes the seam Ambiguous
//                       (fail closed).
//   strengthFloor       the float behind the `subss` and the `addss` of the strength expression (read through RipDeref32).
//   ndcSignMask         the constant behind `xorps xmm2,[rip+..]` of the NDC y: the sign flip the core's ProjectOffset assumes.
// Struct offsets (settings +0x4c/+0x54/+0x78, renderer +0x428/+0x700, device +0x8c) are carried by instructions inside the shapes, which
// keep them FIXED (the consumer reads memory at them): the Offset seams below decode them from the same instructions and check a sanity
// range, but because the shapes pin the bytes they are NOT self-healing in practice (a moved offset refuses the hooks). The pair
// relations the consumer assumes (extent +4, scene size w/h, device w/h) are pinned only where one instruction or one fixed pair carries
// them (qword load of the scene size, the +0x90/+0x8c pair, the +0x700/+0x704 pair).
//
// Not here: jitter.cameraGlobalRva / cameraOffset / jitterPointerOffset (AnchorsQuality.h owns them). The 0x3028 and 0x278 displacements
// appear FIXED inside the patterns below (the consumer's world tie depends on them); that duplication across groups is intended.
// `jitterSource.cameraGlobal` decodes the producer's load of the same camera global under THIS group's id (no field is bound, no seam of
// another group is named), so that the world tie is complete: the shape pins the 0x3028, this seam says which global.
//
// TWO ANCHORS PER SEAM WHERE TWO EXIST. Seams with one anchor have only one instruction or one site to look at, and say so where they are
// written: the four single-instruction struct offsets (mode, scale, amount, extents), `ndcSignMask`, `cameraGlobal`, `deviceSize` (one pattern
// for its two template copies). A shape seam has exactly one anchor on purpose (see above).
// Nothing here runs at load time: the function is called by BuildRegistry() when the registry is built.

#include "ResolveTypes.h"

#include <string>

namespace ext::ffxiv::resolve
{
// ---- THE HOOK SITES AS DATA (P3 iterates these; resolve_jitter_shapes checks them against the registry) --------------------------------
// One row per block VerifyAnchors compares (same order). `location` is the seam the Rva field is bound to, `shape` the seam the Guard field
// is bound to (the same seam for deviceSize, whose only anchor is its strict 20 bytes). For every row with location != shape: the shape seam is a
// CodeSite with exactly ONE anchor, a guard at the location seam (shape value == location value), and carries failClosed.
struct JitterHookSite
{
    const char* location; // seam id, bound to the Rva field `rvaKey`
    const char* shape;    // seam id, bound to the Guard field `guardKey`
    const char* rvaKey;   // GameBuild field key of the location's Rva
    const char* guardKey; // GameBuild field key of the shape's Guard
    const char* block;    // the VerifyAnchors block name of this site
};
inline constexpr JitterHookSite kJitterHookSites[] = {
    { "jitterSource.generator", "jitterSource.generatorShape", "jitterSource.generatorRva", "jitterSource.generatorInstructions", "generator" },
    { "jitterSource.cameraGetter", "jitterSource.cameraGetterShape", "jitterSource.cameraGetterRva", "jitterSource.cameraGetterInstructions", "cameraGetter" },
    { "jitterSource.producerCall", "jitterSource.producerCallShape", "jitterSource.producerCallRva", "jitterSource.producerCallInstructions", "producerCall" },
    { "jitterSource.cameraHead", "jitterSource.cameraHeadShape", "jitterSource.cameraHeadRva", "jitterSource.cameraHeadInstructions", "cameraHead" },
    { "jitterSource.phases", "jitterSource.phasesShape", "jitterSource.phasesRva", "jitterSource.phasesInstructions", "phases" },
    { "jitterSource.strength", "jitterSource.strengthShape", "jitterSource.strengthRva", "jitterSource.strengthInstructions", "strength" },
    { "jitterSource.deviceSize", "jitterSource.deviceSize", "jitterSource.deviceSizeRva", "jitterSource.deviceSizeInstructions", "deviceSize" },
};

// The three globals the hooks read memory through (ReadInputs / the observe hook). VerifyAnchors does NOT look at them.
struct JitterGlobal
{
    const char* seam;   // a Global seam
    const char* rvaKey; // its Rva field
};
inline constexpr JitterGlobal kJitterGlobals[] = {
    { "jitterSource.settingsPointer", "jitterSource.settingsPointerRva" },
    { "jitterSource.rendererPointer", "jitterSource.rendererPointerRva" },
    { "jitterSource.devicePointer", "jitterSource.devicePointerRva" },
};

// The seams that have NO GameBuild field (nothing the hooks read is filled by them): P3 must gate on the seam itself (`Usable`) until fields exist.
inline constexpr const char* kJitterGateSeams[] = { "jitterSource.strengthFloor", "jitterSource.ndcSignMask", "jitterSource.cameraGlobal" };

// ---- THE GATE P3 MUST APPLY (everything the resolver cannot enforce by itself; each line has a test) ------------------------------------
//  1. for every kJitterHookSites row: rb.Usable(location) && rb.Usable(shape), on EVERY build (known or unknown). A shape is never filled
//     from the table (failClosed), so UsableOrTable is not the question for these seams.
//  2. for every kJitterGlobals row: rb.Usable(seam) on EVERY build; the required MINIMUM is a non-zero Rva field. A global whose derivations
//     disagree is a `Conflict`. On an UNKNOWN build that leaves its field ZERO and VerifyAnchors does not look at the field: measured with the
//     real VerifyAnchors, ONE moved RIP displacement in the camera head (settings) or in the strength block (renderer) gives Conflict,
//     settingsPointerRva / rendererPointerRva == 0 and VerifyAnchors PASSING (only the device global is caught, by the TargetIs tie of
//     deviceSize). Without the gate ReadInputs reads base + 0, the PE header, as a pointer. The minimum, three lines:
//        if (b.settingsPointerRva == 0 || b.rendererPointerRva == 0 || b.devicePointerRva == 0) refuse;
//     That check is NOT equivalent to Usable(seam): on a KNOWN build the globals are not failClosed, so a Conflict global is filled from the
//     table (non-zero Rva, fellBackToTable, Usable false) and only `rb.Usable(seam)` refuses it. `Usable` on every build is the stricter form
//     and the one that matches rule 1; the zero-Rva check alone closes the unknown-build hole only.
//     (resolve_jitter_anchors, TestMovedDisplacementIsAConflict, records the VerifyAnchors result of each unknown-build case, applies both
//     forms, and has the known-build row.)
//  3. for every kJitterGateSeams row: rb.Usable(seam) (no field exists yet: strengthFloor, ndcSignMask, cameraGlobal).
//  4. then VerifyAnchors over the live bytes (the Guard compare).
// Core files are NOT changed by the P2c group: this is P3 work.
//
inline void AddJitterAnchors(Registry& r)
{
    // ------------------------------------------------------------------------------------------------------------------------------
    // Strict shapes (one guard anchor each). Displacements of RIP loads and call targets are masked, everything else is fixed.
    // ------------------------------------------------------------------------------------------------------------------------------

    // The whole generator, 0x2ebd90 .. 0x2ebec1 (ret). Masked: three RIP loads of constants, the two calls to the float-to-int helper.
    constexpr const char* kGeneratorShape =
        "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 70 0F 29 70 E8 48 8B F2 0F 29 78 D8 33 D2 44 0F 29 40 C8 49 8B F8 44 0F 29 48 B8 "
        "0F 57 FF F3 44 0F 10 0D ?? ?? ?? ?? 0F 57 F6 44 0F 29 50 A8 41 8B C1 F7 B4 24 A0 00 00 00 F3 44 0F 10 15 ?? ?? ?? ?? "
        "45 0F 28 C2 8D 5A 01 8B CB 85 DB 0F 84 93 00 00 00 0F 57 C0 F3 45 0F 59 C1 8B C1 83 E0 01 F3 48 0F 2A C0 8B C1 F3 41 0F 59 C0 "
        "F3 0F 58 F0 0F 57 C0 F3 48 0F 2A C0 F3 41 0F 59 C1 E8 ?? ?? ?? ?? F3 48 0F 2C C8 85 C9 75 C8 F3 44 0F 10 05 ?? ?? ?? ?? "
        "0F 1F 40 00 66 0F 1F 84 00 00 00 00 00 0F 57 C0 B8 AB AA AA AA F7 E3 F3 45 0F 5E D0 8B CB D1 EA 8D 04 52 2B C8 8B C1 "
        "F3 48 0F 2A C0 8B C3 F3 41 0F 59 C2 F3 0F 58 F8 0F 57 C0 F3 48 0F 2A C0 F3 41 0F 5E C0 E8 ?? ?? ?? ?? F3 48 0F 2C D8 85 DB "
        "75 BB 4C 8D 5C 24 70 49 8B 5B 10 F3 41 0F 5C F1 45 0F 28 43 D0 F3 41 0F 5C F9 45 0F 28 4B C0 45 0F 28 53 B0 F3 0F 11 36 "
        "49 8B 73 18 0F 28 74 24 60 F3 0F 11 3F 0F 28 7C 24 50 49 8B E3 5F C3";

    constexpr const char* kCameraGetterShape = "48 8B 81 78 02 00 00 C3";

    // The camera update's head, 0x25f690 .. 0x25f6dd. Masked: the stack-cookie load, the settings load and the displacement of the final `jne`
    // (`75 0F`, a jump over the 15 bytes 0x25f6de..0x25f6ec that this shape does not cover: the displacement is the length of code the hooks never
    // read, so a change THERE must not cost the shape; the opcode, the compare before it and everything the consumer reads stay fixed).
    constexpr const char* kCameraHeadShape =
        "40 55 57 48 8D AC 24 68 FE FF FF 48 81 EC 98 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 D0 00 00 00 48 8B 01 48 8B F9 "
        "48 89 9C 24 B8 02 00 00 FF 50 38 48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54 41 80 F8 02 74 06 83 78 2C 02 75 ??";

    // The phases block, 0x25f721 .. 0x25f798 (no gap to the strength block). Masked: the renderer, device and constant loads, the call target.
    constexpr const char* kPhasesShape =
        "48 8B 05 ?? ?? ?? ?? 0F 57 F6 48 8B 9F 78 02 00 00 BA FF FF FF FF 48 8B 88 28 04 00 00 48 8B F1 8B C1 48 C1 EE 20 F3 48 0F 2A F0 "
        "41 80 F8 02 74 33 48 8B 05 ?? ?? ?? ?? 0F 57 C0 F3 0F 10 0D ?? ?? ?? ?? 8B 88 8C 00 00 00 F3 48 0F 2A C1 F3 0F 5E C6 "
        "E8 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? F3 48 0F 2C D0 44 8B 4B 10 4C 8D 43 04 89 54 24 20 48 8D 4C 24 30 48 8B D3";

    // The strength block, 0x25f799 .. 0x25f865: the generator call through the store of the NDC pair into the jitter object.
    // Masked: the call target and the RIP loads (renderer, settings, floor, floor, sign mask, three constant loads).
    constexpr const char* kStrengthShape =
        "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 48 8B 03 48 8D 57 50 48 89 44 24 38 0F B7 81 00 07 00 00 66 0F 6E C8 "
        "0F B7 81 04 07 00 00 48 8B 8F 78 02 00 00 0F 5B C9 66 0F 6E C0 48 8B 05 ?? ?? ?? ?? 0F 5B C0 F3 0F 10 50 78 "
        "F3 0F 5C 15 ?? ?? ?? ?? F3 0F 5E C8 0F 57 C0 F3 0F 59 48 4C F3 48 0F 2A C6 F3 0F 59 C9 F3 0F 59 D1 F3 0F 58 15 ?? ?? ?? ?? "
        "F3 0F 58 D2 0F 28 CA F3 0F 59 54 24 3C F3 0F 59 4C 24 38 0F 57 15 ?? ?? ?? ?? F3 0F 5E D0 0F 28 05 ?? ?? ?? ?? F3 0F 5E CE "
        "0F 29 45 D0 0F 28 05 ?? ?? ?? ?? F3 0F 11 4C 24 38 0F 28 0D ?? ?? ?? ?? 0F 29 4D E0 0F 29 45 F0 F3 0F 11 54 24 3C "
        "48 8B 44 24 38 48 89 41 08";

    // The NGX packet producer, 0x3740c9 .. 0x374137: the camera global, renderer and device loads, the world camera argument, the call of
    // the camera getter and the read of the returned object. Masked: the three RIP loads, the call target and the TLS slot immediate.
    constexpr const char* kProducerShape =
        "48 8B 05 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 8B 88 28 30 00 00 48 8B 86 68 01 00 00 49 89 06 "
        "48 8B 86 78 01 00 00 48 89 86 00 01 00 00 48 8B 87 50 06 00 00 48 89 86 08 01 00 00 48 8B 47 70 48 89 86 10 01 00 00 "
        "4C 89 BE 18 01 00 00 E8 ?? ?? ?? ?? 0F 57 C0 41 BD ?? ?? ?? ?? 0F 57 C9 48 8B 08 48 89 8E 20 01 00 00";
    constexpr std::int32_t kProducerCallInShape = 0x54; // the E8 sits 0x54 bytes into kProducerShape

    // ------------------------------------------------------------------------------------------------------------------------------
    // The camera update (location of everything else). Two body anchors, no prologue: the function start comes from .pdata, so a
    // changed frame size costs the shape, not the location. The camera update is covered by two chained unwind entries (0x25f690 and the
    // part from 0x25f6b9), both resolve to 0x25f690.
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        AnchorSpec body;
        body.name = "headBody";
        body.pattern = "48 8B 01 48 8B F9 48 89 9C 24 B8 02 00 00 FF 50 38 48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54 41 80";
        body.valueFrom = ValueFrom::EnclosingFunction;

        // the extents of the renderer (+0x700 / +0x704) read as two u16: a distinctive pair of immediates inside the strength block
        AnchorSpec extents;
        extents.name = "headExtents";
        extents.pattern = "0F B7 81 00 07 00 00 66 0F 6E C8 0F B7 81 04 07 00 00";
        extents.valueFrom = ValueFrom::EnclosingFunction;
        r.seams.push_back({ "jitterSource.cameraHead", SeamKind::Function, { body, extents } });
    }

    // A guard at a seam, never scanned: the pattern is verified at value(from) + fromOffset (ONE such anchor makes a shape seam).
    auto shape = [](const char* from, std::int32_t fromOffset, const char* pattern, std::int32_t valueOffset) -> AnchorSpec
    {
        AnchorSpec a;
        a.name = "shape";
        a.pattern = pattern;
        a.fromSeam = from;
        a.fromOffset = fromOffset;
        a.valueOffset = valueOffset;
        return a;
    };

    r.seams.push_back({ "jitterSource.cameraHeadShape", SeamKind::CodeSite, { shape("jitterSource.cameraHead", 0, kCameraHeadShape, 0) }, true });

    // ------------------------------------------------------------------------------------------------------------------------------
    // The Halton generator (Detoured). Prologue hits: 5 to 8 (it is NOT unique); both anchors here are body / caller side.
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        // inside the function: the argument roles and the phase-count divide, from `mov rsi,rdx` to `mov ecx,ebx` (starts 0x14 into it)
        AnchorSpec body;
        body.name = "generatorBody";
        body.pattern = "48 8B F2 0F 29 78 D8 33 D2 44 0F 29 40 C8 49 8B F8 44 0F 29 48 B8 0F 57 FF F3 44 0F 10 0D ?? ?? ?? ?? 0F 57 F6 "
                       "44 0F 29 50 A8 41 8B C1 F7 B4 24 A0 00 00 00 F3 44 0F 10 15 ?? ?? ?? ?? 45 0F 28 C2 8D 5A 01 8B CB";
        body.valueFrom = ValueFrom::EnclosingFunction;

        // caller side: the argument setup and the call in the camera update (rdx = jitter object, r8 = +4, r9d = [rbx+0x10], [rsp+0x20] = edx)
        AnchorSpec caller;
        caller.name = "generatorCaller";
        caller.pattern = "44 8B 4B 10 4C 8D 43 04 89 54 24 20 48 8D 4C 24 30 48 8B D3 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0";
        caller.valueFrom = ValueFrom::Target;
        caller.decode = { DecodeKind::Rel32, 20, 0, 0, 0 };
        r.seams.push_back({ "jitterSource.generator", SeamKind::Function, { body, caller } });
        r.seams.push_back({ "jitterSource.generatorShape", SeamKind::CodeSite, { shape("jitterSource.generator", 0, kGeneratorShape, 0) }, true });
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // The camera getter (Detoured, a leaf: no .pdata entry). Its 8 bytes hit twice (0x25f610 and 0x148c408): the first anchor is
    // disambiguated by position (the getter is the method right before the camera update), the second finds it from its caller.
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        AnchorSpec bytes;
        bytes.name = "getterBytes";
        bytes.pattern = kCameraGetterShape;
        bytes.minMargin = 0; // the other raw hit is a byte-identical decoy: the predicate is the disambiguator
        bytes.predicates = { pred::Before("jitterSource.cameraHead", 0x1000) };

        // caller side: the producer's call, decoded to its target
        AnchorSpec caller;
        caller.name = "getterCaller";
        caller.pattern = "4C 89 BE 18 01 00 00 E8 ?? ?? ?? ?? 0F 57 C0 41 BD ?? ?? ?? ?? 0F 57 C9 48 8B 08 48 89 8E 20 01 00 00";
        caller.valueFrom = ValueFrom::Target;
        caller.decode = { DecodeKind::Rel32, 7, 0, 0, 0 };
        r.seams.push_back({ "jitterSource.cameraGetter", SeamKind::Function, { bytes, caller } });
        r.seams.push_back({ "jitterSource.cameraGetterShape", SeamKind::CodeSite, { shape("jitterSource.cameraGetter", 0, kCameraGetterShape, 0) }, true });
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // The producer's call of the getter (the observe hook compares its return address, call + 5, derived by a binding).
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        // Two anchors on the same E8 from opposite sides: what FOLLOWS the call (the returned object is read) and what PRECEDES it (the world
        // camera argument and the stores that must not touch rcx). They share only the five bytes of the E8 itself. (The caller anchor of the
        // getter contains `producerCall` entirely, so it is not a third independent view.)
        AnchorSpec call;
        call.name = "producerCall";
        call.pattern = "E8 ?? ?? ?? ?? 0F 57 C0 41 BD ?? ?? ?? ?? 0F 57 C9 48 8B 08"; // the TLS slot immediate is masked, see kProducerShape
        call.decode = { DecodeKind::Rel32, 0, 0, 0, 0 };
        call.predicates = { pred::CallsSeam("jitterSource.cameraGetter") };

        AnchorSpec pre;
        pre.name = "producerPre";
        pre.pattern = "48 8B 88 28 30 00 00 48 8B 86 68 01 00 00 49 89 06 48 8B 86 78 01 00 00 48 89 86 00 01 00 00 48 8B 87 50 06 00 00 "
                      "48 89 86 08 01 00 00 48 8B 47 70 48 89 86 10 01 00 00 4C 89 BE 18 01 00 00 E8 ?? ?? ?? ??";
        pre.valueOffset = 0x3f; // the E8 is 0x3f bytes into the pattern
        pre.decode = { DecodeKind::Rel32, 0x3f, 0, 0, 0 };
        pre.predicates = { pred::CallsSeam("jitterSource.cameraGetter") };
        r.seams.push_back({ "jitterSource.producerCall", SeamKind::CodeSite, { call, pre } });

        // the shape starts 0x54 bytes before the call; the call's target is the location anchor's (the same E8), so it is not checked twice
        r.seams.push_back({ "jitterSource.producerCallShape",
                            SeamKind::CodeSite,
                            { shape("jitterSource.producerCall", -kProducerCallInShape, kProducerShape, kProducerCallInShape) },
                            true });
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // The phases block and the strength block, both inside the camera update (0x25f690), adjacent.
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        // the renderer load, the jitter object, `mov edx,-1` (the DLSS phase count) and the scene size: the start of the phases block
        AnchorSpec phases;
        phases.name = "phasesImmediate";
        phases.pattern = "48 8B 05 ?? ?? ?? ?? 0F 57 F6 48 8B 9F 78 02 00 00 BA FF FF FF FF 48 8B 88 28 04 00 00";
        phases.predicates = { pred::SameFunctionAs("jitterSource.cameraHead") };

        // the DLSS branch, the device load and width and the call that computes the other modes' phase count: a later slice of the block,
        // 0x2b bytes in (shares no byte with the first anchor)
        AnchorSpec branch;
        branch.name = "phasesBranch";
        branch.pattern = "41 80 F8 02 74 33 48 8B 05 ?? ?? ?? ?? 0F 57 C0 F3 0F 10 0D ?? ?? ?? ?? 8B 88 8C 00 00 00 F3 48 0F 2A C1 F3 0F 5E C6 "
                         "E8 ?? ?? ?? ??";
        branch.valueOffset = -0x2b;
        branch.predicates = { pred::SameFunctionAs("jitterSource.cameraHead") };
        r.seams.push_back({ "jitterSource.phases", SeamKind::CodeSite, { phases, branch } });
        r.seams.push_back({ "jitterSource.phasesShape", SeamKind::CodeSite, { shape("jitterSource.phases", 0, kPhasesShape, 0) }, true });

        // the call of the generator and the first loads after it
        AnchorSpec strength;
        strength.name = "strengthCall";
        strength.pattern = "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 48 8B 03 48 8D 57 50 48 89 44 24 38";
        strength.decode = { DecodeKind::Rel32, 0, 0, 0, 0 };
        strength.predicates = { pred::CallsSeam("jitterSource.generator"), pred::SameFunctionAs("jitterSource.cameraHead") };

        // the same call seen from the argument setup before it (the generator seam scans this very pattern for its own caller anchor); it
        // shares only the E8 and the 12 bytes after it with the first anchor
        AnchorSpec args;
        args.name = "strengthArgs";
        args.pattern = "44 8B 4B 10 4C 8D 43 04 89 54 24 20 48 8D 4C 24 30 48 8B D3 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0";
        args.valueOffset = 20; // the E8 is 20 bytes into the pattern
        args.decode = { DecodeKind::Rel32, 20, 0, 0, 0 };
        args.predicates = { pred::CallsSeam("jitterSource.generator"), pred::SameFunctionAs("jitterSource.cameraHead") };
        r.seams.push_back({ "jitterSource.strength", SeamKind::CodeSite, { strength, args } });

        // the E8 at the start is the location anchor's call (its target was compared with the generator there)
        r.seams.push_back({ "jitterSource.strengthShape", SeamKind::CodeSite, { shape("jitterSource.strength", 0, kStrengthShape, 0) }, true });
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // The device size reader. TargetIs ties it to the device global the phases block and the producer load. Its only anchor is its
    // strict 20 bytes, so the Guard field is bound to this seam itself.
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        AnchorSpec size;
        size.name = "outputPair";
        size.pattern = "48 8B 0D ?? ?? ?? ?? 44 8B 81 90 00 00 00 8B 91 8C 00 00 00";
        size.expectedHits = 2; // two byte-identical template instances (0xd3f73, 0xd4013); the first is the table's. Either proves the pair.
        size.pickIndex = 0;
        size.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        size.predicates = { pred::TargetIs("jitterSource.devicePointer") };
        r.seams.push_back({ "jitterSource.deviceSize", SeamKind::CodeSite, { size }, true });
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // The globals: guards at VERIFIED shapes (so only verified code derives them), RIP decode, bss.
    // ------------------------------------------------------------------------------------------------------------------------------
    auto globalLoad = [&shape](const char* from, std::int32_t fromOffset, const char* pattern, const char* name) -> AnchorSpec
    {
        AnchorSpec a = shape(from, fromOffset, pattern, 0);
        a.name = name;
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        a.predicates = { pred::TargetIn(RegionKind::Bss) };
        return a;
    };
    r.seams.push_back({ "jitterSource.settingsPointer",
                        SeamKind::Global,
                        { globalLoad("jitterSource.cameraHeadShape", 0x34, "48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54", "settingsInHead"),
                          globalLoad("jitterSource.strengthShape", 0x3c, "48 8B 05 ?? ?? ?? ?? 0F 5B C0 F3 0F 10 50 78", "settingsInStrength") } });
    r.seams.push_back({ "jitterSource.rendererPointer",
                        SeamKind::Global,
                        { globalLoad("jitterSource.phasesShape", 0, "48 8B 05 ?? ?? ?? ?? 0F 57 F6 48 8B 9F 78 02 00 00", "rendererInPhases"),
                          globalLoad("jitterSource.strengthShape", 5, "48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0", "rendererInStrength"),
                          globalLoad("jitterSource.producerCallShape", -0x4d, "48 8B 3D ?? ?? ?? ?? 48 8B 1D", "rendererInProducer") } });
    // NOT derived from deviceSize: that seam is tied to this one with TargetIs (a cycle otherwise)
    r.seams.push_back({ "jitterSource.devicePointer",
                        SeamKind::Global,
                        { globalLoad("jitterSource.phasesShape", 0x31, "48 8B 05 ?? ?? ?? ?? 0F 57 C0 F3 0F 10 0D", "deviceInPhases"),
                          globalLoad("jitterSource.producerCallShape", -0x46, "48 8B 1D ?? ?? ?? ?? 48 8B 88 28 30 00 00", "deviceInProducer") } });
    // The camera global the producer loads (the first instruction of kProducerShape): the same global as jitter.cameraGlobalRva, which
    // AnchorsQuality.h owns. Decoded here under this group's own id (no seam of another group is named) so that the world tie of the hooks is
    // complete: the producer calls the getter with [camera global + 0x3028], the shape pins the 0x3028, this seam says WHICH global. No
    // GameBuild field is bound (the quality writer owns it); P3 and the merged golden compare the two values. One anchor: the producer is the
    // only block of this group that loads the camera global.
    r.seams.push_back({ "jitterSource.cameraGlobal",
                        SeamKind::Global,
                        { globalLoad("jitterSource.producerCallShape", -kProducerCallInShape, "48 8B 05 ?? ?? ?? ?? 48 8B 3D", "cameraInProducer") } });

    // ------------------------------------------------------------------------------------------------------------------------------
    // Struct offsets carried by instructions (see the header comment: the shapes keep them fixed, these decode and bound them). One anchor
    // each for mode (+0x54, only the camera head reads it), scale (+0x4c), amount (+0x78) and the extents (+0x700): no other instruction of
    // the blocks in scope carries them. Scene size and device width have two readers and two anchors.
    // ------------------------------------------------------------------------------------------------------------------------------
    auto field = [&shape](const char* from, std::int32_t fromOffset, const char* pattern, const char* name, std::int32_t at, std::uint8_t width,
                          std::uint32_t lo, std::uint32_t hi) -> AnchorSpec
    {
        AnchorSpec a = shape(from, fromOffset, pattern, 0);
        a.name = name;
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::Field, at, 0, 0, width };
        a.predicates = { pred::ValueIn(lo, hi) };
        return a;
    };
    r.seams.push_back({ "jitterSource.settingsModeOffset", SeamKind::Offset,
                        { field("jitterSource.cameraHeadShape", 0x3d, "44 0F B6 40 ?? 41 80 F8 02", "modeByte", 4, 1, 0x10, 0x7f) } });
    r.seams.push_back({ "jitterSource.settingsScaleOffset", SeamKind::Offset,
                        { field("jitterSource.strengthShape", 0x5a, "F3 0F 59 48 ?? F3 48 0F 2A C6", "scale", 4, 1, 0x10, 0x7f) } });
    r.seams.push_back({ "jitterSource.settingsAmountOffset", SeamKind::Offset,
                        { field("jitterSource.strengthShape", 0x46, "F3 0F 10 50 ?? F3 0F 5C 15", "amount", 4, 1, 0x10, 0x7f) } });
    r.seams.push_back({ "jitterSource.rendererSceneSizeOffset", SeamKind::Offset,
                        { field("jitterSource.phasesShape", 0x16, "48 8B 88 ?? ?? 00 00 48 8B F1", "sceneInPhases", 3, 4, 0x100, 0xfff),
                          field("jitterSource.producerCallShape", 0x62,
                                "8B 87 ?? ?? 00 00 4C 89 BC 24 90 00 00 00 89 84 24 90 00 00 00 8B 87 ?? ?? 00 00 89 84 24 94 00 00 00",
                                "sceneInProducer", 2, 4, 0x100, 0xfff) } });
    r.seams.push_back({ "jitterSource.rendererExtentOffset", SeamKind::Offset,
                        { field("jitterSource.strengthShape", 0x1c, "0F B7 81 ?? ?? 00 00 66 0F 6E C8 0F B7 81 ?? ?? 00 00", "extents", 3, 4, 0x100, 0xfff) } });
    r.seams.push_back({ "jitterSource.deviceOutputSizeOffset", SeamKind::Offset,
                        { field("jitterSource.phasesShape", 0x43, "8B 88 ?? ?? 00 00 F3 48 0F 2A C1", "widthInPhases", 2, 4, 0x10, 0xff),
                          field("jitterSource.deviceSize", 0xe, "8B 91 ?? ?? 00 00", "widthInDeviceSize", 2, 4, 0x10, 0xff) } });

    // ------------------------------------------------------------------------------------------------------------------------------
    // Constants the strength expression reads (GameBuild holds them as strengthFloorBits and ndcSignMaskBits; the seam checks the live value and the gate requires it). The value is
    // the constant itself (the float's bits), read from .rdata through the RIP reference of the instruction.
    // ------------------------------------------------------------------------------------------------------------------------------
    {
        // exactly 0.001f: the value FfxivJitterSourceCore.h hard-codes (a range such as 0.000488..0.00195 would claim to verify what the core
        // assumes while accepting a floor off by up to a factor of two)
        constexpr std::uint32_t kFloor = 0x3a83126fu;
        AnchorSpec sub = shape("jitterSource.strengthShape", 0x4b, "F3 0F 5C 15 ?? ?? ?? ??", 0);
        sub.name = "floorInSubss";
        sub.valueFrom = ValueFrom::Target;
        sub.decode = { DecodeKind::RipDeref32, 0, 4, 8, 0 };
        sub.predicates = { pred::ValueEquals(kFloor) };
        AnchorSpec add = shape("jitterSource.strengthShape", 0x6c, "F3 0F 58 15 ?? ?? ?? ??", 0);
        add.name = "floorInAddss";
        add.valueFrom = ValueFrom::Target;
        add.decode = { DecodeKind::RipDeref32, 0, 4, 8, 0 };
        add.predicates = { pred::ValueEquals(kFloor) };
        r.seams.push_back({ "jitterSource.strengthFloor", SeamKind::Offset, { sub, add } });

        // the sign flip of the NDC y: xorps xmm2,[rip+..] with the sign mask 0x80000000 (one anchor: the only reader in scope)
        AnchorSpec sign = shape("jitterSource.strengthShape", 0x87, "0F 57 15 ?? ?? ?? ??", 0);
        sign.name = "signMask";
        sign.valueFrom = ValueFrom::Target;
        sign.decode = { DecodeKind::RipDeref32, 0, 3, 7, 0 };
        sign.predicates = { pred::ValueEquals(0x80000000u) };
        r.seams.push_back({ "jitterSource.ndcSignMask", SeamKind::Offset, { sign } });
    }

    // ------------------------------------------------------------------------------------------------------------------------------
    // Bindings. Rva fields: the LOCATION seams (EntryPatched keeps a foreign hook fail closed). Guard fields: the SHAPE seams.
    // ------------------------------------------------------------------------------------------------------------------------------
    r.bindings.push_back({ "jitterSource.generatorRva", "jitterSource.generator", 0 });
    r.bindings.push_back({ "jitterSource.cameraGetterRva", "jitterSource.cameraGetter", 0 });
    r.bindings.push_back({ "jitterSource.producerCallRva", "jitterSource.producerCall", 0 });
    r.bindings.push_back({ "jitterSource.producerReturnRva", "jitterSource.producerCall", 0, 5 }); // the call is 5 bytes
    r.bindings.push_back({ "jitterSource.cameraHeadRva", "jitterSource.cameraHead", 0 });
    r.bindings.push_back({ "jitterSource.phasesRva", "jitterSource.phases", 0 });
    r.bindings.push_back({ "jitterSource.strengthRva", "jitterSource.strength", 0 });
    r.bindings.push_back({ "jitterSource.generatorCallReturnRva", "jitterSource.strength", 0, 5 }); // the generator call is 5 bytes
    r.bindings.push_back({ "jitterSource.deviceSizeRva", "jitterSource.deviceSize", 0 });
    r.bindings.push_back({ "jitterSource.settingsPointerRva", "jitterSource.settingsPointer", 0 });
    r.bindings.push_back({ "jitterSource.rendererPointerRva", "jitterSource.rendererPointer", 0 });
    r.bindings.push_back({ "jitterSource.devicePointerRva", "jitterSource.devicePointer", 0 });

    r.bindings.push_back({ "jitterSource.generatorInstructions", "jitterSource.generatorShape", 0 });
    r.bindings.push_back({ "jitterSource.cameraGetterInstructions", "jitterSource.cameraGetterShape", 0 });
    r.bindings.push_back({ "jitterSource.producerCallInstructions", "jitterSource.producerCallShape", 0 });
    r.bindings.push_back({ "jitterSource.cameraHeadInstructions", "jitterSource.cameraHeadShape", 0 });
    r.bindings.push_back({ "jitterSource.phasesInstructions", "jitterSource.phasesShape", 0 });
    r.bindings.push_back({ "jitterSource.strengthInstructions", "jitterSource.strengthShape", 0 });
    r.bindings.push_back({ "jitterSource.deviceSizeInstructions", "jitterSource.deviceSize", 0 });

    r.bindings.push_back({ "jitterSource.settingsScaleOffset", "jitterSource.settingsScaleOffset", 0 });
    r.bindings.push_back({ "jitterSource.settingsModeOffset", "jitterSource.settingsModeOffset", 0 });
    r.bindings.push_back({ "jitterSource.settingsAmountOffset", "jitterSource.settingsAmountOffset", 0 });
    r.bindings.push_back({ "jitterSource.rendererSceneSizeOffset", "jitterSource.rendererSceneSizeOffset", 0 });
    r.bindings.push_back({ "jitterSource.rendererExtentOffset", "jitterSource.rendererExtentOffset", 0 });
    r.bindings.push_back({ "jitterSource.deviceOutputSizeOffset", "jitterSource.deviceOutputSizeOffset", 0 });

    // The two Detoured functions must never resolve to the same address (they are attached in one transaction).
    r.mustDiffer.push_back({ "jitterSource.generator", "jitterSource.cameraGetter" });
}
} // namespace ext::ffxiv::resolve
