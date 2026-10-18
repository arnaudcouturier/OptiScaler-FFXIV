#pragma once
// Counters of the character preview feature, read by the menu card and the log lines. Relaxed atomics: they are
// statistics, not synchronisation.

#include <array>
#include <atomic>
#include <cstdint>

#include "ext/ffxiv/chara/CharaDecision.h"

namespace ext::ffxiv::chara
{
struct Stats
{
    // Classification (every immediate-context draw while the feature is on)
    std::atomic<uint64_t> classified { 0 };
    std::array<std::atomic<uint64_t>, 5> kind {}; // indexed by DrawKind
    std::atomic<uint64_t> finisherAccepted { 0 };
    std::atomic<uint64_t> finisherRejected { 0 };
    std::array<std::atomic<uint64_t>, kFinisherVerdicts> finisherVerdict {}; // indexed by FinisherVerdict (the check itself)
    std::atomic<uint64_t> finisherOffTable { 0 };   // finisher shader drawing outside the preview renderer's outputs (not a preview draw)
    std::atomic<uint64_t> finisherNotWanted { 0 };  // accepted finisher while PreviewDLSS is off (nothing further is expected)
    std::atomic<uint64_t> finisherNoDlss { 0 };     // accepted finisher while PreviewDLSS is on but the path is not available
    std::array<std::atomic<uint64_t>, 12> finishVerdict {}; // what the DLSS path did with an accepted finisher (CharaDlssRules.h FinishVerdict)
    std::atomic<uint64_t> depthOnlyTable { 0 };     // depth-only draws (no render target) into a depth texture of the table
    std::array<std::atomic<uint32_t>, 4> tagged {}; // shaders tagged so far: geometry, finisher, equipment display, UI
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
