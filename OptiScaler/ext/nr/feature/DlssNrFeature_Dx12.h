#pragma once

#include <d3d12.h>
#include "ext/gpu/GpuSubmission.h"
#include <string>

#include "ext/nr/pass/DlssNr_Common.h"
#include <nvsdk_ngx.h>

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
class Config;

namespace DlssNr
{
// NR's GPU-lifetime owner (ext::gpu::Owner): begin / quarantine / reset. Registered once, on the first call of
// EnsureGpuHooks, which also installs the queue/Reset hooks (ext::restrack::EnsureHooks). Replaces the reference
// build's HookLateNrQueue; call it with the game's device before NR records anything.
ext::gpu::GpuSubmission BeginGpuSubmission(UINT count, ID3D12CommandList* const* lists);
void QuarantineGpuSubmission(UINT count, ID3D12CommandList* const* lists);
void NotifyGpuReset(ID3D12CommandList* commands);
bool EnsureGpuHooks(ID3D12Device* device);

// ---- Preview seam (ext/ffxiv/chara/CharaNr.cpp) ----------------------------------------------------------------------------
// The character preview runs its own NR model instances and needs three things that live in this module's private state: the
// loaded forwarder's function table, the model file and the driver core's capability block. Nothing else of g_nr is exposed, and
// the main NR path does not change behaviour. The capability block is shared with the main pass: a caller sets the extras to null
// resources (the forwarder's set_extras) before every create and every evaluate.
using PfnPreviewCreate = void*(__cdecl*)(const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                                         unsigned int, unsigned int, int, float, int, float, float, float, int, int);
using PfnPreviewEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                                         ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int, unsigned int,
                                         unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, int, int,
                                         float, int, float, float, float, int, float, float);
using PfnPreviewRelease = void(__cdecl*)(void*);
using PfnPreviewSetExtras = void(__cdecl*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                           unsigned int, unsigned int, unsigned int);

struct PreviewBackend
{
    PfnPreviewCreate create = nullptr;
    PfnPreviewEvaluate evaluate = nullptr;
    PfnPreviewRelease release = nullptr;
    PfnPreviewSetExtras setExtras = nullptr; // may be null (an older forwarder)
    void* capabilityParams = nullptr;
    std::wstring modelPath;
    std::wstring dataPath;
    const char* (*lastModelError)() = nullptr; // may be null
    uint64_t revision = 0;                     // the NR runtime selection this backend was acquired for (ext/nr/runtime)
};

enum class PreviewBackendStatus
{
    Ready,
    ForwarderMissing,
    ModelMissing,
    CapabilityFailed,
};

// Loads the forwarder (once), finds the model file and obtains the capability block on `device`. Safe to call repeatedly.
PreviewBackendStatus AcquirePreviewBackend(ID3D12Device* device, PreviewBackend& out);

// The current NR runtime revision (the model file the card selected). A stream built for another revision is rebuilt from the new
// model path: re-acquire the backend (its modelPath and revision follow the selection) and rebuild the streams.
uint64_t PreviewRuntimeRevision();

// Before a preview creates or evaluates a model of `modelPath`: the shared capability block must hold that module's callbacks.
void PreviewBindModel(const std::wstring& modelPath);

// The tuning the main NR pass creates its first model pass with ([DlssNr] preset, intensity, style, structure, tone, skin, mask).
struct PreviewTuning
{
    int preset = 0;
    float intensity = 1.0f;
    int style = 0;
    float structure = 1.0f;
    float tone = 1.0f;
    float skin = -1.0f;
    bool autoMask = true;
};
PreviewTuning CurrentPreviewTuning();

// True while the graphics bridge is shutting down (the main NR is suspended).
bool PreviewBackendSuspended();

// Held around create, set_extras, BeginEvaluate/evaluate/EndEvaluate of a preview model: the main pass holds the same lock around its
// own evaluate, and the native wrapper keeps per-evaluate state that two threads must not interleave.
class PreviewEvaluateScope
{
  public:
    PreviewEvaluateScope();
    ~PreviewEvaluateScope();
    PreviewEvaluateScope(const PreviewEvaluateScope&) = delete;
    PreviewEvaluateScope& operator=(const PreviewEvaluateScope&) = delete;

  private:
    void* _impl = nullptr;
};

// The state envelope every NR compute pass runs in (root-signature tracking off, restored afterwards), for a caller-owned list.
class PreviewStateEnvelope
{
  public:
    explicit PreviewStateEnvelope(ID3D12GraphicsCommandList* commands);
    ~PreviewStateEnvelope();
    PreviewStateEnvelope(const PreviewStateEnvelope&) = delete;
    PreviewStateEnvelope& operator=(const PreviewStateEnvelope&) = delete;

  private:
    void* _impl = nullptr;
};

