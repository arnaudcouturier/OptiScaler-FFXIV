#pragma once
// Automatic ReShade scene depth, the D3D11 half: copies the game's depth (cropped by the subrect origin) into an owned R32_FLOAT
// texture of the effect runtime's back-buffer size, nearest-mapped from the render size, and hands the copy to the depth binder
// (ReShadeDepthCore.h) through a sink. Shader: shaders/depth_crop.hlsl. Tests: tests/ext/reshade/reshade_depth_copy.cpp (WARP).
//
// Threading: Init, Capture, Withdraw and Release run on the thread that owns the immediate context (the NGX evaluate thread).
// Cost per Capture: one transient shader resource view of the game's depth, a 16-byte constant upload, two context-state swaps and one
// full-screen draw into the R32_FLOAT target (about 33 MB at 3840x2160, created once per output size).
//
// The draw runs in a private ID3DDeviceContextState inside a BypassScope, so the game's pipeline state (predication included) is
// untouched and our own draw hooks ignore it. Nothing escapes the entry points: every D3D step is SEH-guarded, and the unbind and the
// swap back are separate steps, so a failed unbind cannot keep the game's state from coming back. A swap back that itself failed is
// done again by the next Capture or Release, before anything else, with the game's own state object (kept alive until then).
//
// Limits: queries are not part of the state object, so an occlusion query that is open across a Capture counts the pass's pixels
// (the output size). DrawTriangleFn is fixed at construction: the wiring constructs the pass once the draw it wants to pass exists.
//
// Publishing: the pass owns the order. The binder keeps the raw value of the view of the capture it holds and AddRefs it per pass,
// so a new capture is published BEFORE the view of the old one is released; Release(), a device change and the destructor publish
// valid = false BEFORE anything is released. The sink must outlive the pass, and the destructor must not run from a static destructor
// at DLL unload: call Release() while the sink and the device are alive.

#include "ext/reshade/ReShadeDepthCore.h"

#include <d3d11_1.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>

