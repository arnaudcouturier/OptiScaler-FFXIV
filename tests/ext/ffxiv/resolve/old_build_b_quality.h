#pragma once
// Recorded bytes of build B for the quality group. Rows with confidence exactly "B-read" (the
// bytes were read from the B file; the B executable no longer exists). Nothing derived is copied.
//
//   chara.accessor   B 0x26ab40  48 8b 05 49 e9 68 02 | 8b ca ...          C 0x26abd0  only the displacement differs (idx 3-5)
//   settings block   B 0x374347  37 bytes                                   C 0x3745d7  idx 3-5 (the RIP displacement of the settings global)
//   querySize        B 0x3520c0  28 bytes = prologue + the manager load     C 0x352350  idx 24-26 (the RIP displacement of the manager global)
//   rendererUpdate   B 0x2da290  18 bytes = prologue + the cookie load     C 0x2da5b0  idx 14-16 (the RIP displacement of the stack cookie)
//   resizeCallbacks  B 0x236a40  20 bytes = the prologue                    C 0x2369a0  none
// B: 0x26ab40 + 7 + 0x0268e949 = 0x28f9490 (the manager pointer, in the bss tail of B's .data);
//    0x374347 + 7 + 0x02585122 = 0x28f9470 (the settings global); 0x3520c0 + 0x15 + 7 + 0x025a73bc = 0x28f9498 (the manager global).
// What these rows can prove: only the bytes that were recorded. The anchors of querySize / rendererUpdate / resizeCallbacks are longer than the
// recorded prefixes (112 / 73 / 137 bytes against 28 / 18 / 20), so on build B they are tested for PREFIX compatibility (the recorded bytes match
// the pattern's first positions, and every byte that differs between B and C is masked), never as a resolution of B.
#include "old_build_b.h"

namespace oldb
{
inline constexpr Row kCharaAccessorB = { "chara.accessor", 0x26ab40, "488b0549e968028bca488b84c8c8020000c3", "", "B-read" };
inline constexpr std::uint32_t kCharaManagerB = 0x28f9490;

// The same function as build C has it (read from the C executable, equals GameBuild.h CharaViewBuild).
inline constexpr Row kCharaAccessorC = { "chara.accessor", 0x26abd0, "488b05b9386a028bca488b84c8c8020000c3", "", "C-exe" };
inline constexpr std::uint32_t kCharaManagerC = 0x290e490;

// settings block (the whole 37-byte guard of the old table): recorded in full, so the anchor is resolved on a B image
inline constexpr Row kSettingsB = { "quality.settingsInstructions", 0x374347, "4c8b05225158024533e44180785402740645386044747145386045746b410fb6405583f806", "", "B-read" };
inline constexpr std::uint32_t kSettingsGlobalB = 0x28f9470;
// the first bytes of the three functions with the longer anchors
inline constexpr Row kQuerySizeB = { "quality.querySize", 0x3520c0, "48895c2408488974241048897c241841564883ec20488b05bc735a02", "", "B-read" };
inline constexpr Row kRendererUpdateB = { "quality.rendererUpdate", 0x2da290, "4c8bdc574881ecd0000000488b05f6c95f02", "", "B-read" };
inline constexpr Row kResizeCallbacksB = { "quality.resizeCallbacks", 0x236a40, "48895c241048896c24184889742420574883ec20", "", "B-read" };
} // namespace oldb
