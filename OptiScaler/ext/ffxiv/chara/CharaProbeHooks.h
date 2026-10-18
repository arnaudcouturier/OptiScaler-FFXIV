#pragma once
// The seam between the preview modules (CharaEnhance.cpp, CharaDlss.cpp) and the preview probe (CharaProbe.cpp).
// Light on purpose: no D3D implementation dependency and no link dependency on the probe, so a module that includes this
// header builds (and is tested) without the probe sources. While the probe is off, the whole cost of a trace is one load of a
// null pointer.

#include <atomic>
#include <cstdint>
#include <d3d11.h>

#include "ext/core/ExtConfig.h"

namespace ext::ffxiv::chara::probe
{
// [FFXIV] PreviewProbe: the master switch of the probe (one config bool).
inline bool ProbeOn() { return ext::Cfg().ffxiv.chara.previewProbe.value_or_default(); }

enum class SwapKind : uint8_t
{
    Aa,    // the spatial AA result replaced the image a UI draw samples
    Dlss,  // the composed DLSS image replaced it
    Flood, // the debug magenta replaced it
};

inline constexpr const char* SwapKindName(SwapKind k)
{
    switch (k)
    {
    case SwapKind::Aa: return "AA";
    case SwapKind::Dlss: return "DLSS";
    default: return "flood";
    }
}

// What was bound at the traced pixel-shader slot around one swapped draw. Pointer values only: no reference is kept.
struct SwapRecord
{
    SwapKind kind = SwapKind::Aa;
    uint32_t slot = 0;
    uintptr_t before = 0;      // the game's view, before the swap
    uintptr_t afterSwap = 0;   // right after the swap, before the original draw ran
    uintptr_t afterDraw = 0;   // after the draw and the restore
    uintptr_t replacement = 0; // the view the module wanted bound
};

// Installed by the probe while it is on (and removed when it turns off). `arm` decides, per swap, whether to trace it (the probe
// answers once per burst and kind); `report` receives the finished record.
struct SwapSink
{
    bool (*arm)(SwapKind kind) = nullptr;
    void (*report)(const SwapRecord& record) = nullptr;
};

inline std::atomic<const SwapSink*> g_swapSink { nullptr };

// Wraps one swapped draw:
//   SwapTrace trace(context, kind, slot, replacement);
//   <swap the view in>; trace.Applied(); <original draw>; <put the game's view back>; trace.Restored();
// Null-safe and free when no sink is installed or the probe is off: then every call is a no-op and no COM call is made. When
// armed it reads the view at `slot` three times (before, after the swap, after the restore) and releases each reference at
// once. It never changes any state.
class SwapTrace
{
  public:
    SwapTrace(ID3D11DeviceContext* context, SwapKind kind, uint32_t slot, ID3D11ShaderResourceView* replacement)
    {
        const SwapSink* sink = g_swapSink.load(std::memory_order_relaxed);
        if (sink == nullptr || context == nullptr || sink->arm == nullptr || sink->report == nullptr || !ProbeOn() || !sink->arm(kind))
            return;
        _sink = sink;
        _context = context;
        _record.kind = kind;
        _record.slot = slot;
        _record.replacement = reinterpret_cast<uintptr_t>(replacement);
        _record.before = Bound();
    }

    SwapTrace(const SwapTrace&) = delete;
    SwapTrace& operator=(const SwapTrace&) = delete;

    // Call after the replacement was bound, before the draw.
    void Applied()
    {
        if (_sink != nullptr)
            _record.afterSwap = Bound();
    }

    // Call after the game's view was bound again. Reports once.
    void Restored()
    {
        if (_sink == nullptr)
            return;
        _record.afterDraw = Bound();
        const SwapSink* sink = _sink;
        _sink = nullptr;
        sink->report(_record);
    }

  private:
    uintptr_t Bound() const
    {
        ID3D11ShaderResourceView* view = nullptr;
        _context->PSGetShaderResources(_record.slot, 1, &view);
        const uintptr_t value = reinterpret_cast<uintptr_t>(view);
        if (view != nullptr)
            view->Release();
        return value;
    }

    const SwapSink* _sink = nullptr;
    ID3D11DeviceContext* _context = nullptr;
    SwapRecord _record {};
};
} // namespace ext::ffxiv::chara::probe
