#pragma once
// What the NR scene pass does when the runtime selection moves to a new revision, as a small state machine over a host interface so
// tests/ext/nr/nr_runtime_switch.cpp can run it against a fake host. The real host is the pass in pass/DlssNr_Dx12.cpp.
//
// Order matters and is pinned by the test:
//   1. a failure caused by the OLD model is forgotten (the new model gets its own chance; failures of the forwarder or the GPU stay),
//   2. every feature built from the old model is parked behind its GPU fences (never released while the GPU may still use it),
//   3. the temporal history is cleared and the reuse caches are dropped (the module identity changed),
//   4. only then the new model path is made current, so nothing can create a feature from a half-switched state.
// A revision that has no model leaves the pass unavailable with the reason, and a later revision recovers it.

#include "ext/nr/runtime/NrRuntimeSelection.h"

#include <atomic>
#include <cstdint>

namespace ext::nr::runtime
{
enum class SwitchResult
{
    None,        // same revision as before
    Initial,     // the first revision this pass sees (nothing was built yet)
    Switched,    // moved to another model
    Unavailable, // moved to "no model" (an explicit choice that is not installed, or nothing installed)
};

// Host interface (any type with these members works):
//   void ClearModelFailure();                 // forget a failure caused by the previous model
//   void ParkFeatures();                      // park the scene, extra-pass and pre-SR features behind their fences
//   void ClearHistory();                      // reset the temporal history of every pass
//   void DropReuseCaches();                   // reuse caches and the reuse verdict belong to the old module
//   void UseModel(const Snapshot&);           // the new model becomes current
//   void MarkUnavailable(const Snapshot&);    // no model: report it, keep running as a pass-through
// ---- The failure latch --------------------------------------------------------------------------------------------------------
// A pass that failed stays off (retrying every frame turns a failure into a crash). Who caused it decides what can end it:
//   Model  - the model file was the cause (it is missing, would not initialise, refused to run): a new runtime revision gives the next
//            model its own chance, so a switch forgets it;
//   Global - anything else (the forwarder, the NGX core, the colour codec, a texture, the GPU at shutdown): a switch changes nothing.
// A Global failure is never replaced by, or erased through, a Model one. Every place that gives up goes through FailModel or FailGlobal;
// the source scan in tests/ext/nr/run_nr_runtime_wiring.ps1 fails when a site sets the latch any other way, or when a new site is not
// in its classification table.
enum class FailureKind
{
    None,
    Model,
    Global,
};

struct FailureState
{
    bool failed = false;
    FailureKind kind = FailureKind::None;
    std::string reason;
};

inline void FailGlobal(FailureState& s, std::string reason)
{
    s.failed = true;
    s.kind = FailureKind::Global;
    s.reason = std::move(reason);
}

inline void FailModel(FailureState& s, std::string reason)
{
    if (s.failed && s.kind == FailureKind::Global)
        return; // the first, non-model cause stays the reason
    s.failed = true;
    s.kind = FailureKind::Model;
    s.reason = std::move(reason);
}

// A switch: forget a failure the model caused. True when something was forgotten.
inline bool ForgetModelFailure(FailureState& s)
{
    if (!s.failed || s.kind != FailureKind::Model)
        return false;
    s = FailureState {};
    return true;
}

// The Retry button: forget everything.
inline void ClearAllFailures(FailureState& s) { s = FailureState {}; }

// ---- The shared capability block ----------------------------------------------------------------------------------------------
// All model copies are created and evaluated against the NGX core's one parameter block, and a module's callbacks are written into it
// by that module's PopulateParameters_Impl. So before a create or an evaluate on module X the block has to hold X's callbacks:
// EnsureBound runs the host's rebind when the block was last bound to another module (or never), and remembers a rebind only when it
// worked, so a failed one is tried again at the next call. Callers hold the pass mutex.
class BlockBinding
{
  public:
    bool Needs(const std::filesystem::path& module) const { return _bound != module; }
    void Note(const std::filesystem::path& module) { _bound = module; }
    void Forget() { _bound.clear(); }

  private:
    std::filesystem::path _bound;
};

template <class Rebind>
bool EnsureBound(BlockBinding& binding, const std::filesystem::path& module, Rebind&& rebind)
{
    if (!binding.Needs(module))
        return true;
    if (!rebind(module))
        return false;
    binding.Note(module);
    return true;
}

// When live switching is off for the rest of the session, and why (null: allowed). The hybrid FP4 precision caches its prepared weights
// per device, not per model file, so once it has been on the model stays where it is: turning the precision off again must not allow a
// switch that leaves the old weights behind. The AMD bridge loads its model once. Thread safe.
class SessionLock
{
  public:
    const char* Reason(bool hybridPrecisionNow, bool amdBridgeActive)
    {
        if (hybridPrecisionNow)
            _hybridSeen = true;
        if (_hybridSeen)
            return "the hybrid FP4 model precision was used in this session and keeps its prepared weights per device";
        if (amdBridgeActive)
            return "the AMD bridge loads its model once";
        return nullptr;
    }

  private:
    std::atomic<bool> _hybridSeen { false };
};

template <class Host>
SwitchResult ApplyRuntimeChange(Host& host, const Snapshot& snapshot, uint64_t& appliedRevision)
{
    if (snapshot.revision == appliedRevision)
        return SwitchResult::None;
    const bool first = appliedRevision == 0;
    host.ClearModelFailure();
    host.ParkFeatures();
    host.ClearHistory();
    host.DropReuseCaches();
    appliedRevision = snapshot.revision;
    if (snapshot.Available())
    {
        host.UseModel(snapshot);
        return first ? SwitchResult::Initial : SwitchResult::Switched;
    }
    host.MarkUnavailable(snapshot);
    return SwitchResult::Unavailable;
}
} // namespace ext::nr::runtime
