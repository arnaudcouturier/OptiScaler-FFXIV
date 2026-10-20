// Pins the one supported FFXIV build (game version 2026.10.16) independently of GameBuild.h: every value below was found again in
// that executable (docs/FFXIV-PATCH-RUNBOOK.md) and is written here as a literal, so a wrong edit of the header fails this test.
#include "../chara/check.h"
#include "pch.h"

#include "ext/ffxiv/GameBuild.h"

#include <array>
#include <cstdint>

using namespace ext::ffxiv;

namespace
{
constexpr std::uint32_t kStamp = 0x6ad10bfd;
constexpr std::uint32_t kSize = 0x381f000;

void TestIdentity()
{
    CHECK(kCurrentBuild.timeDateStamp == kStamp && kCurrentBuild.sizeOfImage == kSize);
    CHECK(MatchBuild(kStamp, kSize) == &kCurrentBuild);

    // The previous build (2026.10.x before the patch) is no longer supported: a single-build design.
    CHECK(MatchBuild(0x6ac3dc96, 0x380a000) == nullptr);
    // near misses: one field right, the other wrong; off by one; swapped; zero
    CHECK(MatchBuild(kStamp, 0x380a000) == nullptr);
    CHECK(MatchBuild(0x6ac3dc96, kSize) == nullptr);
    CHECK(MatchBuild(kStamp + 1, kSize) == nullptr);
    CHECK(MatchBuild(kStamp - 1, kSize) == nullptr);
    CHECK(MatchBuild(kStamp, kSize + 0x1000) == nullptr);
    CHECK(MatchBuild(kStamp, kSize - 0x1000) == nullptr);
    CHECK(MatchBuild(kSize, kStamp) == nullptr);
    CHECK(MatchBuild(0, 0) == nullptr);
    // this test executable is not the game
    CHECK(MatchCurrentProcess() == nullptr);
}

template <typename A, std::size_t N> bool SameBytes(const A& a, const std::uint8_t (&expected)[N])
{
    if (a.size() != N)
        return false;
    for (std::size_t i = 0; i < N; ++i)
        if (a[i] != expected[i])
            return false;
    return true;
}

void TestNativeQuality()
{
    const NativeQualityBuild& q = kCurrentBuild.nativeQuality;
    CHECK(q.settingsInstructionsRva == 0x3745d7);
    CHECK(q.querySizeRva == 0x352350);
    CHECK(q.rendererUpdateRva == 0x2da5b0);
    CHECK(q.resizeCallbacksRva == 0x2369a0);
    CHECK(q.settingsPointerRva == 0x290e470);
    CHECK(q.managerPointerRva == 0x290e498);
    CHECK(q.devicePointerRva == 0x2907d00);

    // The three RIP-relative guards carry the displacement of their global (instruction address + length + disp32 = global).
    static const std::uint8_t settings[] = {0x4c, 0x8b, 0x05, 0x92, 0x9e, 0x59, 0x02, 0x45, 0x33, 0xe4, 0x41, 0x80, 0x78, 0x54, 0x02, 0x74, 0x06, 0x45,
                                            0x38, 0x60, 0x44, 0x74, 0x71, 0x45, 0x38, 0x60, 0x45, 0x74, 0x6b, 0x41, 0x0f, 0xb6, 0x40, 0x55, 0x83, 0xf8, 0x06};
    static const std::uint8_t querySize[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24,
                                             0x18, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0x2c, 0xc1, 0x5b, 0x02};
    static const std::uint8_t update[] = {0x4c, 0x8b, 0xdc, 0x57, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xc6, 0x16, 0x61, 0x02};
    static const std::uint8_t resize[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x20};
    CHECK(SameBytes(q.settingsInstructions, settings));
    CHECK(SameBytes(q.querySizeInstructions, querySize));
    CHECK(SameBytes(q.rendererUpdateInstructions, update));
    CHECK(SameBytes(q.resizeCallbacksInstructions, resize));
    CHECK(q.settingsInstructionsRva + 7 + 0x02599e92u == q.settingsPointerRva);
    CHECK(q.querySizeRva + 21 + 7 + 0x025bc12cu == q.managerPointerRva);

    CHECK(q.settingsModeOffset == 0x54 && q.settingsQualityOffset == 0x44 && q.settingsDlaaOffset == 0x45 && q.settingsRendererOffset == 0x47 &&
          q.settingsAdditionalOffset == 0x49 && q.settingsSelectorOffset == 0x55);
    CHECK(q.managerContextOffset == 0x4220 && q.contextFlagsOffset == 0x181 && q.contextCacheOffset == 0x150 && q.deviceCallbacksOffset == 0x30 &&
          q.rendererSceneSizeOffset == 0x428);
}

void TestCompanion()
{
    const CompanionBuild& c = kCurrentBuild.companion;
    CHECK(c.batch16Rva == 0x6e56d0 && c.batch32Rva == 0x6e6270);
    CHECK(c.enqueueRva == 0x240b80 && c.bindStateRva == 0x22d750);
    CHECK(c.nameplateModulePointerRva == 0x2ab8650);
    CHECK(c.layoutSlotTableRva == 0x217c760);

    static const std::uint8_t batch[] = {0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55};
    static const std::uint8_t enqueue[] = {0x48, 0x63, 0x41, 0x0c, 0x4c, 0x8b, 0xda, 0x4c, 0x8d, 0x04, 0x40, 0x8b, 0x41, 0x08, 0x42, 0x83};
    static const std::uint8_t bind[] = {0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xec, 0x48, 0x80, 0x7a, 0x49, 0x00, 0x4c, 0x8b, 0xf2, 0x48};
    CHECK(SameBytes(c.batchInstructions, batch));
    CHECK(SameBytes(c.enqueueInstructions, enqueue));
    CHECK(SameBytes(c.bindStateInstructions, bind));

    // structure offsets re-read in the 2026.10.16 executable
    CHECK(c.rendererBatchListOffset == 0x580 && c.rendererBatchCountOffset == 0x588 && c.rendererIndexBufferOffset == 0x1e0 &&
          c.rendererVertexAllocationOffset == 0x570 && c.rendererVertexStrideOffset == 0x500 && c.rendererVertexOffsetOffset == 0x504 &&
          c.rendererVertexSlotStride == 0x10);
    CHECK(c.indexBufferCapacityOffset == 0x38 && c.indexBufferFlagsOffset == 0x40 && c.indexBufferCpuStartOffset == 0x60 && c.indexBufferCpuEndOffset == 0x68);
    CHECK(c.nativeContextOffset == 0x17b8 && c.drawStateFlagOffset == 0x49 && c.drawStateCommandBackPointerOffset == 0x20);
    CHECK(c.batchKindOffset == 0 && c.batchCountOffset == 0x20 && c.batchVerticesOffset == 0x28 && c.batchFormatOffset == 0x30 && c.entryPointerOffset == 0x08 &&
          c.nativeBatchEntrySize == 16 && c.nativeBatchHeaderSize == 64);
    CHECK(c.drawCommandType == 6 && c.drawCommandSize == 0xb0 && c.layoutSlotEntrySize == 8);
}

void TestCharaView()
{
    const CharaViewBuild& v = kCurrentBuild.charaView;
    CHECK(v.accessorRva == 0x26abd0);
    CHECK(v.managerPointerRva == 0x290e490);
    CHECK(v.accessorRva + v.ripInstructionLength + 0x026a38b9u == v.managerPointerRva); // the displacement in the accessor of this build
}

void TestJitter()
{
    const JitterBuild& j = kCurrentBuild.jitter;
    CHECK(j.cameraGlobalRva == 0x290dba0);
    CHECK(j.cameraOffset == 0x3028 && j.jitterPointerOffset == 0x278);
    CHECK(j.jitterXOffset == 0 && j.jitterYOffset == 4 && j.ndcXOffset == 8 && j.ndcYOffset == 12 && j.counterOffset == 16 && j.objectBytes == 20);
}
} // namespace

int main()
{
    TestIdentity();
    TestNativeQuality();
    TestCompanion();
    TestCharaView();
    TestJitter();
    return TEST_RESULT("game_build");
}
