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

// The dispatcher's cheap gate in front of every draw (the debug aids keep it running as before). Both paths keep it open while they
// still hold GPU objects after their key was switched off, so the maintenance that releases them still runs.
constexpr bool DispatcherWanted(bool aaWanted, bool srWanted, bool nrWanted, bool nrHoldsResources, bool srHoldsObjects, bool debugAids)
{
    return aaWanted || srWanted || nrWanted || nrHoldsResources || srHoldsObjects || debugAids;
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

// What a UI draw that samples a preview image gets.
enum class ConsumerSource : uint8_t
{
    Native,   // nothing replaces the image
    Flood,    // the debug flood test
    SrResult, // the super-resolution result (also covers NR that ran before it: SR wins)
    NrResult, // the NR result at native size
};

constexpr ConsumerSource ChooseConsumerSource(bool floodOn, bool srResultReady, bool nrResultReady)
{
    if (floodOn)
        return ConsumerSource::Flood;
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

// NR bring-up runs after the finisher wrote the native image: the finisher check passed, NR may run and a UI draw sampled this
// preview image recently (a live window; a finisher output nobody shows gets no model). It does not wait for the SR guides.
// `srResultWins`: the SR path evaluated this render, so the consumer will draw the SR result and an NR result would be thrown away
// (the production entry that runs NR on the SR output is still a stub); the bring-up is not run then, and the NR cost is not paid.
constexpr bool RunBringUpNow(const PlumbingFacts& f, bool finisherAccepted, bool consumerRecent, bool srResultWins)
{
    return finisherAccepted && consumerRecent && NrUsable(f) && !srResultWins;
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

// The production entry (NR on the SR output) is offered only after an SR evaluate succeeded and NR may run.
constexpr bool RunProductionNow(const PlumbingFacts& f, bool srEvaluateSucceeded) { return srEvaluateSucceeded && NrUsable(f); }
} // namespace ext::ffxiv::chara::nr
