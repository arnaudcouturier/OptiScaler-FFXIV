#pragma once
// C7: maps the six draw/shader/layout observation callbacks between Companion's own CaptureCallbacks
// (ext/ffxiv/companion/Companion.h) and B6's independently-defined CompanionCallbacks
// (ext/ffxiv/lighting/FfxivLightingCapture.h). The two structs are deliberately kept distinct -- PORT-LOG
// "S7 CORE checkpoint": each slice stays independently buildable, with no cross-feature include on draw
// paths -- but must stay field-compatible. This generic template does the field-by-field copy once, for any
// pair of structs sharing that shape, so neither real header needs to see the other and a plain CPU test can
// exercise the mapping without pulling in d3d11.h/detours.h.
namespace ext::ffxiv::companion
{
template <class Dst, class Src>
inline Dst MapCaptureCallbacks(const Src& src)
{
    static_assert(sizeof(Dst) == sizeof(Src),
                  "capture-callback struct shape drifted between Companion and FfxivLightingCapture; update the field mapping");
    Dst dst {};
    dst.tagShader = src.tagShader;
    dst.tagPixel = src.tagPixel;
    dst.tagLayout = src.tagLayout;
    dst.observeDraw = src.observeDraw;
    dst.interceptVisibilityIndexed = src.interceptVisibilityIndexed;
    dst.aroundIndexed = src.aroundIndexed;
    return dst;
}
} // namespace ext::ffxiv::companion
