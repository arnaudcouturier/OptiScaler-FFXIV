#pragma once
// The rules that connect preview NR to the preview dispatcher and to the super-resolution (SR) path. Pure: the dispatcher and
// CharaDlss.cpp pass facts in and act on the answers, so every rule is tested without a device (chara_nr_plumbing).
//
// Why this exists: the plumbing that finds the finisher, the preview images and the consumers lives next to the SR path. NR must run
// with the SR path latched, failed or switched off, and SR must not care when NR fails. The answers below keep the two apart.

#include <atomic>
#include <cstdint>

#include "ext/ffxiv/chara/CharaNrRules.h"

namespace ext::ffxiv::chara::nr
{
struct PlumbingFacts
{
    bool srWanted = false;        // [FFXIV] PreviewDLSS
    bool nrWanted = false;        // [FFXIV] PreviewNR
    bool srLatched = false;       // the SR path failed for the session
    bool nrLatched = false;       // preview NR failed for the session
    bool nrAvailable = true;      // the module's last availability check passed (bridge up, NVIDIA card, files present)
    bool srGateOpen = false;      // the SR gate says the isolated DLSS path can run (NGX, device, entry points)
    bool nrHoldsResources = false; // NR streams or GPU objects still exist
    uint32_t srSlotsHeld = 0;     // SR slots whose GPU objects still exist
};

// The SR path may do work: wanted, not latched, gate open.
constexpr bool SrUsable(const PlumbingFacts& f) { return f.srWanted && !f.srLatched && f.srGateOpen; }

// Preview NR may do work (its own availability, bridge and files, is checked by the module itself).
constexpr bool NrUsable(const PlumbingFacts& f) { return f.nrWanted && !f.nrLatched && f.nrAvailable; }

// dlss::CanRun(): at least one path can do work for a draw now. This (not PlumbingActive) decides the route of a draw, so a draw is not
// handed to a plumbing that is only draining objects of a latched or switched-off path.
constexpr bool PathCanRun(const PlumbingFacts& f) { return SrUsable(f) || NrUsable(f); }

// dlss::Active(): the plumbing (draw offers, copy and clear hooks, maintenance) is alive. It stays alive while either path may run
// and while any GPU object of either path still exists, so a latched path is drained by the maintenance calls.
constexpr bool PlumbingActive(const PlumbingFacts& f)
{
    return SrUsable(f) || NrUsable(f) || f.srSlotsHeld != 0 || f.nrHoldsResources;
}

// The hooks that only the SR path needs (clear of the G-buffer, query bookkeeping, vertex shader, motion replay).
constexpr bool SrHooksNeeded(const PlumbingFacts& f) { return SrUsable(f) || f.srSlotsHeld != 0; }

// The same answer as SrHooksNeeded(facts), for the hook forwarders that run on every copy, clear, query and shader creation of the game:
// the cheap conditions come first and the dearer ones (`srLatched`, `srGateOpen`: callables) are evaluated only when the answer still
// depends on them. With PreviewDLSS off and no slot held (the default configuration) nothing but the configuration key and one counter is read.
template <class LatchedFn, class GateFn>
constexpr bool SrHooksNeededLazy(uint32_t srSlotsHeld, bool srWanted, LatchedFn&& srLatched, GateFn&& srGateOpen)
{
    return srSlotsHeld != 0 || (srWanted && !srLatched() && srGateOpen());
}

// The dispatcher's cheap gate in front of every draw (the debug aids keep it running as before). Both paths keep it open while they
// still hold GPU objects after their key was switched off, so the maintenance that releases them still runs.
constexpr bool DispatcherWanted(bool aaWanted, bool srWanted, bool nrWanted, bool nrHoldsResources, bool srHoldsObjects, bool debugAids)
{
    return aaWanted || srWanted || nrWanted || nrHoldsResources || srHoldsObjects || debugAids;
}

// Whether the SR plumbing still holds GPU objects whatever the config says now: preview slots (images and NGX features), the cyan tint
// targets of the debug tint, or the helper passes (shaders and private state of the copy, guides, alpha and tint passes). The dispatcher's gate
// and its maintenance stay on while it does, so they are released after PreviewDLSS / PreviewNR / PreviewDebugTint are switched off.
constexpr bool PlumbingHoldsObjects(uint32_t slotsHeld, uint32_t tintTargets, bool passesReady)
{
    return slotsHeld != 0 || tintTargets != 0 || passesReady;
}

// The helper passes are released once nothing uses them and the SR path is disabled (switched off or latched). While the SR path is
// enabled they stay (a window reopened later finds them built).
constexpr bool ReleaseHelperPasses(bool srDisabled, uint32_t slotsHeld, uint32_t tintTargets, bool passesReady)
{
    return srDisabled && passesReady && slotsHeld == 0 && tintTargets == 0;
}

struct MaintenancePlan
{
    bool sr = false; // call dlss::OnFrameEnd / dlss::Maintain
    bool nr = false; // call nr::Maintain
};

// The dispatcher's maintenance forwarding. Both paths keep being maintained while they hold objects, also with their key switched off:
// that is how the models and the preview slots (images and NGX features) are freed after PreviewNR / PreviewDLSS is turned off.
constexpr MaintenancePlan PlanMaintenance(bool srWanted, bool nrWanted, bool plumbingActive, bool nrHoldsResources, bool srHoldsObjects)
{
    return MaintenancePlan { ((srWanted || nrWanted) && plumbingActive) || srHoldsObjects, nrWanted || nrHoldsResources };
}

// Remembers one boolean and says when it changed. Used for "the SR hooks are being fed": whenever that flips either way, the open-query
// bookkeeping of the motion replay is forgotten (an End seen by nobody would keep every replay refused). It is asked from the hook
// forwarders themselves, which run whether or not the dispatcher's gate is open, so an off-on switch is never missed.
class EdgeTracker
{
  public:
    bool Changed(bool now) { return _last.exchange(now, std::memory_order_relaxed) != now; }

