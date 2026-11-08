#include "pch.h"

#include "ext/ffxiv/engine/SsrPass.h"
#include "ext/core/D3D11ExplicitState.h"
#include "ext/core/InjectedStateWindowDiagnostic.h"
#include "ext/core/InjectedOmTraceDiagnostic.h"
#include "ext/ffxiv/engine/SsrNativeContext.h"
#include "ext/ffxiv/engine/WaterExecutionTrace.h"

#include <Ssr_Ssr.h>
#include <Ssr_Vs.h>

#include <cstring>

// Screen-space reflections: the device side of the pass (ext/ffxiv/engine/SsrPass.h). One draw on the game's own immediate context between an
// explicit save and restore of the state it touches (ext/core/D3D11ExplicitState.h), three owned objects (the copy of the scene, the encoded
// planar map and the copy of the camera table) and a timestamp pair that is read a run or two later and never waited for.
namespace ext::ffxiv::engine::ssr
{
namespace
{
constexpr uint64_t kDiagnosticEngineCopyMaxBytes = 64ull * 1024ull * 1024ull;

// What this pass touches on the game's context: the IA layout and topology, VS and PS, PS resource slots 0..3 and constant buffer 0, the
// rasterizer state and the viewports, blend, depth-stencil, and the render targets with the UAV slots. HS, DS, GS, stream-out and predication
// are what the private device-context state used to provide as "none".
constexpr ext::core::explicit_state::Spec kSpec { /*psSrvCount*/ 4, /*psSamplerCount*/ 1, /*psConstantBuffers*/ 1 };
}

void Pass::ResetDiagnosticCopy() noexcept
{
    _diagnosticSrv.Reset();
    _diagnosticCopy.Reset();
    _diagnosticCopyAvailable = false;
}

using Microsoft::WRL::ComPtr;

namespace
{
// The register slots of ext/ffxiv/engine/shaders/Ssr.hlsl. Change them together.
constexpr uint32_t kSlotPlanar = 0;
constexpr uint32_t kSlotVPosition = 1;
constexpr uint32_t kSlotScene = 2;
constexpr uint32_t kSlotCamera = 3;

// A byte range of a constant buffer as the box a partial copy takes: for a buffer the box is in bytes, one row, one slice.
D3D11_BOX ByteBox(uint32_t offset, uint32_t bytes)
{
    D3D11_BOX box {};
    box.left = offset;
    box.top = 0;
    box.front = 0;
    box.right = offset + bytes;
    box.bottom = 1;
    box.back = 1;
    return box;
}
} // namespace

void Pass::Reset()
{
    ResetDiagnosticCopy();
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _sceneSrv.Reset();
    // The scene's render-target view is part of the same owned object: a view holds a reference on the copy, and the copy holds one on the device,
    // so a reset that left it behind would keep the scene allocation (63 MB at 4K) and the device that made it alive until the next size change -
    // and for good on the unload path, which runs no further pass.
    _sceneRtv.Reset();
    _scene.Reset();
    _cameraView.Reset();
    _camera.Reset();
    _params.Reset();
    _rasterizer.Reset();
    _search.Reset();
    _vs.Reset();
    _device.Reset();
    _disjoint.Reset();
    _begin.Reset();
    _end.Reset();
    _width = _height = 0;
    _sceneWidth = _sceneHeight = 0;
    _sceneClearPending = true; // no copy exists: the next one is cleared before it is written
    _targetsCreated = 0;
    _failure = nullptr;
    _diagnosticCopyAvailable = false;
    _timingPending = false;
    _timingBegins = _timingEnds = 0;
    _timingGetDataCalls = 0;
    _diagnosticUavMaskBeforeCopy = _diagnosticUavMaskAfterCopy = 0;
    _diagnosticPredicatePresent = false;
    _actualDrawSubmissions = 0;
    _haveGpuMs = false;
    _gpuMs = 0.0;
}

bool Pass::Init(ID3D11Device* device)
{
    if (FAILED(device->CreateVertexShader(Ssr_Vs_cso, sizeof Ssr_Vs_cso, nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(Ssr_Ssr_cso, sizeof Ssr_Ssr_cso, nullptr, &_search)))
    {
        _failure = "the screen-space reflection shaders could not be created";
        return false;
    }

    D3D11_RASTERIZER_DESC raster {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster, &_rasterizer)))
    {
        _failure = "the rasterizer state could not be created";
        return false;
    }

