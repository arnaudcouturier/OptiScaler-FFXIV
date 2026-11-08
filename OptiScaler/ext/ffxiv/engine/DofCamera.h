#pragma once
// Depth of field, Route B (ext/ffxiv/engine): the frame's captures (DofCamera.cpp), the march's own pattern on this
// stage's own needs.
//
// The stage reads three things a frame, all copied GPU side at the draws that bind them and mapped at a later frame's
// tone map, never waiting for the GPU:
//  - the camera buffer the light draws bind (the march's own capture shape: the largest bound buffer in 0x160..0x400
//    that is not the light), whose projection at +0x120 and inverse projection at +0x0E0 are the CoC's metres;
//  - the fog post's 32-byte viewport buffer (cDynamicViewportResolutionParam), whose second float4 is read and logged only: the fog post's
//    offset row 1 both the pack's reconstruction and the focus centre read walk;
//  - the scene depth, as the fog post binds it (the fog post's own t0), kept as a reference the way the march keeps its
//    own depth view.
//
// The class is device work only (no product state), so tests/ext/ffxiv/engine drives it on a software adapter with real
// buffers. Nothing of the game is ever written; the reads never wait for the GPU (the D3D11_MAP_FLAG_DO_NOT_WAIT rule).

#include "ext/ffxiv/engine/DofRules.h"
#include "ext/ffxiv/engine/ShaftsSun.h" // the ring cadence, reused as types

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::dof
{
// One frame's camera, read back from the ring: the projection the band derives the planes from and the inverse
// projection the pack's reconstruction walks. The bytes are this module's own decode of the buffer the light draws bind
// (the march's own offsets, its own file's fields reused as constants).
struct CameraRead
{
    bool landed = false;          // the camera's bytes were mapped
    bool usable = false;          // the layout's rows decoded, and the band accepts the plane pair they derive
    bool notReady = false;        // a copy the read was allowed to take had not landed (the GPU is behind)
    float projection[16] {};
    float inverseProjection[16] {};
    DepthBand band;
    uint64_t writtenFrame = 0;
};

// The camera reader: one staging buffer per ring slot (the ShaftsSun pattern, this stage's own instance), the whole
// kCameraBytes window copied from whatever buffer the light draw bound, at the draw's capture.
class CameraReader
{
  public:
    // Copies this frame's camera bytes into the ring slot for `frame`. False when the staging buffer could not be
    // created (the caller keeps its hold, one line says so).
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* camera, uint64_t frame);

    // Reads the newest copy the ring still holds of the frames before this one, newest first and without waiting for
    // the GPU (the ring's age rule). `block` waits instead; only a test asks for it.
    CameraRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    bool HaveRead() const noexcept { return _haveRead; }
    bool HaveUsable() const noexcept { return _haveUsable; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }
    uint32_t Reads() const noexcept { return _reads; }
    uint32_t NotReady() const noexcept { return _notReady; }
    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> camera;
        const void* source = nullptr;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveRead = false;
    bool _haveUsable = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
    uint32_t _reads = 0;
    uint32_t _notReady = 0;
};

// One frame's copy of the fog post's 32-byte viewport buffer, second float4 (the march's FogReader stores the same row). The stage maps
// pixels to NDC from the render window instead; this copy is what the one log line next to that mapping prints.
struct RowRead
{
    bool landed = false;
    bool usable = false;
    float ndcScale[2] {};
    float ndcOffset[2] {};
    uint64_t writtenFrame = 0;
};

// The mapping row's reader: one staging buffer per ring slot, copied at the fog post's draw beside the depth.
class RowReader
{
  public:
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* row, uint64_t frame);
    RowRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);
    bool HaveRead() const noexcept { return _haveRead; }
    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> row;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
};

// The scene depth view, kept the march's own KeepDepth way: an own view is not needed (the pass reads the view the fog
// post bound for one draw inside its own state), so the reference is kept and the sizes recorded.
class DepthKeep
{
  public:
    void Keep(ID3D11ShaderResourceView* source, uint32_t format, uint32_t width, uint32_t height);
    ID3D11ShaderResourceView* View() const noexcept { return _view.Get(); }
    uint32_t Format() const noexcept { return _format; }
    uint32_t Width() const noexcept { return _width; }
    uint32_t Height() const noexcept { return _height; }
    void Reset();

  private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _view;
    uint32_t _format = 0, _width = 0, _height = 0;
};
} // namespace ext::ffxiv::dof
