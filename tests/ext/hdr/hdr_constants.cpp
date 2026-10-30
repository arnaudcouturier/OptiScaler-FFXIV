#include "ext/hdr/HdrConstants.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

int main()
{
    using namespace ext::hdr;

    static_assert(sizeof(HdrConstants) == 80, "20 dwords");
    static_assert(sizeof(HdrConstants) / 4 == 20, "root signature carries 20 constants");

    // Defaults: the legacy two-rule composition, zero selects it in the shader.
    {
        HdrConstantSource src;
        src.width = 3840;
        src.height = 2160;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.w == 3840 && k.h == 2160);
        assert(k.continuous == 0);
        assert(k.paper == 203.0f && k.peak == 1100.0f && k.expansion == 0.5f);
        assert(k.uiExpansion == k.expansion); // unset follows HighlightExpansion
        assert(k.scene == 0);
    }
    // Switching the composition off selects the continuous law in the shader.
    {
        HdrConstantSource src;
        src.legacyComposition = false;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.continuous == 1);
    }
    // A numeric UI expansion overrides the highlight expansion, and only the continuous law reads it.
    {
        HdrConstantSource src;
        src.legacyComposition = false;
        src.uiExpansionSet = true;
        src.uiExpansion = 0.8f;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.uiExpansion == 0.8f && k.expansion == 0.5f);
    }
    // No verified scene: the whole frame stays at paper white, the interface included, as before.
    {
        HdrConstantSource src;
        src.sceneRequested = true;
        src.sceneAvailable = false;
        src.uiExpansionSet = true;
        src.uiExpansion = 0.8f;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.scene == 0 && k.expansion == 0.0f && k.uiExpansion == 0.0f);
    }
    // A verified scene keeps both expansions.
    {
        HdrConstantSource src;
        src.sceneRequested = true;
        src.sceneAvailable = true;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.scene == 1 && k.expansion == 0.5f && k.uiExpansion == 0.5f);
    }
    // Without scene mode nothing is forced to paper white.
    {
        HdrConstantSource src;
        src.sceneRequested = false;
        src.sceneAvailable = false;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.scene == 0 && k.expansion == 0.5f && k.uiExpansion == 0.5f);
    }
    // The shared clamps are unchanged: peak, paper white, expansions, grading.
    {
        HdrConstantSource src;
        src.peak = 99999.0f;
        src.paper = 10.0f;
        src.expansion = 2.0f;
        src.contrast = 99.0f;
        src.uiExpansionSet = true;
        src.uiExpansion = -1.0f;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.peak == 4000.0f && k.paper == 80.0f && k.expansion == 1.0f);
        assert(k.contrast == 1.5f && k.uiExpansion == 0.0f);
    }
    // Non-finite config values fall back instead of poisoning the constants.
    {
        HdrConstantSource src;
        src.peak = std::numeric_limits<float>::quiet_NaN();
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.peak == 1100.0f);
    }
    // The preview-mask flag reaches the constants: Convert's barriers read it, not the always-zero trailing row.
    {
        HdrConstantSource src;
        src.sceneRequested = true;
        src.sceneAvailable = true;
        src.hasPreviewMask = true;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.scene == 1 && k.hasPreviewMask == 1);
        assert(k.reserved[0] == 0 && k.reserved[1] == 0);
        assert(NeedsPreviewMaskBarrier(k));
    }
    // A verified scene without a mask records no mask transitions.
    {
        HdrConstantSource src;
        src.sceneRequested = true;
        src.sceneAvailable = true;
        assert(!NeedsPreviewMaskBarrier(BuildHdrConstants(src)));
    }
    // A mask without a verified scene records no mask transitions either.
    {
        HdrConstantSource src;
        src.hasPreviewMask = true;
        const HdrConstants k = BuildHdrConstants(src);
        assert(k.scene == 0 && k.hasPreviewMask == 1);
        assert(!NeedsPreviewMaskBarrier(k));
    }
    // The UI expansion snapshot: one read, so the menu thread's Reset cannot land between two reads.
    {
        const auto set = ResolveUiExpansion(std::optional<float> { 0.8f }, 0.5f);
        assert(set.first && set.second == 0.8f);
        const auto unset = ResolveUiExpansion(std::optional<float> {}, 0.5f);
        assert(!unset.first && unset.second == 0.5f);
    }

    std::puts("PASS: HDR constants default to the legacy composition, resolve the UI expansion, force paper white without a scene, keep the shared clamps, gate the preview-mask barriers");
    return 0;
}