    D3D11_BUFFER_DESC params {};
    params.ByteWidth = sizeof(Constants);
    params.Usage = D3D11_USAGE_DYNAMIC;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&params, nullptr, &_params)))
    {
        _failure = "the parameter buffer could not be created";
        return false;
    }

    // The camera table, GPU side. A destination this pass reads through a shader resource view rather than a constant buffer: a partial-box copy
    // into a constant buffer is not something every driver accepts.
    D3D11_BUFFER_DESC camera {};
    camera.ByteWidth = kCameraBytes;
    camera.Usage = D3D11_USAGE_DEFAULT;
    camera.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC cameraView {};
    cameraView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    cameraView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    cameraView.Buffer.FirstElement = 0;
    cameraView.Buffer.NumElements = kCameraBytes / 16;
    if (FAILED(device->CreateBuffer(&camera, nullptr, &_camera)) ||
        FAILED(device->CreateShaderResourceView(_camera.Get(), &cameraView, &_cameraView)))
    {
        _failure = "the camera table's copy could not be created";
        return false;
    }

    // No private pipeline state: the game's own is captured and put back explicitly around the draw (ext/core/D3D11ExplicitState.h).
    // The draw's own GPU time: a timestamp pair inside a disjoint query, read one run later without ever waiting for the GPU.
    D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC stampDesc { D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(device->CreateQuery(&disjointDesc, &_disjoint)) || FAILED(device->CreateQuery(&stampDesc, &_begin)) ||
        FAILED(device->CreateQuery(&stampDesc, &_end)))
    {
        _disjoint.Reset();
        _begin.Reset();
        _end.Reset(); // no timings, the pass still runs: the measurement is the only thing that is lost
    }
    _device = device;
    return true;
}

bool Pass::EnsureTargets(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t sceneWidth, uint32_t sceneHeight)
{
    if (_out && _scene && _width == width && _height == height && _sceneWidth == sceneWidth && _sceneHeight == sceneHeight)
        return true;
    _outSrv.Reset();
    _outRtv.Reset();
    _out.Reset();
    _sceneSrv.Reset();
    _sceneRtv.Reset();
    _scene.Reset();
    _width = _height = 0;
    _sceneWidth = _sceneHeight = 0;
    _sceneClearPending = true;

    // The copy of the frame's scene, at the display allocation's own size and format: the picture is the part of it the render window covers,
    // and the mapping from a texel of ours to the scene texels behind it is the two allocations' ratio, exactly as the ambient occlusion pass maps
    // its occlusion texture to the depth. Only the window is copied into it, so a session that renders the picture smaller than the allocation
    // pays for the picture and not for the padding. The render-target view is only there to clear it once (Run does that on the first copy): a
    // copy that does not land then leaves a zeroed position buffer, which the march reads as sky and answers with the engine's own texels.
    D3D11_TEXTURE2D_DESC sceneDesc {};
    sceneDesc.Width = sceneWidth;
    sceneDesc.Height = sceneHeight;
    sceneDesc.MipLevels = 1;
    sceneDesc.ArraySize = 1;
    sceneDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sceneDesc.SampleDesc.Count = 1;
    sceneDesc.Usage = D3D11_USAGE_DEFAULT;
    sceneDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC sceneRtvDesc {};
    sceneRtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sceneRtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sceneSrvDesc {};
    sceneSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sceneSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sceneSrvDesc.Texture2D.MipLevels = 1;

    // What the water draws bind: the engine's own format and size, so a shader that samples it (with its own sampler state, its own addressing
    // and any bilinear filter) sees the shape it expects.
    D3D11_TEXTURE2D_DESC outDesc {};
    outDesc.Width = width;
    outDesc.Height = height;
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC outRtvDesc {};
    outRtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outRtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc {};
    outSrvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    outSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    outSrvDesc.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateTexture2D(&sceneDesc, nullptr, &_scene)) ||
        FAILED(device->CreateRenderTargetView(_scene.Get(), &sceneRtvDesc, &_sceneRtv)) ||
        FAILED(device->CreateShaderResourceView(_scene.Get(), &sceneSrvDesc, &_sceneSrv)) ||
        FAILED(device->CreateTexture2D(&outDesc, nullptr, &_out)) || FAILED(device->CreateRenderTargetView(_out.Get(), &outRtvDesc, &_outRtv)) ||
        FAILED(device->CreateShaderResourceView(_out.Get(), &outSrvDesc, &_outSrv)))
    {
        _outSrv.Reset();
        _outRtv.Reset();
        _out.Reset();
        _sceneSrv.Reset();
        _sceneRtv.Reset();
        _scene.Reset();
        _failure = "the reflection targets could not be created";
        return false;
    }
    _width = width;
    _height = height;
    _sceneWidth = sceneWidth;
    _sceneHeight = sceneHeight;
    ++_targetsCreated;
    return true;
}