  private:
    std::atomic<bool> _last { false };
};

// What a UI draw that samples a preview image gets, best first (after the debug flood, which replaces everything):
//   production result > super-resolution result > bring-up NR result > the native image.
enum class ConsumerSource : uint8_t
{
    Native,           // nothing replaces the image
    Flood,            // the debug flood test
    ProductionResult, // the super-resolution image after NR (2x, original alpha): NR was run on the SR output of this render
    SrResult,         // the super-resolution result (NR did not run on it, or failed: fail open)
    NrResult,         // the bring-up NR result at native size
};

// `productionReady` means the composed image of the slot was made from the production NR output (it implies `srResultReady`: it is the
// same composed image). The bring-up result is only asked for when no SR result exists (WantNrResult), so it never competes with these.
constexpr ConsumerSource ChooseConsumerSource(bool floodOn, bool productionReady, bool srResultReady, bool nrResultReady)
{
    if (floodOn)
        return ConsumerSource::Flood;
    if (productionReady)
        return ConsumerSource::ProductionResult;
    if (srResultReady)
        return ConsumerSource::SrResult;
    if (nrResultReady)
        return ConsumerSource::NrResult;
    return ConsumerSource::Native;
}

// Whether the consumer asks the NR module for a result for one sampled image: NR may run, the flood test is not replacing the image,
// the SR result is not already there (it wins) and the image is one of the preview renderer's outputs. The one rule for an image with an
// SR slot and for one without (the consumer used to repeat it inline for the second case).
constexpr bool WantNrResult(bool nrOn, bool floodOn, bool srResultReady, bool previewOutput)
{
    return nrOn && !floodOn && !srResultReady && previewOutput;
}

// The super-resolution path is in charge of a preview's picture: it can run (`srUsable`) and this preview has neither failed in it
// (`slotFailed`) nor been declined as too large for it (`tooLarge`). While it is in charge, a render it did not evaluate (the finisher was
// rejected, the guides were not ready yet, a stale or waiting verdict) keeps the native image, exactly as with PreviewDLSS alone; NR works on
// the SR result (production) and never on a render of its own.
constexpr bool SrInCharge(bool srUsable, bool slotFailed, bool tooLarge) { return srUsable && !slotFailed && !tooLarge; }

// NR bring-up runs after the finisher wrote the native image: the finisher check passed, NR may run and a UI draw sampled this
// preview image recently (a live window; a finisher output nobody shows gets no model). It does not wait for the SR guides.
// It is for the previews the SR path does not serve: PreviewDLSS off, the SR path latched or unusable, a slot that failed, a preview too
// large for it (`srInCharge`, see SrInCharge). With the SR path in charge the bring-up is never run: a bring-up stream beside the production
// stream of one window would be rebuilt back and forth by the 500 ms settle rule, and the throwaway bring-up model would be built first.
constexpr bool RunBringUpNow(const PlumbingFacts& f, bool finisherAccepted, bool consumerRecent, bool srInCharge)
{
    return finisherAccepted && consumerRecent && NrUsable(f) && !srInCharge;
}

// A finisher of a preview image went by and preview NR produced no result for that render (the finisher was rejected, the bring-up was
// not run for it: nobody shows this preview, the SR result wins, a pre-condition failed; or it ran and returned without a result): an
// older NR result of that image must not be shown for it. Only needed while the module holds anything; with nothing held there is no
// result to withdraw and the call costs nothing.
constexpr bool InvalidateNow(const PlumbingFacts& f, bool nrProducedResult) { return !nrProducedResult && f.nrHoldsResources; }

// Which preview images a UI draw sampled lately (the liveness signal for NR, independent of the SR slots).
class ConsumerSeen
{
  public:
    static constexpr size_t kCapacity = 16;

