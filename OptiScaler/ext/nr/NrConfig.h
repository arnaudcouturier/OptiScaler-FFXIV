#pragma once
// NR config keys (docs/SEAMS.md section 3.4). Owner: A1-A12, B1-B9. Section [DlssNr], key names identical to the
// reference build. Dropped with their code: ProbeD3D11 (E2), ProxyProbe/UseProxy (E1), WhitePointFromExposure and the
// legacy ScanAnchorValue/ScanAnchorWhitePoint (migration only). AutoCapture is a real key (default false = the
// reference build, which never ran the capture it declared). Use: ext::Cfg().nr.enabled.value_or_default()

#include <Config.h>

#include <cstdint>
#include <string>

namespace ext::config
{
class Binder;
}

namespace ext
{
struct NrConfig
{
    // Master switch of the NR pass. Off by default: an undocumented feature driven directly through its snippet.
    CustomOptional<bool> enabled { false };
    // Run the NR pass on the upscaler's colour input at render resolution, immediately before SR. Off keeps the post-upscale placement.
    CustomOptional<bool> runBeforeSr { false };
    // Generate NR before SR, upscale its signed contribution with a private DLSS feature and apply it after the game's upscaler. Takes precedence over RunBeforeSR.
    CustomOptional<bool> deferredDlss { false };
    // Experimental current-frame bounds for the private PreSR contribution.
    CustomOptional<bool> preSrTrailGuard { false };
    CustomOptional<bool> splitFrameWork { false };
    CustomOptional<bool> residualFg { false };
    // Which NR model file runs: empty or "auto" = the newest installed (ext/nr/runtime), "legacy" = the file found by the old search, or a
    // folder name such as NR.310.9 (OptiScaler\NR.<version>\nvngx_dlssnr.dll). Switched live from the menu; not part of the saved NR presets.
    CustomOptional<std::string> runtime { std::string() };
    // 0 NVIDIA FP8 (default), 4 experimental NVFP4 hybrid (coerced to {0,4} on load).
    CustomOptional<uint32_t> precision { 0 };
    CustomOptional<bool> residualFgApproxCamera { false };
    // Toggles the pass in game. Unbound by default.
    CustomOptional<int> toggleKey { UnboundKey };
    // Steps the uniform model resolution by 5% per press in game. Unbound by default.
    CustomOptional<int> scaleUpKey { UnboundKey };
    CustomOptional<int> scaleDownKey { UnboundKey };
    // Flips the enlargement mode (Classic/Matched residual) in game. Unbound by default.
    CustomOptional<int> transferToggleKey { UnboundKey };
    // Experimental: pause the NR pass while the game window is not the foreground window or is minimised (the plain
    // upscaler output is shown), resume when focus returns. Does not change `enabled`. Decision logic: NrFocusGate.h.
    CustomOptional<bool> disableOnFocusLoss { false };
    // Another name for disableOnFocusLoss used by another build, read so that a copied ini keeps working. No menu control,
    // never read by the NR pass: PostLoad moves its value into disableOnFocusLoss (unless that is set) and clears it
    // (NrFocusKey.h). Declared without a default, like the other ini-only aliases, so an explicit value is kept until then.
    CustomOptional<bool, NoDefault> pauseOnFocusLoss;
    // How much of the model's edit reaches the frame (luminance part).
    CustomOptional<float> transferStrength { 1.0f };
    // How much of the model's colour shift reaches the frame.
    CustomOptional<float> colourStrength { 1.0f };
    // The most the pass may multiply or divide a pixel by.
    CustomOptional<float> maxRatio { 2.0f };
    // How a model that worked below the frame's size is brought back: 0 classic, 1 matched residual.
    CustomOptional<uint32_t> transfer { 1 };
    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified.
    CustomOptional<uint32_t> debugView { 0 };
    // 0 off, 1 side by side, 2 a wipe.
    CustomOptional<uint32_t> compare { 0 };
    CustomOptional<float> compareSplit { 0.5f };
    // Side by side only. 1 fits the whole frame, 2 fills the half and crops the sides.
    CustomOptional<float> compareZoom { 1.0f };
    // Which side the edited frame sits on.
    CustomOptional<bool> compareSwap { false };
    // Labels drawn onto the two sides of a comparison.
    CustomOptional<bool> compareTags { false };
    CustomOptional<float> tagScale { 1.5f };
    // Gamut-bound the edit between passes and blend it back toward the preceding input.
    CustomOptional<bool> spatialCompression { false };
    CustomOptional<float> spatialCenterX { 80.0f };
    CustomOptional<float> spatialCenterY { 80.0f };
    CustomOptional<float> spatialWorkX { 90.0f };
    CustomOptional<float> spatialWorkY { 90.0f };
    CustomOptional<float> spatialOffsetX { 0.0f };
    CustomOptional<float> spatialOffsetY { 0.0f };
    CustomOptional<float> spatialShiftX { 0.0f };
    CustomOptional<float> spatialShiftY { 0.0f };
    // Which profile the spatial switch drives: 0 peripheral (the compression curve above), 1 foveated (any other value is peripheral).
    CustomOptional<uint32_t> spatialProfile { 0 };
    // Foveated profile, in pixels of the output: the full-density centre, one falloff length, and the edge density per axis (0.25..1).
    CustomOptional<float> foveaWidth { 2400.0f };
    CustomOptional<float> foveaHeight { 1350.0f };
    CustomOptional<float> foveaFalloff { 400.0f };
    CustomOptional<float> foveaEdgeX { 0.5f };
    CustomOptional<float> foveaEdgeY { 0.5f };
    // Spatial feedback between passes (clamped to 0..1 on load).
    CustomOptional<float> passFeedback { 1.0f };
    // Clamped to 1..2 on load.
    CustomOptional<uint32_t> vitEvery { 1 };
    CustomOptional<bool> vitAllowUnverified { false };
    // Log-only capture of the first NR evaluations' CUDA launches (raw parameter bytes) and the ViT module blobs, for profiling a new
    // NR model file (docs/nr-vit-reuse-profile-runbook.md). Never changes a launch; needs to be set before the game starts.
    CustomOptional<bool> vitCapture { false };
    CustomOptional<bool> vitFirstPassOnly { true };
    // Fraction of the frame's resolution the model works at (only the model's contribution is computed small).
    CustomOptional<float> workingScale { 1.0f };
    // Weight the enlargement of a reduced model's answer by the depth under each filter tap, so it does not blend across silhouettes. DX12
    // only; inactive with spatial compression and at model resolution 100% or above.
    CustomOptional<bool> depthAwareResolve { false };
    // Per-axis model resolution (DX12 only; the Vulkan and AMD paths use WorkingScale alone). 0 follows WorkingScale, otherwise the scale
    // of that axis, 0.25..2. The model works at (x * width) by (y * height).
    CustomOptional<float> workingScaleX { 0.0f };
    CustomOptional<float> workingScaleY { 0.0f };
    // Downscaler for NR supersampling (working scale > 1).
    CustomOptional<Scaler> scalingDownscaler { Scaler::Lanczos3 };
    // Write one set of matched before/after frames per session after the capture delay (180 frames). SEAMS 3.4: real key, default false keeps the oracle behaviour (no capture).
    CustomOptional<bool> autoCapture { false };
    // Look for the exposure the game computed but never handed to the upscaler (developer override, no menu control).
    CustomOptional<bool> scanExposure { false };
    // 0 the paper white slider, 1 the exposure the game hands the upscaler, 2 scanned exposure.
    CustomOptional<uint32_t> whitePointSource { 1 };
    // Multiplies the (auto or manual) white point before the encode.
    CustomOptional<float> whitePointScale { 1.0f };
    // Trim on an exposure-derived white point, kept apart from the manual divisor.
    CustomOptional<float> whitePointTrim { 1.0f };
    // The scan's trim, kept apart from the exposure texture's.
    CustomOptional<float> scanTrim { 1.0f };
    // Multi-point anchor table, "scan:white;scan:white;..." ascending.
    CustomOptional<std::string> scanAnchors { std::string() };
    // Whether the scan's number rises or falls with the light.
    CustomOptional<bool> scanInverted { false };
    CustomOptional<bool> scanMeter { false };
    // FFXIV native DX11 scanned lighting: reset NR history on abrupt tone changes (needs WhitePointSource=2).
    CustomOptional<bool> lightingHistory { true };
    CustomOptional<uint32_t> preset { 0 };
    CustomOptional<float> intensity { 1.0f };
    // 0 default (standard), 1 natural, 2 cinematic.
    CustomOptional<uint32_t> style { 0 };
    CustomOptional<float> localStructure { 1.0f };
    CustomOptional<float> localTone { 1.0f };
    // -1 means follow local structure.
    CustomOptional<float> skinStructure { -1.0f };
    CustomOptional<bool> autoMask { true };
    // Optional final-composition filter, not NVIDIA's semantic auto mask.
    CustomOptional<bool> skinProtection { false };
    CustomOptional<bool> skinToneEnabled { true };
    CustomOptional<float> skinDetail { 1.0f };
    CustomOptional<float> skinColour { 1.0f };
    CustomOptional<float> environmentDetail { 1.0f };
    CustomOptional<float> environmentColour { 1.0f };
    CustomOptional<bool> showSkinMask { false };
    // Sequential model layers between one encode and one final composition (clamped in the consumer).
    CustomOptional<uint32_t> passes { 1 };
    CustomOptional<bool> unlockPasses { false };
    // RenoDX reversible proxy mode, 0 = soft-knee encode + our composition, 1/2 Neutwo composed/replace, 3/4 hybrid
    // composed/replace, 5 linear light + composed (the collaborator's mode: the normalised frame, clipped at white, no curve).
    CustomOptional<uint32_t> reversibleMode { 0 };
    // Off keeps the pass running but shows the clean upscaler frame.
    CustomOptional<bool> applyModel { true };
    // Freeze the NR pass's input so a live setting change re-renders the SAME frame.
    CustomOptional<bool> holdFrame { false };

