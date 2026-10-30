#pragma once
// Rules of the simulation-tick lease, with no Windows header and no game access: what may be leased, what the observer skips, what the release
// restores, and when another attempt is due. The product side (ext/latency/TickLease.cpp) supplies the memory operations and the clock, so every
// rule here is unit-tested on an ordinary variable (tests/ext/latency/tick_lease.cpp).
//
// THE RULE: the lease writes one pointer-sized slot inside a read-only image table, so every check runs BEFORE the write and any doubt refuses:
//   * the pointers the install is about to read must be committed and readable (the image's global slot in the game image, the Framework object in
//     image or heap memory, the vtable slot in read-only image data): a pointer to unmapped memory is refused before it is dereferenced,
//   * the vtable pointer of the Framework object must lie in read-only image data (a heap copy or a rotated table is refused),
//   * the slot must already hold exactly image base + the resolved tick RVA (another tool's hook, a moved slot, a build whose Tick is a different
//     function: refused),
//   * the counter offsets, when they resolved, must have the layout the tick's own instructions have. Offsets that did not resolve are skipped, so
//     a telemetry-only seam can never refuse the install, and an offset that did not resolve is never read. Among the resolved ones the layout is
//     the only check that they belong together, and a mismatch means one of them is not what its instruction read.
// A refusal writes nothing, so the game keeps running exactly as it did; the caller logs once and retries within the cap.
//
// THE FALLBACK IS "DO NOTHING": with the key off nothing is installed, nothing is read and the observer never exists; a refusal, a missing seam or
// a disabled feature all end in the same state as a build without this code. The probe's own log line says why.

#include "ext/ffxiv/resolve/GateTypes.h"

#include <atomic>
#include <cstdint>

