#pragma once
// Pure rules of the signed-motion replay (CharaMotion.cpp). The game's own motion output is an 8-bit render target and
// cannot hold signed vectors, so a G-buffer draw is issued a second time into a private float target that receives only
// the pixel shader's fourth colour output (SV_Target3). These rules decide, from facts about the bound state, whether
// that second draw is safe; nothing here touches D3D objects except plain descriptor structs.

#include <array>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <memory>
#include <unordered_set>

#include "ext/ffxiv/chara/CharaCameraRules.h"
#include "ext/ffxiv/chara/CharaDecision.h"

namespace ext::ffxiv::chara::motion
{
// ---- Replay decision ----------------------------------------------------------------------------------------------

struct ReplayFacts
{
    bool immediate = true;
    bool ownDraw = false;         // one of the feature's own draws, or a replay of a native draw by another module
    uint32_t openQueries = 0;     // occlusion / statistics / predicate queries that are open (a replay would count twice)
    bool replayAllowedByQueries = true; // false while queries opened before the module started cannot be excluded

    std::array<bool, 5> rtvBound {};   // render targets 0..4
    std::array<int, 5> rtvEntry { kEntryNone, kEntryNone, kEntryNone, kEntryNone, kEntryNone }; // table entry of each
    bool dsvBound = false;
    uint32_t rtv0Samples = 1;
    bool anyUav = false;               // an unordered access view is bound to the output merger
    bool anyStreamOut = false;         // a stream-output target is bound

