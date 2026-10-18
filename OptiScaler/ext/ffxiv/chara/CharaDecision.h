#pragma once
// Pure draw classifier of the character preview feature: facts about one draw call in, what the draw is out. Nothing here
// touches D3D, so every rule is tested exhaustively. The dispatcher (CharaDispatch.cpp) gathers the facts.
//
// A preview render is one cycle of: depth prepass -> G-buffer draws -> ... -> finisher (the final composition draw). The
// images it uses are the entries of the renderer's table (CharaTable.h): 0..7 outputs, 8..12 G-buffer, 13..17 transparent
// G-buffer, 18..19 depth candidates.

#include <cstddef>
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
    uint32_t targetFormat = 0;  // DXGI format of render target 0's texture
    uint32_t rtvFormat = 0;     // DXGI format of render target 0's view (log only: sRGB or typeless views differ from the texture)
    bool viewportMatchesTarget = false; // viewport 0 has the texture's width and height

    // For a UI shader (psUi or psEquipDisplay): the table entry of the first bound pixel-shader resource that is a preview
    // image, -1 when none. A UI draw is a consumer only when it samples a preview image.
    int srvEntry = kEntryNone;

    // Fallback finisher test for a shader created before the hooks (no tag): a pass is active, the target is one of the
    // outputs 0..7 at the pass size in B8G8R8A8, and any bound shader resource is table entry 8.
    bool anySrvIsEntry8 = false;

    // True once a pixel shader with the finisher tag has been created in this session. The shape test above exists for a
    // finisher shader created before the hooks (which therefore never carried the tag); when the tagged shader is known, a
    // draw that only has the finisher's shape is some other pass of the preview renderer (the second in-game test saw one
    // that writes the same output with a depth test) and must not count as a finisher.
    bool finisherTagKnown = false;

    // Pass
    bool passActive = false;
    uint32_t passWidth = 0;
    uint32_t passHeight = 0;
};

inline bool IsPrepass(const DrawFacts& f)
{
    return !f.rtv0Present && f.dsvPresent && (f.dsvEntry == 18 || f.dsvEntry == 19) && f.vsCb0Bytes >= kCameraBlockBytes;
}

inline bool IsOutputEntry(int entry) { return entry >= 0 && entry <= 7; }

// A draw with the finisher shader that does not render into one of the preview renderer's outputs is not a preview draw: the
// shader is used elsewhere too (the first in-game test saw it drawing into a 960x540 target of the main scene). Such a draw
// must neither count as a rejected finisher nor end a pass that is running.
inline bool IsOffTableFinisher(const DrawFacts& f) { return f.psFinisher && !IsOutputEntry(f.rtv0Entry); }

// An untagged draw that looks like the finisher: a pass is active, the target is an output at the pass size in B8G8R8A8 and
// the draw samples G-buffer entry 8.
inline bool HasFinisherShape(const DrawFacts& f)
{
    return !f.psFinisher && f.passActive && f.rtv0Present && f.rtv0Entry >= 0 && f.rtv0Entry <= 7 &&
           f.targetWidth == f.passWidth && f.targetHeight == f.passHeight && f.targetFormat == kFormatB8G8R8A8Unorm &&
           f.anySrvIsEntry8;
}

// A draw that has the finisher's shape but is not treated as one because the tagged finisher shader is known.
inline bool IsShapeOnlyFinisher(const DrawFacts& f) { return f.finisherTagKnown && HasFinisherShape(f); }

inline bool IsFinisher(const DrawFacts& f)
{
    if (f.psFinisher)
        return IsOutputEntry(f.rtv0Entry);
    return !f.finisherTagKnown && HasFinisherShape(f);
}

// True when a G-buffer draw (or a prepass) is one the module can follow: one sample, at most 4096 pixels each way, and
// the viewport covers the texture exactly. Anything else leaves the draw native.
inline bool GeometryAccepted(const DrawFacts& f)
{
    return f.samples == 1 && f.targetWidth <= kMaxGeometryExtent && f.targetHeight <= kMaxGeometryExtent &&
           f.viewportMatchesTarget;
}

// The dispatcher's cheapest gate, evaluated before any state of the draw is read (about ten COM calls): a draw can only be a preview
// draw when the preview renderer's table holds a valid image. A prepass needs a depth candidate, a G-buffer draw an entry 8/13, a
// finisher an output entry, a consumer a preview image among its shader resources; with an empty table none of them exists, so every
// draw classifies as Ignore (tested exhaustively). The debug aids (probe, flood) look at draws without the table and keep the full path.
constexpr bool ClassificationNeeded(bool tableHasValidEntry, bool debugAidsLive) { return tableHasValidEntry || debugAidsLive; }

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

