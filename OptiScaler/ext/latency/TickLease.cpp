#include "pch.h"

#include "ext/latency/TickLease.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/ActiveBuild.h"
#include "ext/latency/TickProbe.h"

#include <State.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

// The one write of the latency work: the game's Framework vtable slot, replaced by an observer that calls the old pointer (ext/latency/TickLease.h
// for the surface, ext/latency/TickLeaseCore.h for every rule and its tests).
//
// Nothing in this file patches code. The checks of TickLeaseCore.h run first, the compare-exchange runs second, and a failed check or a lost exchange
// leaves the game exactly as it was. With the key off this file's only reachable code is the mirrored key load per bridge present.
namespace ext::latency
{
namespace
{
using Clock = std::chrono::steady_clock;

std::int64_t NowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

enum class Status : std::uint8_t
{
    Idle,      // no attempt yet, or a NotReady attempt
    Installed, // our observer sits in the slot
    Refused,   // an identity check said no: nothing was written
    Restored,  // the unhook path put the old pointer back
    Pinned,    // the unhook path found the slot holding something else: the module stays mapped
};

std::atomic<Status> g_status { Status::Idle };
std::atomic<bool> g_attempting { false };
// The function the observer calls. Published BEFORE the slot exchange and never cleared while the slot can still point at the observer: a tick that
// takes the slot the instant the exchange lands must find a callable original. (It was null for a few microseconds after the exchange before this
// round, and the observer then returned the game's own teardown value without running the tick.)
std::atomic<void*> g_original { nullptr };
// The slot's canonical value as the install verified it: a fallback no observer can reach after a successful install, and the only reason the
// observer has no null branch.
std::atomic<void*> g_canonical { nullptr };
std::atomic<std::uintptr_t> g_frameCounterAt { 0 }; // absolute address of the Framework's Framecounter, 0 when its offset did not resolve

Attempts g_attempts;

void* WinLoad(void* slot) { return *static_cast<void* volatile*>(slot); }
bool WinExchange(void* slot, void* expected, void* value)
{
    return InterlockedCompareExchangePointer(static_cast<void* volatile*>(slot), value, expected) == expected;
}

// The kind of memory a pointer may lie in: the game image's own data (the global slot and the vtable) or, for the Framework object itself, the heap
// as well. Everything else is refused before it is dereferenced.
enum class MemoryKind : std::uint8_t
{
    ImageData,
    ImageOrHeap,
};

// True when [p, p + size) is committed, readable, not guarded, and lies in the kind of memory asked for, inside a region that covers the whole span.
// Walks the regions because one span can cross a page, and the queried region is not required to be the first one. This is the check in front of every
// dereference of a pointer the anchor family supplied.
bool ReadableSpan(const void* p, std::size_t size, std::uintptr_t imageBase, MemoryKind kind)
{
    if (p == nullptr || size == 0)
        return false;
    auto at = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t end = at + size;
    if (at < 0x10000 || end < at)
        return false;
    while (at < end)
    {
        MEMORY_BASIC_INFORMATION mbi {};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return false;
        const bool imageData = mbi.Type == MEM_IMAGE && reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) == imageBase;
        const bool heap = kind == MemoryKind::ImageOrHeap && mbi.Type == MEM_PRIVATE;
        if (!imageData && !heap)
            return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= at)
            return false;
        at = regionEnd;
    }
    return true;
}

// True when [p, p + size) lies in a readable, non-writable MEM_IMAGE region of this process's main module: the game's own vtable, in a read-only
// section, and not a heap copy of it. The span is required to fit inside the queried region(s) too (a slot near the end of a section cannot be read
// past it).
bool ReadOnlyImageSpan(const void* p, std::size_t size, std::uintptr_t imageBase)
{
    if (p == nullptr || size == 0)
        return false;
    auto at = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t end = at + size;
    if (at < 0x10000 || end < at)
        return false;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (at < end)
    {
        MEMORY_BASIC_INFORMATION mbi {};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE || reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != imageBase)
            return false;
        if ((mbi.Protect & kWritable) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0 || (mbi.Protect & PAGE_GUARD) != 0)
            return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= at)
            return false;
        at = regionEnd;
    }
    return true;
}

