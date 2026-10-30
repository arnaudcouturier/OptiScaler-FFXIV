#pragma once
// Anchors for native quality, the CharaView accessor and the camera global.
//
// OWNER: the quality seams. This is the ONLY resolver file that seam work edits (plus its own tests under tests/ext/ffxiv/resolve/).
// It must NOT edit: ResolveTypes.h, ImageView.h, DenseImage.h, LiveImage.h, Pattern.h, Scan.h, Unwind.h, Resolver.h, ResolveLog.h,
// Entry.h, ResolveCache.h, FieldTable.h, Registry.h, AnchorsCompanion.h, AnchorsJitter.h, GameBuild.h, CharaTable.h, Game.cpp.
//
// The fields of GameBuild this group covers (FieldTable.h keys, each bound below to the seam that finds it):
//   nativeQuality.settingsInstructionsRva / settingsInstructions        nativeQuality.querySizeRva / querySizeInstructions
//   nativeQuality.rendererUpdateRva / rendererUpdateInstructions        nativeQuality.resizeCallbacksRva / resizeCallbacksInstructions
//   nativeQuality.settingsPointerRva / managerPointerRva / devicePointerRva
//   nativeQuality.settingsMode/Quality/Dlaa/SelectorOffset, managerContext/contextFlags/contextCache/deviceCallbacks/rendererSceneSizeOffset
//   charaView.accessorRva / managerPointerRva / targetsOffset
//   jitter.cameraGlobalRva / cameraOffset / jitterPointerOffset
// NOT bound (no instruction of this group carries them; the resolver logs them as "not self-healing"): nativeQuality.settingsRendererOffset (0x47),
// settingsAdditionalOffset (0x49) (the hooks only use them to size a snapshot), charaView.transparentOffset / depthOffset0 / depthOffset1 /
// entryResourceOffset / entryWidthOffset / entryHeightOffset (read by CharaTable.h, no anchor instruction carries them), jitter.jitterX/Y /
// ndcX/Y / counterOffset (the layout of the 20-byte jitter object, written by the generator, not by an instruction of this group).
//
// DESIGN (the safety gap, read this before editing a pattern):
//   On an UNKNOWN build a bound Guard field is filled with the LIVE bytes found at the resolved site, so the hooks' own byte compare checks
//   nothing any more: the anchors below are the ONLY verification. Every pattern was therefore written from the code that CONSUMES the
//   seam (ext/ffxiv/quality/FfxivNativeQualityHooks.h, ext/ffxiv/chara/CharaTable.*, ext/ffxiv/jitter/FfxivJitterProbe.*):
//     * a RIP displacement or a call target is masked only when it is DECODED into a global / another seam, or when the consumer does not
//       depend on it (the stack-cookie load, a constant-pool load, a call that only forwards control);
//     * a struct offset the consumer reads is masked ONLY together with an Offset seam that decodes that exact byte from the same
//       instruction (the hooks read the offset from the resolved build, so a shifted offset is followed, with a sanity range through
//       pred::ValueIn). Two offsets are members of a pair: the display-size cache +0x150/+0x154 (the hooks WRITE 8 zero bytes there) is fully
//       fixed in its guard window, so it is verified, never healed, and the +4 relation holds; the scene size +0x428/+0x42c keeps only the
//       second member fixed in the scan pattern, and the seam reports whatever the first compare says: that +4 relation is NOT enforced (the
//       hooks only READ it, for a log line);
//     * short jumps (rel8) stay FIXED (they are function-local); rel32 jumps and calls are masked (the brief's rule);
//     * the SIGNATURE of a hooked function and the bytes a Detour overwrites are pinned by ONE strict anchor per function, held by a SHAPE
//       seam (next section). The anchors of the LOCATION seams only have to find the function; a status of the location seam, FoundSingleAnchor
//       OR plain Found, says nothing about the bytes a Detour overwrites. What no pattern can pin (register data flow over long spans) is
//       a known limit of this group.
//
// TWO TIERS PER HOOKED FUNCTION (querySize, rendererUpdate, resizeCallbacks; the design of AnchorsJitter.h):
//   * a LOCATION seam (quality.querySize / rendererUpdate / resizeCallbacks, Function): found by several anchors (entry, body, caller), bound to the
//     Rva field. Its status is NOT the proof that the prologue is the verified one: a prologue-only change of rendererUpdate (three anchors: entry
//     lost, body and caller agree) is plain Found, no WARN, `--strict` exit 0; so is one of querySize (three anchors since LOW-1: entry, body and the
//     manager constructor's lea); only resizeCallbacks (two anchors) comes out FoundSingleAnchor. An earlier version of this note said that every
//     such case is FoundSingleAnchor: that was wrong for rendererUpdate (and, since the third anchor, for querySize).
//   * a SHAPE seam (quality.querySizeShape / rendererUpdateShape / resizeCallbacksShape; a CodeSite with failClosed): exactly ONE guard anchor
//     (fromSeam = the location seam, fromOffset 0) holding the strict entry pattern, the very text of the location seam's `entry` anchor: the
//     prologue, the argument moves, the output stores. The Guard field (...Instructions) is bound to the SHAPE seam and the Rva field to the
//     location seam. ONE anchor on purpose: with two, a failing piece next to a passing one reads FoundSingleAnchor, which is usable, which fills
//     the Guard field with live bytes. A shape that does not match is NotFound, failClosed keeps it out of the table fallback on a known build,
//     and the Guard field stays ZERO: the hooks' own compare (live bytes against the Guard field) refuses to install on every build.
//     On an unknown build a Guard field bound to a LOCATION seam would hold the live bytes of whatever address the seam resolved to (a wrong
//     function found through the caller anchor alone), the compare would compare them with themselves and install.
//   * the first 14 bytes of every strict pattern (the entry of the two Detour targets and of the direct-call target) are FIXED, no wildcard: the
//     length of the patch the vendored Detours writes was not verified (only the first instruction of querySize is exactly 5 bytes), so the
//     bytes a patch may relocate are treated as pinned. A test holds this.
//   * every wildcard that no anchor of this group decodes must be listed, with the reason nobody needs the byte, in kMaskedOnPurpose of
//     tests/ext/ffxiv/resolve/resolve_quality_sweep.cpp (set equality): masking one more byte of a pattern fails that test until somebody
//     writes down why it is free. The same test flips every fixed byte once.
//
// Hook-site rule (the consumer is FfxivNativeQualityHooks.h):
//   1. A Detour site (querySize, rendererUpdate) and the DIRECT CALL of resizeCallbacks (FfxivNativeQualityHooks.h dispatchResizeCallbacks) are
//      usable only when BOTH the location seam and the shape seam are usable: `rb.UsableOrTable(location) && rb.UsableOrTable(shape)` (that is
//      Usable on an unknown build; on a known build the location may have fallen back to the table, a failClosed shape never does). Gate on the
//      two seam statuses, never on the status of one anchor. The hooks keep their compare of the live bytes against the Guard field.
//   2. The 8-byte WRITE at [[manager + managerContextOffset] + contextCacheOffset] (the display-size cache invalidation) must not happen unless
//      quality.contextCacheOffset is plain Found, STATUS Found of the seam itself (a single anchor that pins BOTH members of the +0x150/+0x154 pair:
//      verified, never healed); a value that a known build filled from the table (UsableOrTable) does not allow the write. On an UNKNOWN build
//      FoundSingleAnchor does not allow it either. On the KNOWN build a FoundSingleAnchor that rests on a weak
//      dependency (SeamResult::viaWeakDependency: a foreign hook over the helper quality.cacheCompare) does allow it, because the +0x150/+0x154 pair
//      is verified offline against that exact executable (Gate.h FoundOrWeakSingle); and quality.managerPointer, quality.managerContextOffset, quality.contextFlagsOffset (the `(flags & 1) && !(flags & 0x30)`
//      gate), quality.settingsPointer and the settings mode offset are usable (Usable, same rule). A NotFound Offset seam leaves the stale table default in `build`
//      (offsets keep their defaults), so every Offset seam the code reads must be gated on its own status.
//   3. quality.settingsInstructions is its own shape (one scanned strict anchor): its Guard and Rva stay on the same seam.
//   4. The four settings offsets that have a seam here (mode, quality, dlaa, selector) resolve at most 0x55: the consumer's snapshot of the settings
//      struct is a compile-time 0x56 bytes (kSettingsSnapshotSize, taken from the table) indexed without bounds checks. Lift the cap only together
//      with a snapshot sized from the resolved offsets.
//   Helper seams (no GameBuild field): quality.contextResolver, quality.cacheCompare, camera.getter. They exist so that an Offset seam has a
//   verified function to read its instruction from.
//
// Nothing here runs at load time: the function is called by BuildRegistry() when the registry is built.

