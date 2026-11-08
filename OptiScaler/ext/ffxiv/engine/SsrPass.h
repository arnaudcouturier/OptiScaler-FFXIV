#pragma once
// Screen-space reflections: the pass itself (ext/ffxiv/engine/shaders/Ssr.hlsl).
//
// One draw on the game's own immediate context, inside a private device-context state (the pattern the fork's other passes use, so no game
// binding is touched by it): over the region of the planar map that lies inside the render window, it marches the engine's view-position buffer
// against this pass's copy of the scene and writes the reflection into an owned B8G8R8A8_UNORM target - the format and size of the texture it
// replaces.
//
// The owned target starts as a copy of the engine's own planar map, so every texel outside the region, and every texel whose ray misses, keeps
// the engine's value: the readers sample the whole texture and this module only answers for the part its own march found something in.
//
// The camera table (g_CameraParameter, 0x160 bytes through the projection) is copied GPU side into a buffer of this pass with a partial
// CopySubresourceRegion, and the scene is copied with a partial copy of the render window out of the draw's own render target: nothing is ever
// read back to the CPU, so no frame stalls on a map.
//
// Everything the pass creates lives here and is released with it; nothing of the game is written, cleared or rebound by this class.

#include "ext/ffxiv/engine/SsrRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::engine::ssr
{
enum class DiagnosticStages : uint8_t { All, PreparationOnly, PipelineOnly, CombinedNoDraw, SwapOnly, TargetBindingsOnly, OtherBindingsOnly };

struct DiagnosticOperations
{
    uint32_t copies = 0, clears = 0, maps = 0, stateSwaps = 0, fullscreenDraws = 0;
    uint32_t timingBegins = 0, timingEnds = 0, timingGetData = 0, sceneOmSets = 0;
    uint32_t pipelineBinds = 0;
    int32_t trackedOmBefore = -1, trackedOmAfter = -1;
    bool nativeTargetRestored = false;
};
// What one run needs: the water draw's own resources, its measured sizes and window, and the tuning keys.
struct PassInputs
{
    ID3D11ShaderResourceView* planar = nullptr;   // the reader's g_SamplerReflectionMap (the engine's map, for the texels this pass does not answer for)
    ID3D11Texture2D* planarTexture = nullptr;     // the resource behind that view, copied into the owned target before the region is written
    ID3D11ShaderResourceView* vposition = nullptr; // the reader's g_SamplerVPosition (the view-space position of every pixel)
    ID3D11Texture2D* scene = nullptr;             // the resource of the draw's own render target: the scene the frame had rendered so far
    ID3D11Buffer* camera = nullptr;               // the reader's g_CameraParameter
    ReaderFacts facts;
    Params params;
};

class Pass
{
  public:
    // Runs the pass for one frame and returns the texture to bind at the reflection slot, or null when it did not run. A null return is the
    // fail-open answer: the caller keeps the engine's own texture bound and counts the refusal.
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const PassInputs& inputs, bool diagnosticCopy = false, bool diagnosticSkipDraw = false,
                                  bool diagnosticSkipTiming = false, bool diagnosticKeepSceneTargetBound = false,
                                  DiagnosticStages diagnosticStages = DiagnosticStages::All);
    const DiagnosticOperations& LastDiagnosticOperations() const noexcept { return _lastOperations; }
    bool DiagnosticEngineCopyPerformedThisRun() const noexcept { return _diagnosticEngineCopyThisRun; }
    ID3D11ShaderResourceView* DiagnosticEngineCopy() const noexcept { return _diagnosticSrv.Get(); }
    bool DiagnosticCopyAvailable() const noexcept { return _diagnosticCopyAvailable; }
    void ResetDiagnosticCopy() noexcept;

    // Forgets every device object (a device change, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many times the owned targets were created: one pair per planar size, so a test can pin that a frame does not create D3D objects and
    // that a size change does.
    uint32_t TargetsCreated() const noexcept { return _targetsCreated; }

    // The GPU time of the draw, measured with timestamp queries and read a run or two later (never waited for). `HasGpuMilliseconds` is false
    // until the first measurement has landed.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // Whether this pass can time itself at all: a device that refused the timestamp queries leaves the draw unmeasured, and the caller's log line
    // then says so instead of waiting for a figure that will never come.
    bool CanMeasureGpuTime() const noexcept { return _disjoint && _begin && _end; }

    // The timing pair's bookkeeping, for the test that pins the pairing: a run ends a pair only when it began one.
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }
    uint32_t GpuTimingGetDataCalls() const noexcept { return _timingGetDataCalls; }
    uint8_t DiagnosticUavMaskBeforeCopy() const noexcept { return _diagnosticUavMaskBeforeCopy; }
    uint8_t DiagnosticUavMaskAfterCopy() const noexcept { return _diagnosticUavMaskAfterCopy; }
    bool DiagnosticPredicatePresent() const noexcept { return _diagnosticPredicatePresent; }
    uint32_t ActualDrawSubmissions() const noexcept { return _actualDrawSubmissions; }

    // The encoded result of the last run (the texture the water draws bind) and its resource, for the caller's frame memory.
    ID3D11ShaderResourceView* OutputView() const noexcept { return _outSrv.Get(); }
    ID3D11Texture2D* OutputTexture() const noexcept { return _out.Get(); }
    // Exact buffers consumed by the last pass, for opt-in coherent replay snapshots.
    ID3D11Buffer* CameraBuffer() const noexcept { return _camera.Get(); }
    ID3D11Buffer* ParametersBuffer() const noexcept { return _params.Get(); }

    // The owned copy of the scene and its render-target view. A reset has to leave neither behind (the view holds a reference on the copy, the copy
    // one on the device), so a lifetime test reads them back after Reset.
    ID3D11Texture2D* SceneTexture() const noexcept { return _scene.Get(); }
    ID3D11RenderTargetView* SceneTargetView() const noexcept { return _sceneRtv.Get(); }

    // The size of the encoded result: the planar map's own size, which is not the scene copy's (the two differ by the allocation ratio). A test
    // that reads the result back needs it, and a caller that stamps the frame's memory can report it without holding the texture.
    uint32_t OutputWidth() const noexcept { return _width; }
    uint32_t OutputHeight() const noexcept { return _height; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureTargets(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t sceneWidth, uint32_t sceneHeight);
    void CollectGpuTiming(ID3D11DeviceContext* context, bool skip);
    bool BeginGpuTiming(ID3D11DeviceContext* context, bool skip);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _search;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params; // the scalars of the draw (SsrRules.h's Constants)
    Microsoft::WRL::ComPtr<ID3D11Buffer> _camera; // 0x160 bytes: g_CameraParameter +0x000..0x160
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _cameraView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene; // the copy of the frame's scene, at the draw's own render target's size and format
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv; // only used to clear the fresh copy once, so a dropped copy reads as sky
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _out;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _outRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _outSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _diagnosticCopy;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _diagnosticSrv;
    bool _diagnosticCopyAvailable = false;
    bool _diagnosticEngineCopyThisRun = false;
    DiagnosticOperations _lastOperations {};
    uint32_t _width = 0, _height = 0;             // the planar target's size
    uint32_t _sceneWidth = 0, _sceneHeight = 0;   // the scene copy's size
    bool _sceneClearPending = true;               // the fresh scene copy has not been cleared yet
    uint32_t _targetsCreated = 0;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    uint32_t _timingGetDataCalls = 0;
    uint8_t _diagnosticUavMaskBeforeCopy = 0, _diagnosticUavMaskAfterCopy = 0;
    bool _diagnosticPredicatePresent = false;
    uint32_t _actualDrawSubmissions = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine::ssr
