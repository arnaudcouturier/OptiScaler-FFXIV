#pragma once
// Anchors for the game's simulation tick: the Framework object's Tick virtual function, the global that holds the Framework instance, and the
// three counters inside that instance the tick updates.
//
// OWNER: the latency group. This is the ONLY resolver file that group edits (plus its own tests under tests/ext/ffxiv/resolve/).
// It must NOT edit: ResolveTypes.h, ImageView.h, DenseImage.h, LiveImage.h, Pattern.h, Scan.h, Unwind.h, Resolver.h, ResolveLog.h,
// Entry.h, ResolveCache.h, FieldTable.h, Registry.h, AnchorsJitter.h, AnchorsQuality.h, GameBuild.h.
//
// The fields of GameBuild this group covers (FieldTable.h keys, each bound below to the seam that finds it):
//   latency.tickRva                 latency.frameworkInstanceRva
//   latency.frameCounterOffset      latency.perfCounterValueOffset      latency.perfCounterFreqOffset
//
// WHY THE ANCHORS LOOK INSIDE THE FUNCTION AND NOT AT ITS ENTRY
//   The consumer (ext/latency) does not patch code at all: it replaces the function's pointer in the vtable of the Framework object (the slot
//   below) and calls the old pointer. A foreign INLINE hook over the entry - Dalamud's framework update hook is one - writes the entry bytes and
//   leaves the rest of the body intact, so an anchor that starts at the entry would report EntryPatched (which is never usable) and turn the
//   feature off in exactly the sessions that have Dalamud. Both anchors below are therefore interior blocks, several instructions after the entry,
//   and the seam's value is the start of the function they lie in (.pdata, chains followed).
//
//   That makes this seam a CodeSite whose value is the enclosing function start: a deliberate use of the kind for "an address whose first bytes
//   we do not care about". A CodeSite is never checked for a jump at the value (Resolver.h), and Resolver.h forbids an entryAnchor on a CodeSite
//   ("a CodeSite is not an entry"), so the kind is used exactly as documented: the value is an RVA inside code that is not (necessarily) an entry.
//   The identity of the slot is verified at install time instead, live: `instance->vtable[4]` must hold `image base + tickRva` exactly, and the
//   vtable pointer must lie in read-only image data (ext/latency/TickLeaseCore.h). A vtable that has been rotated, a slot another tool owns, or a
//   build whose Tick is a different function all read as "refused", and nothing is written. The pointers the install reads before that check are
//   verified to be committed, readable memory of the right kind first, so a wrong instance global on an unknown build is refused instead of
//   dereferenced.
//
//   Two independent interior anchors, so a patch that rewrites one block leaves the other: one anchor resolving alone is FoundSingleAnchor, which is
//   usable (a WARN in the log), and the install-time identity check still decides.
//
// WHY THE OFFSETS ARE MASKED AND DECODED
//   The two displacements inside `tickClock` and the one inside the prologue are the Framework instance offsets this group binds
//   (frameCounterOffset, perfCounterValueOffset, perfCounterFreqOffset). They are wildcarded here only because the three Offset seams below
//   decode exactly those bytes from the same instruction text, with a sanity range, and the values differ between builds (the 2024 build is
//   8 bytes below the October one in all three). The rest of both blocks is fixed: it is instruction semantics (`inc`, `sub`, `cvtsi2sd`, the two
//   virtual calls of the teardown), not prologue bytes, which is why both blocks matched at the same displacement from the entry (+0x1a) in two
//   executables 28 months apart.
//
// Nothing here runs at load time: the function is called by BuildRegistry() when the registry is built.

#include "ResolveTypes.h"

