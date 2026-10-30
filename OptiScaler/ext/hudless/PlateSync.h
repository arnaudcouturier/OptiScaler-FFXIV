#pragma once
// The two locks of the native nameplate engine and the rule that keeps them acyclic: the pass mutex serialises the
// presenting threads, one leaf mutex guards the slot table and the drawable record, and the only order is pass mutex
// then leaf mutex. Nothing that can call back into the plate code, take another lock or block runs while the leaf is
// held: the leaf covers slot bookkeeping, ID3D12Fence::GetCompletedValue and plain copies only.
//
// The test build compiles this header with PLATES_CHECKED_MUTEX and gets a mutex that reports a same-thread re-lock
// instead of deadlocking on it, counts the leaf locks the current thread holds, and gives every stand-in of a call the
// leaf may contain (a fake game-side source, the log, a D3D wrapper) a way to fail the test when the leaf is held.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#ifdef PLATES_CHECKED_MUTEX
#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <vector>
#endif

namespace ext::hudless::plates::sync
{
#ifdef PLATES_CHECKED_MUTEX
namespace detail
{
inline std::mutex& ReportMutex()
{
    static std::mutex mutex;
    return mutex;
}
inline std::vector<std::string>& Reports()
{
    static std::vector<std::string> reports;
    return reports;
}

inline void Report(const char* what)
{
    std::lock_guard lock(ReportMutex());
    Reports().push_back(what);
}

// Leaf locks the current thread holds: the one counter AssertNoLocksHeld looks at.
inline int& LeafDepth()
{
    static thread_local int depth = 0;
    return depth;
}
} // namespace detail

// A lock that reports a same-thread re-lock (instead of waiting for itself) and whose leaves are counted. Production
// uses std::mutex: these checks cost a thread id and a counter per lock, and the engine is on the present path.
class CheckedMutex
{
  public:
    explicit CheckedMutex(bool leaf) : leaf_(leaf) {}
    CheckedMutex(const CheckedMutex&) = delete;
    CheckedMutex& operator=(const CheckedMutex&) = delete;

    void lock()
    {
        const DWORD self = GetCurrentThreadId();
        // The one legal order is pass mutex then leaf: taking the pass mutex while this thread holds the leaf is the
        // inversion the design forbids.
        if (!leaf_ && detail::LeafDepth() > 0)
            detail::Report("pass mutex taken while the leaf is held");
        std::unique_lock<std::mutex> state(state_);
        if (owner_ == self)
        {
            detail::Report(leaf_ ? "leaf mutex re-locked by its owner" : "pass mutex re-locked by its owner");
            ++nested_;
            return;
        }
        free_.wait(state, [this] { return owner_ == 0; });
        owner_ = self;
        if (leaf_)
            ++detail::LeafDepth();
    }

    void unlock()
    {
        const DWORD self = GetCurrentThreadId();
        std::unique_lock<std::mutex> state(state_);
        if (owner_ == self && nested_ > 0)
        {
            --nested_; // the bogus acquisition above
            return;
        }
        if (owner_ != self)
        {
            detail::Report(leaf_ ? "leaf mutex unlocked by a thread that does not own it"
                         : "pass mutex unlocked by a thread that does not own it");
            return;
        }
        owner_ = 0;
        if (leaf_)
            --detail::LeafDepth();
        free_.notify_one();
    }

