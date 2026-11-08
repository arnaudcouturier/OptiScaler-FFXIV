#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"

#include <algorithm>
#include <format>
#include <sstream>

namespace ext
{
namespace
{
// Registered at static init, read on the thread that loads the config: plain storage is enough, and it must not allocate (this can run inside the
// first Config::Instance() call).
KeysLoadedHook g_keysLoadedHooks[kMaxKeysLoadedHooks] {};
std::size_t g_keysLoadedHookCount = 0;
} // namespace

void AddKeysLoadedHook(KeysLoadedHook hook)
{
    if (hook == nullptr || g_keysLoadedHookCount >= kMaxKeysLoadedHooks)
        return;
    g_keysLoadedHooks[g_keysLoadedHookCount++] = hook;
}
} // namespace ext

namespace ext::config
{
namespace detail
{
std::optional<std::string> ReadRaw(CSimpleIniA& ini, const char* section, const char* key, bool lowercase)
{
    std::string value = ini.GetValue(section, key, "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "auto")
        return std::nullopt;

    return lowercase ? lower : value;
}

std::optional<bool> ParseBool(const std::optional<std::string>& raw)
{
    if (raw == "true")
        return true;
    if (raw == "false")
        return false;

    return std::nullopt;
}

std::optional<int64_t> ParseInt(const std::optional<std::string>& raw)
{
    if (!raw)
        return std::nullopt;

    const std::string& s = *raw;

    try
    {
        size_t idx = 0;
        int64_t result;

        // same hex prefix detection as upstream readInt
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            result = std::stoll(s, &idx, 16);
        else
            result = std::stoll(s, &idx, 10);

        // whole string has to be consumed
        return idx == s.size() ? std::optional<int64_t>(result) : std::nullopt;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<float> ParseFloat(const std::optional<std::string>& raw)
{
    if (!raw)
        return std::nullopt;

    std::istringstream iss(*raw);
    float result;

    if ((iss >> result) && iss.eof())
        return result;

    return std::nullopt;
}

std::string FormatBool(std::optional<bool> v)
{
    if (!v)
        return "auto";

    return *v ? "true" : "false";
}

std::string FormatInt(std::optional<int64_t> v, bool hex)
{
    if (!v)
        return "auto";

    // hex only for positive values: key codes are saved as 0x.., "unbound" (-1) stays decimal
    if (hex && *v > 0)
        return std::format("{:#x}", *v);

    return std::to_string(*v);
}

std::string FormatFloat(std::optional<float> v)
{
    if (!v)
        return "auto";

    return std::to_string(*v);
}

std::string WideToUtf8(const std::wstring& w) { return wstring_to_string(w); }

std::wstring Utf8ToWide(const std::string& s) { return string_to_wstring(s); }
} // namespace detail

namespace
{
const std::vector<Binding>& Table()
{
    static const std::vector<Binding> table = []
    {
        Binder b;
        Cfg().Bind(b);
        return b.Table();
    }();

    return table;
}
} // namespace

void Load(CSimpleIniA& ini)
{
    for (const auto& b : Table())
        b.load(b, ini);

    Cfg().PostLoad();
}

void Save(CSimpleIniA& ini)
{
    for (const auto& b : Table())
        b.save(b, ini);

    Cfg().hdr.Retire(ini);

    if (ext::ExtConfig::updateCheckOnSaved != nullptr)
        ext::ExtConfig::updateCheckOnSaved(ini);
}

void SaveXeFG(CSimpleIniA& ini)
{
    for (const auto& b : Table())
    {
        if (strcmp(b.section, "XeFG") == 0)
            b.save(b, ini);
    }
}
} // namespace ext::config

namespace ext
{
void FfxivConfig::Bind(config::Binder& b)
{
    b.Add("FFXIV", "JitterProbe", jitterProbe);
    b.Add("FFXIV", "JitterCorrection", jitterCorrection);
    b.Add("FFXIV", "JitterScaleOverride", jitterScaleOverride);
    b.Add("FFXIV", "JitterSource", jitterSource);
    b.Add("FFXIV", "FfxivLegacyGameBuild", legacyGameBuild);
    b.Add("FFXIV", "FfxivUnknownBuildHooks", unknownBuildHooks);
    b.Add("FFXIV", "FfxivForceUnknownBuild", forceUnknownBuild);
    b.Add("FFXIV", "AutomaticReShadeDepth", automaticReShadeDepth);
    b.Add("FFXIV", "DrawDiagnostics", drawDiagnostics);
    b.Add("FFXIV", "DrawDiagnosticsFrames", drawDiagnosticsFrames);
    b.Add("FFXIV", "DrawDiagnosticsDelaySeconds", drawDiagnosticsDelaySeconds);
    b.Add("FFXIV", "DrawDiagnosticsKey", drawDiagnosticsKey);
    b.Add("FFXIV", "DrawDiagnosticsDump", drawDiagnosticsDump);
    b.Add("FFXIV", "DrawDiagnosticsDumpBudgetMiB", drawDiagnosticsDumpBudgetMiB);
    b.Add("FFXIV", "VfxMaskProbe", vfxMaskProbe);
    b.Add("FFXIV", "ContactShadows", contactShadows);
    b.Add("FFXIV", "ContactShadowsLength", contactShadowsLength);
    b.Add("FFXIV", "ContactShadowsStrength", contactShadowsStrength);
    b.Add("FFXIV", "ContactShadowsSteps", contactShadowsSteps);
    b.Add("FFXIV", "ContactShadowsSunSpace", contactShadowsSunSpace);
    b.Add("FFXIV", "ContactShadowsSunSign", contactShadowsSunSign);
    b.Add("FFXIV", "ContactShadowsDump", contactShadowsDump);
    b.Add("FFXIV", "ShadowMapping", shadowMapping);           // ext: shadow mapping (one hot-path bit, ext/ExtFeatures.h)
    b.Add("FFXIV", "ShadowLift", shadowLift);                 // ext: the mask lift, read by ShadowsDispatch.cpp
    b.Add("FFXIV", "ShadowEdgeSoftness", shadowEdgeSoftness); // ext: the tent's reach, read by ShadowsDispatch.cpp
    b.Add("FFXIV", "NightAmbientLift", nightAmbientLift);     // ext: the ambient lift, read by ShadowsDispatch.cpp
    b.Add("FFXIV", "NightCurveToggle", nightCurveToggle);     // ext: the curve toggle, read by ShadowsDispatch.cpp
    b.Add("FFXIV", "EnginePassOverrides", enginePassOverrides);
    b.Add("FFXIV", "AmbientSkyVisibilityScale", ambientSkyVisibilityScale);
    b.Add("FFXIV", "WetnessOverride", wetnessOverride);
    b.Add("FFXIV", "CharaAmbient", charaAmbient);           // ext: the lighting tab's chara ambient (one hot-path bit, ext/ExtFeatures.h)
    b.Add("FFXIV", "CharaAmbientLift", charaAmbientLift);    // ext: the block's reciprocal-scale lift, read by CharaAmbientApply.cpp
    b.Add("FFXIV", "AmbientOcclusion", ambientOcclusion);
    b.Add("FFXIV", "AmbientOcclusionRadius", ambientOcclusionRadius);
    b.Add("FFXIV", "AmbientOcclusionStrength", ambientOcclusionStrength);
    b.Add("FFXIV", "AmbientOcclusionBlend", ambientOcclusionBlend);
    b.Add("FFXIV", "AmbientOcclusionQuality", ambientOcclusionQuality);
    b.Add("FFXIV", "Puddles", puddles);
    b.Add("FFXIV", "PuddlesCoverage", puddlesCoverage);
    b.Add("FFXIV", "PuddlesScale", puddlesScale);
    b.Add("FFXIV", "PuddlesSoftness", puddlesSoftness);
    b.Add("FFXIV", "PuddlesGlossTarget", puddlesGlossTarget);
    b.Add("FFXIV", "PuddlesGlossChannel", puddlesGlossChannel);
    b.Add("FFXIV", "PuddlesGlossWet", puddlesGlossWet);
    b.Add("FFXIV", "PuddlesReflection", puddlesReflection);
    b.Add("FFXIV", "PuddlesReflectionStrength", puddlesReflectionStrength);
    b.Add("FFXIV", "PuddlesDarkening", puddlesDarkening);
    b.Add("FFXIV", "PuddlesRipple", puddlesRipple);
    b.Add("FFXIV", "PuddlesReflectionSteps", puddlesReflectionSteps);
    b.Add("FFXIV", "PuddlesReach", puddlesReach);
    b.Add("FFXIV", "LightShafts", lightShafts);
    b.Add("FFXIV", "LightShaftsIntensity", lightShaftsIntensity);
    b.Add("FFXIV", "LightShaftsDecay", lightShaftsDecay);
    b.Add("FFXIV", "LightShaftsSamples", lightShaftsSamples);
    b.Add("FFXIV", "LightShaftsDensity", lightShaftsDensity);
    b.Add("FFXIV", "LightShaftsColor", lightShaftsColor);
    b.Add("FFXIV", "GodraysVolumetric", godraysVolumetric);
    b.Add("FFXIV", "GodraysSteps", godraysSteps);
    b.Add("FFXIV", "GodraysResolution", godraysResolution);
    b.Add("FFXIV", "GodraysFogBoost", godraysFogBoost);
    b.Add("FFXIV", "GodraysHeightLift", godraysHeightLift);
    b.Add("FFXIV", "GodraysIntensity", godraysIntensity);
    b.Add("FFXIV", "GodraysColor", godraysColor);
    b.Add("FFXIV", "GodraysShadowSoftness", godraysShadowSoftness);
    b.Add("FFXIV", "GodraysJitter", godraysJitter);
    b.Add("FFXIV", "GodraysPhaseBoost", godraysPhaseBoost);
    b.Add("FFXIV", "GodraysDensity", godraysDensity);
    b.Add("FFXIV", "GodraysAnisotropy", godraysAnisotropy);
    b.Add("FFXIV", "Gi", gi);
    b.Add("FFXIV", "GiRays", giRays);
    b.Add("FFXIV", "GiSteps", giSteps);
    b.Add("FFXIV", "GiRange", giRange);
    b.Add("FFXIV", "GiStrength", giStrength);
    b.Add("FFXIV", "GiResolution", giResolution);
    b.Add("FFXIV", "DepthOfField", depthOfField);
    b.Add("FFXIV", "DofFocusMode", dofFocusMode);
    b.Add("FFXIV", "DofFocusDistance", dofFocusDistance);
    b.Add("FFXIV", "DofAperture", dofAperture);
    b.Add("FFXIV", "DofStrength", dofStrength);
    b.Add("FFXIV", "DofSteps", dofSteps);
    b.Add("FFXIV", "DofMode", dofMode);
    b.Add("FFXIV", "FlareCard", flareCard);            // ext: the lens flare's card (one hot-path bit, ext/ExtFeatures.h)
    b.Add("FFXIV", "FlareParts", flareParts);          // ext: the parts bitfield, read by FlareDraw.cpp
    b.Add("FFXIV", "FlareRingScale", flareRingScale);  // ext: the ring's colour, read by FlareDraw.cpp
    b.Add("FFXIV", "FlareSunScale", flareSunScale);    // ext: the sun sprite's colour, read by FlareDraw.cpp
    b.Add("FFXIV", "FlareGhostScale", flareGhostScale);// ext: the ghost chain's step, read by FlareDraw.cpp
    b.Add("FFXIV", "ScreenSpaceReflections", screenSpaceReflections);
    b.Add("FFXIV", "ScreenSpaceReflectionsDiagnostic", screenSpaceReflectionsDiagnostic);
    b.Add("FFXIV", "ScreenSpaceReflectionsSteps", screenSpaceReflectionsSteps);
    b.Add("FFXIV", "ScreenSpaceReflectionsThickness", screenSpaceReflectionsThickness);
    b.Add("FFXIV", "ScreenSpaceReflectionsBlend", screenSpaceReflectionsBlend);
    b.Add("FFXIV", "ScreenSpaceReflectionsFade", screenSpaceReflectionsFade);
    chara.Bind(b);
}

namespace ffxiv::ui
{
void UiConfig::Bind(config::Binder& b) { b.Add("HudLess", "Diagnostics", diagnostics); }
} // namespace ffxiv::ui

void UpscalerConfig::Bind(config::Binder& b)
{
    b.Add("QualityOverrides", "ForcePerfQuality", forcePerfQuality);
    b.Add("Hotfix", "RenderSizeFromWidthHeight", renderSizeFromWidthHeight);
    b.Add("Mipmap", "MipmapBiasAuto", mipBiasAuto);
    b.Add("Mipmap", "AutomaticFFXIV", mipBiasAutoAlias);
    b.Add("Mipmap", "MipmapBiasObserve", mipBiasObserve);
    b.Add("Mipmap", "MipmapBiasAutoOffset", mipBiasAutoOffset);
}

void ReShadeConfig::Bind(config::Binder& b)
{
    b.Add("HudLess", "ReShadeProbe", probe);
    b.Add("HudLess", "ReShadePresets", presets);
}

void WindowConfig::Bind(config::Binder& b) { b.Add("Window", "Mode", mode); }

void MenuStyleConfig::Bind(config::Binder& b) { b.Add("Menu", "Style", look); }

void NgxReleaseConfig::Bind(config::Binder& b) { b.Add("NgxRelease", "SkipReleaseSleep", skipReleaseSleep); }

void BridgeConfig::Bind(config::Binder& b) { b.Add("Bridge", "FlushAfterSignal", flushAfterSignal); }

void FramePacingConfig::Bind(config::Binder& b)
{
    b.Add("FramePacing", "Trace", trace);
    b.Add("FramePacing", "GpuTimers", gpuTimers);
    b.Add("FramePacing", "HiddenPresentDoNotWait", hiddenPresentDoNotWait);
    b.Add("FramePacing", "FgRecorder", fgRecorder);
    b.Add("FramePacing", "FgRecorderMarkKey", fgRecorderMarkKey, true); // hex when positive (a key code)
    b.Add("FramePacing", "TickProbe", tickProbe);
}

void StatusConfig::Bind(config::Binder& b) { b.Add("Status", "Enabled", fileEnabled); }

void ExtConfig::Bind(config::Binder& b)
{
    fg.Bind(b);
    ffxiv.Bind(b);
    ui.Bind(b);
    nr.Bind(b);
    hdr.Bind(b);
    hudless.Bind(b);
    upscaler.Bind(b);
    reshade.Bind(b);
    window.Bind(b);
    menuStyle.Bind(b);
    ngxRelease.Bind(b);
    bridge.Bind(b);
    framePacing.Bind(b);
    status.Bind(b);
}

void ExtConfig::PostLoad()
{
    if (updateCheckOnLoaded != nullptr)
        updateCheckOnLoaded();
    // Live-key mirrors (AddKeysLoadedHook): the ini has been read, so a module that must not read its key per frame updates its copy here.
    for (std::size_t i = 0; i < g_keysLoadedHookCount; ++i)
    {
        if (g_keysLoadedHooks[i] != nullptr)
            g_keysLoadedHooks[i]();
    }

    fg.PostLoad();
    nr.PostLoad();
    hdr.PostLoad();
    hudless.PostLoad();
    upscaler.PostLoad();
    window.PostLoad();
    ffxiv.PostLoad();
    bridge.PostLoad();
}

ExtConfig& Cfg()
{
    static ExtConfig cfg;
    return cfg;
}
} // namespace ext
