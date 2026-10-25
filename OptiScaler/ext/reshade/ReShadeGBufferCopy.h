#pragma once
// Automatic ReShade G-buffers, the D3D11 half: an add-on (REST) binds the game's G-buffers (normals, decals, water decals, motion
// vectors, material) to ReShade's semantics. They are output-sized with the picture only in the top-left render window, exactly like
// the depth, and effects read them with UV 0..1. This pass copies the render window of the add-on's own typed view into an owned,
// output-sized texture of the SAME format, stretched by the exact-integer nearest mapping of the depth pass (origin 0,0), and hands
// the views of that copy to the binder (ReShadeDepthCore.h, IRemapEngine). Shader: shaders/gbuffer_remap.hlsl. Tests:
// tests/ext/reshade/reshade_gbuffer_copy.cpp (WARP).
//
// A sibling of DepthCopy (ReShadeDepthCopy.h) with the same mechanics, and kept apart from it: one GBufferRemap per G-buffer kind,
// never shared with the depth pass or another kind. The draw runs in a private ID3DDeviceContextState inside a BypassScope, so the
// game's pipeline state (predication included) is untouched and our own draw hooks ignore it. Nothing escapes the entry points: every
// D3D step is SEH-guarded, and the unbind and the swap back are separate steps, so a failed unbind cannot keep the game's state from
// coming back. A swap back that itself failed is done again by the next Remap for that context (or Release), before anything else,
// with the game's own state object (kept alive until then); the attempts are capped (kMaxRecoverAttempts), the pass then gives the
// state up on purpose, logs it once and LEAKS its reference to the game's state object (see DepthCopy). A deferred context is refused
// before the recovery, so it never touches the immediate context.
//
// The source is the add-on's view, bound as given: the pass reads ITS description (format, dimension, first mip) and never makes a view
// of the source. Formats: the float, UNORM and SNORM families read as four floats (ClassifyRemapFormat). A view that would convert on
// read (sRGB) or that is not a float / UNORM / SNORM colour format is left to the add-on (UnsupportedFormat). The 8-bit four-channel
// UNORM formats get the copy on a TYPELESS resource with a UNORM view and an sRGB twin, so a variable that asks for sRGB gets the
// same bytes read through the twin, as with the add-on's own binding.
//
// Values: the copy keeps the source's values. The one exception is MotionVectors with scaleMotion: the vectors are in render pixels
// (or a fraction of the render frame), the copy has the output size, so the first two channels are multiplied per axis by
// reportedScale / render (the MV scale the game tells the upscaler: under DLSS's convention stored value x scale = displacement in render
// pixels), or by output / render when the game reports none (MotionMultiplier). That scaling is DERIVED, not yet confirmed in game; it
// is only allowed where the format can hold the scaled values (float and SNORM, ClassifyRemapFormat::valueScalable).
//
// Lifetime: the views handed out (RemapOut) are NOT AddRef'd by the pass. The binder takes its own reference before it binds (same
// thread, no gap), so a reference the caller took keeps a copy alive after the pass dropped its own: a new target (new output size
// or format) is made and drawn FIRST, and the old views are released only after the new ones are ready. A failed draw keeps the old
// target and clears the cache key.
//
// Cache: the key is (the add-on's view, the stamp, the render size, the output size, scaleMotion, the two reported motion scales). The
// same key as the last successful Remap on a live target returns the same views with out.cached = true and draws nothing, not even a
// state swap (and without the context-state mutex).
//
// Negative cache: a target that could not be created (a format the GPU cannot render to) is remembered per (view format, output size)
// until either changes, the pass is released or the device changes; Remap then fails at once without a D3D call that creates anything.
// Failures of the transient steps (reading the view, the draw) are never remembered.
//
// Threading: Init, Remap and Release run on the thread that runs begin_effects (two possible paths: the HudLess UI-boundary
// render_effects call and ReShade's own pass in Present); the process-wide context-state mutex (d3d11::ContextStateMutex) serializes
// the swap-in .. draw .. swap-back sequence with the depth pass and the other G-buffer passes. The destructor must not run from a static destructor at DLL unload: call Release() while the device is alive.

#include "ext/reshade/ReShadeDepthCopy.h" // kMaxEdge, kMaxRecoverAttempts, PackPair, DrawTriangleFn
#include "ext/reshade/ReShadeDepthCore.h" // GBuffer, ViewDesc, RemapRequest, RemapStatus, RemapOut, IRemapEngine