// The 16 bytes at the tick, so a foreign inline hook over the entry (a jump where `push rbx` belongs) is visible in the log next to the values the
// lease verified. Read from the game's own image, never executed.
std::string EntryBytes(std::uintptr_t address)
{
    std::uint8_t bytes[16] {};
    std::memcpy(bytes, reinterpret_cast<const void*>(address), sizeof(bytes));
    char buf[3 * sizeof(bytes) + 1] {};
    for (std::size_t i = 0; i < sizeof(bytes); ++i)
        std::snprintf(buf + 3 * i, sizeof(buf) - 3 * i, "%02X ", bytes[i]);
    return std::string(buf);
}

// What the unhook path needs: the slot that was written. Nothing else is kept.
struct Target
{
    void** slot = nullptr;
};

std::atomic<Target*> g_target { nullptr }; // never freed: a tick may still be running at unload

// The leased function. `this` is the Framework object and the result is returned in al: the signature of the virtual function it replaces (x64 has
// one calling convention). Every path calls the original: the pass-through (shutdown, or the parked state the controller will set) runs it without
// recording, the recording path brackets it.
bool Observer(void* framework)
{
    const auto original = reinterpret_cast<TickFn>(g_original.load(std::memory_order_acquire));
    const TickFn call = original != nullptr ? original : reinterpret_cast<TickFn>(g_canonical.load(std::memory_order_relaxed));
    if (PassThrough(State::Instance().isShuttingDown, /*parked*/ false))
        return call(framework);
    // The probe records only once it is armed, which happens at the first bridge present after this install. A tick that was already running at
    // that moment is passed through here, not half-recorded: the begin-only tap would have no end and the core drops a lone end.
    if (!Armed())
        return call(framework);

    const std::uintptr_t counterAt = g_frameCounterAt.load(std::memory_order_relaxed);
    const bool frameCounter = counterAt != 0;
    std::uint32_t before = 0;
    std::uint32_t after = 0;
    if (frameCounter)
        before = *reinterpret_cast<const volatile std::uint32_t*>(counterAt);
    OnTickBegin(before, frameCounter);
    const bool result = call(framework);
    if (frameCounter)
        after = *reinterpret_cast<const volatile std::uint32_t*>(counterAt);
    OnTickEnd(after);
    return result;
}

