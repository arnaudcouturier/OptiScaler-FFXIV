#pragma once
// Recorded bytes of build B (PE stamp 0x6ac3dc96) for the companion group. Rows with the
// confidence "B-read" only (bytes read from the B executable and recorded in the shipped B GameBuild.h; the executable is gone).
// What B has NOT recorded for this group is listed here so that nobody plants it:
//   * the nameplate module global (B 0x2aa35c0): B-rva-only-medium, a bss pointer, no bytes (reference-chain evidence only);
//   * every byte beyond the first 16 of the batch functions, of enqueue and of bindState (only the 16-byte guards were recorded), so the long
//     anchors (batch entry-to-shift, enqueue tail, bindState body, dispatcher call, all Offset seams, the index-buffer map) have NO previous-build
//     evidence. Single-build only; they are proven on build C bytes and by the decoy tests.
//
//   B 0x6e5570 / 0x6e6110  batch16 / batch32, the same 16 bytes (C 0x6e56d0 / 0x6e6270, +0x160): unchanged B to C
//   B 0x240bc0             enqueue            (C 0x240b80, -0x40): unchanged
//   B 0x22d7f0             bindState          (C 0x22d750, -0xa0): unchanged
//   B 0x217bc00            layout slot table  (C 0x217c760, +0xb60): the 72 bytes are unchanged
#include "old_build_b.h"

namespace oldb
{
inline constexpr Row kBatch16B = { "companion.batch16", 0x6e5570, "8854241048894c240855535641544155", "", "B-read" };
inline constexpr Row kBatch32B = { "companion.batch32", 0x6e6110, "8854241048894c240855535641544155", "", "B-read" };
inline constexpr Row kEnqueueB = { "companion.enqueue", 0x240bc0, "4863410c4c8bda4c8d04408b41084283", "", "B-read" };
inline constexpr Row kBindStateB = { "companion.bindState", 0x22d7f0, "405641564883ec48807a49004c8bf248", "", "B-read" };
inline constexpr Row kLayoutTableB = { "companion.layoutSlotTable", 0x217bc00,
                                       "0000000000000000000000000000000000000000000000000000000000000000"
                                       "01000000050000000200000004000000020000000300000001000000020000000100000001000000",
                                       "", "B-read" };
// the nameplate global: recorded as an RVA only (and the fixture says so)
inline constexpr Row kNameplateGlobalB = { "companion.nameplateModulePointer", 0x2aa35c0, "", "", "B-rva-only-medium" };
} // namespace oldb