namespace ext::reshade::depth
{
// Largest source, render and output edge: the D3D11 texture limit, and what keeps the shader's integer maths in 32 bits
// ((2 * 0x3FFF + 1) * 0x4000 < 2^32).
inline constexpr uint32_t kMaxEdge = 0x4000;

// ---- pure helpers -------------------------------------------------------------------------------------------------------
// The typed view format a depth source is read with, DXGI_FORMAT_UNKNOWN for a format the pass does not read.
DXGI_FORMAT SrvFormatFor(DXGI_FORMAT source);

// What the pass needs to know about the game's depth texture.
struct SourceDesc
{
    uint32_t width = 0, height = 0;
    uint32_t arraySize = 0;
    uint32_t sampleCount = 0;
    uint32_t bindFlags = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// One capture request.
struct CaptureArgs
{
    uint32_t outputW = 0, outputH = 0; // size of the copy: the effect runtime's back buffer (ConsumerInfo width x height)
    uint32_t baseX = 0, baseY = 0;     // top-left of the rendered region inside the game's depth (Depth.Subrect.Base)
    uint32_t renderW = 0, renderH = 0; // size of the rendered region
    uint64_t frame = 0;                // DepthCapture::frame
    int64_t tickMs = 0;                // DepthCapture::tickMs
};

enum class Result : uint8_t
{
    Ok,
    NullArgument,     // no context or no source
    DeferredContext,  // the context is not the immediate one
    DeviceUnavailable, // the pass could not be set up on the context's device (or the device was removed)
    BadOutputSize,
    BadRenderSize,
    BadSourceSize,
    NotTexture2D,
    ArrayTexture,
    Multisampled,
    NoShaderResource,
    UnsupportedFormat,
    NotContained,     // the render region does not lie inside the source
    ResourceFailure,  // a view, the target or the constant buffer could not be created or written
    Fault,            // the draw failed (exception or access violation); the game's state was put back
};
const char* ResultName(Result result);

// Source, render and output checks in one place (no D3D calls). Result::Ok when the pass can run.
Result CheckSource(const SourceDesc& source, const CaptureArgs& args);

// Layout of cbuffer Crop (register b0), 16 bytes. Every field packs two 16-bit values, x in the low half (sizes and origins are
// at most 0x4000): the origin, the render size and the output size.
struct CropConstants
{
    uint32_t origin = 0;
    uint32_t render = 0;
    uint32_t output = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(CropConstants) == 16, "must match cbuffer Crop in depth_crop.hlsl");
constexpr uint32_t PackPair(uint32_t x, uint32_t y) { return (x & 0xFFFFu) | (y << 16); }
CropConstants MakeConstants(const CaptureArgs& args);

// The device the pass was set up on must be replaced: another device was given, or the old one reports it was removed.
bool DeviceReplaced(const void* owned, const void* given, HRESULT removedReason);

// A device the set-up failed on carries a tag (private data, so it dies with the device and a new device at the same address is not
// refused): Capture does not try it again.
void MarkDeviceFailed(ID3D11Device* device);
bool DeviceMarkedFailed(ID3D11Device* device);

// ---- the sink -----------------------------------------------------------------------------------------------------------
// Receives every capture the pass publishes (valid = false withdraws). In the wiring it forwards to DepthBinder::PublishCapture.
class ICaptureSink
{
  public:
    virtual ~ICaptureSink() = default;
    virtual void Publish(const DepthCapture& capture) noexcept = 0;
};

// ---- the pass -----------------------------------------------------------------------------------------------------------
// Issues the full-screen triangle (Draw(3, 0)) on the immediate context; the default is the context's own Draw. A hook of the
// wiring (the original, unhooked Draw) or a test goes here.
using DrawTriangleFn = void (*)(ID3D11DeviceContext*);

// What a Capture did and read, for the once-per-size-change log line of the wiring. Filled as far as the call got.
struct CaptureReport
{
    Result result = Result::Ok;
    SourceDesc source;                       // the game's depth as read (zero when it could not be read)
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // the typed view format the source was read with
    CaptureArgs args;                        // as requested
    bool targetCreated = false;              // this call created the R32_FLOAT target (first capture or a new output size)
};

class DepthCopy
{
  public:
    explicit DepthCopy(ICaptureSink& sink, DrawTriangleFn draw = nullptr);
    // Release(): publishes valid = false first.
    ~DepthCopy();
    DepthCopy(const DepthCopy&) = delete;
    DepthCopy& operator=(const DepthCopy&) = delete;

    // Sets the pass up on `device` (private context state, shaders, constant buffer). Capture does this by itself for the
    // device of the context it is given; a device it could not be set up on is tagged and never tried again.
    bool Init(ID3D11Device* device);
    // valid = false first (if a capture is published), then every resource goes. Safe to call twice.
    void Release();
    // valid = false for the capture that is published (the consumer went away); keeps the resources.
    void Withdraw();
    bool Ready() const { return _state != nullptr; }
    uint32_t InitFailures() const { return _initFailures; }

    // Copies the game's depth into the target and publishes it. False (nothing new published) when the request is refused or
    // the draw failed; a failure after the target was touched also withdraws the capture that was published. `report` may be null.
    bool Capture(ID3D11DeviceContext* context, ID3D11Resource* source, const CaptureArgs& args, CaptureReport* report = nullptr) noexcept;

  private:
    struct Target
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t width = 0, height = 0;
    };
    struct InitJob;
    struct Job;
    static void InitThunk(void* job);
    static void ProbeThunk(void* job);
    static void PrepareThunk(void* job);
    static void DrawThunk(void* job);
    static void UnbindThunk(void* job);
    static void SwapBackThunk(void* job);

    bool RecoverState();
    bool CaptureImpl(ID3D11DeviceContext* context, ID3D11Resource* source, const CaptureArgs& args, CaptureReport& report);
    void PublishValid(const Target& target, const CaptureArgs& args);
    void Teardown();

    ICaptureSink& _sink;
    DrawTriangleFn _draw;
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    uint32_t _initFailures = 0;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
    Target _target;
    bool _published = false; // a valid capture is with the sink
    // A swap back that failed: the game's state object and the context it belongs to, until a later call has done it again
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _stuckPrevious;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> _stuckContext;
};
} // namespace ext::reshade::depth