// One install attempt. Returns the verdict so the caller can decide between a retry, a refusal and a stop.
LeaseVerdict Attempt()
{
    // Read once per attempt: the gate has published by now (the first device hook), and the mirrored key being off means no attempt at all.
    if (!KeyOn())
        return LeaseVerdict::NotReady;
    const ext::ffxiv::GameBuild* build = ext::ffxiv::BuildFor(ext::ffxiv::resolve::Feature::LatencyTick);
    if (build == nullptr || build->latency.tickRva == 0 || build->latency.frameworkInstanceRva == 0)
        return LeaseVerdict::NotReady;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0)
        return LeaseVerdict::NotReady;

    // Which telemetry offsets resolved: only those may be compared or read (an unresolved one keeps the table default, which describes no known
    // layout, so reading it would be a blind read and comparing it would refuse a lease for a reason that is not about the executable).
    const std::uint8_t resolved = ext::ffxiv::ActiveGate().latencyOffsetsResolved;

    LeaseCheck check;
    check.canonicalTick = reinterpret_cast<const void*>(base + build->latency.tickRva);
    check.resolvedOffsets = resolved;
    check.frameCounterOffset = build->latency.frameCounterOffset;
    check.perfCounterValueOffset = build->latency.perfCounterValueOffset;
    check.perfCounterFreqOffset = build->latency.perfCounterFreqOffset;
    check.layoutConsistent = LayoutConsistent(resolved, check.frameCounterOffset, check.perfCounterValueOffset, check.perfCounterFreqOffset);

    // 1. the image global that holds the Framework pointer, before it is read
    const auto* instanceAt = reinterpret_cast<const void*>(base + build->latency.frameworkInstanceRva);
    check.instanceSlotReadable = ReadableSpan(instanceAt, sizeof(void*), base, MemoryKind::ImageData);
    if (!check.instanceSlotReadable)
        return CheckLease(check);
    const void* instance = *reinterpret_cast<const void* const*>(instanceAt);
    check.instance = instance;
    if (instance == nullptr)
        return CheckLease(check); // not published yet: a retry, not a refusal

    // 2. the Framework object, readable for the widest read the observer will make of it (the frame counter), before it is dereferenced
    const std::size_t instanceSpan = (resolved & ext::ffxiv::resolve::kLatencyFrameCounterResolved) != 0
                                         ? static_cast<std::size_t>(check.frameCounterOffset) + sizeof(std::uint32_t)
                                         : sizeof(void*);
    check.instanceReadable = ReadableSpan(instance, instanceSpan, base, MemoryKind::ImageOrHeap);
    if (!check.instanceReadable)
        return CheckLease(check);
    const void* vtable = *static_cast<const void* const*>(instance);
    check.vtable = vtable;
    if (vtable == nullptr)
        return CheckLease(check); // NotReady

    // 3. the slot the write will touch, before the identity check dereferences it
    check.vtableReadable = ReadableSpan(vtable, 5 * sizeof(void*), base, MemoryKind::ImageData);
    check.vtableInReadOnlyImage = check.vtableReadable && ReadOnlyImageSpan(vtable, 5 * sizeof(void*), base);
    if (CheckLease(check) != LeaseVerdict::Ready)
        return CheckLease(check);

    auto* slot = const_cast<void**>(static_cast<void* const*>(vtable) + 4);

    // 4. pin this module before the write: a tick can be running on the game thread while the process unloads, and a pin that failed must not leave a
    // slot pointing into an unloadable module.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Observer), &self))
        return LeaseVerdict::NotPinned;

    // 5. everything the observer reads is published BEFORE the slot can hold its address. Nothing is cleared later except on a failed exchange, when
    // the slot never held the observer and no tick can reach it.
    auto* target = new Target { slot };
    g_target.store(target, std::memory_order_release);
    g_canonical.store(const_cast<void*>(check.canonicalTick), std::memory_order_relaxed);
    g_frameCounterAt.store((resolved & ext::ffxiv::resolve::kLatencyFrameCounterResolved) != 0
                               ? reinterpret_cast<std::uintptr_t>(instance) + check.frameCounterOffset
                               : 0,
                           std::memory_order_relaxed);
    g_original.store(const_cast<void*>(check.canonicalTick), std::memory_order_release);

    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous))
    {
        g_original.store(nullptr, std::memory_order_release);
        g_target.store(nullptr, std::memory_order_release);
        return LeaseVerdict::NotWritable;
    }
    const SlotOps ops { slot, &WinLoad, &WinExchange };
    void* originalValue = nullptr;
    const InstallOutcome outcome = LeaseSlot(ops, const_cast<void*>(check.canonicalTick), reinterpret_cast<void*>(&Observer), originalValue);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    if (outcome != InstallOutcome::Installed)
    {
        g_original.store(nullptr, std::memory_order_release);
        g_target.store(nullptr, std::memory_order_release);
        return LeaseVerdict::SlotMismatch;
    }

    char line[256];
    std::snprintf(line, sizeof(line), "installed instance=%p vtable=%p slot=%p original=%p order=slot-first counter=%s entry=%s", instance, vtable,
                  static_cast<void*>(slot), originalValue, (resolved & ext::ffxiv::resolve::kLatencyFrameCounterResolved) != 0 ? "on" : "n/a",
                  EntryBytes(base + build->latency.tickRva).c_str());
    Note(line);
    return LeaseVerdict::Ready;
}