namespace ext::latency
{
// The Framework tick: a virtual function of the Framework object, so `this` is the only argument and the result is returned in al.
using TickFn = bool (*)(void* framework);

// The layout the tick's instructions encode inside the Framework object: Framecounter is 0x20 above the last QueryPerformanceCounter value and
// that is 8 above the counter frequency.
inline constexpr std::uint32_t kFrameCounterGap = 0x20;
inline constexpr std::uint32_t kPerfCounterGap = 0x8;

// ---- the install checks ------------------------------------------------------------------------------------------------------------------

enum class LeaseVerdict : std::uint8_t
{
    Ready,          // the slot holds the canonical tick: lease it
    NotReady,       // the Framework instance is not published yet: retry, this is not a refusal
    NotReadable,    // a pointer the install must read is not committed, readable memory of the right kind: refuse before dereferencing it
    NotReadOnly,    // the vtable pointer is not in read-only image data: refuse
    SlotMismatch,   // the slot does not hold the canonical tick: refuse
    LayoutMismatch, // two resolved counter offsets contradict each other: refuse
    NotWritable,    // the slot could not be made writable (a tool watches the page protection): refuse
    NotPinned,      // the module could not be pinned: refuse rather than leave a tick calling into an unloadable module
};

// The three offsets are telemetry, and each is used only when its seam resolved (`resolvedOffsets`, the bits of the gate report). An offset that did
// not resolve is never read; a build that resolved only some of them still gets a lease.
struct LeaseCheck
{
    const void* instance = nullptr;      // *(void**) (image base + frameworkInstanceRva)
    const void* vtable = nullptr;        // *(void* const*) instance
    const void* canonicalTick = nullptr; // image base + tickRva
    std::uint8_t resolvedOffsets = 0;    // which of the three offsets resolved (kLatency*Resolved in resolve/GateTypes.h)
    std::uint32_t frameCounterOffset = 0;      // the value decoded for Framecounter (read only when its bit is set)
    std::uint32_t perfCounterValueOffset = 0;  // the value decoded for the last QueryPerformanceCounter value
    std::uint32_t perfCounterFreqOffset = 0;   // the value decoded for the QueryPerformanceCounter frequency
    bool instanceSlotReadable = true;    // the image global holding the instance pointer is committed, readable image data of this module
    bool instanceReadable = true;        // the Framework object is committed and readable for the counter offset (image data or heap)
    bool vtableReadable = true;          // the eight bytes of slot 4 are committed and readable
    bool vtableInReadOnlyImage = false;  // the vtable pointer lies in a readable, non-writable section of the game image
    bool layoutConsistent = true;        // the offsets that resolved have the layout above
};

// The layout the tick's own instructions encode, checked only between offsets that resolved: a pair with an unresolved member is not compared (the
// resolver's sanity range is all that vouches for it, and it is read at most as telemetry). `resolved` is the mask above.
inline bool LayoutConsistent(std::uint8_t resolved, std::uint32_t frameCounterOffset, std::uint32_t perfCounterValueOffset,
                             std::uint32_t perfCounterFreqOffset)
{
    const bool haveFrameCounter = (resolved & ext::ffxiv::resolve::kLatencyFrameCounterResolved) != 0;
    const bool havePerfValue = (resolved & ext::ffxiv::resolve::kLatencyPerfValueResolved) != 0;
    const bool havePerfFreq = (resolved & ext::ffxiv::resolve::kLatencyPerfFreqResolved) != 0;
    if (haveFrameCounter && havePerfValue && frameCounterOffset != perfCounterValueOffset + kFrameCounterGap)
        return false;
    if (havePerfValue && havePerfFreq && perfCounterValueOffset != perfCounterFreqOffset + kPerfCounterGap)
        return false;
    return true;
}

// The all-resolved form the table's own values are checked with (both real builds).
inline bool LayoutConsistent(std::uint32_t frameCounterOffset, std::uint32_t perfCounterValueOffset, std::uint32_t perfCounterFreqOffset)
{
    return LayoutConsistent(ext::ffxiv::resolve::kLatencyAllOffsetsResolved, frameCounterOffset, perfCounterValueOffset, perfCounterFreqOffset);
}

inline LeaseVerdict CheckLease(const LeaseCheck& c)
{
    if (!c.instanceSlotReadable || !c.instanceReadable)
        return LeaseVerdict::NotReadable; // the pointer is there but the memory behind it is not: never dereference it
    if (c.instance == nullptr)
        return LeaseVerdict::NotReady;
    if (c.vtable == nullptr)
        return LeaseVerdict::NotReady;
    if (!c.vtableReadable)
        return LeaseVerdict::NotReadable;
    if (!c.layoutConsistent)
        return LeaseVerdict::LayoutMismatch;
    if (!c.vtableInReadOnlyImage)
        return LeaseVerdict::NotReadOnly;
    if (c.canonicalTick == nullptr)
        return LeaseVerdict::NotReady;
    const void* const* slots = static_cast<const void* const*>(c.vtable);
    if (slots[4] != c.canonicalTick)
        return LeaseVerdict::SlotMismatch;
    return LeaseVerdict::Ready;
}

inline const char* VerdictName(LeaseVerdict v)
{
    switch (v)
    {
    case LeaseVerdict::Ready:
        return "ready";
    case LeaseVerdict::NotReady:
        return "the framework instance is not there yet";
    case LeaseVerdict::NotReadable:
        return "a pointer the lease must read is not committed, readable memory of the expected kind";
    case LeaseVerdict::NotReadOnly:
        return "the vtable is not in read-only image data (a copy, not the game's table)";
    case LeaseVerdict::SlotMismatch:
        return "the vtable slot does not hold this build's tick";
    case LeaseVerdict::LayoutMismatch:
        return "the resolved instance offsets do not have the layout the tick's code encodes";
    case LeaseVerdict::NotWritable:
        return "the vtable slot could not be made writable";
    case LeaseVerdict::NotPinned:
        return "the module could not be pinned, so the observer could outlive it";
    }
    return "?";
}

// The observer's pass-through: while the process shuts down or the lease is parked, the old function runs and nothing is recorded. Both are
// one relaxed load away on the tick path. The probe has no park state in this build (parking arrives with the controller the probe is meant to
// justify); the rule stays here so that state machine is the one tested rule it will call.
inline bool PassThrough(bool shuttingDown, bool parked) { return shuttingDown || parked; }

// ---- the slot write ----------------------------------------------------------------------------------------------------------------------

// The two pointer operations the lease needs, as a policy: the product passes VirtualProtect plus InterlockedCompareExchangePointer, a test passes
// the two functions below on an ordinary variable.
struct SlotOps
{
    void* slot = nullptr;                                     // the address of the pointer-sized slot
    void* (*load)(void* slot) = nullptr;                      // the slot's current value
    bool (*exchange)(void* slot, void* expected, void* value) = nullptr; // compare-exchange, true when it happened
};

// A plain load and a compare-exchange on a local variable, for tests and for a first look without touching page protection.
inline void* PlainLoad(void* slot) { return *static_cast<void**>(slot); }
inline bool PlainExchange(void* slot, void* expected, void* value)
{
    void** p = static_cast<void**>(slot);
    if (*p != expected)
        return false;
    *p = value;
    return true;
}

enum class InstallOutcome : std::uint8_t
{
    Installed,
    NotCanonical,   // the slot no longer holds the canonical tick (someone else wrote it since the check)
    ExchangeFailed, // the compare-exchange did not happen
};

enum class RestoreOutcome : std::uint8_t
{
    Restored,
    ForeignOwner, // the slot holds something other than our observer: it is left alone
};

// Puts `observer` over `canonical`. `original` receives the value the slot held, which is the function the observer calls.
inline InstallOutcome LeaseSlot(const SlotOps& ops, void* canonical, void* observer, void*& original)
{
    original = nullptr;
    if (ops.slot == nullptr || ops.load == nullptr || ops.exchange == nullptr)
        return InstallOutcome::ExchangeFailed;
    if (ops.load(ops.slot) != canonical)
        return InstallOutcome::NotCanonical;
    if (!ops.exchange(ops.slot, canonical, observer))
        return InstallOutcome::ExchangeFailed;
    original = canonical;
    return InstallOutcome::Installed;
}

// Puts `original` back, but only while the slot still holds our observer: a tool that chained over us keeps the slot, and the caller then keeps
// this module mapped (ext/latency/TickLease.cpp).
inline RestoreOutcome RestoreSlot(const SlotOps& ops, void* observer, void* original)
{
    if (ops.slot == nullptr || ops.load == nullptr || ops.exchange == nullptr)
        return RestoreOutcome::ForeignOwner;
    if (ops.load(ops.slot) != observer)
        return RestoreOutcome::ForeignOwner;
    if (!ops.exchange(ops.slot, observer, original))
        return RestoreOutcome::ForeignOwner;
    return RestoreOutcome::Restored;
}

// ---- when another attempt is due ---------------------------------------------------------------------------------------------------------

inline constexpr std::uint32_t kRetryPresents = 120;             // an attempt is due every 120 bridge presents
inline constexpr std::int64_t kRetryWindowUs = 600'000'000;      // ... for at most 10 minutes after the first one
inline constexpr std::uint32_t kRefusalCap = 8;                  // a refusal is not "not ready yet": after this many, stop for the session

// The retry plan. The first attempt is due at once (the first device hook); then one per kRetryPresents presents, for at most kRetryWindowUs, and
// never again after kRefusalCap refusals or a successful install. The present thread advances it, the device-hook and present threads may record an
// outcome, and Due() can be read while an attempt finishes, so every member is atomic: the reads may interleave (a present counted a moment late),
// they never tear.
class Attempts
{
  public:
    bool Due(std::int64_t nowUs) const
    {
        if (_settled.load(std::memory_order_acquire))
            return false;
        const bool attempted = _attempted.load(std::memory_order_acquire);
        if (attempted && nowUs - _firstUs.load(std::memory_order_relaxed) >= kRetryWindowUs)
            return false;
        return !attempted || _presents.load(std::memory_order_relaxed) - _presentsAtAttempt.load(std::memory_order_relaxed) >= kRetryPresents;
    }

