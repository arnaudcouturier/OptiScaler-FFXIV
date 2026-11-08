#pragma once

#include <d3d11.h>
#include <cstdint>

// Dispatches through the context's current method entry after private D3D11
// state work changes it. The literal entry is captured before hook attachment;
// it is never the Detours-mutated trampoline.
namespace ext::core::draw_dispatch
{
enum class Api : uint8_t { Draw, Indexed, Instanced, IndexedInstanced, Auto, IndexedIndirect, Indirect };

struct ForwardEvent
{
    ID3D11DeviceContext* context;
    Api api;
    unsigned slot;
    void* literal;
    void* current;
    void* chosen;
    bool trampoline;
    bool freshReentry;
    uint8_t phase;
};
using ForwardObserver = void (*)(void*, const ForwardEvent&) noexcept;
struct ObserverFrame { void* user; ForwardObserver callback; const ObserverFrame* previous; };
inline thread_local const ObserverFrame* currentObserver = nullptr;
inline thread_local uint8_t currentForwardPhase = 0;
class ForwardPhaseScope
{
public:
    explicit ForwardPhaseScope(uint8_t phase) noexcept : previous(currentForwardPhase) { currentForwardPhase = phase; }
    ~ForwardPhaseScope() noexcept { currentForwardPhase = previous; }
    ForwardPhaseScope(const ForwardPhaseScope&) = delete;
    ForwardPhaseScope& operator=(const ForwardPhaseScope&) = delete;
private:
    uint8_t previous;
};
class ForwardObserverScope
{
public:
    ForwardObserverScope(void* user, ForwardObserver callback) noexcept : frame{user, callback, currentObserver} { currentObserver = &frame; }
    ~ForwardObserverScope() noexcept { currentObserver = frame.previous; }
    ForwardObserverScope(const ForwardObserverScope&) = delete;
    ForwardObserverScope& operator=(const ForwardObserverScope&) = delete;
private:
    ObserverFrame frame;
};
inline void Observe(const ForwardEvent& event) noexcept
{
    if (currentObserver && currentObserver->callback) currentObserver->callback(currentObserver->user, event);
}

struct Frame
{
    ID3D11DeviceContext* context;
    Api api;
    const Frame* previous;
};
inline thread_local const Frame* currentFrame = nullptr;

class FreshEntryScope
{
public:
    FreshEntryScope(ID3D11DeviceContext* context, Api api) noexcept
        : frame{context, api, currentFrame} { currentFrame = &frame; }
    ~FreshEntryScope() noexcept { currentFrame = frame.previous; }
    FreshEntryScope(const FreshEntryScope&) = delete;
    FreshEntryScope& operator=(const FreshEntryScope&) = delete;
private:
    Frame frame;
};

inline bool IsFreshReentry(ID3D11DeviceContext* context, Api api) noexcept
{
    for (auto* frame = currentFrame; frame; frame = frame->previous)
        if (frame->context == context && frame->api == api) return true;
    return false;
}

struct LayerFrame
{
    ID3D11DeviceContext* context;
    Api api;
    const void* layer;
    const LayerFrame* previous;
};
inline thread_local const LayerFrame* currentLayerFrame = nullptr;

class LayerScope
{
public:
    LayerScope(ID3D11DeviceContext* context, Api api, const void* layer) noexcept
        : frame{context, api, layer, currentLayerFrame}, entered(true)
    {
        for (auto* active = currentLayerFrame; active; active = active->previous)
            if (active->context == context && active->api == api && active->layer == layer)
            { entered = false; break; }
        if (entered) currentLayerFrame = &frame;
    }
    ~LayerScope() noexcept { if (entered) currentLayerFrame = frame.previous; }
    explicit operator bool() const noexcept { return entered; }
    LayerScope(const LayerScope&) = delete;
    LayerScope& operator=(const LayerScope&) = delete;
private:
    LayerFrame frame;
    bool entered;
};

template<class Method, class... Args>
inline void Forward(ID3D11DeviceContext* context, unsigned slot, void* literal,
                    Method trampoline, Api api, Args... args)
{
    auto** vtable = *reinterpret_cast<void***>(context);
    auto* current = vtable[slot];
    const bool fresh = IsFreshReentry(context, api);
    if (current == literal || fresh)
    {
        Observe({ context, api, slot, literal, current, reinterpret_cast<void*>(trampoline), true, fresh, currentForwardPhase });
        trampoline(context, args...);
        return;
    }
    Observe({ context, api, slot, literal, current, current, false, false, currentForwardPhase });
    FreshEntryScope scope(context, api);
    reinterpret_cast<Method>(current)(context, args...);
}
} // namespace ext::core::draw_dispatch