    bool psGeometryTag = false;        // the pixel shader carries the geometry role
    uint32_t vsCb0Bytes = 0;           // byte width of vertex-shader constant buffer 0
    uint32_t psCb2Bytes = 0;           // byte width of pixel-shader constant buffer 2
    // Accept any constant buffer size that can hold the camera block (camera::SizeAccepted) instead of exactly the 59-float4 block:
    // a shader that reads the block as a view into a larger buffer has the G-buffer shape too. Off by default.
    bool wideShape = false;
};

enum class Skip : uint8_t
{
    None,
    NotImmediate,
    OwnDraw,
    OpenQuery,
    MissingTargets, // render target 0 or 4, or the depth target, is not bound
    NotOpaqueGBuffer, // render target 0 is not table entry 8
    MultiSample,
    UnorderedAccess,
    StreamOut,
    UnknownShader,  // untagged shader that does not have the G-buffer shape
    NoTarget,       // the private target could not be provided
    DepthStateFailed,
};

inline constexpr const char* SkipName(Skip s)
{
    switch (s)
    {
    case Skip::None: return "none";
    case Skip::NotImmediate: return "deferred context";
    case Skip::OwnDraw: return "own draw";
    case Skip::OpenQuery: return "open query";
    case Skip::MissingTargets: return "render targets 0/4 or depth missing";
    case Skip::NotOpaqueGBuffer: return "not the opaque G-buffer";
    case Skip::MultiSample: return "multisampled";
    case Skip::UnorderedAccess: return "UAV bound";
    case Skip::StreamOut: return "stream-out bound";
    case Skip::UnknownShader: return "unknown shader shape";
    case Skip::NoTarget: return "no motion target";
    case Skip::DepthStateFailed: return "depth state";
    }
    return "";
}

inline constexpr uint32_t kGBufferCbBytes = kCameraBlockBytes; // 59 float4

struct ReplayDecision
{
    bool replay = false;
    bool tagShader = false; // the shader was not tagged yet but has the G-buffer shape: tag it for the next draws
    Skip skip = Skip::None;
};

inline ReplayDecision DecideReplay(const ReplayFacts& f)
{
    ReplayDecision d;
    auto skip = [&](Skip s) {
        d.skip = s;
        return d;
    };

    if (f.ownDraw)
        return skip(Skip::OwnDraw);
    if (!f.immediate)
        return skip(Skip::NotImmediate);
    if (f.openQueries != 0 || !f.replayAllowedByQueries)
        return skip(Skip::OpenQuery);
    if (!f.rtvBound[0] || !f.rtvBound[4] || !f.dsvBound)
        return skip(Skip::MissingTargets);
    if (f.rtvEntry[0] != 8)
        return skip(Skip::NotOpaqueGBuffer);
    if (f.rtv0Samples != 1)
        return skip(Skip::MultiSample);
    if (f.anyUav)
        return skip(Skip::UnorderedAccess);
    if (f.anyStreamOut)
        return skip(Skip::StreamOut);

    if (f.psGeometryTag)
    {
        // A shader already known as a G-buffer shader is replayed without looking at its constant buffers.
        d.replay = true;
        return d;
    }

    // Untagged: accept only the exact G-buffer shape (targets 0..4 are table entries 8..12 in order, vertex cb0 and
    // pixel cb2 are the 59-vector camera block) and remember the shader.
    for (size_t i = 0; i < 5; ++i)
    {
        if (!f.rtvBound[i] || f.rtvEntry[i] != 8 + static_cast<int>(i))
            return skip(Skip::UnknownShader);
    }
    // The camera block is 59 float4 (944 bytes) when the shader reads the whole buffer; a shader may read it as a view into a larger
    // one. `wideShape` accepts any size the camera snapshot accepts, for both buffers, and keeps the render target requirement above:
    // only draws of the preview renderer's own G-buffer qualify either way.
    const bool cameraBlock = f.wideShape ? (camera::SizeAccepted(f.vsCb0Bytes) && camera::SizeAccepted(f.psCb2Bytes))
                                         : (f.vsCb0Bytes == kGBufferCbBytes && f.psCb2Bytes == kGBufferCbBytes);
    if (!cameraBlock)
        return skip(Skip::UnknownShader);
    d.replay = true;
    // Only the exact shape proves that the shader reads the camera block, so only that one tags the shader for the session: a wider size
    // may be a buffer that merely fits, and a tag taken from it would follow that shader for every draw it makes.
    d.tagShader = f.vsCb0Bytes == kGBufferCbBytes && f.psCb2Bytes == kGBufferCbBytes;
    return d;
}

// Whether a G-buffer draw gives the guides their inputs (camera copy, depth view, depth convention). A shader already known as a G-buffer
// shader carries the geometry role, so its draws do. A draw the replay accepted is proof as well: the wider shape is replayed without being
// tagged (only the exact 944-byte block proves that the shader reads the camera block), so without this the camera of a pass whose opaque
// draws are all of the wider shape would never be copied and the pass could not finish.
constexpr bool CapturesGuides(bool geometryRole, bool replayed) { return geometryRole || replayed; }

// ---- Depth convention and the replay depth state -------------------------------------------------------------------

// The scene uses a reversed depth buffer when the bound depth state compares with GREATER or GREATER_EQUAL.
inline bool IsReversedZ(bool haveState, D3D11_COMPARISON_FUNC func)
{
    return haveState && (func == D3D11_COMPARISON_GREATER || func == D3D11_COMPARISON_GREATER_EQUAL);
}

// What one bound depth-stencil state says about the convention. No state bound is the default state (depth test on, LESS). A state
// with the depth test off, or a function that does not order depths (ALWAYS, NEVER, EQUAL, NOT_EQUAL), says nothing: the draw that
// binds it (a fullscreen or masking draw) must not decide the convention.
enum class DepthConvention : uint8_t
{
    Unknown,
    Normal,
    Reversed,
};

inline DepthConvention ConventionOf(bool haveState, bool depthEnable, D3D11_COMPARISON_FUNC func)
{
    if (!haveState)
        return DepthConvention::Normal;
    if (!depthEnable)
        return DepthConvention::Unknown;
    switch (func)
    {
    case D3D11_COMPARISON_GREATER:
    case D3D11_COMPARISON_GREATER_EQUAL: return DepthConvention::Reversed;
    case D3D11_COMPARISON_LESS:
    case D3D11_COMPARISON_LESS_EQUAL: return DepthConvention::Normal;
    default: return DepthConvention::Unknown;
    }
}

// The convention of the scene: the last draw that said something decides, a draw that says nothing keeps the earlier answer.
class DepthConventionMemo
{
  public:
    void Observe(DepthConvention c)
    {
        if (c != DepthConvention::Unknown)
            _known = c;
    }
    bool Reversed() const { return _known == DepthConvention::Reversed; }
    bool Known() const { return _known != DepthConvention::Unknown; }
    void Reset() { _known = DepthConvention::Unknown; }