#include "ResolveTypes.h"

namespace ext::ffxiv::resolve
{
inline void AddQualityAnchors(Registry& r)
{
    // ---- CharaView accessor (the first full seam; CharaTable.h still does its own scan until that seam exists) ----
    //
    //   mov rax,[rip+disp32] ; mov ecx,edx ; mov rax,[rax+rcx*8+0x2c8] ; ret      18 bytes, a leaf function (no .pdata entry)
    //
    // The pattern is the one of CharaViewBuild (GameBuild.h): the four displacement bytes are masked, everything else is fixed. On the
    // current executable it is unique without any predicate; TargetIn(Bss) is kept as a second safety (the manager global lives in
    // the bss tail of .data) and as the decoy filter the tests prove. The nearest other site in .text differs in 4 of the 14 fixed bytes.
    // The accessor is too short for the EntryPatched retry (4 fixed bytes would remain after masking the entry), so a foreign hook on
    // it reads as NotFound; a hook on a leaf accessor this small is not a realistic case.
    constexpr const char* kAccessorPattern = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3";

    AnchorSpec accessor;
    accessor.name = "accessorLoad";
    accessor.pattern = kAccessorPattern;
    accessor.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    accessor.predicates = { pred::TargetIn(RegionKind::Bss) };
    r.seams.push_back({ "chara.accessor", SeamKind::Function, { accessor } });

    // The manager pointer: the global the accessor loads. Not scanned: the load is verified (as a guard) at the accessor and decoded.
    AnchorSpec manager;
    manager.name = "accessorDisp";
    manager.pattern = kAccessorPattern;
    manager.fromSeam = "chara.accessor";
    manager.valueFrom = ValueFrom::Target;
    manager.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    manager.predicates = { pred::TargetIn(RegionKind::Bss) };
    r.seams.push_back({ "chara.managerPointer", SeamKind::Global, { manager } });

    r.bindings.push_back({ "charaView.accessorRva", "chara.accessor", 0 });
    r.bindings.push_back({ "charaView.managerPointerRva", "chara.managerPointer", 0 });

    // The offset of the render-target table inside the manager object: the `+0x2c8` of the accessor's last load. PINNED, NOT SELF-HEALING:
    // the accessor's own scan (above, whose pattern a test pins) keeps these four bytes fixed, so this guard cannot disagree with it; it reads
    // the value as a decoded field so the table field is bound and compared on the golden test. Own pattern text (offset masked).
    // The other CharaView offsets (transparent / depth / entry layout) are read by CharaTable.h only: no anchor instruction carries them.
    {
        AnchorSpec a;
        a.name = "accessorTargets";
        a.pattern = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 ?? ?? ?? ?? C3";
        a.fromSeam = "chara.accessor";
        a.valueFrom = ValueFrom::Target;
        a.decode = { DecodeKind::Field, 13, 0, 0, 4 };
        a.predicates = { pred::ValueIn(0x100, 0x1000) };
        r.seams.push_back({ "chara.targetsOffset", SeamKind::Offset, { a } });
        r.bindings.push_back({ "charaView.targetsOffset", "chara.targetsOffset", 0 });
    }

    // ================================ native quality ================================
    //
    // Patterns (all measured on the build-C executable: exactly one hit each; the notes give the instruction the bytes encode).

    // 0x3745d7 (CodeSite, inside the quality-table resolver 0x374590): the settings read path the DLAA gate and the install guard rely on.
    //   mov r8,[settings] ; xor r12d,r12d ; cmp byte [r8+MODE],2 ; je +6 ; cmp [r8+QUALITY],r12b ; je ; cmp [r8+DLAA],r12b ; je ;
    //   movzx eax,byte [r8+SELECTOR] ; cmp eax,6
    // Masked: the settings displacement (decoded into quality.settingsPointer) and the four struct offsets (decoded into the four Offset seams).
    constexpr const char* kSettingsBlock = "4C 8B 05 ?? ?? ?? ?? 45 33 E4 41 80 78 ?? 02 74 06 45 38 60 ?? 74 71 45 38 60 ?? 74 6B 41 0F B6 40 ?? 83 F8 06";

    // 0x352350 (querySize, a detour target): prologue, manager load, context load, settings mode test, the call that fills the quality
    // answer, the three stores that define optimal / minimum / maximum, `mov al,1`. 112 bytes. Masked: the two RIP loads (decoded), the context
    // offset 0x4220 and the mode offset (decoded), the call target. Fixed: the argument moves (display = rcx, optimal = rdx, minimum = r8,
    // maximum = r9) and the three output stores (+0xd4 / +0xdc / +0xe4): the detour's signature.
    constexpr const char* kQuerySizeEntry =
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F9 49 8B F0 4C 8B F2 48 8B 98 ?? ?? ?? ?? "
        "48 85 DB 74 41 48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 75 34 48 8B D1 45 33 C0 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 22 48 8B 83 D4 00 00 00 49 "
        "89 06 48 8B 83 DC 00 00 00 48 89 06 48 8B 83 E4 00 00 00 48 89 07 B0 01";
    // the same without the prologue and without the manager load (starts at `mov rdi,r9`): the function start comes from .pdata
    constexpr const char* kQuerySizeBody =
        "49 8B F9 49 8B F0 4C 8B F2 48 8B 98 ?? ?? ?? ?? 48 85 DB 74 41 48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 75 34 48 8B D1 45 33 C0 48 8B CB E8 "
        "?? ?? ?? ?? 84 C0 74 22 48 8B 83 D4 00 00 00 49 89 06 48 8B 83 DC 00 00 00 48 89 06 48 8B 83 E4 00 00 00 48 89 07 B0 01";

    // 0x2da5b0 (rendererUpdate, a detour target): `mov r11,rsp ; push rdi ; sub rsp,0xd0 ; stack cookie ; movss xmm0,[rcx+0x710] ; ... ;
    // movaps xmm3,xmm1 ; mov rbx,rcx ; ... ; mov r14,[device]`. 73 bytes. rcx = renderer and xmm1 = elapsed are what the detour forwards.
    constexpr const char* kRendererUpdateEntry =
        "4C 8B DC 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 F3 0F 10 81 10 07 00 00 0F 57 D2 F3 0F 59 05 ?? ?? ?? "
        "?? 0F 28 D9 49 89 5B 10 48 8B D9 49 89 6B 18 0F 57 ED 4D 89 73 E8 4C 8B 35 ?? ?? ?? ??";
    // the same from `movss xmm0,[rcx+0x710]` on (0x1a bytes after the entry): survives a changed prologue, the start comes from .pdata
    constexpr const char* kRendererUpdateBody =
        "F3 0F 10 81 10 07 00 00 0F 57 D2 F3 0F 59 05 ?? ?? ?? ?? 0F 28 D9 49 89 5B 10 48 8B D9 49 89 6B 18 0F 57 ED 4D 89 73 E8 4C 8B 35 ?? "
        "?? ?? ??";
    // 0x2b9e69, the only direct call: `mov rcx,[renderer global] ; movaps xmm1,xmm10 ; call rendererUpdate ; mov rcx,[..] ; test rcx,rcx ; je ; call`
    constexpr const char* kRendererUpdateCaller = "48 8B 0D ?? ?? ?? ?? 41 0F 28 CA E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05 E8 ?? ?? ?? ??";

    // 0x2dab0d inside rendererUpdate: the game's own resize dispatch that the hook replicates, followed by the settings tests and the extent
    // copy:  mov rcx,[r14+CALLBACKS] ; call resizeCallbacks ; mov rax,[settings] ; cmp [rax+MODE],2 ; je ; cmp [rax+QUALITY],0 ; je ;
    //        cmp [rax+DLAA],0 ; je ; movzx eax,word [rbx+0x702] ; jmp ; movzx eax,word [rbx+0x704] ; mov [rbx+0x700],ax
    constexpr const char* kResizeBlock =
        "49 8B 4E ?? E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 74 06 80 78 ?? 00 74 0F 80 78 ?? 00 74 09 0F B7 83 02 07 00 00 EB 07 0F "
        "B7 83 04 07 00 00 66 89 83 00 07 00 00";
    // 0x2dab9a inside rendererUpdate: the device output width / height against the renderer scene size:  mov rcx,[device] ;
    //   mov eax,[rcx+0x8c] ; cmp [rbx+SCENE],eax ; jne ; mov eax,[rcx+0x90] ; cmp [rbx+0x42c],eax ; je
    constexpr const char* kSizeBlock = "48 8B 0D ?? ?? ?? ?? 8B 81 8C 00 00 00 39 83 ?? ?? ?? ?? 75 0E 8B 81 90 00 00 00 39 83 2C 04 00 00 74 35";

    // 0x2369a0 (resizeCallbacks, called directly, never detoured): the lock, the loop over {function, context} pairs at +0x30 / count +0x38,
    // the AND of the results, the unlock, the result in al. 137 bytes; the two indirect calls are masked (imports).
    constexpr const char* kResizeEntry =
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 59 08 48 8B F1 48 8B CB FF 15 ?? ?? ?? ?? 8B 46 38 40 B5 01 48 8B "
        "7E 30 85 C0 74 35 4C 89 74 24 30 8B F0 45 33 F6 0F 1F 40 00 48 8B 07 48 85 C0 74 10 48 8B 4F 08 FF D0 84 C0 40 0F B6 ED 41 0F 44 EE "
        "48 83 C7 10 48 83 EE 01 75 DE 4C 8B 74 24 30 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 5C 24 38 40 0F B6 C5 48 8B 6C 24 40 48 8B 74 24 48 48 "
        "83 C4 20 5F C3";

    // 0x3537c8, the manager constructor: `mov [rdi+CONTEXT],rax ; lea rcx,[querySize] ; mov rax,[renderer global] ; mov [rax+0x730],rcx ; ...`
    // (the context offset and the lea are decoded; the renderer global and the two calls are not read by anyone)
    constexpr const char* kManagerCtor = "48 89 87 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 89 88 30 07 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ??";

    // Windows: the part of a function a guard anchor needs, verified at a fixed offset from the seam's address. They stop short of the prologue
    // on purpose: a guard that re-verified the whole entry pattern would die with the entry anchor on a build whose prologue changed (one
    // register), taking the global or the offset with it, although the instructions that carry them are intact.
    // querySize + 0x15: the manager load and the context load  (mov rax,[manager] ; mov rdi,r9 ; mov rsi,r8 ; mov r14,rdx ; mov rbx,[rax+CONTEXT] ; test)
    constexpr const char* kQueryManagerWindow = "48 8B 05 ?? ?? ?? ?? 49 8B F9 49 8B F0 4C 8B F2 48 8B 98 ?? ?? ?? ?? 48 85 DB";
    // querySize + 0x31: the settings mode test  (mov rax,[settings] ; cmp byte [rax+MODE],2 ; jne)
    constexpr const char* kQuerySettingsWindow = "48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 75 34";
    // querySize + 0x3e: the call that fills the answer  (mov rdx,rcx ; xor r8d,r8d ; mov rcx,rbx ; call contextResolver ; test al,al ; je)
    constexpr const char* kQueryCallWindow = "48 8B D1 45 33 C0 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 22";
    // contextResolver + 8: `test byte [rcx+FLAGS],1 ; mov r14,rdx ; mov rdi,rcx ; jne`. The consumer reads `flags & 1` (this test) and the offset; the
    // distance of the jne over the slow path (`call 0x374a10 / 0x374860`) is nothing it depends on, so the displacement is masked: an edit of that
    // slow path must not kill the anchor (the opcode stays fixed: it is the sense of the test).
    constexpr const char* kContextFlagsWindow = "F6 81 ?? ?? ?? ?? 01 4C 8B F2 48 8B F9 75 ??";
    // 0x37479c, inside contextResolver (a later chained range): the second flags anchor and the only one that pins the `& 0x30` the consumer reads:
    // `movzx eax,byte [rdi+FLAGS] ; mov r15,[rsp+0x30] ; test al,0x30 ; jne` (rdi = the context, loaded by `mov rdi,rcx` in the window above). The
    // displacement of the jne is masked for the same reason. 11 fixed bytes, ONE hit, 13 sites at distance 4 (the same without the trailing
    // `75 ??`: 10 fixed, 12 sites at distance 3).
    constexpr const char* kContextFlagsLoad = "0F B6 87 ?? ?? ?? ?? 4C 8B 7C 24 30 A8 30 75 ??";
    // cacheCompare + 0xd: the two cached display-size loads and their compares. BOTH members of the pair are fixed (+0x150 and +0x154): the hooks
    // write 8 zero bytes at the first one, so the width/height pair must be exactly what the game compares; the Field decode reads pinned bytes
    constexpr const char* kCacheWindow = "8B 81 50 01 00 00 48 8B F2 48 8B D1 39 06 75 0F 8B 81 54 01 00 00 39 46 04";
    // the call right before the settings block (0x3745c7): `mov rdx,r14 ; mov rcx,rdi ; mov [rsp+0x68],r12 ; call cacheCompare`
    constexpr const char* kCallContext = "49 8B D6 48 8B CF 4C 89 64 24 68 E8 ?? ?? ?? ??";
    // 0x374f10, the display-size comparison: `mov [rsp+8],rcx ; push rsi ; sub rsp,0x90 ; mov eax,[rcx+CACHE] ; mov rsi,rdx ; mov rdx,rcx ;
    // cmp [rsi],eax ; jne ; mov eax,[rcx+0x154] ; cmp [rsi+4],eax ; je` : the cached display width / height the hook invalidates.
    constexpr const char* kCacheCompare =
        "48 89 4C 24 08 56 48 81 EC 90 00 00 00 8B 81 ?? ?? ?? ?? 48 8B F2 48 8B D1 39 06 75 0F 8B 81 54 01 00 00 39 46 04 0F 84 ?? ?? ?? ??";

    // A guard at a seam, never scanned: the pattern is verified at value(from) + fromOffset (ONE such anchor makes a shape seam).
    auto shape = [](const char* from, const char* pattern) -> AnchorSpec
    {
        AnchorSpec a;
        a.name = "shape";
        a.pattern = pattern;
        a.fromSeam = from;
        return a;
    };

    // ---- the settings block and what it carries ----
    {
        AnchorSpec a;
        a.name = "settingsBlock";
        a.pattern = kSettingsBlock;
        // CodeSite: a position inside the resolver, not an entry. Single anchor: no second independent site has this exact 37-byte shape
        // (the settings tests in rendererUpdate / querySize have another order and other registers; they feed the global and offset seams).
        r.seams.push_back({ "quality.settingsInstructions", SeamKind::CodeSite, { a } });
        r.bindings.push_back({ "nativeQuality.settingsInstructionsRva", "quality.settingsInstructions", 0 });
        r.bindings.push_back({ "nativeQuality.settingsInstructions", "quality.settingsInstructions", 0 });
    }

    // ---- querySize: three location anchors, and the shape seam that pins the signature (strict entry pattern) ----
    {
        AnchorSpec entry;
        entry.name = "entry";
        entry.pattern = kQuerySizeEntry;
        entry.entryAnchor = true;
        AnchorSpec body;
        body.name = "body";
        body.pattern = kQuerySizeBody;
        body.valueFrom = ValueFrom::EnclosingFunction;
        // the third view, independent of the function's own bytes: the manager constructor stores the address of the function into the renderer's
        // size-query slot (`lea rcx,[querySize]`, decoded) and nothing else reads that. With it a lost prologue leaves entry lost + body + constructor
        // agreeing: plain Found instead of FoundSingleAnchor (and no weak cascade). The signature is the shape seam's job, not this anchor's.
        AnchorSpec stored;
        stored.name = "storedByManagerCtor";
        stored.pattern = kManagerCtor;
        stored.valueFrom = ValueFrom::Target;
        stored.decode = { DecodeKind::RipDisp32, 7, 3, 7, 0 };
        stored.predicates = { pred::TargetIn(RegionKind::Code) };
        r.seams.push_back({ "quality.querySize", SeamKind::Function, { entry, body, stored } });
        r.seams.push_back({ "quality.querySizeShape", SeamKind::CodeSite, { shape("quality.querySize", kQuerySizeEntry) }, true });
        r.bindings.push_back({ "nativeQuality.querySizeRva", "quality.querySize", 0 });
        r.bindings.push_back({ "nativeQuality.querySizeInstructions", "quality.querySizeShape", 0 });
    }

    // ---- rendererUpdate: entry, body and the caller locate it; the shape seam pins rcx = renderer / xmm1 = elapsed inside the function ----
    {
        AnchorSpec entry;
        entry.name = "entry";
        entry.pattern = kRendererUpdateEntry;
        entry.entryAnchor = true;
        AnchorSpec body;
        body.name = "body";
        body.pattern = kRendererUpdateBody;
        body.valueFrom = ValueFrom::EnclosingFunction;
        AnchorSpec caller;
        caller.name = "caller";
        caller.pattern = kRendererUpdateCaller;
        caller.valueFrom = ValueFrom::Target;
        caller.decode = { DecodeKind::Rel32, 11, 0, 0, 0 };
        r.seams.push_back({ "quality.rendererUpdate", SeamKind::Function, { entry, body, caller } });
        r.seams.push_back({ "quality.rendererUpdateShape", SeamKind::CodeSite, { shape("quality.rendererUpdate", kRendererUpdateEntry) }, true });
        r.bindings.push_back({ "nativeQuality.rendererUpdateRva", "quality.rendererUpdate", 0 });
        r.bindings.push_back({ "nativeQuality.rendererUpdateInstructions", "quality.rendererUpdateShape", 0 });
    }

    // ---- resizeCallbacks: the call inside rendererUpdate (its target) and its own entry locate it (the direct call of the hooks); the shape
    //      seam pins the lock / loop / AND of the results ----
    {
        AnchorSpec caller;
        caller.name = "callInRendererUpdate";
        caller.pattern = kResizeBlock;
        caller.valueFrom = ValueFrom::Target;
        caller.decode = { DecodeKind::Rel32, 4, 0, 0, 0 };
        caller.predicates = { pred::SameFunctionAs("quality.rendererUpdate") };
        AnchorSpec entry;
        entry.name = "entry";
        entry.pattern = kResizeEntry;
        entry.entryAnchor = true;
        r.seams.push_back({ "quality.resizeCallbacks", SeamKind::Function, { caller, entry } });
        r.seams.push_back({ "quality.resizeCallbacksShape", SeamKind::CodeSite, { shape("quality.resizeCallbacks", kResizeEntry) }, true });
        r.bindings.push_back({ "nativeQuality.resizeCallbacksRva", "quality.resizeCallbacks", 0 });
        r.bindings.push_back({ "nativeQuality.resizeCallbacksInstructions", "quality.resizeCallbacksShape", 0 });
    }

    // ---- the three globals ----
    {
        // settings: S1 (the settings block), the resize block of rendererUpdate and the querySize guard: three different functions load it
        AnchorSpec block;
        block.name = "settingsBlockLoad";
        block.pattern = kSettingsBlock;
        block.fromSeam = "quality.settingsInstructions";
        block.valueFrom = ValueFrom::Target;
        block.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        block.predicates = { pred::TargetIn(RegionKind::Bss) };
        AnchorSpec resize;
        resize.name = "resizeBlockLoad";
        resize.pattern = kResizeBlock;
        resize.valueFrom = ValueFrom::Target;
        resize.decode = { DecodeKind::RipDisp32, 9, 3, 7, 0 };
        resize.predicates = { pred::TargetIn(RegionKind::Bss) };
        AnchorSpec query;
        query.name = "querySizeLoad";
        query.pattern = kQuerySettingsWindow;
        query.fromSeam = "quality.querySize";
        query.fromOffset = 0x31;
        query.valueFrom = ValueFrom::Target;
        query.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        query.predicates = { pred::TargetIn(RegionKind::Bss) };
        r.seams.push_back({ "quality.settingsPointer", SeamKind::Global, { block, resize, query } });
        r.bindings.push_back({ "nativeQuality.settingsPointerRva", "quality.settingsPointer", 0 });

        // manager: the load of querySize right before `mov rbx,[rax+0x4220]`. Single anchor: no other site loads it next to that offset.
        AnchorSpec contextLoad;
        contextLoad.name = "contextLoad";
        contextLoad.pattern = kQueryManagerWindow;
        contextLoad.fromSeam = "quality.querySize";
        contextLoad.fromOffset = 0x15;
        contextLoad.valueFrom = ValueFrom::Target;
        contextLoad.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        contextLoad.predicates = { pred::TargetIn(RegionKind::Bss) };
        r.seams.push_back({ "quality.managerPointer", SeamKind::Global, { contextLoad } });
        r.bindings.push_back({ "nativeQuality.managerPointerRva", "quality.managerPointer", 0 });

        // device: the load into r14 at the top of rendererUpdate, and the load of the scene-size block (same function)
        AnchorSpec top;
        top.name = "r14Load";
        top.pattern = kRendererUpdateBody;
        top.fromSeam = "quality.rendererUpdate";
        top.fromOffset = 0x1a;
        top.valueFrom = ValueFrom::Target;
        top.decode = { DecodeKind::RipDisp32, 0x28, 3, 7, 0 };
        top.predicates = { pred::TargetIn(RegionKind::Bss) };
        AnchorSpec size;
        size.name = "sizeBlockLoad";
        size.pattern = kSizeBlock;
        size.valueFrom = ValueFrom::Target;
        size.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        size.predicates = { pred::TargetIn(RegionKind::Bss), pred::SameFunctionAs("quality.rendererUpdate") };
        r.seams.push_back({ "quality.devicePointer", SeamKind::Global, { top, size } });
        r.bindings.push_back({ "nativeQuality.devicePointerRva", "quality.devicePointer", 0 });
    }

    // ---- the struct offsets (Offset seams; the value is the decoded field, the sanity range is pred::ValueIn) ----
    {
        auto field = [](const char* name, const char* pattern, int at, int width) {
            AnchorSpec a;
            a.name = name;
            a.pattern = pattern;
            a.valueFrom = ValueFrom::Target;
            a.decode = { DecodeKind::Field, at, 0, 0, static_cast<std::uint8_t>(width) };
            return a;
        };
        auto fromSeam = [](AnchorSpec a, const char* seam, int fromOffset = 0) {
            a.fromSeam = seam;
            a.fromOffset = fromOffset;
            return a;
        };
        auto seam = [&r](const char* id, std::vector<AnchorSpec> anchors, std::uint32_t lo, std::uint32_t hi, const char* key) {
            for (AnchorSpec& a : anchors)
                a.predicates.push_back(pred::ValueIn(lo, hi));
            r.seams.push_back({ id, SeamKind::Offset, std::move(anchors) });
            r.bindings.push_back({ key, id, 0 });
        };

        // the settings struct: byte offsets (disp8), 0x10..0x55. The upper bound is the consumer's, not the encoding's: FfxivNativeQualityHooks.h
        // snapshots the struct into a std::array of kSettingsSnapshotSize = 0x56 bytes (a compile-time value taken from the table offsets) and
        // indexes it with the resolved offsets WITHOUT a bounds check, so an offset above 0x55 would read past the array. Keep this cap until the snapshot
        // is sized from the resolved offsets (settingsRenderer 0x47 and settingsAdditional 0x49, the other members, have no seam here).
        constexpr std::uint32_t kSnapshotLast = 0x55;
        seam("quality.settingsModeOffset",
             { fromSeam(field("settingsBlock", kSettingsBlock, 13, 1), "quality.settingsInstructions"), field("resizeBlock", kResizeBlock, 18, 1),
               fromSeam(field("querySize", kQuerySettingsWindow, 9, 1), "quality.querySize", 0x31) },
             0x10, kSnapshotLast, "nativeQuality.settingsModeOffset");
        seam("quality.settingsQualityOffset",
             { fromSeam(field("settingsBlock", kSettingsBlock, 20, 1), "quality.settingsInstructions"), field("resizeBlock", kResizeBlock, 24, 1) },
             0x10, kSnapshotLast, "nativeQuality.settingsQualityOffset");
        seam("quality.settingsDlaaOffset",
             { fromSeam(field("settingsBlock", kSettingsBlock, 26, 1), "quality.settingsInstructions"), field("resizeBlock", kResizeBlock, 30, 1) },
             0x10, kSnapshotLast, "nativeQuality.settingsDlaaOffset");
        // single anchor: the selector byte is read by the settings block only
        seam("quality.settingsSelectorOffset", { fromSeam(field("settingsBlock", kSettingsBlock, 33, 1), "quality.settingsInstructions") }, 0x10,
             kSnapshotLast, "nativeQuality.settingsSelectorOffset");

        // manager -> context (the quality table's owner): the load in querySize and the store in the manager constructor
        seam("quality.managerContextOffset",
             { fromSeam(field("querySize", kQueryManagerWindow, 19, 4), "quality.querySize", 0x15), field("managerCtor", kManagerCtor, 3, 4) }, 0x1000, 0x10000,
             "nativeQuality.managerContextOffset");
        // device -> callbacks: `mov rcx,[r14+0x30]` right before the call (single anchor)
        seam("quality.deviceCallbacksOffset", { field("resizeBlock", kResizeBlock, 3, 1) }, 0x08, 0x7f, "nativeQuality.deviceCallbacksOffset");
        // renderer -> scene size: the first compare of the size block (the second compare keeps +0x42c fixed; the +4 relation is not enforced, the
        // hooks only read the size for a log line; single anchor)
        {
            AnchorSpec a = field("sizeBlock", kSizeBlock, 15, 4);
            a.predicates = { pred::SameFunctionAs("quality.rendererUpdate") };
            seam("quality.rendererSceneSizeOffset", { a }, 0x100, 0x2000, "nativeQuality.rendererSceneSizeOffset");
        }
    }

    // ---- the context chain: quality.contextResolver (0x374590, helper) carries the flags offset; quality.cacheCompare (0x374f10, helper)
    //      carries the display-size cache offset ----
    {
        AnchorSpec viaBlock;
        viaBlock.name = "viaSettingsBlock";
        viaBlock.pattern = kSettingsBlock;
        viaBlock.valueFrom = ValueFrom::EnclosingFunction;
        AnchorSpec viaCall;
        viaCall.name = "calledByQuerySize";
        viaCall.pattern = kQueryCallWindow;
        viaCall.fromSeam = "quality.querySize";
        viaCall.fromOffset = 0x3e;
        viaCall.valueFrom = ValueFrom::Target;
        viaCall.decode = { DecodeKind::Rel32, 9, 0, 0, 0 };
        r.seams.push_back({ "quality.contextResolver", SeamKind::Function, { viaBlock, viaCall } });

        AnchorSpec flags;
        flags.name = "flagsTest";
        flags.pattern = kContextFlagsWindow;
        flags.fromSeam = "quality.contextResolver";
        flags.fromOffset = 8;
        flags.valueFrom = ValueFrom::Target;
        flags.decode = { DecodeKind::Field, 2, 0, 0, 4 };
        flags.predicates = { pred::ValueIn(0x100, 0x400) };
        // the second, independent anchor: the other instruction that reads the same byte of the context (and the `& 0x30` right behind it); it is
        // scanned, and SameFunctionAs(contextResolver) is its second disambiguator (the instruction is in a chained range of that function)
        AnchorSpec flagsLoad;
        flagsLoad.name = "flagsLoad";
        flagsLoad.pattern = kContextFlagsLoad;
        flagsLoad.minMargin = 4; // measured: 13 sites at distance 4, none closer
        flagsLoad.valueFrom = ValueFrom::Target;
        flagsLoad.decode = { DecodeKind::Field, 3, 0, 0, 4 };
        flagsLoad.predicates = { pred::SameFunctionAs("quality.contextResolver"), pred::ValueIn(0x100, 0x400) };
        r.seams.push_back({ "quality.contextFlagsOffset", SeamKind::Offset, { flags, flagsLoad } });
        r.bindings.push_back({ "nativeQuality.contextFlagsOffset", "quality.contextFlagsOffset", 0 });

        AnchorSpec viaSite;
        viaSite.name = "calledBeforeSettingsBlock";
        viaSite.pattern = kCallContext;
        viaSite.fromSeam = "quality.settingsInstructions";
        viaSite.fromOffset = -16;
        viaSite.valueFrom = ValueFrom::Target;
        viaSite.decode = { DecodeKind::Rel32, 11, 0, 0, 0 };
        AnchorSpec body;
        body.name = "entry";
        body.pattern = kCacheCompare;
        body.entryAnchor = true;
        r.seams.push_back({ "quality.cacheCompare", SeamKind::Function, { viaSite, body } });

        AnchorSpec cache;
        cache.name = "cacheLoad";
        cache.pattern = kCacheWindow;
        cache.fromSeam = "quality.cacheCompare";
        cache.fromOffset = 0x0d;
        cache.valueFrom = ValueFrom::Target;
        cache.decode = { DecodeKind::Field, 2, 0, 0, 4 };
        cache.predicates = { pred::ValueIn(0x100, 0x400) };
        r.seams.push_back({ "quality.contextCacheOffset", SeamKind::Offset, { cache } }); // single anchor; the window pins +0x150 and +0x154 (verified, not healed)
        r.bindings.push_back({ "nativeQuality.contextCacheOffset", "quality.contextCacheOffset", 0 });
    }

    // ================================ camera ================================
    //
    // G = [exe + cameraGlobalRva] ; S = [G + cameraOffset] ; P = [S + jitterPointerOffset] (FfxivJitterProbe.h). Own seam ids: this group never
    // names a jitterSource.* seam (duplicates across groups are fine, the golden test compares the table fields).
    //   accessor 0x795b90 (leaf): mov rax,[G] ; mov rax,[rax+0x3028] ; ret      (the six accessors of this shape differ in offset and global)
    //   producer window 0x3740c9..0x374122: three global loads, mov rcx,[rax+CAMERA] (rcx = S, kept intact by the stores that follow) and the
    //   call of the camera getter (an E8 whose target is the getter; mov rax,[rcx+0x278] ; ret).
    constexpr const char* kCameraAccessor = "48 8B 05 ?? ?? ?? ?? 48 8B 80 28 30 00 00 C3";
    constexpr const char* kCameraProducer =
        "48 8B 05 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 8B 86 68 01 00 00 49 89 06 48 8B 86 78 01 00 00 "
        "48 89 86 00 01 00 00 48 8B 87 50 06 00 00 48 89 86 08 01 00 00 48 8B 47 70 48 89 86 10 01 00 00 4C 89 BE 18 01 00 00 E8 ?? ?? ?? ??";
    constexpr const char* kCameraGetter = "48 8B 81 ?? ?? ?? ?? C3";
    {
        AnchorSpec acc;
        acc.name = "accessor";
        acc.pattern = kCameraAccessor;
        // DEVIATION from the default margin 3, measured: 11 fixed bytes, 22 sites at distance 2. 16 of them are loads of THIS global
        // (0x290dba0, offset 0x3028) into another register: they would decode the same value. The other 6 are sibling accessors of other
        // globals (0x290e490 with offsets 0x228 / 0x258 / 0x260 / 0x5f0, 0x290ee90 with 0x570 / 0x578) that differ only in the low bytes of
        // their offset constant. No predicate separates those better than the offset bytes themselves, so the value is protected by the
        // second anchor instead: a drifted accessor that matched a sibling gives another global, which the producer window contradicts
        // (Conflict, inert). With the window gone this anchor alone is a FoundSingleAnchor.
        acc.minMargin = 2;
        acc.valueFrom = ValueFrom::Target;
        acc.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        acc.predicates = { pred::TargetIn(RegionKind::Bss) };
        AnchorSpec prod;
        prod.name = "producerWindow";
        prod.pattern = kCameraProducer;
        prod.valueFrom = ValueFrom::Target;
        prod.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
        prod.predicates = { pred::TargetIn(RegionKind::Bss) };
        r.seams.push_back({ "camera.global", SeamKind::Global, { acc, prod } });
        r.bindings.push_back({ "jitter.cameraGlobalRva", "camera.global", 0 });

        // the getter (leaf: no .pdata entry): the target of the producer's call
        AnchorSpec getter;
        getter.name = "calledByProducer";
        getter.pattern = kCameraProducer;
        getter.valueFrom = ValueFrom::Target;
        getter.decode = { DecodeKind::Rel32, 84, 0, 0, 0 };
        r.seams.push_back({ "camera.getter", SeamKind::Function, { getter } });

        // G -> S offset: the accessor keeps +0x3028 fixed (11 fixed bytes only identify it that way), the producer window decodes it
        AnchorSpec accOffset;
        accOffset.name = "accessor";
        accOffset.pattern = kCameraAccessor;
        accOffset.minMargin = 2; // same pattern, same measured margin as above
        accOffset.valueFrom = ValueFrom::Target;
        accOffset.decode = { DecodeKind::Field, 10, 0, 0, 4 };
        accOffset.predicates = { pred::ValueIn(0x1000, 0x10000) };
        AnchorSpec prodOffset;
        prodOffset.name = "producerWindow";
        prodOffset.pattern = kCameraProducer;
        prodOffset.valueFrom = ValueFrom::Target;
        prodOffset.decode = { DecodeKind::Field, 24, 0, 0, 4 };
        prodOffset.predicates = { pred::ValueIn(0x1000, 0x10000) };
        r.seams.push_back({ "camera.cameraOffset", SeamKind::Offset, { accOffset, prodOffset } });
        r.bindings.push_back({ "jitter.cameraOffset", "camera.cameraOffset", 0 });

        // S -> P offset: `mov rax,[rcx+0x278] ; ret` at the getter (single anchor)
        AnchorSpec pOffset;
        pOffset.name = "getterBody";
        pOffset.pattern = kCameraGetter;
        pOffset.fromSeam = "camera.getter";
        pOffset.valueFrom = ValueFrom::Target;
        pOffset.decode = { DecodeKind::Field, 3, 0, 0, 4 };
        pOffset.predicates = { pred::ValueIn(0x100, 0x400) };
        r.seams.push_back({ "camera.jitterPointerOffset", SeamKind::Offset, { pOffset } });
        r.bindings.push_back({ "jitter.jitterPointerOffset", "camera.jitterPointerOffset", 0 });
    }
}
} // namespace ext::ffxiv::resolve