#include <d3d11_1.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace ext::reshade::depth
{
// ---- pure helpers -------------------------------------------------------------------------------------------------------
// What the pass does with a view format. Supported: the float, UNORM and SNORM families listed in the .cpp, all read through
// Texture2D<float4>. Everything else is unsupported: typeless formats, every *_SRGB view format (a read through it would convert),
// UINT / SINT, depth, BC, UNKNOWN and anything not listed.
struct RemapFormat
{
    bool supported = false;
    bool valueScalable = false;                  // float or SNORM class: the only ones whose vector values may be scaled
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN; // the format of the copy's texture (the typeless parent for the 8888 twins)
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN; // the copy's srv (the UNORM view for the 8888 twins)
    DXGI_FORMAT srgbTwin = DXGI_FORMAT_UNKNOWN;  // the copy's second srv, UNKNOWN when the format has no sRGB twin
};
RemapFormat ClassifyRemapFormat(DXGI_FORMAT view);

// The pure request check, in this order: the view must have been read and be a plain 2D view (NotPlain2D), its format must be
// supported - and for a scaled MotionVectors request also valueScalable - (UnsupportedFormat), then the sizes (Refused): source,
// render and output edges in 1 .. kMaxEdge, the render window inside the source and not larger than the output on either axis. Ok when
// the pass can run. Whether the request is for the pass's own G-buffer kind is the pass's business (it refuses a mismatch as Refused).
RemapStatus CheckRemap(const ViewDesc& source, const RemapRequest& request);

// The multiplier of the first two channels. MotionVectors with scaleMotion, per axis: mvScale / render when the game reported a usable
// scale on that axis (finite and > 0; 1920 over 1920 is exactly 1), else output / render (2.0 at 3840 over 1920, 1.5 at 3840 over 2560);
// 1.0 for a render size of 0. Every other buffer, or scaleMotion false: exactly 1.0f (so the shader's multiply is bit exact). DERIVED, not
// yet confirmed in game.
struct ValueMultiplier
{
    float x = 1.0f, y = 1.0f;
};
ValueMultiplier MotionMultiplier(GBuffer buffer, bool scaleMotion, uint32_t outputW, uint32_t outputH, uint32_t renderW, uint32_t renderH,
                                float mvScaleX = 0.0f, float mvScaleY = 0.0f);

// Layout of cbuffer Remap (register b0), 16 bytes: the render size and the output size (x in the low 16 bits, sizes are at most
// kMaxEdge), then the multiplier. Mirrors gbuffer_remap.hlsl.
struct RemapConstants
{
    uint32_t render = 0;
    uint32_t output = 0;
    float mulX = 1.0f;
    float mulY = 1.0f;
};
static_assert(sizeof(RemapConstants) == 16, "must match cbuffer Remap in gbuffer_remap.hlsl");
RemapConstants MakeRemapConstants(const RemapRequest& request);

// The device the pass was set up on must be replaced: another device was given, or the old one reports it was removed.
bool RemapDeviceReplaced(const void* owned, const void* given, HRESULT removedReason);

// A device the set-up failed on carries a tag (private data, so it dies with the device and a new device at the same address is not
// refused): Remap does not try it again. Its own tag, apart from the depth pass's.
void MarkRemapDeviceFailed(ID3D11Device* device);
bool RemapDeviceMarkedFailed(ID3D11Device* device);

// What the pass needs to know about an add-on's view, read under a fault guard (the handle is untrusted). False = it could not be read.
// plain2D = a TEXTURE2D view of mip 0 of a one-slice, single-sample 2D texture.
bool ReadViewDesc(ID3D11ShaderResourceView* view, ViewDesc& out) noexcept;

// ---- the pass -----------------------------------------------------------------------------------------------------------
// What a Remap did and read, for the once-per-change log line of the wiring. Filled as far as the call got.
struct RemapReport
{
    RemapStatus status = RemapStatus::Failed;
    ViewDesc source;             // the add-on's view as read (zero when it could not be read)
    bool targetCreated = false;  // this call created the target (first remap, a new output size or a new format)
    bool cached = false;
};

class GBufferRemap
{
  public:
    // `kind` is authoritative: a request for another buffer is refused. `draw` as in DepthCopy (null = the context's own Draw).
    explicit GBufferRemap(GBuffer kind, DrawTriangleFn draw = nullptr);
    // Release(); a swap back that is still outstanding is given up on purpose (the game's state object is leaked, see the top).
    ~GBufferRemap();
    GBufferRemap(const GBufferRemap&) = delete;
    GBufferRemap& operator=(const GBufferRemap&) = delete;

    // Sets the pass up on `device` (private context state, shaders, constant buffer). Remap does this by itself for the device of the
    // context it is given; a device it could not be set up on is tagged and never tried again.
    bool Init(ID3D11Device* device);
    // Drops the target and every resource (the pass's own references; a reference the caller took keeps a copy alive). Safe twice.
    void Release();
    bool Ready() const { return _state != nullptr; }
    uint32_t InitFailures() const { return _initFailures; }
    GBuffer Kind() const { return _kind; }

    // Copies the render window of `srv` into the target and fills `out`. `srv` is the add-on's view (the key of the cache and the
    // one that is bound); `srvSrgb` is its sRGB twin, which is not read: the pass reads the UNORM view (a read through the sRGB one
    // would convert) and hands out an sRGB twin of its own. `report` may be null. Anything but Ok leaves `out` empty and the add-on's
    // binding to the caller.
    RemapStatus Remap(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* srvSrgb,
                      const RemapRequest& request, RemapOut& out, RemapReport* report = nullptr) noexcept;

  private:
    struct Target
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvSrgb; // null when the format has no sRGB twin
        uint32_t width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // the format of the add-on's view this target copies (the key of "same format")
    };
    struct Key
    {
        bool valid = false;
        const ID3D11ShaderResourceView* srv = nullptr; // compared as a pointer, never dereferenced
        uint64_t stamp = 0;
        uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
        bool scaleMotion = false;
        uint32_t motionScaleX = 0, motionScaleY = 0; // the bits of the reported scales (so a NaN is a key like any other)
    };
    // A target that could not be created: remembered per (view format, output size) until one changes, Release or a device change.
    struct TargetFailure
    {
        bool valid = false;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
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
    static void ScrubThunk(void* job);

    // Puts the game's state back (see the top). True = nothing is outstanding. A failed attempt counts; the last one gives up.
    bool RecoverState();
    // Gives the outstanding state up deliberately: logs once and leaks the reference to the game's state object.
    void DropStuck(const char* why);
    RemapStatus RemapImpl(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, const RemapRequest& request, RemapOut& out,
                          RemapReport& report);
    static void Fill(const Target& target, RemapOut& out);
    void Teardown();

    GBuffer _kind;
    DrawTriangleFn _draw;
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    uint32_t _initFailures = 0;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
    Target _target;
    Key _key;
    TargetFailure _targetFailure;
    // A swap back that failed: the game's state object and the context it belongs to, until a later call has done it again (or the
    // pass gave up, see the top). _stuckAttempts counts the failed attempts since.
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _stuckPrevious;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> _stuckContext;
    uint32_t _stuckAttempts = 0;
};

// ---- the engine ---------------------------------------------------------------------------------------------------------
// The live IRemapEngine: one GBufferRemap per kind (created lazily, never shared), the untrusted-handle reads and the routing.
// The raw (unhooked) Draw of the lighting hooks is used only for the context they hooked (glue::UseRawDraw); with any other context,
// or no hooks, the pass draws with the context's own Draw inside the bypass scope. Everything noexcept.
using HookedContextFn = ID3D11DeviceContext* (*)();
class LiveRemapEngine final : public IRemapEngine
{
  public:
    // Both may be null (no raw draw: always the context's own Draw).
    explicit LiveRemapEngine(DrawTriangleFn rawDraw = nullptr, HookedContextFn hookedContext = nullptr);
    ~LiveRemapEngine() override;
    LiveRemapEngine(const LiveRemapEngine&) = delete;
    LiveRemapEngine& operator=(const LiveRemapEngine&) = delete;

    bool DescribeView(uint64_t view, ViewDesc& out) noexcept override;
    RemapStatus Remap(const RemapRequest& request, RemapOut& out) noexcept override;
    // Releases every pass (publishes nothing: there is no sink). A later Remap sets the pass up again.
    void Release() noexcept override;

  private:
    struct Slot
    {
        std::unique_ptr<GBufferRemap> pass;
        bool rawDraw = false; // the draw the pass was made with
    };
    DrawTriangleFn _rawDraw;
    HookedContextFn _hookedContext;
    Slot _slots[kGBuffers];
};
} // namespace ext::reshade::depth
