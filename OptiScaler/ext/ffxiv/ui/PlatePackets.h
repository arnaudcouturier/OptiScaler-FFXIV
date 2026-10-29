#pragma once
// Developer probe of the UI draws (docs/HUDLESS.md, "Developer probes"), packet side: which game packets (queue key, vertices) sit behind a
// logged draw. Log only, behind [HudLess] PlateDiagnostics (ext::hudless::plate::Enabled()). The hooks that see the packets are the game's
// UI batch functions, its draw command enqueue and its draw state bind (CompanionNative.cpp); the draw hooks hand the result to the draw
// rows (PlateDraws.cpp). The probe has its own state: it never touches the visibility layer's queue, binding or tracking flag.
//
// This header is included by CompanionNative.cpp and stays light: one atomic test per hook, the work is out of line in PlatePackets.cpp.

#include "ext/ffxiv/ui/PlateDraws.h"
#include "ext/hudless/PlateProbe.h"
#include "ext/hudless/PlateProbeFormat.h"

#include <cstdint>
#include <vector>

namespace ext::hudless::plate
{
struct DrawRecord;
}

namespace ext::ffxiv::ui::plate
{
namespace detail
{
bool BeginBatch(uintptr_t base, uintptr_t renderer, uint32_t indexBytes);
void EndBatch();
void Enqueue(uintptr_t command);
void Bind(uintptr_t nativeContext, uintptr_t state);
} // namespace detail

// UI batch functions (batch16 / batch32): the packets of the batch are read when the frame is one the probe logs. Scope guard.
class BatchScope
{
  public:
    BatchScope(uintptr_t base, uintptr_t renderer, uint32_t indexBytes)
    {
        if (ext::hudless::plate::Enabled())
            _active = detail::BeginBatch(base, renderer, indexBytes);
    }
    ~BatchScope()
    {
        if (_active)
            detail::EndBatch();
    }
    BatchScope(const BatchScope&) = delete;
    BatchScope& operator=(const BatchScope&) = delete;

  private:
    bool _active = false;
};

// The game's draw command enqueue, before the original runs.
inline void OnEnqueue(uintptr_t command)
{
    if (ext::hudless::plate::Enabled())
        detail::Enqueue(command);
}

// The game's draw state bind, after the original ran.
inline void OnBindState(uintptr_t nativeContext, uintptr_t state)
{
    if (ext::hudless::plate::Enabled())
        detail::Bind(nativeContext, state);
}

// What is known about the packets behind the draw in progress on this thread.
struct PacketInfo
{
    ext::hudless::plate::RowOutcome outcome = ext::hudless::plate::RowOutcome::NoPacket;
    const ext::hudless::plate::DrawRecord* record = nullptr; // the record of the draw, when one was bound
};
PacketInfo CurrentPacket();

// The frame the drawing side is in (or is about to start): the packet side reads game memory only around frames the probe logs.
void SetFrameHint(uint64_t frame);

// The boxes of the candidate draws of the last logged frame, for the overlay (PlateOverlay.cpp).
struct OverlayBox
{
    float box[4] = { 0, 0, 0, 0 };
    uint32_t rtWidth = 0, rtHeight = 0, vpWidth = 0, vpHeight = 0;
};
struct OverlayFrame
{
    uint64_t frame = 0; // 0 = no logged frame yet
    std::vector<OverlayBox> boxes;
};
void PublishOverlay(OverlayFrame&& frame);
bool CopyOverlay(OverlayFrame& out);
uint64_t LastLoggedFrame(); // the frame of the last PublishOverlay, 0 = none yet
void CheckOverlaySlot();    // once: logs when PlateOverlay is on and the menu's overlay table has no room for the probe's overlay (PlateOverlay.cpp)
} // namespace ext::ffxiv::ui::plate