void Pass::CollectGpuTiming(ID3D11DeviceContext* context, bool skip)
{
    if (skip || !_timingPending || !_disjoint || !_begin || !_end)
        return;
    // Every read carries D3D11_ASYNC_GETDATA_DONOTFLUSH: a GetData without it can flush the command queue to
    // resolve the query, and this runs on the game's own immediate context once a frame - the periodic hitch a
    // session reads as a lag. Not finished is the expected answer most frames: the pair is looked at again on the
    // next run, never waited for, exactly the idiom the fork's own GpuBridgeTimers reads with.
    constexpr UINT kGetDataFlags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
    ++_timingGetDataCalls;
    ++_lastOperations.timingGetData;
    if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, kGetDataFlags) != S_OK)
        return; // not finished: looked at again on the next run, never waited for
    UINT64 begin = 0, end = 0;
    _timingGetDataCalls += 2;
    _lastOperations.timingGetData += 2;
    const HRESULT haveBegin = context->GetData(_begin.Get(), &begin, sizeof begin, kGetDataFlags);
    const HRESULT haveEnd = context->GetData(_end.Get(), &end, sizeof end, kGetDataFlags);
    _timingPending = false;
    if (!disjoint.Disjoint && disjoint.Frequency > 0 && haveBegin == S_OK && haveEnd == S_OK && end > begin)
    {
        _gpuMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
        _haveGpuMs = true;
    }
}

bool Pass::BeginGpuTiming(ID3D11DeviceContext* context, bool skip)
{
    if (skip || !_disjoint || !_begin || !_end || _timingPending)
        return false; // the previous pair is still in the queue: this run is not timed, and it must not end that pair either
    context->Begin(_disjoint.Get());
    ++_lastOperations.timingBegins;
    context->End(_begin.Get());
    _timingPending = true;
    ++_timingBegins;
    return true;
}