    void Record(uintptr_t key, uint64_t now)
    {
        if (key == 0)
            return;
        size_t pick = kCapacity;
        for (size_t i = 0; i < kCapacity; ++i)
        {
            if (_key[i] == key)
            {
                _last[i] = now;
                return;
            }
            if (pick == kCapacity || _key[i] == 0 || (_key[pick] != 0 && _last[i] < _last[pick]))
                pick = i;
        }
        _key[pick] = key; // a free entry, else the one seen longest ago
        _last[pick] = now;
    }

    bool Recent(uintptr_t key, uint64_t now, uint64_t windowMs) const
    {
        if (key == 0)
            return false;
        for (size_t i = 0; i < kCapacity; ++i)
            if (_key[i] == key)
                return now >= _last[i] && now - _last[i] <= windowMs;
        return false;
    }

  private:
    uintptr_t _key[kCapacity] {};
    uint64_t _last[kCapacity] {};
};

// The production entry (NR on the SR output) is offered only after an SR evaluate succeeded for this render, NR may run, and a UI draw
// showed this preview lately (the same liveness rule as the bring-up: a finisher output nobody shows gets no model).
constexpr bool RunProductionNow(const PlumbingFacts& f, bool srEvaluateSucceeded, bool consumerRecent)
{
    return srEvaluateSucceeded && consumerRecent && NrUsable(f);
}

// NR produced a result for this render when either entry did. The dispatcher withdraws the older NR result of the image otherwise
// (InvalidateNow); the production entry runs inside the SR step, before that decision, so its success has to count: without it every
// production result would be marked stale in the render that made it.
constexpr bool NrProducedForRender(bool bringUpProduced, bool productionProduced) { return bringUpProduced || productionProduced; }

// The colour the alpha pass recomposes the original alpha onto: the production output when NR produced one, else the SR output. The
// alpha pass is the last step of the SR path, so a failed NR must leave it exactly what it was without NR.
enum class AlphaInput : uint8_t
{
    SrOutput,
    NrOutput,
};

constexpr AlphaInput ChooseAlphaInput(bool productionProduced) { return productionProduced ? AlphaInput::NrOutput : AlphaInput::SrOutput; }

// ---- The debug tint (PreviewDebugTint) ----------------------------------------------------------------------------------------------
// The tint is the on-screen proof of which image reached the screen: magenta for the super-resolution image (as before), cyan for an
// image NR produced (the bring-up result and the production output both), none for the native image.
enum class TintKind : uint8_t
{
    None,
    Sr, // magenta
    Nr, // cyan
};

// The alpha pass of the SR path: tinted when PreviewDebugTint is on, cyan when its colour came from NR.
constexpr TintKind AlphaTintFor(bool tintOn, AlphaInput input)
{
    return !tintOn ? TintKind::None : (input == AlphaInput::NrOutput ? TintKind::Nr : TintKind::Sr);
}

// The bring-up result is a finished image that a UI draw samples as it is, so its tint is an extra pass over it (the result image is
// owned by the D3D12 queue's pass and cannot be tinted in place). Due when the tint is on, the consumer is given that result, and the pass
// has not been done for this result yet (`serial` identifies a result; the tint target keeps the last one it was made from).
constexpr bool ResultTintPassDue(bool tintOn, ConsumerSource source, uint64_t tintedSerial, uint64_t resultSerial)
{
    return tintOn && source == ConsumerSource::NrResult && (tintedSerial == 0 || tintedSerial != resultSerial);
}

// Once per cause: a fallback of the production entry is counted every time and logged the first time its cause is seen.
class CauseLog
{
  public:
    static constexpr unsigned kCauses = 32;
    // True when the caller should write the line for `cause` now.
    bool First(unsigned cause)
    {
        if (cause >= kCauses)
            return false;
        const uint32_t bit = 1u << cause;
        const bool seen = (_seen & bit) != 0;
        _seen |= bit;
        return !seen;
    }

