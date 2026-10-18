#pragma once
// The rules that connect preview NR to the preview dispatcher and to the super-resolution (SR) path. Pure: the dispatcher and
// CharaDlss.cpp pass facts in and act on the answers, so every rule is tested without a device (chara_nr_plumbing).
//
// Why this exists: the plumbing that finds the finisher, the preview images and the consumers lives next to the SR path. NR must run
// with the SR path latched, failed or switched off, and SR must not care when NR fails. The answers below keep the two apart.

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
    bool srGateOpen = false;      // the SR gate says the isolated DLSS path can run (NGX, device, entry points)
    bool nrHoldsResources = false; // NR streams or GPU objects still exist
    uint32_t srSlotsHeld = 0;     // SR slots whose GPU objects still exist
};

// The SR path may do work: wanted, not latched, gate open.
constexpr bool SrUsable(const PlumbingFacts& f) { return f.srWanted && !f.srLatched && f.srGateOpen; }

// Preview NR may do work (its own availability, bridge and files, is checked by the module itself).
constexpr bool NrUsable(const PlumbingFacts& f) { return f.nrWanted && !f.nrLatched; }

// dlss::Active(): the plumbing (draw offers, copy and clear hooks, maintenance) is alive. It stays alive while either path may run
// and while any GPU object of either path still exists, so a latched path is drained by the maintenance calls.
constexpr bool PlumbingActive(const PlumbingFacts& f)
{
    return SrUsable(f) || NrUsable(f) || f.srSlotsHeld != 0 || f.nrHoldsResources;
}

// The hooks that only the SR path needs (clear of the G-buffer, query bookkeeping, vertex shader, motion replay).
constexpr bool SrHooksNeeded(const PlumbingFacts& f) { return SrUsable(f) || f.srSlotsHeld != 0; }

// The dispatcher's cheap gate in front of every draw (the debug aids keep it running as before).
constexpr bool DispatcherWanted(bool aaWanted, bool srWanted, bool nrWanted, bool nrHoldsResources, bool debugAids)
{
    return aaWanted || srWanted || nrWanted || nrHoldsResources || debugAids;
}

struct MaintenancePlan
{
    bool sr = false; // call dlss::OnFrameEnd / dlss::Maintain
    bool nr = false; // call nr::Maintain
};

// The dispatcher's maintenance forwarding. NR keeps being maintained while it holds resources, also with the key switched off:
// that is how the models are freed after PreviewNR is turned off.
constexpr MaintenancePlan PlanMaintenance(bool srWanted, bool nrWanted, bool plumbingActive, bool nrHoldsResources)
{
    return MaintenancePlan { (srWanted || nrWanted) && plumbingActive, nrWanted || nrHoldsResources };
}

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

// NR bring-up runs after the finisher wrote the native image: the finisher check passed and NR may run. It does not wait for the
// SR guides (replay, depth, camera).
constexpr bool RunBringUpNow(const PlumbingFacts& f, bool finisherAccepted) { return finisherAccepted && NrUsable(f); }

// The production entry (NR on the SR output) is offered only after an SR evaluate succeeded and NR may run.
constexpr bool RunProductionNow(const PlumbingFacts& f, bool srEvaluateSucceeded) { return srEvaluateSucceeded && NrUsable(f); }
} // namespace ext::ffxiv::chara::nr