// FFXIV bridge lifecycle: stop new work and retire completed PreSR generations
// before calling the driver's NGX shutdown. Never call from DllMain. A genuine
// NGX reinitialization can resume without changing the user's saved NR setting.
void SuspendForBridgeShutdown();
void ResumeAfterBridgeInit();
// Consumed once by the DX11 bridge immediately after the matching upscale.
// -1 means current scene; 0..2 means alternating NR and its actual scene age.
int ConsumePresentationGuideDelay();

inline constexpr unsigned int MaxPassCount = 30;
inline constexpr unsigned int DefaultMaxPassCount = 3;

// The model runs immediately after the game's upscaler, before the interface is drawn. It is shown a
// display-referred proxy of that frame -- the sort of picture it was trained on -- and its answer is
// composed back over the untouched original.
// Runs the model over Output on the same command list, immediately after the upscaler has written it.
// Called only for upscaler evaluates -- never for frame generation, which is the whole point.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
// timingQueue is the queue this command list will be executed on, when the caller knows it.
// State::currentCommandQueue only exists once a D3D12 swapchain has been created, which a Vulkan
// game never does -- so without this the pass runs and never reports what it cost.
// rayReconstruction identifies the feature for history reset and the SR-only deferred experiment.
// Placement and model cost controls are shared by SR and RR+SR.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue = nullptr, bool rayReconstruction = false,
                          unsigned long long submissionEpoch = 0,
                          unsigned int guideSourceWidth = 0, unsigned int guideSourceHeight = 0);

// Runs the same pass over Color immediately before SR or RR+SR consumes it. The call is a no-op
// unless RunBeforeSR is enabled. Color is returned in its original readable state.
void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue = nullptr,
                           unsigned long long submissionEpoch = 0, bool rayReconstruction = false,
                           unsigned int guideSourceWidth = 0, unsigned int guideSourceHeight = 0);



// Frame generation titles tag their UI layer through Streamline; a copy of it makes the HUD mask
// exact at the finished frame. Called at tag time.




// The settings panel: the body of the ext::menu card `dlss_nr` (ext/nr/menu/DlssNr_Menu.cpp, registered by NrMenu.cpp).
// It reads and writes ext::Cfg().nr and runs inside upstream's ScopedCard.
void RenderMenu(float menuResScale);

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailure();


// What scale this game's buffer is on, measured from the untouched copy of each frame.
//
// A suggestion only. Nothing applies it: the menu shows it and the user takes it or does not, which
// keeps the number visible and adjustable rather than a value that moved on its own. Confidence is
// how settled recent readings are -- 1 means they agree, 0 means the scene is changing under the
// measurement and no single value would serve.
struct CalibrationReading
{
    float suggestion = 0.0f;

    // How much recent readings agree. This is steadiness, not correctness: a frozen frame agrees with
    // itself perfectly, so a loading screen scores full marks for a number that means nothing. Read it
    // together with usable.
    float steadiness = 0.0f;

    unsigned long long samples = 0;

    // Whether the scene is worth measuring at all. False when the frame is already tone mapped -- the
    // divisor does nothing there and the reading would be a meaningless 0.9 -- or when too little of
    // the picture is lit to say where the top of the range is. A dark cave gives a small number very
    // steadily, which is the trap this exists to close.
    bool usable = false;
    const char* why = "";
};

CalibrationReading Calibration();

// Whether the model is loaded and running, for the overlay.
bool IsRunning();
std::string SpatialStatus();

// What the spatial profiles amount to for the frame being processed, for the menu: the share of the ordinary NR pixels the model
// handles, its size, and the output the Foveated pixel sizes are written against.
struct SpatialReadout
{
    bool valid = false; // a frame has been seen
    bool requested = false, active = false;
    float share = 1.0f; // 1 while the profile is inactive
    unsigned int modelW = 0, modelH = 0;
    unsigned int outputW = 0, outputH = 0;
};
SpatialReadout CurrentSpatialReadout();
void CurrentModelSize(unsigned int& width, unsigned int& height);
// Private residual-upscaler status; separate from the NR model's own running status/time.
std::string DeferredDlssStatus();
// True while DisableOnFocusLoss has the pass paused because the game window is not focused or is minimised.
bool PausedByFocus();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
std::string FailureReason();

// What the game offers by way of exposure. Observed every frame whether or not the setting is on, so
// the menu can say whether turning it on would do anything here.
struct ExposureStatus
{
    unsigned long long seenFrames = 0;   // evaluates observed; 0 means nothing has run yet
    bool offeredNow = false;             // a texture on the most recent frame
    bool everOffered = false;            // a texture on any frame so far
    float exposure = 0.0f;               // last value read back, 0 if none
    float preExposure = 1.0f;
};

ExposureStatus GameExposureStatus();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// What the white point meter last settled on, or 0 when it is not running. For the menu.


// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