  private:
    bool leaf_;
    std::mutex state_;
    std::condition_variable free_;
    DWORD owner_ = 0;
    int nested_ = 0;
};

using Mutex = CheckedMutex;

// Called by every stand-in of a call the leaf mutex may contain: fails the test when this thread holds the leaf.
inline void AssertNoLocksHeld(const char* what)
{
    if (detail::LeafDepth() > 0)
        detail::Report(what);
}

inline int Violations()
{
    std::lock_guard lock(detail::ReportMutex());
    return static_cast<int>(detail::Reports().size());
}

inline std::string LastViolation()
{
    std::lock_guard lock(detail::ReportMutex());
    return detail::Reports().empty() ? std::string() : detail::Reports().back();
}

inline void ClearViolations()
{
    std::lock_guard lock(detail::ReportMutex());
    detail::Reports().clear();
}

// A point the tests can pause the engine at (the pass holding a pin and not yet done with it, a frame between its
// produce signal and its promote). Nothing is compiled in for the product: PLATES_TEST_POINT is a no-op there.
using TestPointHook = void (*)(const char* point);
inline std::atomic<TestPointHook>& TestPointSink()
{
    static std::atomic<TestPointHook> sink { nullptr };
    return sink;
}
inline void SetTestPoint(TestPointHook hook) { TestPointSink().store(hook, std::memory_order_relaxed); }
inline void TestPoint(const char* point)
{
    const TestPointHook hook = TestPointSink().load(std::memory_order_relaxed);
    if (hook != nullptr)
        hook(point);
}

// Every slot the engine hands out, with the number of pass reads pinned on it at that instant: a soak uses it to check
// that a slot a presenting thread is reading is never reserved. Nothing is compiled in for the product.
using ReserveHook = void (*)(int slot, unsigned pins);
inline std::atomic<ReserveHook>& ReserveSink()
{
    static std::atomic<ReserveHook> sink { nullptr };
    return sink;
}
inline void SetReserveHook(ReserveHook hook) { ReserveSink().store(hook, std::memory_order_relaxed); }
inline void ReservePoint(int slot, unsigned pins)
{
    const ReserveHook hook = ReserveSink().load(std::memory_order_relaxed);
    if (hook != nullptr)
        hook(slot, pins);
}

// A forced failure of one device-bound creation, for the rules that cannot be reached on a software adapter: neither a
// pass object nor a fence creation can be made to fail there, so the latches that keep a broken device from being retried
// on every present would stay untested. Each request is spent by the next attempt of its kind. Nothing is compiled in for
// the product.
inline std::atomic<unsigned>& ForcedPassBuilds()
{
    static std::atomic<unsigned> left { 0 };
    return left;
}
inline std::atomic<unsigned>& ForcedProduceFences()
{
    static std::atomic<unsigned> left { 0 };
    return left;
}
inline void FailPassBuilds(unsigned count) { ForcedPassBuilds().store(count, std::memory_order_relaxed); }
inline void FailProduceFences(unsigned count) { ForcedProduceFences().store(count, std::memory_order_relaxed); }
inline bool TakeForcedFailure(std::atomic<unsigned>& left)
{
    for (unsigned value = left.load(std::memory_order_relaxed); value > 0; value = left.load(std::memory_order_relaxed))
        if (left.compare_exchange_weak(value, value - 1, std::memory_order_relaxed))
            return true;
    return false;
}
#else
// The production build: a plain mutex. The flag only picks the diagnostic behaviour of the test build, so the engine
// source is the same in both.
class Mutex
{
  public:
    explicit Mutex(bool) {}
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

  private:
    std::mutex mutex_;
};

inline void AssertNoLocksHeld(const char*) {}
inline int Violations()
{
    return 0;
}
inline std::string LastViolation()
{
    return {};
}
inline void SetReserveHook(void (*)(int, unsigned)) {}
inline void FailPassBuilds(unsigned) {}
inline void FailProduceFences(unsigned) {}
#endif

#ifdef PLATES_CHECKED_MUTEX
#define PLATES_TEST_POINT(point) ::ext::hudless::plates::sync::TestPoint(point)
#define PLATES_TEST_RESERVE(slot, pins) ::ext::hudless::plates::sync::ReservePoint(slot, pins)
#define PLATES_TEST_FAIL_PASS_BUILD()                                                                                 \
    ::ext::hudless::plates::sync::TakeForcedFailure(::ext::hudless::plates::sync::ForcedPassBuilds())
#define PLATES_TEST_FAIL_PRODUCE_FENCE()                                                                               \
    ::ext::hudless::plates::sync::TakeForcedFailure(::ext::hudless::plates::sync::ForcedProduceFences())
#else
// Production: the test points do not exist.
#define PLATES_TEST_POINT(point) ((void) 0)
#define PLATES_TEST_RESERVE(slot, pins) ((void) 0)
#define PLATES_TEST_FAIL_PASS_BUILD() (false)
#define PLATES_TEST_FAIL_PRODUCE_FENCE() (false)
#endif

// One scope of either mutex. Named so both builds use the same type, and so the order is visible at every call site.
class Lock
{
  public:
    explicit Lock(Mutex& mutex) : mutex_(mutex) { mutex_.lock(); }
    ~Lock() { mutex_.unlock(); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

  private:
    Mutex& mutex_;
};
} // namespace ext::hudless::plates::sync