ID3D11ShaderResourceView* Pass::Run(ID3D11DeviceContext* context, const PassInputs& inputs, bool diagnosticCopy, bool diagnosticSkipDraw,
                                    bool diagnosticSkipTiming, bool diagnosticKeepSceneTargetBound, DiagnosticStages diagnosticStages)
{
    ext::core::om_trace::PassScope omTrace(context);
    if (auto* capture = omTrace.Get())
    {
        auto** table = *reinterpret_cast<void***>(context);
        capture->vtable = reinterpret_cast<uintptr_t>(table);
        capture->vFirst = reinterpret_cast<uintptr_t>(table[0]);
        capture->v33 = reinterpret_cast<uintptr_t>(table[33]);
        capture->v34 = reinterpret_cast<uintptr_t>(table[34]);
        IUnknown* incomingIdentity = nullptr;
        if (SUCCEEDED(context->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&incomingIdentity))) && incomingIdentity)
        {
            capture->incomingIdentity = reinterpret_cast<uintptr_t>(incomingIdentity);
            incomingIdentity->Release();
        }
        ID3D11DeviceContext1* context1 = nullptr;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))) && context1)
        {
            capture->context1 = reinterpret_cast<uintptr_t>(context1);
            context1->Release();
        }
        IUnknown* unwrapped = nullptr;
        capture->unwrapHr = context->QueryInterface(ext::ffxiv::engine::ssr::native_context::ReShadeUnwrappedObject,
                                                     reinterpret_cast<void**>(&unwrapped));
        if (SUCCEEDED(capture->unwrapHr) && unwrapped)
        {
            capture->unwrapped = reinterpret_cast<uintptr_t>(unwrapped);
            ID3D11DeviceContext* unwrappedContext = nullptr;
            if (SUCCEEDED(unwrapped->QueryInterface(IID_PPV_ARGS(&unwrappedContext))) && unwrappedContext)
            {
                capture->unwrappedContext = reinterpret_cast<uintptr_t>(unwrappedContext);
                auto** unwrapTable = *reinterpret_cast<void***>(unwrappedContext);
                capture->unwrapVtable = reinterpret_cast<uintptr_t>(unwrapTable);
                capture->unwrapVFirst = reinterpret_cast<uintptr_t>(unwrapTable[0]);
                capture->unwrapV33 = reinterpret_cast<uintptr_t>(unwrapTable[33]);
                capture->unwrapV34 = reinterpret_cast<uintptr_t>(unwrapTable[34]);
                unwrappedContext->Release();
            }
            IUnknown* identity = nullptr;
            if (SUCCEEDED(unwrapped->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) && identity)
            {
                capture->identity = reinterpret_cast<uintptr_t>(identity);
                identity->Release();
            }
            unwrapped->Release();
        }
    }
    // Every run starts with no reason: the caller logs whatever this run left behind, and a reason from an earlier failure would name the wrong
    // fault (a map that failed after a copy that could not be created would report the copy's reason).
    _failure = nullptr;
    if (context == nullptr || inputs.planar == nullptr || inputs.planarTexture == nullptr || inputs.vposition == nullptr || inputs.scene == nullptr ||
        inputs.camera == nullptr)
        return nullptr;
    // Stage controls exclude these operations regardless of the older diagnostic flags.
    if (diagnosticStages != DiagnosticStages::All)
    {
        diagnosticSkipDraw = true;
        diagnosticSkipTiming = true;
        diagnosticKeepSceneTargetBound = true;
    }
    // The pass runs on the shape the pure rules accepted; a caller that skipped them gets the engine's texture back.
    if (Classify(inputs.facts) != Decline::None)
        return nullptr;
    // The part of the target the engine's own producer fills, and the only part this pass writes.
    const ao::Region region = RegionOf(inputs.facts);
    if (region.Empty())
    {
        _failure = "the render window covers no whole texel of the planar map";
        return nullptr;
    }

    omTrace.PhaseNow(ext::core::om_trace::Phase::Prep);
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
        return nullptr;
    if (device.Get() != _device.Get())
    {
        Reset();
        if (!Init(device.Get()))
            return nullptr;
    }

    // Per-run evidence starts after Init/Reset, before this run issues any D3D commands.
    _lastOperations = {};
    _diagnosticEngineCopyThisRun = false;
    // Read after device initialization, which resets the per-pass diagnostic metadata.
    if (diagnosticCopy && diagnosticStages == DiagnosticStages::All)
    {
        _diagnosticUavMaskBeforeCopy = _diagnosticUavMaskAfterCopy = 0;
        BOOL predicate = FALSE;
        ID3D11Predicate* predicateObject = nullptr;
        context->GetPredication(&predicateObject, &predicate);
        _diagnosticPredicatePresent = predicateObject != nullptr;
        if (predicateObject) predicateObject->Release();
    }

    D3D11_BUFFER_DESC cameraDesc {};
    inputs.camera->GetDesc(&cameraDesc);
    if (cameraDesc.ByteWidth < kCameraBytes)
    {
        _failure = "the camera table is smaller than the matrices the shader reads";
        return nullptr;
    }
    if (!EnsureTargets(device.Get(), inputs.facts.planarWidth, inputs.facts.planarHeight, inputs.facts.sceneWidth, inputs.facts.sceneHeight))
        return nullptr;

    if (diagnosticStages == DiagnosticStages::SwapOnly)
    {
        omTrace.PhaseNow(ext::core::om_trace::Phase::BeforePrep);
        UINT trackedOm = 0;
        ComPtr<ID3D11RenderTargetView> nativeTargetBefore;
        if (diagnosticCopy)
        {
            if (ext::core::state_window::ReadTrackedOm(context, trackedOm)) _lastOperations.trackedOmBefore = static_cast<int32_t>(trackedOm);
            context->OMGetRenderTargets(1, &nativeTargetBefore, nullptr);
        }
        ComPtr<ID3D11DeviceContext1> swapContext;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&swapContext)))) return nullptr;
        // The stage that measures the exchange's own cost: one explicit capture and one restore with nothing set in between (`stateSwaps`
        // counts these two exchanges, as it counted the two swaps).
        omTrace.PhaseNow(ext::core::om_trace::Phase::SwapIn);
        {
            ext::core::explicit_state::Guard guard(context, swapContext.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
            ++_lastOperations.stateSwaps;
            const uint64_t window = ext::core::state_window::Begin(context);
            ext::core::state_window::End(window);
            omTrace.PhaseNow(ext::core::om_trace::Phase::SwapOut);
            guard.Restore();
            ++_lastOperations.stateSwaps;
        }
        omTrace.PhaseNow(ext::core::om_trace::Phase::Restored);
        if (diagnosticCopy)
        {
            if (ext::core::state_window::ReadTrackedOm(context, trackedOm)) _lastOperations.trackedOmAfter = static_cast<int32_t>(trackedOm);
            ComPtr<ID3D11RenderTargetView> nativeTargetAfter;
            context->OMGetRenderTargets(1, &nativeTargetAfter, nullptr);
            _lastOperations.nativeTargetRestored = nativeTargetAfter.Get() == nativeTargetBefore.Get();
        }
        return _outSrv.Get();
    }

    // A same-shaped private SRV for the controlled live diagnostic. Allocate lazily, then refresh once on every successful pass run.
    // A refusal here fails the pass open rather than claiming that private-view mode was exercised.
    const bool preparation = diagnosticStages == DiagnosticStages::All || diagnosticStages == DiagnosticStages::PreparationOnly || diagnosticStages == DiagnosticStages::CombinedNoDraw;
    const bool pipeline = diagnosticStages != DiagnosticStages::PreparationOnly;
    if (diagnosticCopy && preparation)
    {
        D3D11_TEXTURE2D_DESC planarDesc{};
        inputs.planarTexture->GetDesc(&planarDesc);
        const uint64_t cloneBytes = uint64_t(planarDesc.Width) * planarDesc.Height * 4ull;
        const bool cloneSupported = planarDesc.ArraySize == 1 && planarDesc.MipLevels == 1 && planarDesc.SampleDesc.Count == 1 &&
                                    planarDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && cloneBytes <= kDiagnosticEngineCopyMaxBytes;
        if (cloneSupported)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC cloneViewDesc{};
            inputs.planar->GetDesc(&cloneViewDesc);
            D3D11_TEXTURE2D_DESC cloneDesc = planarDesc;
            cloneDesc.Usage = D3D11_USAGE_DEFAULT;
            cloneDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            cloneDesc.CPUAccessFlags = 0;
            cloneDesc.MiscFlags = 0;
            D3D11_TEXTURE2D_DESC existing{};
            if (_diagnosticCopy) _diagnosticCopy->GetDesc(&existing);
            if (!_diagnosticCopy || existing.Width != cloneDesc.Width || existing.Height != cloneDesc.Height || existing.Format != cloneDesc.Format)
            {
                _diagnosticSrv.Reset(); _diagnosticCopy.Reset();
                if (FAILED(device->CreateTexture2D(&cloneDesc, nullptr, &_diagnosticCopy)) ||
                    FAILED(device->CreateShaderResourceView(_diagnosticCopy.Get(), &cloneViewDesc, &_diagnosticSrv)))
                {
                    _diagnosticSrv.Reset(); _diagnosticCopy.Reset();
                }
            }
            if (_diagnosticCopy)
            {
                context->CopyResource(_diagnosticCopy.Get(), inputs.planarTexture);
                ++_lastOperations.copies;
                _diagnosticEngineCopyThisRun = true;
                _diagnosticCopyAvailable = true;
            }
        }
    }
    ComPtr<ID3D11DeviceContext1> context1;
    if (pipeline && FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        _failure = "ID3D11DeviceContext1 is not available";
        return nullptr;
    }

    // The camera table, GPU side: one small copy, no map, no readback.
    const D3D11_BOX cameraBox = ByteBox(0, kCameraBytes);
    if (preparation)
    {
    context->CopySubresourceRegion(_camera.Get(), 0, 0, 0, 0, inputs.camera, 0, &cameraBox);
    ++_lastOperations.copies;

    // A fresh copy of the scene starts cleared. The march reads a zero position as sky, so a copy that does not land (a driver that drops a copy
    // out of a bound target, a device lost) answers every texel with the engine's own instead of marching whatever the allocation held.
    if (_sceneClearPending && _sceneRtv)
    {
        const float nothing[4] { 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearRenderTargetView(_sceneRtv.Get(), nothing);
        ++_lastOperations.clears;
        _sceneClearPending = false;
    }

    // The scene the frame had rendered when this draw arrived: the render window's own texels out of the draw's render target. The copy is taken
    // before anything this pass binds, and the source is the game's resource, which this pass only reads - but at this moment the game still has
    // that resource bound as this draw's render target. D3D11 permits the read (a copy source may be bound; only a resource the shader samples may
    // not be an output), but the shape is one a runtime or a driver may treat as undefined, so the render targets and the depth-stencil are unbound
    // for the copy and the game's own views come back immediately after, before this pass touches anything else. The cost is one state query, two
    // binds and the released references, once a frame.
    // The window's own texels, clamped to the copy's size: the window may hang a fraction of a texel past the allocation (the half-texel of slack the
    // rules allow for the engine's +0.5 viewport origin), and a copy box that reaches past the destination is an out-of-bounds copy rather than a
    // fail-open. The rule is the pure one the host test covers.
    const SceneCopy copy = SceneCopyOf(inputs.facts);
    if (copy.width == 0 || copy.height == 0)
    {
        _failure = "the render window covers no texel of the scene copy";
        return nullptr;
    }
    D3D11_BOX windowBox {};
    windowBox.left = copy.x;
    windowBox.top = copy.y;
    windowBox.front = 0;
    windowBox.right = copy.x + copy.width;
    windowBox.bottom = copy.y + copy.height;
    windowBox.back = 1;
    if (diagnosticCopy && diagnosticStages != DiagnosticStages::PipelineOnly)
    {
        ID3D11UnorderedAccessView* views[D3D11_PS_CS_UAV_REGISTER_COUNT] {};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, views);
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) { if (views[i]) { _diagnosticUavMaskBeforeCopy |= static_cast<uint8_t>(1u << i); views[i]->Release(); } }
    }
    if (!diagnosticKeepSceneTargetBound && diagnosticStages != DiagnosticStages::PreparationOnly)
    {
        // The render targets, the depth-stencil view and the UAV slots are read together and put back together: OMSetRenderTargets drops a bound
        // UAV, and this unbind and rebind runs on the game's own state. (Reading and rebinding only the render targets, as this block used to,
        // lost a bound UAV every run.)
        // `context1` is always set here: every stage but PreparationOnly queries it above, and PreparationOnly skips this branch.
        ext::core::explicit_state::OmSnapshot om(context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ++_lastOperations.sceneOmSets;
        context->CopySubresourceRegion(_scene.Get(), 0, copy.x, copy.y, 0, inputs.scene, 0, &windowBox);
        ++_lastOperations.copies;
        om.Restore();
        ++_lastOperations.sceneOmSets;
    }
    else
    {
        context->CopySubresourceRegion(_scene.Get(), 0, copy.x, copy.y, 0, inputs.scene, 0, &windowBox);
        ++_lastOperations.copies;
    }
    if (diagnosticCopy && diagnosticStages != DiagnosticStages::PipelineOnly)
    {
        ID3D11UnorderedAccessView* views[D3D11_PS_CS_UAV_REGISTER_COUNT] {};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, views);
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) { if (views[i]) { _diagnosticUavMaskAfterCopy |= static_cast<uint8_t>(1u << i); views[i]->Release(); } }
    }

    // The engine's own texels, so every texel outside the region below is the engine's answer and not an uninitialised target: a reader that
    // samples the whole map (bilinear taps at the region's edge included) sees the picture it saw before.
    context->CopySubresourceRegion(_out.Get(), 0, 0, 0, 0, inputs.planarTexture, 0, nullptr);
    ++_lastOperations.copies;


    // The scalars, one dynamic buffer.
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT mapResult = context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ++_lastOperations.maps;
    if (FAILED(mapResult) || mapped.pData == nullptr)
        return nullptr;
    const Constants constants = ConstantsOf(inputs.facts, inputs.params);
    std::memcpy(mapped.pData, &constants, sizeof constants);
    context->Unmap(_params.Get(), 0);
    } // preparation

    if (!pipeline)
        return _outSrv.Get();

    if (pipeline) CollectGpuTiming(context, diagnosticSkipTiming);
    UINT trackedOm = 0;
    if (diagnosticCopy && ext::core::state_window::ReadTrackedOm(context, trackedOm)) _lastOperations.trackedOmBefore = static_cast<int32_t>(trackedOm);
    ComPtr<ID3D11RenderTargetView> nativeTargetBefore;
    if (diagnosticCopy) context->OMGetRenderTargets(1, &nativeTargetBefore, nullptr);
    omTrace.PhaseNow(ext::core::om_trace::Phase::SwapIn);
    // The game's own state is captured here (one explicit exchange, counted in `stateSwaps` as the swap was) and has to come back, also when the
    // draw throws through this fork's hooks: the guard restores in its destructor, and the scope closes the state window and the trace phases.
    ext::core::explicit_state::Guard stateGuard(context, context1.Get(), device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1, kSpec);
    ++_lastOperations.stateSwaps;
    struct StateScope
    {
        ext::core::explicit_state::Guard& guard;
        DiagnosticOperations* operations = nullptr;
        uint64_t window = 0;
        ext::core::om_trace::PassScope* trace = nullptr;
        bool open = true;
        void Close()
        {
            if (!open)
                return;
            open = false;
            ext::core::state_window::End(window);
            if (trace) trace->PhaseNow(ext::core::om_trace::Phase::SwapOut);
            guard.Restore();
            ++operations->stateSwaps;
            if (trace) trace->PhaseNow(ext::core::om_trace::Phase::Restored);
        }
        ~StateScope() { Close(); }
    } stateRestore { stateGuard, &_lastOperations, ext::core::state_window::Begin(context), &omTrace };
    stateGuard.Neutralise(); // what the private state's defaults provided: no HS, DS, GS, stream-out target or predicate
    const bool targetBindingsOnly = diagnosticStages == DiagnosticStages::TargetBindingsOnly;
    const bool otherBindingsOnly = diagnosticStages == DiagnosticStages::OtherBindingsOnly;
    const float blendFactor[4] {};
    ID3D11Buffer* buffers[1] { _params.Get() };
    ID3D11SamplerState* samplers[1] {};
    // The draw runs over the region only: the engine's own map is the picture in a session that renders the picture smaller than the allocation,
    // so the texels outside it keep the copy taken above.
    const D3D11_VIEWPORT viewport { float(region.x), float(region.y), float(region.width), float(region.height), 0.0f, 1.0f };

    omTrace.PhaseNow(ext::core::om_trace::Phase::PrivateBind);
    if (!targetBindingsOnly)
    {
    context->IASetInputLayout(nullptr);
    ++_lastOperations.pipelineBinds;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ++_lastOperations.pipelineBinds;
    context->VSSetShader(_vs.Get(), nullptr, 0);
    ++_lastOperations.pipelineBinds;
    context->PSSetConstantBuffers(0, 1, buffers);
    ++_lastOperations.pipelineBinds;
    context->PSSetSamplers(0, 1, samplers);
    ++_lastOperations.pipelineBinds;
    context->RSSetState(_rasterizer.Get());
    ++_lastOperations.pipelineBinds;
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    ++_lastOperations.pipelineBinds;
    context->OMSetDepthStencilState(nullptr, 0);
    ++_lastOperations.pipelineBinds;
    context->RSSetViewports(1, &viewport);
    ++_lastOperations.pipelineBinds;
    }
    bool timed = false;
    if (!targetBindingsOnly)
        timed = BeginGpuTiming(context, diagnosticSkipTiming);
    if (!otherBindingsOnly)
    {
    ID3D11RenderTargetView* outRtv = _outRtv.Get();
    context->OMSetRenderTargets(1, &outRtv, nullptr);
    ++_lastOperations.pipelineBinds;
    }
    if (!targetBindingsOnly)
    {
    ID3D11ShaderResourceView* views[4] {};
    views[kSlotPlanar] = inputs.planar;
    views[kSlotVPosition] = inputs.vposition;
    views[kSlotScene] = _sceneSrv.Get();
    views[kSlotCamera] = _cameraView.Get();
    context->PSSetShaderResources(0, 4, views);
    ++_lastOperations.pipelineBinds;
    context->PSSetShader(_search.Get(), nullptr, 0);
    ++_lastOperations.pipelineBinds;
    if (!diagnosticSkipDraw)
    {
        water_trace::PrivateDrawScope privateDraw(context);
        context->Draw(3, 0);
        ++_actualDrawSubmissions;
        ++_lastOperations.fullscreenDraws;
    }

    ID3D11ShaderResourceView* none[4] {};
    context->PSSetShaderResources(0, 4, none);
    ++_lastOperations.pipelineBinds;
    }
    if (!otherBindingsOnly)
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ++_lastOperations.pipelineBinds;
    }
    if (timed && _end && _disjoint)
    {
        context->End(_end.Get());
        context->End(_disjoint.Get());
        ++_lastOperations.timingEnds;
        ++_timingEnds;
        // `_timingPending` stays set: the pair is in the queue until CollectGpuTiming reads it on a later run.
    }
    stateRestore.Close();
    if (diagnosticCopy && ext::core::state_window::ReadTrackedOm(context, trackedOm)) _lastOperations.trackedOmAfter = static_cast<int32_t>(trackedOm);
    if (diagnosticCopy)
    {
        ComPtr<ID3D11RenderTargetView> nativeTargetAfter;
        context->OMGetRenderTargets(1, &nativeTargetAfter, nullptr);
        _lastOperations.nativeTargetRestored = nativeTargetAfter.Get() == nativeTargetBefore.Get();
    }

    _failure = nullptr;
    return _outSrv.Get();
}
} // namespace ext::ffxiv::engine::ssr
