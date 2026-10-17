#pragma once
// Counters of the character preview feature, read by the menu card and the log lines. Relaxed atomics: they are
// statistics, not synchronisation.

#include <array>
#include <atomic>
#include <cstdint>

namespace ext::ffxiv::chara
{
struct Stats
{
    // Classification (every immediate-context draw while the feature is on)
    std::atomic<uint64_t> classified { 0 };
    std::array<std::atomic<uint64_t>, 5> kind {}; // indexed by DrawKind
    std::atomic<uint64_t> finisherAccepted { 0 };
    std::atomic<uint64_t> finisherRejected { 0 };
    std::atomic<uint64_t> passes { 0 };
    std::atomic<uint64_t> frames { 0 };

    // Spatial AA
    std::atomic<uint64_t> equipShaderSeen { 0 }; // equipment display shader drawn on the immediate context
    std::atomic<uint64_t> equipMatched { 0 };    // ... and its t0 is table entry 0
    std::atomic<uint64_t> aaProcessed { 0 };     // draws that got the filtered image
    std::atomic<uint64_t> aaSkipped { 0 };       // matched but left native (unsupported format/size, resource failure)
    std::atomic<uint64_t> deferredSeen { 0 };    // equipment display shader drawn on a deferred context (left native)
};

inline Stats g_stats;
} // namespace ext::ffxiv::chara