// Runs one attempt unless another thread is already in one, and records the outcome in the retry plan.
void TryInstall()
{
    if (g_status.load(std::memory_order_relaxed) == Status::Installed || g_attempts.Settled())
        return;
    if (!KeyOn())
        return;
    bool expected = false;
    if (!g_attempting.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    const LeaseVerdict verdict = Attempt();
    const bool installed = verdict == LeaseVerdict::Ready;
    const bool refused = !installed && verdict != LeaseVerdict::NotReady;
    if (installed)
        g_status.store(Status::Installed, std::memory_order_release);
    else if (refused)
    {
        g_status.store(Status::Refused, std::memory_order_release);
        Note((std::string("refused: ") + VerdictName(verdict)).c_str());
    }
    g_attempts.Record(installed, refused, NowUs());
    g_attempting.store(false, std::memory_order_release);
}
} // namespace

void OnDeviceHooked()
{
    // The config load already mirrored the key; reading it again here costs one ini read at startup and covers a load that ran before the hook was
    // registered.
    RefreshKeys();
    if (!KeyOn())
        return;
    // The key is on but the build's tick is not enabled: say so once, with the gate's own reason, instead of retrying for ten minutes.
    if (ext::ffxiv::BuildFor(ext::ffxiv::resolve::Feature::LatencyTick) == nullptr)
    {
        const std::string reason = ext::ffxiv::ActiveGate().Reason(ext::ffxiv::resolve::Feature::LatencyTick);
        Note(("unavailable: " + reason).c_str());
        return;
    }
    TryInstall();
}

void OnBridgePresent()
{
    // The key is the first thing read: with it off this seam is one relaxed load and nothing else - no key read, no counter, no attempt bookkeeping.
    if (!KeyOn())
        return;
    g_attempts.Present();
    const Status status = g_status.load(std::memory_order_relaxed);
    if (status == Status::Installed || status == Status::Restored || status == Status::Pinned)
        return;
    if (g_attempts.Due(NowUs())) // a refusal retries like a missing instance does; the plan stops after kRefusalCap refusals
        TryInstall();
}

bool ReadFrameCounter(std::uint32_t& value)
{
    const std::uintptr_t at = g_frameCounterAt.load(std::memory_order_relaxed);
    if (at == 0)
        return false;
    value = *reinterpret_cast<const volatile std::uint32_t*>(at);
    return true;
}

void OnUnload()
{
    Target* target = g_target.load(std::memory_order_acquire);
    if (target == nullptr)
        return;
    DWORD previous = 0;
    if (!VirtualProtect(target->slot, sizeof(void*), PAGE_READWRITE, &previous))
    {
        g_status.store(Status::Pinned, std::memory_order_release);
        Note("not restored: the slot could not be made writable, the module stays mapped");
        return;
    }
    // g_original is deliberately NOT cleared: the module is pinned, and a stale pointer to the game's own tick is harmless where a null one would not
    // be if a tick raced this restore.
    const SlotOps ops { target->slot, &WinLoad, &WinExchange };
    const RestoreOutcome outcome = RestoreSlot(ops, reinterpret_cast<void*>(&Observer), g_original.load(std::memory_order_acquire));
    DWORD ignored = 0;
    VirtualProtect(target->slot, sizeof(void*), previous, &ignored);
    if (outcome == RestoreOutcome::Restored)
    {
        g_status.store(Status::Restored, std::memory_order_release);
        Note("restored");
    }
    else
    {
        // Another owner chained over the observer: leave its pointer alone, the module stays mapped (it was pinned at install).
        g_status.store(Status::Pinned, std::memory_order_release);
        Note("not restored: the slot no longer holds the probe's observer, the module stays mapped");
    }
}

bool Leased() { return g_status.load(std::memory_order_acquire) == Status::Installed; }

const char* StateText()
{
    switch (g_status.load(std::memory_order_acquire))
    {
    case Status::Idle:
        return "not installed";
    case Status::Installed:
        return "installed";
    case Status::Refused:
        return "refused";
    case Status::Restored:
        return "restored";
    case Status::Pinned:
        return "the slot was taken back by another owner, the module stays mapped";
    }
    return "?";
}
} // namespace ext::latency
