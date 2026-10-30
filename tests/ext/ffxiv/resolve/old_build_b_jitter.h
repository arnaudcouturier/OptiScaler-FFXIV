#pragma once
// Recorded bytes for the jitter-source group.
//
//   C rows ("C-exe"): bytes read from the build-C executable (analysis/patch-20261016/ffxiv_dx11.exe, stamp 0x6ad10bfd) at the RVA shown. They are the
//   committed evidence of the always-on tests (the real executable is never committed). The camera update, the producer region and the generator are
//   contiguous byte runs, so the blocks keep their real neighbours.
//   B rows: ONLY recorded rows whose confidence is exactly "B-read" or "B-read-partial" (maskB honoured, never planted):
//     jitterSource.generator   B 0x2ebae0, 20 bytes, B-read (the first 20 bytes of the generator; C has the same 16 + 4)
//     jitterSource.phases      B 0x25f6a1, 48 bytes, B-read-partial (idx 3..6, the displacement of the renderer load, are NOT recorded)
//     jitter.fallbackImmediate B 0x25f6b2, 5 bytes, B-read (mov edx,-1)
//   Everything else of this group has NO recorded B bytes (camera getter, camera head, strength, deviceSize, producer call are RVA-only or not
//   recorded; the derived-from-C / derived-from-B-RVAs tiers are refused). The B evidence is therefore a PREFIX evidence: the strict patterns of the
//   generator and phases start with exactly these bytes (see resolve_jitter_b_fixture.cpp). It says nothing about the rest of the blocks.
#include "old_build_b.h"

