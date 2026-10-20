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