    // Per-pass overrides for passes 2 and 3; an absent value inherits pass 1 (PassProfiles.h).
    CustomOptional<uint32_t, NoDefault> pass2Preset;
    CustomOptional<uint32_t, NoDefault> pass2Style;
    CustomOptional<float, NoDefault> pass2Intensity;
    CustomOptional<float, NoDefault> pass2LocalStructure;
    CustomOptional<float, NoDefault> pass2LocalTone;
    CustomOptional<float, NoDefault> pass2SkinStructure;
    CustomOptional<bool, NoDefault> pass2AutoMask;
    CustomOptional<uint32_t, NoDefault> pass3Preset;
    CustomOptional<uint32_t, NoDefault> pass3Style;
    CustomOptional<float, NoDefault> pass3Intensity;
    CustomOptional<float, NoDefault> pass3LocalStructure;
    CustomOptional<float, NoDefault> pass3LocalTone;
    CustomOptional<float, NoDefault> pass3SkinStructure;
    CustomOptional<bool, NoDefault> pass3AutoMask;

    // Passes 4..30 (Pass<N>Style/Intensity/LocalStructure/LocalTone/SkinStructure/AutoMask); passes 2 and 3 have
    // their own keys above.
    struct ExtraPass
    {
        CustomOptional<uint32_t, NoDefault> style;
        CustomOptional<float, NoDefault> intensity, structure, tone, skin;
        CustomOptional<bool, NoDefault> autoMask;
    };
    static constexpr int kExtraPassCount = 27;
    ExtraPass extraPasses[kExtraPassCount];

    void Bind(config::Binder& b);
    // Validation the reference build ran right after reading the keys (Precision, VitEvery, PassFeedback)
    void PostLoad();
};
} // namespace ext
