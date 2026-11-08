#include "check.h"

#include "ext/ffxiv/engine/SsrPairRules.h"

int main()
{
    using namespace ext::ffxiv::engine::ssr::pair;
    CHECK(Fits(0, 1));
    CHECK(Fits(kByteBudget - 16, 16));
    CHECK(!Fits(kByteBudget - 15, 16));
    CHECK(!Fits(0, kByteBudget + 1));
    CHECK(!Fits(0, 0));
    // Two actual 4K FP16 scene/position pairs plus 1080p BGRA fallback/output pairs.
    constexpr uint64_t frameBytes = 2ull * 3840 * 2160 * 8 + 2ull * 1920 * 1080 * 4 + 4096;
    CHECK(Fits(frameBytes, frameBytes));
    CHECK(!TimedOut(10, 1210));
    CHECK(TimedOut(10, 1211));
    CHECK(!TimedOut(10, 9));
    CHECK(IsPairSlot(0));
    CHECK(IsPairSlot(1));
    CHECK(!IsPairSlot(2));
    CHECK_EQ(std::string(kManifestHeader), "file,frameToken,moduleRun,width,height,dxgiFormat,rowBytes\n");
    CHECK_EQ(FileName(0, "scene"), "frame1_scene");
    CHECK_EQ(FileName(1, "camera"), "frame2_camera");
    return Finish("ssr_pair_rules");
}
