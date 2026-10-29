#pragma once
// S10 package E: the decision logic behind ApplyPresetsInPlace (ReShadeBoundary.h), over D0's backend interface so it is
// tested with a fake (tests/ext/reshade/reshade_boundary_apply.cpp). No ReShade, D3D or ImGui types, no globals, no logging.
//
// Threading: Apply()/FrameEnded() run on the render thread at the UI boundary. The runtime events (init/destroy) may arrive on
// another thread; they and Apply() share one recursive mutex, held across the whole ReShade call so a runtime cannot be
// destroyed under render_effects. The flag-only events (reloaded, preset path, technique state) take no lock: ReShade may fire
// them from inside render_effects.

#include "ext/reshade/ReShadeBoundary.h"
#include "ext/reshade/ReShadeProbe.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace ext::reshade::boundary
{
// True while this thread runs the live in-place preset pass (Applier::Apply's render_effects call below): the
// begin/finish-effects events fired inside it belong to the fork's own pass, not to ReShade's end-of-frame work.
// Thread-local call state like probe::InProbeCall, but separate from it: the probe ledger classifies events by
// InProbeCall and must never see the live pass as a probe call. The GPU stage timers skip on either flag.
bool InLiveEffectsCall();
struct LiveEffectsScope
{
    LiveEffectsScope();
    ~LiveEffectsScope();
    LiveEffectsScope(const LiveEffectsScope&) = delete;
    LiveEffectsScope& operator=(const LiveEffectsScope&) = delete;
};

class Applier
{
  public:
    static constexpr uint32_t kRecountEvery = 1200; // safety recount of the enabled techniques (calls)

    explicit Applier(probe::IBackend& backend);

    // ---- ReShade events (any thread, never throw)
    void OnInitRuntime(uint64_t runtime);
    void OnDestroyRuntime(uint64_t runtime);
    void OnReloadedEffects(uint64_t runtime); // flag only: the technique list changed, effects are loaded
    void OnTechniquesChanged();               // flag only: preset path or a technique state changed

    // ---- boundary thread
    // `context`/`target` are an ID3D11DeviceContext* and the ID3D11Resource* bound as render target 0.
    ApplyResult Apply(PresetsMode mode, bool registered, void* context, void* target);
    void FrameEnded() { _appliedThisFrame.store(false, std::memory_order_relaxed); }

    ApplyStats GetStats() const;
    uint32_t AliveRuntimes() const;

  private:
    struct Slot
    {
        std::atomic<uint64_t> id { 0 };
        std::atomic<bool> reloaded { false };
    };
    static constexpr size_t kSlots = 8;

    ApplyResult Done(ApplyResult result, const char* detail);
    ApplyResult Done(ApplyResult result, const std::string& detail) { return Done(result, detail.c_str()); }
    Slot* FindSlot(uint64_t runtime);
    std::vector<uint64_t> AliveIds() const;

    probe::IBackend& _backend;
    std::recursive_mutex _mutex; // runtime lifetime vs. the ReShade call
    std::array<Slot, kSlots> _slots {};
    uint32_t _generation = 1; // bumped (under the mutex) at every init/destroy

    // runtime choice for (context, generation)
    bool _pickValid = false;
    void* _pickContext = nullptr;
    uint32_t _pickGeneration = 0;
    uint64_t _pickRuntime = 0;
    std::string _pickWhy;

    // enabled-technique count
    std::atomic<bool> _techDirty { true };
    bool _techKnown = false;
    uint64_t _techRuntime = 0;
    uint32_t _techGeneration = 0;
    uint32_t _techTotal = 0, _techEnabled = 0;
    uint32_t _techAge = 0;

    std::atomic<bool> _appliedThisFrame { false };
    std::atomic<bool> _inert { false };

    // statistics
    std::atomic<uint64_t> _calls { 0 };
    std::array<std::atomic<uint64_t>, kApplyResults> _results {};
    std::atomic<uint64_t> _callUsMax { 0 }, _callUsTotal { 0 }, _runtimeEvents { 0 };
    mutable std::mutex _detailMutex;
    std::string _lastDetail;
};
} // namespace ext::reshade::boundary

namespace ext::reshade::boundary
{
// The process-wide applier over the live backend (ReShadeBoundary.cpp). Heap-allocated, never destroyed.
Applier& Instance();
} // namespace ext::reshade::boundary
