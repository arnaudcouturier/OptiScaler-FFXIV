#pragma once

#include <cstdint>

namespace ext::core::diagnostic
{
// Opt-in, thread-local observer suppression for diagnostic private GPU work.
// This header deliberately owns no resources and performs no allocation/logging.
inline thread_local unsigned suppressionDepth = 0;
inline thread_local std::uint64_t suppressedCallbacks = 0;

class SuppressObserversScope
{
public:
    explicit SuppressObserversScope(bool active) noexcept : active_(active)
    {
        if (active_) ++suppressionDepth;
    }
    SuppressObserversScope(const SuppressObserversScope&) = delete;
    SuppressObserversScope& operator=(const SuppressObserversScope&) = delete;
    ~SuppressObserversScope() noexcept
    {
        if (active_) --suppressionDepth;
    }
private:
    bool active_;
};

inline bool ObserversSuppressed() noexcept { return suppressionDepth != 0; }
inline std::uint64_t TotalSuppressedCallbacks() noexcept { return suppressedCallbacks; }
inline void NoteSuppressedCallback() noexcept { ++suppressedCallbacks; }
}
