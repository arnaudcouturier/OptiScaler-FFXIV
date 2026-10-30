#pragma once
// Engine side of the native nameplates (docs/notes/native-nameplates.md, model A): owns three plate-only image slots
// shared with D3D12, publishes exactly one plate record per real present, and draws the newest record's image over
// every output of the frame generator.
//
// The game side reserves a slot at the first accepted plate draw of a frame and draws the plate triangles straight into
// that slot on its own D3D11 device (same immediate context, no copy, no keyed mutex); this side signals a produce fence
// on the frame-generation queue and the output pass waits on it, so a slot is written only when no queue operation
// reads it any more. The pass pins the slot it reads in the same lock section that copies the record.
//
// Two locks only: the pass mutex (presenting threads) and one leaf (the slot table and the drawable record), taken in
// that order. Nothing that can block, log or call back into the plate code runs while the leaf is held.
//
// Key off costs nothing: no seam reaches this module, and every entry point returns on one atomic load.

#include "ext/hudless/PlateEngine.h" // the game side's half of this interface (reservation, draw count, serial)
#include "ext/hudless/PlateMode.h"
#include "ext/hudless/PlateRules.h"
#include "ext/hudless/PlateSlots.h"
#include "ext/hudless/PlateSource.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct ID3D11RenderTargetView;
struct ID3D12CommandQueue;
struct IDXGISwapChain;
class Dx11wDx12SC;

namespace ext::hudless::plates
{
// ---- present seams, game thread --------------------------------------------------------------------------------------
// Present entry, before the bridge copies the backbuffer and before any of its early returns: the frame boundary. A
// reservation left over from an earlier present is dropped here, so a present the bridge abandons cannot hand its slot
// and its "hidden" answer to the next frame.
void OnPresentBegin();
// After the interop copy: finish the open frame, publish exactly one record (Native, Shadow or Hidden), signal the
// produce fence and promote the record. Also refreshes the hide gate and creates at most one slot.
void OnPost(Dx11wDx12SC& sc);
// Teardown, resize, device change: all slots retired, open frame dropped, record cleared, pins dropped.
void OnRelease();

// ---- output pass, presenting threads ---------------------------------------------------------------------------------
// Every output, before the HUD paste: draw the newest hidden record's image (or submit zero instances for a shadow
// record, proving the path end to end). Never waits on the CPU: the produce wait is a queued GPU dependency.
void OnOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);
// The last reason the pass sat an output out (for the diagnostic line).
std::string LastSkip();
// True while the last output drew shifted movers.
bool ShiftingActive();

// ---- any thread ------------------------------------------------------------------------------------------------------
// Hide gate: an armed record fresh within 250 ms and a pass that proved alive within the last 100 ms.
bool HideAllowed();
// True while a plate image is worth building: the key is on, the output is DLSS-G and SDR, the multiplier is at least
// 2 and the preset did not drop the plates under recomposition. One present behind the gate above.
bool CaptureWanted();
// The feature bit the engine owns once the key is off: cleared when no hidden record is drawable any more, then the
// snapshot is re-read (a user who turned the key back on gets the bit back). Null sink = no bit to own.
void SetPlateFeatureBitSink(void (*sink)(bool on));

struct Counters
{
    uint64_t promoted = 0;         // records promoted (Native, Shadow and Hidden together)
    uint64_t hidden = 0;           // ... of which the frame's draws were hidden
    uint64_t shadow = 0;           // ... of which the image was kept and the frame drawn natively
    uint64_t native = 0;           // ... of which nothing was captured (no draw, declined, gate closed)
    uint64_t declined = 0;         // frames whose first plate draw was refused (no slot, key off, gate closed)
    uint64_t slotsCreated = 0;     // slot textures created (one per present at most)
    uint64_t slotsRetired = 0;     // slot textures handed back (key off, resize, device change, release)
    uint64_t empty = 0;            // hidden frames with no draw in their image: published as native, nothing to draw
    uint64_t reclaimed = 0;        // reservations taken back (a present that never finished, a stale frame)
    uint64_t plated = 0;           // outputs the plate image was drawn on
    uint64_t skipped = 0;          // outputs the pass sat out
    uint64_t hiddenSkipped = 0;    // ... of which the record was hidden (the outputs of that frame lost their plates)
    uint64_t promoteFailedHidden = 0; // hidden frames whose promote failed (the slot was freed)
    uint64_t lateOutputs = 0;      // outputs of a batch beyond its multiplier that drew a newer record than its siblings
    uint64_t earlyOutputs = 0;     // ... that drew the same record (they presented before their own promote)
    uint64_t transitions = 0;      // promotes that changed the record kind (a hide/unhide transition)
    uint64_t releasedHidden = 0;   // hidden frames dropped by a release / device change / resize / lost present
    uint64_t affinityBroken = 0;   // engine calls seen on a thread other than the drawing one
    uint64_t pinned = 0;           // pass reads pinned (ever)
    uint64_t pinsOutstanding = 0;  // pins held right now: zero whenever no pass is inside a read
    uint64_t retiredStale = 0;     // retired slot images dropped after their readers never completed
};

Counters Snapshot();
} // namespace ext::hudless::plates