// The first condition of the check that fails, in the order the conditions are listed above. The names are what the log prints.
enum class FinisherVerdict : uint8_t
{
    Accepted,
    NoRenderTarget,
    DepthOrStencilTest,
    NoPass,
    SizeMismatch, // the target is not the size of the pass
    FormatMismatch,
    ViewportOrigin,
    ViewportSize,
    Count,
};

inline constexpr size_t kFinisherVerdicts = static_cast<size_t>(FinisherVerdict::Count);

inline constexpr const char* FinisherVerdictName(FinisherVerdict v)
{
    switch (v)
    {
    case FinisherVerdict::Accepted: return "accepted";
    case FinisherVerdict::NoRenderTarget: return "no render target";
    case FinisherVerdict::DepthOrStencilTest: return "depth or stencil test on";
    case FinisherVerdict::NoPass: return "no pass active";
    case FinisherVerdict::SizeMismatch: return "target size differs from the pass";
    case FinisherVerdict::FormatMismatch: return "target format is not B8G8R8A8";
    case FinisherVerdict::ViewportOrigin: return "viewport origin";
    case FinisherVerdict::ViewportSize: return "viewport size";
    default: return "?";
    }
}

inline FinisherVerdict CheckFinisher(const FinisherCheck& c)
{
    if (!c.rtv0Present)
        return FinisherVerdict::NoRenderTarget;
    if (c.depthOrStencilTest)
        return FinisherVerdict::DepthOrStencilTest;
    if (!c.passActive)
        return FinisherVerdict::NoPass;
    if (c.targetWidth != c.passWidth || c.targetHeight != c.passHeight)
        return FinisherVerdict::SizeMismatch;
    if (c.targetFormat != kFormatB8G8R8A8Unorm)
        return FinisherVerdict::FormatMismatch;
    if (!(c.viewportX >= 0.0f && c.viewportX <= 0.5f && c.viewportY >= 0.0f && c.viewportY <= 0.5f))
        return FinisherVerdict::ViewportOrigin;
    if (c.viewportWidth != static_cast<float>(c.targetWidth) || c.viewportHeight != static_cast<float>(c.targetHeight))
        return FinisherVerdict::ViewportSize;
    return FinisherVerdict::Accepted;
}

inline bool FinisherAccepted(const FinisherCheck& c) { return CheckFinisher(c) == FinisherVerdict::Accepted; }

// The render a pass belongs to: starts at the first accepted geometry draw while no pass is active (or the previous render
// finished), ends at a finisher that fails the check or at the end of the frame.
//
// One deliberate deviation from the specification of Nickle's build: there a pass ends only at the bridge's present, so two renders
// of one presented frame share one pass (the same jitter phase, the guides and motion target of the first render). Here an accepted
// finisher closes the render ("finished"): its facts stay readable (a second finisher of the same render is still checked against
// them), but the next prepass or G-buffer draw starts a new pass and bumps the counter.
//
// The frame counter (bumped by the bridge's present callback, possibly from another thread and at any moment of a render) never
// ends a pass by itself: it is only compared when a prepass or G-buffer draw arrives. A render in progress therefore completes in
// its own pass, and the first geometry draw after the counter moved starts the next one.
struct PassState
{
    bool active = false;
    bool finished = false;     // an accepted finisher closed the render; the next geometry draw starts a new pass
    uint32_t counter = 0;      // passes started so far; also the jitter phase
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t startFrame = 0;   // the frame counter when the pass started
    uint64_t currentFrame = 0; // the newest frame counter seen (NoteFrame)

    // The draw thread learns the frame counter (any value that only grows). Nothing else changes.
    void NoteFrame(uint64_t frame) { currentFrame = frame; }

    // Returns true when this draw started a new pass.
    bool OnGeometry(const DrawFacts& f)
    {
        if (active && !finished && currentFrame == startFrame)
            return false;
        active = true;
        finished = false;
        ++counter;
        width = f.targetWidth;
        height = f.targetHeight;
        startFrame = currentFrame;
        return true;
    }

    void End()
    {
        active = false;
        finished = false;
    }

    // The outcome of a finisher draw: a finisher that fails the check drops the pass, an accepted one closes the render.
    void OnFinisher(FinisherVerdict verdict)
    {
        if (verdict != FinisherVerdict::Accepted)
            End();
        else if (active)
            finished = true;
    }
};

// A prepass or G-buffer draw belongs to the running pass only at the pass's size. A differently sized draw (a second preview
// render that begins before the first one finished) is neither jittered nor replayed: it runs natively.
constexpr bool GeometryFollowsPass(uint32_t passWidth, uint32_t passHeight, uint32_t targetWidth, uint32_t targetHeight)
{
    return passWidth != 0 && passHeight != 0 && targetWidth == passWidth && targetHeight == passHeight;
}
} // namespace ext::ffxiv::chara
