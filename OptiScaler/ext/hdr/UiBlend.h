#pragma once
// Which native blend states the HDR transmittance mask can replay, as one pure function (D3D11 types only, no OptiScaler
// includes). It is the single source of truth for FfxivHdrPreview::BeforeDraw (the refusal decision) and for the UI draw
// census of the boundary diagnostics (ext/ffxiv/ui), so the census cannot answer a different question than the mask asks.
//
// The decision is the one BeforeDraw always made: a draw is refused when alpha-to-coverage is on, when the colour write mask is
// not exactly RGB(+A), or when the blend is enabled with an equation other than ADD of SRC_ALPHA or ONE over INV_SRC_ALPHA.
// "Opaque" (blend off, or ADD with a ZERO destination factor) is replayable. No blend state bound means the D3D11 default
// (blend off, full write mask).

#include <d3d11.h>

#include <cstdint>

namespace ext::hdr::uiblend
{
// What the blend equation is, regardless of the other state bits.
enum class Kind : uint8_t
{
    Opaque,   // blend off, or ADD with a ZERO destination factor
    Alpha,    // ADD, source SRC_ALPHA or ONE, destination INV_SRC_ALPHA (straight and premultiplied alpha)
    Additive, // ADD with a ONE destination factor (glows)
    Other,    // anything else
};

// Why a draw cannot be replayed (the first reason found, in the order of the declaration); None means it can.
enum class Refusal : uint8_t
{
    None,
    AlphaToCoverage,
    WriteMask,
    Additive,
    OtherBlend,
};
inline constexpr unsigned kRefusalCount = 5;

struct Verdict
{
    Kind kind = Kind::Opaque;
    Refusal refusal = Refusal::None;

    bool Opaque() const { return kind == Kind::Opaque; }
    bool Replayable() const { return refusal == Refusal::None; }
};

inline const char* RefusalName(Refusal r)
{
    switch (r)
    {
    case Refusal::AlphaToCoverage: return "alpha_to_coverage";
    case Refusal::WriteMask: return "write_mask";
    case Refusal::Additive: return "additive";
    case Refusal::OtherBlend: return "other_blend";
    default: return "none";
    }
}

// `state` is the bound blend state's description, or null when none is bound (the D3D11 default).
inline Verdict Classify(const D3D11_BLEND_DESC* state)
{
    D3D11_BLEND_DESC bd {};
    if (state)
        bd = *state;
    else
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    const auto& rt = bd.RenderTarget[0];

    const bool opaque = !rt.BlendEnable || (rt.BlendOp == D3D11_BLEND_OP_ADD && rt.DestBlend == D3D11_BLEND_ZERO);
    const bool standard = rt.BlendOp == D3D11_BLEND_OP_ADD && (rt.SrcBlend == D3D11_BLEND_SRC_ALPHA || rt.SrcBlend == D3D11_BLEND_ONE) &&
                          rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA;

    Verdict v;
    if (opaque)
        v.kind = Kind::Opaque;
    else if (standard)
        v.kind = Kind::Alpha;
    else if (rt.BlendOp == D3D11_BLEND_OP_ADD && rt.DestBlend == D3D11_BLEND_ONE)
        v.kind = Kind::Additive;
    else
        v.kind = Kind::Other;

    if (bd.AlphaToCoverageEnable)
        v.refusal = Refusal::AlphaToCoverage;
    else if ((rt.RenderTargetWriteMask & 7) != 7)
        v.refusal = Refusal::WriteMask;
    else if (v.kind == Kind::Additive)
        v.refusal = Refusal::Additive;
    else if (v.kind == Kind::Other)
        v.refusal = Refusal::OtherBlend;
    return v;
}
} // namespace ext::hdr::uiblend
