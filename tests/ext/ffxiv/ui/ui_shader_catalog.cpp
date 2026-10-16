// S10-A: the UI shader catalog is exactly the seven CRCs of FfxivHdrPreview::Tag, the five scene stages map as in
// FfxivSceneHdr::Stage, and nothing else is recognised.
#include "check.h"
#include "ext/ffxiv/ui/UiShaderCatalog.h"

using namespace ext::ffxiv::ui;

int main()
{
    // The seven CRCs, in the order of FfxivHdrPreview::Tag.
    const uint32_t expected[7] { 3865947726u, 2966694105u, 3759127293u, 1898885941u, 4076256744u, 3915247300u, 1823062889u };
    static_assert(kUiCrcs.size() == 7);
    for (int i = 0; i < 7; ++i)
    {
        CHECK_EQ(kUiCrcs[i], expected[i]);
        CHECK(IsUiCrc(expected[i]));
        CHECK_EQ(StageOfCrc(expected[i]), kUiStage);
        CHECK_EQ(SceneStageOfCrc(expected[i]), 0);
    }

    // Distinct entries only.
    for (size_t i = 0; i < kUiCrcs.size(); ++i)
        for (size_t j = i + 1; j < kUiCrcs.size(); ++j)
            CHECK(kUiCrcs[i] != kUiCrcs[j]);

    // Nothing else is a UI CRC: not the five stage CRCs, not neighbours of the real ones, not zero.
    const uint32_t stages[5] { 0x85E777EFu, 0xF8F57F0Au, 0x27EBC404u, 0x1F264D17u, 0xF6E81A1Bu };
    for (int i = 0; i < 5; ++i)
    {
        CHECK(!IsUiCrc(stages[i]));
        CHECK_EQ(SceneStageOfCrc(stages[i]), i + 1);
        CHECK_EQ(StageOfCrc(stages[i]), i + 1);
    }
    for (uint32_t c : expected)
    {
        CHECK(!IsUiCrc(c + 1));
        CHECK(!IsUiCrc(c - 1));
    }
    CHECK(!IsUiCrc(0));
    CHECK(!IsUiCrc(0xFFFFFFFFu));
    CHECK_EQ(StageOfCrc(0), kStageNone);

    return Finish("ui_shader_catalog");
}
