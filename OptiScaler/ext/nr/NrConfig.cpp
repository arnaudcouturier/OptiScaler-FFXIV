#include "pch.h"

#include "ext/nr/NrConfig.h"
#include "ext/core/ExtConfig.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>

namespace ext
{
void NrConfig::Bind(config::Binder& b)
{
    b.Add("DlssNr", "Enabled", enabled);
    b.Add("DlssNr", "RunBeforeSR", runBeforeSr);
    b.Add("DlssNr", "DeferredDLSS", deferredDlss);
    b.Add("DlssNr", "PreSRTrailGuard", preSrTrailGuard);
    b.Add("DlssNr", "SplitFrameWork", splitFrameWork);
    b.Add("DlssNr", "ResidualFG", residualFg);
    b.Add("DlssNr", "Precision", precision);
    b.Add("DlssNr", "ResidualFGApproxCamera", residualFgApproxCamera);
    b.Add("DlssNr", "ToggleKey", toggleKey, true); // hex when positive (a key code), like the reference build
    b.Add("DlssNr", "TransferStrength", transferStrength);
    b.Add("DlssNr", "ColourStrength", colourStrength);
    b.Add("DlssNr", "MaxRatio", maxRatio);
    b.Add("DlssNr", "Transfer", transfer);
    b.Add("DlssNr", "DebugView", debugView);
    b.Add("DlssNr", "Compare", compare);
    b.Add("DlssNr", "CompareSplit", compareSplit);
    b.Add("DlssNr", "CompareZoom", compareZoom);
    b.Add("DlssNr", "CompareSwap", compareSwap);
    b.Add("DlssNr", "CompareTags", compareTags);
    b.Add("DlssNr", "TagScale", tagScale);
    b.Add("DlssNr", "SpatialCompression", spatialCompression);
    b.Add("DlssNr", "SpatialCenterX", spatialCenterX);
    b.Add("DlssNr", "SpatialCenterY", spatialCenterY);
    b.Add("DlssNr", "SpatialWorkX", spatialWorkX);
    b.Add("DlssNr", "SpatialWorkY", spatialWorkY);
    b.Add("DlssNr", "SpatialOffsetX", spatialOffsetX);
    b.Add("DlssNr", "SpatialOffsetY", spatialOffsetY);
    b.Add("DlssNr", "SpatialShiftX", spatialShiftX);
    b.Add("DlssNr", "SpatialShiftY", spatialShiftY);
    b.Add("DlssNr", "PassFeedback", passFeedback);
    b.Add("DlssNr", "VitEvery", vitEvery);
    b.Add("DlssNr", "VitAllowUnverified", vitAllowUnverified);
    b.Add("DlssNr", "VitFirstPassOnly", vitFirstPassOnly);
    b.Add("DlssNr", "WorkingScale", workingScale);
    b.AddEnum("DlssNr", "ScalingDownscaler", scalingDownscaler, Scaler::Count);
    b.Add("DlssNr", "AutoCapture", autoCapture);
    b.Add("DlssNr", "ScanExposure", scanExposure);
    b.Add("DlssNr", "WhitePointSource", whitePointSource);
    b.Add("DlssNr", "WhitePointScale", whitePointScale);
    b.Add("DlssNr", "WhitePointTrim", whitePointTrim);
    b.Add("DlssNr", "ScanTrim", scanTrim);
    b.Add("DlssNr", "ScanAnchors", scanAnchors);
    b.Add("DlssNr", "ScanInverted", scanInverted);
    b.Add("DlssNr", "ScanMeter", scanMeter);
    b.Add("DlssNr", "LightingHistory", lightingHistory);
    b.Add("DlssNr", "Preset", preset);
    b.Add("DlssNr", "Intensity", intensity);
    b.Add("DlssNr", "Style", style);
    b.Add("DlssNr", "LocalStructure", localStructure);
    b.Add("DlssNr", "LocalTone", localTone);
    b.Add("DlssNr", "SkinStructure", skinStructure);
    b.Add("DlssNr", "AutoMask", autoMask);
    b.Add("DlssNr", "SkinProtection", skinProtection);
    b.Add("DlssNr", "SkinToneEnabled", skinToneEnabled);
    b.Add("DlssNr", "SkinDetail", skinDetail);
    b.Add("DlssNr", "SkinColour", skinColour);
    b.Add("DlssNr", "EnvironmentDetail", environmentDetail);
    b.Add("DlssNr", "EnvironmentColour", environmentColour);
    b.Add("DlssNr", "ShowSkinMask", showSkinMask);
    b.Add("DlssNr", "Passes", passes);
    b.Add("DlssNr", "UnlockPasses", unlockPasses);
    b.Add("DlssNr", "ReversibleMode", reversibleMode);
    b.Add("DlssNr", "ApplyModel", applyModel);
    b.Add("DlssNr", "HoldFrame", holdFrame);
    b.Add("DlssNr", "Pass2Preset", pass2Preset);
    b.Add("DlssNr", "Pass2Style", pass2Style);
    b.Add("DlssNr", "Pass2Intensity", pass2Intensity);
    b.Add("DlssNr", "Pass2LocalStructure", pass2LocalStructure);
    b.Add("DlssNr", "Pass2LocalTone", pass2LocalTone);
    b.Add("DlssNr", "Pass2SkinStructure", pass2SkinStructure);
    b.Add("DlssNr", "Pass2AutoMask", pass2AutoMask);
    b.Add("DlssNr", "Pass3Preset", pass3Preset);
    b.Add("DlssNr", "Pass3Style", pass3Style);
    b.Add("DlssNr", "Pass3Intensity", pass3Intensity);
    b.Add("DlssNr", "Pass3LocalStructure", pass3LocalStructure);
    b.Add("DlssNr", "Pass3LocalTone", pass3LocalTone);
    b.Add("DlssNr", "Pass3SkinStructure", pass3SkinStructure);
    b.Add("DlssNr", "Pass3AutoMask", pass3AutoMask);

    // Keys are generated; the binder keeps the pointers, so the strings need stable storage
    static std::deque<std::string> keys;
    for (int i = 0; i < kExtraPassCount; ++i)
    {
        auto& pass = extraPasses[i];
        auto add = [&](const char* suffix) -> const char*
        { return keys.emplace_back(std::format("Pass{}{}", i + 4, suffix)).c_str(); };

        b.Add("DlssNr", add("Style"), pass.style);
        b.Add("DlssNr", add("Intensity"), pass.intensity);
        b.Add("DlssNr", add("LocalStructure"), pass.structure);
        b.Add("DlssNr", add("LocalTone"), pass.tone);
        b.Add("DlssNr", add("SkinStructure"), pass.skin);
        b.Add("DlssNr", add("AutoMask"), pass.autoMask);
    }
}

void NrConfig::PostLoad()
{
    if (precision.value_or_default() != 4)
        precision = 0u;

    if (vitEvery.value_or_default() < 1u || vitEvery.value_or_default() > 2u)
        vitEvery = std::clamp<uint32_t>(vitEvery.value_or_default(), 1u, 2u);

    if (!std::isfinite(passFeedback.value_or_default()))
        passFeedback = 1.0f;
    else
        passFeedback = std::clamp(passFeedback.value_or_default(), 0.0f, 1.0f);
}
} // namespace ext