  private:
    DepthConvention _known = DepthConvention::Unknown;
};

// The depth state of the replay: the game's own state with depth and stencil writes switched off, so the replay only
// reads the already final prepass depth. With no state bound the default test is used (depth on, LESS).
inline D3D11_DEPTH_STENCIL_DESC ReplayDepthDesc(bool haveSource, const D3D11_DEPTH_STENCIL_DESC& source)
{
    D3D11_DEPTH_STENCIL_DESC d {};
    if (!haveSource)
    {
        d.DepthEnable = TRUE;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilReadMask = 0xFF;
        d.StencilWriteMask = 0;
        d.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
        d.BackFace = d.FrontFace;
    }
    else
        d = source;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilWriteMask = 0;
    return d;
}

inline bool SameDepthDesc(const D3D11_DEPTH_STENCIL_DESC& a, const D3D11_DEPTH_STENCIL_DESC& b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// Cache of replay depth states, keyed by the game's state object. A game state object can die and its address be reused
// for a different description, so a hit also compares the description the copy was made from.
template <class Value, size_t N> class DepthStateCache
{
  public:
    struct Entry
    {
        uintptr_t key = 0;
        D3D11_DEPTH_STENCIL_DESC source {};
        Value value {};
    };

    // The cached value for (key, source), or nullptr when absent or made from a different description.
    Value* Find(uintptr_t key, const D3D11_DEPTH_STENCIL_DESC& source)
    {
        for (auto& e : _entries)
        {
            if (e.key == key && SameDepthDesc(e.source, source))
                return std::addressof(e.value); // not &: ComPtr overloads operator& and releases
        }
        return nullptr;
    }

    // Adds an entry; a stale entry with the same key is replaced. Returns nullptr when the cache is full.
    Value* Insert(uintptr_t key, const D3D11_DEPTH_STENCIL_DESC& source, Value value)
    {
        for (auto& e : _entries)
        {
            if (e.key == key)
            {
                e.source = source;
                e.value = std::move(value);
                return std::addressof(e.value); // not &: ComPtr overloads operator& and releases
            }
        }
        for (auto& e : _entries)
        {
            if (e.key == 0)
            {
                e.key = key;
                e.source = source;
                e.value = std::move(value);
                return std::addressof(e.value); // not &: ComPtr overloads operator& and releases
            }
        }
        return nullptr;
    }

    void Clear() { _entries = {}; }
    size_t Size() const
    {
        size_t n = 0;
        for (const auto& e : _entries)
            n += e.key != 0 ? 1 : 0;
        return n;
    }

  private:
    std::array<Entry, N> _entries {};
};

// ---- Open queries -------------------------------------------------------------------------------------------------

// Queries whose count a second draw would corrupt: occlusion (1) and everything from pipeline statistics (4) upward
// (statistics, occlusion predicate, stream-output statistics and predicates). Event, timestamp and disjoint are harmless.
inline constexpr bool QueryTypeCounts(D3D11_QUERY type)
{
    return type == D3D11_QUERY_OCCLUSION || static_cast<int>(type) >= static_cast<int>(D3D11_QUERY_PIPELINE_STATISTICS);
}

// The set of queries between Begin and End. A query that cannot be identified counts as tracked. The hooks only see
// calls while the feature is active, so queries opened earlier are unknown: replays stay off until the first frame end
// after the set started to be fed (`Blind`).
class OpenQuerySet
{
  public:
    void Begin(uintptr_t query, bool tracked)
    {
        if (tracked && query != 0)
            _open.insert(query);
    }

    // An End without a Begin (or for an untracked query) changes nothing.
    void End(uintptr_t query) { _open.erase(query); }

    size_t Count() const { return _open.size(); }
    bool Blind() const { return _blind; }
    bool ReplayAllowed() const { return !_blind && _open.empty(); }

    // The hooks stopped or started being fed (the path went inactive or active): what was open is unknown, and an open query whose
    // End was not seen would keep every replay refused for good, so the set is forgotten.
    void MarkBlind()
    {
        _open.clear();
        _blind = true;
        _blindSince = 0;
    }
    void OnFrameEnd() { _blind = false; }

    // Without a frame callback the gate would never lift: queries opened before the module started do not stay open for
    // long, so it lifts after `kBlindMs` of maintenance calls.
    static constexpr uint64_t kBlindMs = 100;
    void Maintain(uint64_t now)
    {
        if (!_blind)
            return;
        if (_blindSince == 0)
            _blindSince = now;
        else if (now >= _blindSince && now - _blindSince >= kBlindMs)
            _blind = false;
    }

    void Clear()
    {
        _open.clear();
        _blind = true;
        _blindSince = 0;
    }

  private:
    std::unordered_set<uintptr_t> _open;
    bool _blind = true;
    uint64_t _blindSince = 0; // 0 = not started yet
};
} // namespace ext::ffxiv::chara::motion