    void Present() { _presents.fetch_add(1, std::memory_order_relaxed); }

    // `installed`: the lease is in place. `refused`: an identity check said no (anything but NotReady). A NotReady attempt keeps the plan running
    // without counting against the cap.
    void Record(bool installed, bool refused, std::int64_t nowUs)
    {
        if (!_attempted.exchange(true, std::memory_order_acq_rel))
            _firstUs.store(nowUs, std::memory_order_relaxed);
        _presentsAtAttempt.store(_presents.load(std::memory_order_relaxed), std::memory_order_relaxed);
        const std::uint32_t refusals = refused ? _refusals.fetch_add(1, std::memory_order_relaxed) + 1 : _refusals.load(std::memory_order_relaxed);
        if (installed || refusals >= kRefusalCap || nowUs - _firstUs.load(std::memory_order_relaxed) >= kRetryWindowUs)
            _settled.store(true, std::memory_order_release);
    }

    bool Settled() const { return _settled.load(std::memory_order_acquire); }
    std::uint32_t Refusals() const { return _refusals.load(std::memory_order_relaxed); }

  private:
    std::atomic<std::uint64_t> _presents { 0 };
    std::atomic<std::uint64_t> _presentsAtAttempt { 0 };
    std::atomic<std::uint32_t> _refusals { 0 };
    std::atomic<std::int64_t> _firstUs { 0 };
    std::atomic<bool> _attempted { false };
    std::atomic<bool> _settled { false };
};
} // namespace ext::latency
