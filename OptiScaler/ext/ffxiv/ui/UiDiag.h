#pragma once
// S10 package A: the D1 diagnostic outputs (docs/HUDLESS.md section 12) under <game>\OptiScaler\hudless-diag\<run>\.
// Observation only: nothing here changes a draw, a copy or a Present. All file work happens on one writer thread; the
// render thread only formats and enqueues. Every file has a byte budget and a TRUNCATED marker.
//
//   probe.txt   ReShade / Dalamud position (call chain, vtable owners, module list)
//   shaders.csv per distinct pixel shader (rewritten every 1800 frames)
//   frames.txt  first 600 frames, then every 100th, plus rate-limited anomaly frames
//   cost.txt    draws per frame, hook cost, early-out cost (rewritten every 600 frames)
//   rt.txt      render target at each candidate vs the backbuffer, contexts seen (rewritten every 600 frames)
//   snap-*      menu-button snapshots (UiSnapshot.cpp)

#include "ext/ffxiv/ui/UiBoundaryCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ext::ffxiv::ui::diag
{
struct Status
{
    bool running = false; // run folder created
    std::string runDir;
    uint64_t frames = 0;
    double drawsPerFrame = 0, hookUsPerFrame = 0, hookNsPerDraw = 0;
    uint64_t shadersAfterHook = 0;
    uint32_t preHookShaders = 0;
    uint64_t framesR1 = 0, framesR2 = 0, framesSame = 0, framesNone = 0, framesWithScene = 0;
    uint64_t droppedJobs = 0;
    uint32_t snapshotPresses = 0, snapshotImages = 0;
    std::string snapshotState = "idle";
};

// Run folder, created lazily at the first diagnostic Present. Empty until then.
std::string RunDir();

// Render thread, Present entry (armed): probes on the scheduled frames.
void OnPresentEntry(Core& core);

// Render thread, Present exit: frame record, periodic file rewrites, status for the menu.
void OnFrameComplete(Core& core, FrameOutcome&& outcome, const ContextTable::Snapshot& contexts);

// Any thread (the menu): current numbers.
Status GetStatus();
void SetSnapshotStatus(uint32_t presses, uint32_t images, const std::string& state);

// Queues files for the writer thread. Names are relative to the run folder.
void AppendText(const char* file, std::string text);
void SubmitImage(std::string baseName, std::vector<uint8_t> pixels, uint32_t width, uint32_t height, uint32_t dxgiFormat,
                 uint32_t rowPitch, uint32_t frame);
bool ImageBudgetLeft(uint64_t bytes); // reserve `bytes` of the per-run raw image budget
} // namespace ext::ffxiv::ui::diag
