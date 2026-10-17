#pragma once
// Pure draw classifier of the character preview feature: facts about one draw call in, what the draw is out. Nothing here
// touches D3D, so every rule is tested exhaustively. The dispatcher (CharaDispatch.cpp) gathers the facts.
//
// A preview render is one cycle of: depth prepass -> G-buffer draws -> ... -> finisher (the final composition draw). The
// images it uses are the entries of the renderer's table (CharaTable.h): 0..7 outputs, 8..12 G-buffer, 13..17 transparent
// G-buffer, 18..19 depth candidates.

#include <cstdint>

namespace ext::ffxiv::chara
{
enum class DrawKind : uint8_t
{
    Ignore,   // not a preview draw: the native draw runs untouched
    Prepass,  // depth-only draw into a depth candidate, through the vertex shader that carries the camera block
    GBuffer,  // draw into the G-buffer (opaque entry 8 or transparent entry 13)
    Finisher, // the final composition draw of a preview render
    Consumer, // a UI draw that samples a preview image
};

inline constexpr const char* DrawKindName(DrawKind k)
{
    switch (k)
    {
    case DrawKind::Prepass: return "prepass";
    case DrawKind::GBuffer: return "GBuffer";
    case DrawKind::Finisher: return "finisher";
    case DrawKind::Consumer: return "consumer";
    default: return "ignore";
    }
}

inline constexpr int kEntryNone = -1;
inline constexpr uint32_t kCameraBlockBytes = 0x3b0;  // 59 float4: the vertex-shader constant buffer of a prepass draw
inline constexpr uint32_t kFormatB8G8R8A8Unorm = 87;  // DXGI_FORMAT_B8G8R8A8_UNORM
inline constexpr uint32_t kMaxGeometryExtent = 4096;  // geometry draws into larger targets are left alone

struct DrawFacts
{
    // Context
    bool immediate = true;  // false: a deferred context (left native)
    bool reentrant = false; // one of the feature's own draws, or a replay of a native draw by another module

    // Pixel shader roles (ext::ffxiv::chara::Role)
    bool psGeometry = false;
    bool psFinisher = false;
    bool psEquipDisplay = false;
    bool psUi = false; // one of the UI shaders that can show a preview image (the set of ext/hdr/FfxivHdrPreview.h)

    // Targets
    bool rtv0Present = false;
    int rtv0Entry = kEntryNone; // table entry of render target 0's texture (pointer and size), -1 when none
    bool dsvPresent = false;
    int dsvEntry = kEntryNone;  // table entry of the depth texture
    uint32_t vsCb0Bytes = 0;    // byte width of vertex-shader constant buffer 0 (only read for prepass candidates)
    uint32_t samples = 1;       // sample count of the render target (or the depth texture when there is no render target)
    uint32_t targetWidth = 0;   // size of that texture
    uint32_t targetHeight = 0;
    uint32_t targetFormat = 0;  // DXGI format of render target 0
    bool viewportMatchesTarget = false; // viewport 0 has the texture's width and height

    // For a UI shader (psUi or psEquipDisplay): the table entry of the first bound pixel-shader resource that is a preview
    // image, -1 when none. A UI draw is a consumer only when it samples a preview image.
    int srvEntry = kEntryNone;

    // Fallback finisher test for a shader created before the hooks (no tag): a pass is active, the target is one of the
    // outputs 0..7 at the pass size in B8G8R8A8, and any bound shader resource is table entry 8.
    bool anySrvIsEntry8 = false;

    // Pass
    bool passActive = false;
    uint32_t passWidth = 0;
    uint32_t passHeight = 0;
};

inline bool IsPrepass(const DrawFacts& f)
{
    return !f.rtv0Present && f.dsvPresent && (f.dsvEntry == 18 || f.dsvEntry == 19) && f.vsCb0Bytes >= kCameraBlockBytes;
}

inline bool IsFinisher(const DrawFacts& f)
{
    if (f.psFinisher)
        return true;
    return f.passActive && f.rtv0Present && f.rtv0Entry >= 0 && f.rtv0Entry <= 7 && f.targetWidth == f.passWidth &&
           f.targetHeight == f.passHeight && f.targetFormat == kFormatB8G8R8A8Unorm && f.anySrvIsEntry8;
}

// True when a G-buffer draw (or a prepass) is one the module can follow: one sample, at most 4096 pixels each way, and
// the viewport covers the texture exactly. Anything else leaves the draw native.
inline bool GeometryAccepted(const DrawFacts& f)
{
    return f.samples == 1 && f.targetWidth <= kMaxGeometryExtent && f.targetHeight <= kMaxGeometryExtent &&
           f.viewportMatchesTarget;
}

// Classification of one draw. Everything not listed is Ignore.
inline DrawKind Classify(const DrawFacts& f)
{
    if (!f.immediate || f.reentrant)
        return DrawKind::Ignore;

    const bool prepass = IsPrepass(f);
    const bool gbuffer = f.rtv0Present && (f.rtv0Entry == 8 || f.rtv0Entry == 13) && f.dsvPresent && f.samples == 1;
    const bool finisher = IsFinisher(f);
    const bool ui = (f.psUi || f.psEquipDisplay) && f.srvEntry >= 0;

    if (!(prepass || gbuffer || finisher || ui))
        return DrawKind::Ignore;

    // Geometry first: a prepass or G-buffer draw that the module cannot follow is native, not a consumer.
    if (prepass || gbuffer)
    {
        if (!GeometryAccepted(f))
            return DrawKind::Ignore;
        return prepass ? DrawKind::Prepass : DrawKind::GBuffer;
    }
    if (finisher)
        return DrawKind::Finisher;
    return DrawKind::Consumer;
}

// A finisher draw only completes a render when, after the native draw ran: render target 0 exists, no depth or stencil
// test is on, a pass is active at the target's size, the format is B8G8R8A8 and the viewport started within half a pixel
// of the corner and covers the texture. Otherwise the pass is dropped (counted as a fallback).
struct FinisherCheck
{
    bool rtv0Present = false;
    bool depthOrStencilTest = false;
    bool passActive = false;
    uint32_t targetWidth = 0, targetHeight = 0;
    uint32_t passWidth = 0, passHeight = 0;
    uint32_t targetFormat = 0;
    float viewportX = 0.0f, viewportY = 0.0f, viewportWidth = 0.0f, viewportHeight = 0.0f;
};

inline bool FinisherAccepted(const FinisherCheck& c)
{
    return c.rtv0Present && !c.depthOrStencilTest && c.passActive && c.targetWidth == c.passWidth &&
           c.targetHeight == c.passHeight && c.targetFormat == kFormatB8G8R8A8Unorm && c.viewportX >= 0.0f &&
           c.viewportX <= 0.5f && c.viewportY >= 0.0f && c.viewportY <= 0.5f &&
           c.viewportWidth == static_cast<float>(c.targetWidth) && c.viewportHeight == static_cast<float>(c.targetHeight);
}

// The render a pass belongs to: starts at the first accepted geometry draw while no pass is active, ends at a finisher
// that fails the check or at the end of the frame.
struct PassState
{
    bool active = false;
    uint32_t counter = 0; // passes started so far; also the jitter phase
    uint32_t width = 0;
    uint32_t height = 0;

    // Returns true when this draw started a new pass.
    bool OnGeometry(const DrawFacts& f)
    {
        if (active)
            return false;
        active = true;
        ++counter;
        width = f.targetWidth;
        height = f.targetHeight;
        return true;
    }

    void End() { active = false; }
};
} // namespace ext::ffxiv::chara