  private:
    uint32_t _seen = 0;
};

// ---- One render of a preview image ---------------------------------------------------------------------------------------------------
// Whether a UI draw sampled the preview image `texture` within the last `windowMs` (the liveness signal of both NR entries). One rule for the
// slot step and for the finisher step; a render without a known texture is never shown.
inline bool ConsumerRecentFor(const ConsumerSeen& seen, const void* texture, uint64_t now, uint64_t windowMs)
{
    return texture != nullptr && seen.Recent(reinterpret_cast<uintptr_t>(texture), now, windowMs);
}

// The glue takes the decisions of a render at two moments with effects between them, so the functions below own the ORDER and the effects are
// passed in as callables (the real ones in CharaDlss.cpp, recording fakes in the tests):
//   DecideSlotRender     inside RunSlot, after the SR evaluate succeeded: the production entry is offered or not, the alpha pass gets the NR
//                        output or the SR output, and the slot learns whether its composed picture came from NR.
//   DecideFinisherRender at the finisher, after the slot step: the bring-up step, "NR produced a result for this render", and whether the
//                        older NR result of the image is withdrawn.
// Nothing here reads a global or a device. What stays in the glue is the choice of the facts it passes (a fresh snapshot in the slot step, the
// snapshot taken at the top of the dispatcher call in the finisher step) and the one flag that carries `composedFromNr` from the first step to
// the second.

// What the module said for the production entry: its outcome and whether it handed back an image.
struct ProductionAnswer
{
    RunOutcome outcome = RunOutcome::Failed; // Failed also stands for "the call threw"
    bool hasView = false;
};

// An image of the NR output exists only for Ran with a view; every other answer leaves the SR output as it is (fail open).
constexpr bool ProductionProduced(const ProductionAnswer& answer) { return answer.outcome == RunOutcome::Ran && answer.hasView; }

struct SlotRender
{
    bool offered = false;       // the production entry was run for this render
    bool produced = false;      // it returned an NR image
    AlphaInput alphaInput = AlphaInput::SrOutput;
    TintKind tint = TintKind::None;
    bool alphaOk = false;       // the alpha pass succeeded
    bool composedFromNr = false; // the slot's composed picture was made from the NR image (needs the alpha pass to have succeeded)
};

// `runProduction() -> ProductionAnswer` runs the production entry; it is called only when RunProductionNow allows it, and before the alpha
// pass. `alphaPass(AlphaInput, TintKind) -> bool` runs the alpha pass with the colour and tint decided here; it runs once, in every case.
template <class RunProduction, class AlphaPass>
SlotRender DecideSlotRender(const PlumbingFacts& facts, bool consumerRecent, bool tintOn, RunProduction&& runProduction, AlphaPass&& alphaPass)
{
    SlotRender r;
    r.offered = RunProductionNow(facts, true, consumerRecent); // the SR evaluate succeeded: this step is only reached after it
    if (r.offered)
        r.produced = ProductionProduced(runProduction());
    r.alphaInput = ChooseAlphaInput(r.produced);
    r.tint = AlphaTintFor(tintOn, r.alphaInput);
    r.alphaOk = alphaPass(r.alphaInput, r.tint);
    r.composedFromNr = r.alphaOk && r.alphaInput == AlphaInput::NrOutput;
    return r;
}

struct FinisherRender
{
    bool runBringUp = false;           // the bring-up entry was run
    bool bringUpProduced = false;      // and returned a result
    bool nrProducedForRender = false;  // either entry produced a result for this render
    bool invalidate = false;           // the older NR result of the image is withdrawn
};

// `productionComposed`: the slot step's `composedFromNr` (false when the slot step did not run for this render). `runBringUp() -> bool` runs the
// bring-up entry (true: it produced a result); it is called only when RunBringUpNow allows it.
template <class RunBringUp>
FinisherRender DecideFinisherRender(const PlumbingFacts& facts, bool finisherAccepted, bool consumerRecent, bool srInCharge, bool productionComposed,
                                    RunBringUp&& runBringUp)
{
    FinisherRender r;
    r.runBringUp = RunBringUpNow(facts, finisherAccepted, consumerRecent, srInCharge);
    if (r.runBringUp)
        r.bringUpProduced = runBringUp();
    r.nrProducedForRender = NrProducedForRender(r.bringUpProduced, productionComposed);
    r.invalidate = InvalidateNow(facts, r.nrProducedForRender);
    return r;
}
} // namespace ext::ffxiv::chara::nr
