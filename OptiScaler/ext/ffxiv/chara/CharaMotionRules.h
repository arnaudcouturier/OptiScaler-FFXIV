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
    if (f.vsCb0Bytes != kGBufferCbBytes || f.psCb2Bytes != kGBufferCbBytes)
        return skip(Skip::UnknownShader);
    d.replay = true;
    d.tagShader = true;
    return d;
}

// ---- Depth convention and the replay depth state -------------------------------------------------------------------

// The scene uses a reversed depth buffer when the bound depth state compares with GREATER or GREATER_EQUAL.
inline bool IsReversedZ(bool haveState, D3D11_COMPARISON_FUNC func)
{
    return haveState && (func == D3D11_COMPARISON_GREATER || func == D3D11_COMPARISON_GREATER_EQUAL);
}

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

    void MarkBlind()
    {
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
