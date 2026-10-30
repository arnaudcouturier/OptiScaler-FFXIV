#include "pch.h"

#include "ext/nr/NrConfig.h"
#include "ext/nr/NrFocusKey.h"
#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include "ext/nr/native/DlssNrNative.h"

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
    b.Add("DlssNr", "Runtime", runtime);
    b.Add("DlssNr", "Precision", precision);
    b.Add("DlssNr", "ResidualFGApproxCamera", residualFgApproxCamera);
    b.Add("DlssNr", "ToggleKey", toggleKey, true); // hex when positive (a key code), like the reference build
    b.Add("DlssNr", "ScaleUpKey", scaleUpKey, true); // hex when positive (a key code), like the toggle key
    b.Add("DlssNr", "ScaleDownKey", scaleDownKey, true); // hex when positive (a key code), like the toggle key
    b.Add("DlssNr", "TransferToggleKey", transferToggleKey, true); // hex when positive (a key code), like the toggle key
    b.Add("DlssNr", "DisableOnFocusLoss", disableOnFocusLoss);
    b.Add("DlssNr", "PauseOnFocusLoss", pauseOnFocusLoss);
    b.Add("DlssNr", "TransferStrength", transferStrength);
    b.Add("DlssNr", "ColourStrength", colourStrength);
    b.Add("DlssNr", "MaxRatio", maxRatio);
    b.Add("DlssNr", "Transfer", transfer);
    b.Add("DlssNr", "ClassicSharpen", classicSharpen);
    b.Add("DlssNr", "ClassicSharpenStrength", classicSharpenStrength);
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
    b.Add("DlssNr", "SpatialProfile", spatialProfile);
    b.Add("DlssNr", "FoveaWidth", foveaWidth);
    b.Add("DlssNr", "FoveaHeight", foveaHeight);
    b.Add("DlssNr", "FoveaFalloff", foveaFalloff);
    b.Add("DlssNr", "FoveaEdgeX", foveaEdgeX);
    b.Add("DlssNr", "FoveaEdgeY", foveaEdgeY);
    b.Add("DlssNr", "PassFeedback", passFeedback);
    b.Add("DlssNr", "VitEvery", vitEvery);
    b.Add("DlssNr", "VitAllowUnverified", vitAllowUnverified);
    b.Add("DlssNr", "VitCapture", vitCapture);
    b.Add("DlssNr", "VitFirstPassOnly", vitFirstPassOnly);
    b.Add("DlssNr", "WorkingScale", workingScale);
    b.Add("DlssNr", "DepthAwareResolve", depthAwareResolve);
    b.Add("DlssNr", "WorkingScaleX", workingScaleX);
    b.Add("DlssNr", "WorkingScaleY", workingScaleY);
    b.Add("DlssNr", "Governor", governor);
    b.Add("DlssNr", "GovernorTargetFps", governorTargetFps);
    b.Add("DlssNr", "GovernorProbe", governorProbe);
    b.AddEnum("DlssNr", "ScalingDownscaler", scalingDownscaler, Scaler::Count);
    b.Add("DlssNr", "AutoCapture", autoCapture);
    b.Add("DlssNr", "ReplayCapture", replayCapture);
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
    // QS: the key of another build for the same switch moves into DisableOnFocusLoss once and is cleared (see NrFocusKey.h).
    nr::MigrateFocusAlias(disableOnFocusLoss, pauseOnFocusLoss);

    // The hot-path seams of the exposure scan (ext::restrack::NoteUav/NoteResourceCreated) test this feature bit; it
    // stays set for the session once NR was enabled (EvalBefore latches it for a runtime enable).
    if (enabled.value_or_default())
        SetActive(F::Nr, true);

    if (precision.value_or_default() != 4)
        precision = 0u;

    // The runtime creates its CUDA modules before the first evaluation, so the capture must know at load time.
    DlssNrNative::SetCapture(vitCapture.value_or_default());

    if (vitEvery.value_or_default() < 1u || vitEvery.value_or_default() > 2u)
        vitEvery = std::clamp<uint32_t>(vitEvery.value_or_default(), 1u, 2u);

    if (!std::isfinite(passFeedback.value_or_default()))
        passFeedback = 1.0f;
    else
        passFeedback = std::clamp(passFeedback.value_or_default(), 0.0f, 1.0f);
    // Like [DLSSG] AutoMFGTarget: 0 (auto) or a frame rate, anything else means unset.
    if (governorTargetFps.has_value() && (!std::isfinite(governorTargetFps.value()) || governorTargetFps.value() < 0.0f ||
                                          governorTargetFps.value() > 1000.0f))
        governorTargetFps.reset();

    // Off keeps the resolve's own output, so an uninterpretable value fails open to the upstream default strength, clamped.
    if (!std::isfinite(classicSharpenStrength.value_or_default()))
        classicSharpenStrength = 0.4f;
    else
        classicSharpenStrength = std::clamp(classicSharpenStrength.value_or_default(), 0.0f, 1.0f);
}
} // namespace ext
