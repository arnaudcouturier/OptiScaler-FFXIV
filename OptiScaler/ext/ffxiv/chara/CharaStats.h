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
    std::atomic<uint64_t> finisherShapeOnly { 0 };  // draws with only the finisher shape while the tagged finisher shader is known
    std::atomic<uint64_t> finisherOffTable { 0 };   // finisher shader drawing outside the preview renderer's outputs (not a preview draw)
    std::atomic<uint64_t> finisherNotWanted { 0 };  // accepted finisher while PreviewDLSS is off (nothing further is expected)
    std::atomic<uint64_t> finisherNoDlss { 0 };     // accepted finisher while PreviewDLSS is on but the path is not available
    std::array<std::atomic<uint64_t>, 12> finishVerdict {}; // what the DLSS path did with an accepted finisher (CharaDlssRules.h FinishVerdict)
    std::atomic<uint64_t> depthOnlyTable { 0 };     // depth-only draws (no render target) into a depth texture of the table
    std::array<std::atomic<uint32_t>, 4> tagged {}; // shaders tagged so far: geometry, finisher, equipment display, UI
    std::atomic<uint64_t> passes { 0 };
    std::atomic<uint64_t> passesLive { 0 };        // passes of the DLSS path that were jittered and replayed (a preview slot existed)
    std::atomic<uint64_t> slotsAcquired { 0 };     // preview slots claimed by a UI draw
    std::atomic<uint64_t> consumerSwaps { 0 };     // UI draws that got the processed (composed) preview image
    std::atomic<uint64_t> frames { 0 };
    std::atomic<uint32_t> windowsProcessed { 0 }; // preview windows the processed set holds (the budget)
    std::atomic<uint32_t> windowsMax { 0 };       // ... and how many it holds at most

    // Spatial AA
    std::atomic<uint64_t> equipShaderSeen { 0 }; // equipment display shader drawn on the immediate context
    std::atomic<uint64_t> equipMatched { 0 };    // ... and its t0 is table entry 0
    std::atomic<uint64_t> aaProcessed { 0 };     // draws that got the filtered image
    std::atomic<uint64_t> aaSkipped { 0 };       // matched but left native (unsupported format/size, resource failure)
    std::atomic<uint64_t> deferredSeen { 0 };    // equipment display shader drawn on a deferred context (left native)

    // Debug aids
    std::atomic<uint64_t> tintApplied { 0 };     // processed preview images that were made with the debug tint (PreviewDebugTint): composed images (magenta, or cyan for NR) and bring-up NR results (cyan)
    std::atomic<uint64_t> floodSwaps { 0 };      // draws that were given the solid magenta image (PreviewDebugFlood)
    std::atomic<uint64_t> nrSwaps { 0 };         // UI draws that were given the preview NR result
    std::atomic<uint64_t> nrRuns { 0 };          // finisher outputs the dispatcher handed to preview NR that produced a result (bring-up)
    std::atomic<uint64_t> nrProductionRuns { 0 }; // super-resolution outputs preview NR improved and the alpha pass used (production)
    std::atomic<uint64_t> nrProductionSwaps { 0 }; // UI draws that were given a composed image made from the production NR output
    std::atomic<uint64_t> srSourceResets { 0 };    // DLSS history resets given for a change of the evaluate's colour source (NR before DLSS <-> plain), at most one per slot and 500 ms

    // Preview probe (PreviewProbe). Written by the draw thread, read by the menu.
    std::atomic<uint64_t> probeScanned { 0 };       // draws whose first eight pixel-shader resources were compared with the preview textures
    std::atomic<uint64_t> probeSamplerDraws { 0 };  // ... of which sampled a preview or preview-derived texture
    std::atomic<uint64_t> probeCopies { 0 };        // copies / resolves whose source was a preview or preview-derived texture
    std::atomic<uint64_t> probeCopiesRefused { 0 }; // ... of which could not be followed (chain too long, destination is a preview output, tracking off)
    std::atomic<uint64_t> probeDerivedAdded { 0 };  // textures newly added to the derived set (copy destinations and render targets of sampler draws)
    std::atomic<uint32_t> probeDerivedLive { 0 };   // derived textures alive now
    std::atomic<uint32_t> probeCopyLines { 0 };     // copy cases written to the log
    std::atomic<uint32_t> probeSamplerLines { 0 };  // sampler-draw cases written to the log
    std::atomic<uint32_t> probeSwapLines { 0 };     // consumer swap traces written to the log
};

inline Stats g_stats;
} // namespace ext::ffxiv::chara