namespace oldb
{
inline constexpr Row kGeneratorC = { "jitterSource.generator", 0x2ebd90, "488bc44889580848897010574883ec700f2970e8488bf20f2978d833d2440f2940c8498bf8440f2948b80f57fff3440f100d6220e3010f57f6440f2950a8418bc1f7b424a0000000f3440f1015a720e301450f28c28d5a018bcb85db0f84930000000f57c0f3450f59c18bc183e001f3480f2ac08bc1f3410f59c0f30f58f00f57c0f3480f2ac0f3410f59c1e8f797af01f3480f2cc885c975c8f3440f10057520e3010f1f4000660f1f8400000000000f57c0b8abaaaaaaf7e3f3450f5ed08bcbd1ea8d04522bc88bc1f3480f2ac08bc3f3410f59c2f30f58f80f57c0f3480f2ac0f3410f5ec0e89c97af01f3480f2cd885db75bb4c8d5c2470498b5b10f3410f5cf1450f2843d0f3410f5cf9450f284bc0450f2853b0f30f1136498b73180f28742460f30f113f0f287c2450498be35fc3", "", "C-exe" };
inline constexpr Row kCameraGetterC = { "jitterSource.cameraGetter", 0x25f610, "488b8178020000c3", "", "C-exe" };
inline constexpr Row kCameraGetterDecoyC = { "jitterSource.cameraGetter (decoy: the second raw hit of the 8-byte pattern)", 0x148c408, "488b8178020000c3", "", "C-exe" };
inline constexpr Row kCameraUpdateC = { "jitterSource.cameraUpdate (head .. NDC store)", 0x25f690, "405557488dac2468feffff4881ec98020000488b05dfc568024833c4488985d0000000488b01488bf948899c24b8020000ff5038488b05a5ed6a0232d2440fb640544180f802740683782c02750f389706020000740738501a7502b20180bf02020000004889b424c00200000f29b424700200000f85d001000033c90fb6c283bfe0010000550f45c884c90f84b9010000488b0568ed6a020f57f6488b9f78020000baffffffff488b8828040000488bf18bc148c1ee20f3480f2af04180f8027433488b05a7856a020f57c0f30f100d3ce7eb018b888c000000f3480f2ac1f30f5ec6e88865b801f30f5905bce6ee01f3480f2cd0448b4b104c8d430489542420488d4c2430488bd3e8f2c50800488b0debec6a024c8d45d0488b03488d575048894424380fb78100070000660f6ec80fb78104070000488b8f780200000f5bc9660f6ec0488b0594ec6a020f5bc0f30f105078f30f5c1578e5eb01f30f5ec80f57c0f30f59484cf3480f2ac6f30f59c9f30f59d1f30f581557e5eb01f30f58d20f28caf30f5954243cf30f594c24380f571569e8eb01f30f5ed00f2805aee7eb01f30f5ece0f2945d00f28052f1eee01f30f114c24380f280d121eee010f294de00f2945f0f30f1154243c488b44243848894108f30f1049", "", "C-exe" };
inline constexpr Row kProducerC = { "jitterSource.producer (globals .. scene size pair)", 0x3740c9, "488b05d09a5902488b3db9a35902488b1d223c5902488b8828300000488b8668010000498906488b867801000048898600010000488b875006000048898608010000488b4770488986100100004c89be18010000e8eeb4eeff0f57c041bdc40302000f57c9488b0848898e200100008b838c0000008b0df4dd8b02f3480f2ac08b8390000000f3480f2ac8f30f11842490000000f30f118c2494000000488b84249000000048898628010000418bc7488986380100008b87280400004c89bc2490000000898424900000008b872c040000898424940000", "", "C-exe" };
inline constexpr Row kDeviceSizeC = { "jitterSource.deviceSize", 0xd3f73, "488b0d863d8302448b81900000008b918c000000", "", "C-exe" };
inline constexpr Row kDeviceSizeSecondC = { "jitterSource.deviceSize (second template copy)", 0xd4013, "488b0de63c8302448b81900000008b918c000000", "", "C-exe" };
inline constexpr Row kStrengthFloorC = { "jitterSource.strengthFloor (.rdata)", 0x211dd64, "6f12833a0ad7233b6f12833bd68ba13b", "", "C-exe" };
inline constexpr Row kSignMaskC = { "jitterSource.ndcSignMask (.rdata)", 0x211e090, "00000080000000800000008000000080", "", "C-exe" };
inline constexpr Row kUnwindHeadC = { "unwind info of the camera update, first range (UNW_FLAG_CHAININFO flag 3 = EHANDLER|UHANDLER)", 0x25fb9f4, "192304001201530003700250c4ffd901", "", "C-exe" };
inline constexpr Row kUnwindBodyC = { "unwind info of the camera update, second range (chained to 0x25f690)", 0x25fba08, "214b06004b682700436458000834570090f62500b9f62500f4b95f02", "", "C-exe" };
inline constexpr Row kUnwindGeneratorC = { "unwind info of the generator", 0x2604160, "013e10003ea802002a980300228804001b78050014680600106411001034100010d20c70", "", "C-exe" };

// The three RUNTIME_FUNCTION entries of the executable that the group's EnclosingFunction anchors and SameFunctionAs predicates need, as
// { begin, end, unwind } (read from .pdata at RVA 0x2f95000 + 12 * index of the real file). The camera update is split in two chained ranges.
struct PdataEntry
{
    std::uint32_t begin, end, unwind;
};
inline constexpr PdataEntry kPdataC[3] = {
    { 0x25f690, 0x25f6b9, 0x25fb9f4 },
    { 0x25f6b9, 0x25f90e, 0x25fba08 },
    { 0x2ebd90, 0x2ebec2, 0x2604160 },
};

// B rows (see the file header)
inline constexpr Row kGeneratorB = { "jitterSource.generator", 0x2ebae0, "488bc44889580848897010574883ec700f2970e8", "", "B-read" };
inline constexpr Row kPhasesB = { "jitterSource.phases", 0x25f6a1,
                                  "488b05000000000f57f6488b9f78020000baffffffff488b8828040000488bf18bc148c1ee20f3480f2af04180f80274",
                                  "ffffff00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "B-read-partial" };
inline constexpr Row kFallbackImmediateB = { "jitter.fallbackImmediate", 0x25f6b2, "baffffffff", "", "B-read" };
} // namespace oldb