namespace ext::ffxiv::resolve
{
inline void AddLatencyAnchors(Registry& r)
{
    // Framework::Tick, from its interior.
    //
    //   mov rax,[rsp+0x30] ; xorps xmm1,xmm1 ; sub rax,[rbx+VALUE] ; xorps xmm0,xmm0 ; cvtsi2sd xmm0,[rbx+FREQ] ;
    //   cvtsi2sd xmm1,rax ; divsd xmm1,xmm0 ; xorps xmm0,xmm0 ; cvtpd2ps xmm2,xmm1                  (the frame-time block)
    //
    // 43 bytes, +0x1a from the function entry of both executables read for this group. The two displacements are decoded by the perfCounter
    // Offset seams below. Everything else is fixed.
    constexpr const char* kTickClock =
        "48 8B 44 24 30 0F 57 C9 48 2B 83 ?? ?? 00 00 0F 57 C0 F2 48 0F 2A 83 ?? ?? 00 00 F2 48 0F 2A C8 F2 0F 5E C8 0F 57 C0 66 0F 5A D1";

    // The tail of the same function: the game's teardown calls.
    //
    //   cmp byte [rbx+8],0 ; je +n ; mov rax,[rbx] ; mov rcx,rbx ; call qword [rax+0x10] ; test al,al ; je +n ;
    //   mov rax,[rbx] ; mov rcx,rbx ; call qword [rax+0x18] ; xor al,al ; add rsp,0x20 ; pop rbx ; ret
    //
    // Slot 2 and slot 3 of the same vtable (Destroy, Free). The two short-branch displacements are wildcards: they move with anything the compiler
    // inserts inside this block, and the opcodes around them are what identify it. The two indirect calls are virtual dispatches, not imports.
    constexpr const char* kTickTeardown =
        "80 7B 08 00 74 ?? 48 8B 03 48 8B CB FF 50 10 84 C0 74 ?? 48 8B 03 48 8B CB FF 50 18 32 C0 48 83 C4 20 5B C3";

    AnchorSpec tickClock;
    tickClock.name = "tickClock";
    tickClock.pattern = kTickClock;
    tickClock.valueFrom = ValueFrom::EnclosingFunction;

    AnchorSpec tickTeardown;
    tickTeardown.name = "tickTeardown";
    tickTeardown.pattern = kTickTeardown;
    tickTeardown.valueFrom = ValueFrom::EnclosingFunction;

    r.seams.push_back({ "latency.tick", SeamKind::CodeSite, { tickClock, tickTeardown } });
    r.bindings.push_back({ "latency.tickRva", "latency.tick", 0 });

    // The Framework instance: `mov rbx,[rip+disp32] ; mov edi,[rsp+..]`. The global holds the pointer to the object whose vtable slot the consumer
    // leases. Both executables read for this group keep it in the uninitialised tail of .data (TargetIn(Bss)); a build that moved it to an
    // initialised section would fail this predicate and turn the feature off, which is the safe direction.
    // Measured offline with the resolver's margin tool on both executables (the pattern has six fixed bytes): exactly one raw hit each, and the
    // nearest OTHER site differs in one fixed byte - 303 such sites on the October 2026 build and 269 on the 2024 benchmark build. A masked
    // displacement does not count (wildcards are ignored), so none of them is a duplicate of the load; all 303/269 land outside the .bss range that
    // TargetIn(Bss) accepts. That predicate, not the byte pattern, is the disambiguator, which is the sanctioned use of minMargin 0 ("the predicates
    // are the disambiguator"); the install's readable-span and slot-identity checks then decide on the live object.
    AnchorSpec instanceLoad;
    instanceLoad.name = "instanceLoad";
    instanceLoad.pattern = "48 8B 1D ?? ?? ?? ?? 8B 7C 24";
    instanceLoad.valueFrom = ValueFrom::Target;
    instanceLoad.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    instanceLoad.predicates = { pred::TargetIn(RegionKind::Bss) };
    instanceLoad.minMargin = 0;
    r.seams.push_back({ "latency.frameworkInstance", SeamKind::Global, { instanceLoad } });
    r.bindings.push_back({ "latency.frameworkInstanceRva", "latency.frameworkInstance", 0 });

    // FrameCounter: the `inc dword [rcx+disp32]` of the entry. This is the only anchor of the group that starts at the entry, so a foreign inline
    // hook takes it away (NotFound) while the two interior anchors above still find the function. The probe then has no frame number to pair ticks
    // with presents and reports age and W as n/a (ext/latency/TickProbeCore.h); the lease still installs, because this offset is telemetry. The
    // gate carries a resolved bit per offset (GateReport::latencyOffsetsResolved), so an unresolved one is neither compared nor read.
    // SameFunctionAs keeps a stray copy of the prologue elsewhere in .text from being taken for the tick.
    {
        AnchorSpec a;
        a.name = "incFrameCounter";
        a.pattern = "40 53 48 83 EC 20 FF 81 ?? ?? 00 00 48 8B D9 48 8D 4C 24 30 FF 15";
        a.decode = { DecodeKind::Field, 8, 0, 0, 4 };
        a.valueFrom = ValueFrom::Target;
        a.predicates = { pred::ValueIn(0x1000, 0x2000), pred::SameFunctionAs("latency.tick") };
        r.seams.push_back({ "latency.frameCounterOffset", SeamKind::Offset, { a } });
        r.bindings.push_back({ "latency.frameCounterOffset", "latency.frameCounterOffset", 0 });
    }

    // The two QueryPerformanceCounter fields the tick reads: `sub rax,[rbx+VALUE]` and `cvtsi2sd xmm0,[rbx+FREQ]` inside the clock block above.
    // The pattern is that block's own text, so both seams share one scan; the displacement each decodes is the one it is named after.
    {
        AnchorSpec a;
        a.name = "perfCounterValue";
        a.pattern = kTickClock;
        a.decode = { DecodeKind::Field, 11, 0, 0, 4 };
        a.valueFrom = ValueFrom::Target;
        a.predicates = { pred::ValueIn(0x1000, 0x2000), pred::SameFunctionAs("latency.tick") };
        r.seams.push_back({ "latency.perfCounterValueOffset", SeamKind::Offset, { a } });
        r.bindings.push_back({ "latency.perfCounterValueOffset", "latency.perfCounterValueOffset", 0 });
    }
    {
        AnchorSpec a;
        a.name = "perfCounterFreq";
        a.pattern = kTickClock;
        a.decode = { DecodeKind::Field, 23, 0, 0, 4 };
        a.valueFrom = ValueFrom::Target;
        a.predicates = { pred::ValueIn(0x1000, 0x2000), pred::SameFunctionAs("latency.tick") };
        r.seams.push_back({ "latency.perfCounterFreqOffset", SeamKind::Offset, { a } });
        r.bindings.push_back({ "latency.perfCounterFreqOffset", "latency.perfCounterFreqOffset", 0 });
    }
}
} // namespace ext::ffxiv::resolve
