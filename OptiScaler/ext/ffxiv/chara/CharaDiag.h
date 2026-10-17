#pragma once
// Diagnostics of the preview DLSS path. Files are written only while [FFXIV] PreviewGuideCapture is on, capped at 12 files
// and 100 MiB per session (CharaDiagRules.h), under <folder of the fork's module>\OptiScaler\PreviewDLSSDiagnostics\.
// Trace lines need [HDR] PreviewNRTrace and are rate limited. Both are off by default.
//
// A dump copies six same-frame images into staging textures when an evaluation finishes and reads them back later (when the
// GPU is done, without waiting), so the draw thread never stalls on the GPU.

#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ext::ffxiv::chara::diag
{
struct Request
{
    ID3D11Texture2D* native = nullptr;   // the finisher output
    ID3D11Texture2D* input = nullptr;    // upscaler colour input
    ID3D11Texture2D* dlss = nullptr;     // upscaler output
    ID3D11Texture2D* composed = nullptr; // upscaler colour + native alpha
    ID3D11Texture2D* depth = nullptr;
    ID3D11Texture2D* motion = nullptr;
    ID3D11Buffer* camera = nullptr; // the camera copy (its last vector is read back for the log), may be null
    uint32_t width = 0;             // render size
    uint32_t height = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
};

bool CaptureEnabled(); // [FFXIV] PreviewGuideCapture
bool TraceEnabled();   // [HDR] PreviewNRTrace

// Schedules a dump when the gates allow it (config, size, budget, phase). `evaluations` counts successful evaluations
// including the one that just finished. Returns true when a dump was scheduled. Call after the passes ran.
bool Capture(ID3D11DeviceContext* context, const Request& request, uint64_t evaluations, uint64_t nowMs);

// Reads back and writes a finished dump (non-blocking). Call from the periodic maintenance.
void Pump(ID3D11DeviceContext* context, uint64_t nowMs);

// One trace line (rate limited per category, only when PreviewNRTrace is on).
enum class TraceCategory : uint8_t
{
    Finish,
    Guides,
    Consumer,
    Replay,
    Camera,
};
void Trace(TraceCategory category, uint64_t nowMs, const std::string& text);

uint32_t FilesWritten();
uint64_t BytesWritten();

// Output folder (the tests redirect it; an empty path restores the default).
void SetDirectoryForTest(const std::filesystem::path& directory);

void Release();
} // namespace ext::ffxiv::chara::diag
