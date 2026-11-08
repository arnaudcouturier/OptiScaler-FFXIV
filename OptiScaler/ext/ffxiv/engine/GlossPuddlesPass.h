#pragma once
// Puddles: the pass itself (ext/ffxiv/engine/shaders/GlossPuddles.hlsl).
//
// One draw on the game's own immediate context, inside a private device-context state (the pattern the fork's other passes use, so no game
// binding is touched by it): over the region of the engine's G-buffer target that lies inside the render window, reading the reader draw's
// depth, the G-buffer normal and the camera table, writing this module's own texture - a copy of the engine's target with the gloss channel
// shaped on the up-facing ground - in the engine's own size and format.
//
// The owned target starts as a copy of the engine's own texture, so every texel outside the region keeps the engine's value: the readers sample
// the whole target and this module only answers for the part the engine's own geometry pass wrote.
//
// The camera table (g_CameraParameter, 0x160 bytes through the projection) and the engine's wetness amount (g_WetnessParameter's first float) are
// copied GPU side into buffers of this pass with partial CopySubresourceRegion calls: nothing the frame waits on is read back, so no frame stalls on
// a map. Two reads reach the CPU without waiting for the GPU: the camera table, once a session, for the layout check that tells a game patch from a
// loading screen, and the engine's 32-byte wetness amount, once a frame, for the run line. Both are no-wait maps of copies an earlier frame took, so
// they are a frame behind; the shader reads the pass's own buffer, never the readback.
//
// Everything the pass creates lives here and is released with it; nothing of the game is written, cleared or rebound by this class.

#include "ext/ffxiv/engine/GlossPuddlesRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::engine::gloss
{
// What one run needs: the reader draw's own resources, its measured sizes and window, and the tuning keys.
struct PassInputs
{
    ID3D11ShaderResourceView* target = nullptr; // the reader's G-buffer target (the engine's texture, for the copy and the shape)
    ID3D11Texture2D* targetTexture = nullptr;   // the resource behind that view, copied into the owned target before the region is written
    ID3D11ShaderResourceView* depth = nullptr;  // the reader's g_SamplerDepth
    ID3D11ShaderResourceView* normal = nullptr; // the reader's g_SamplerGBuffer (the primary world-space normal)
    ID3D11Buffer* camera = nullptr;             // the reader's g_CameraParameter
    ReaderFacts facts;
    Params params;
};

class Pass
{
  public:
    // Runs the pass for one frame and returns the texture to bind at the target's slot, or null when it did not run. A null return is the
    // fail-open answer: the caller keeps the engine's own texture bound and counts the refusal.
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const PassInputs& inputs);

    // Forgets every device object (a device change, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many times the owned target was created: one per target size and format, so a test can pin that a frame does not create D3D objects
    // and that a size change does.
    uint32_t TargetsCreated() const noexcept { return _targetsCreated; }

    // The encoded result of the last run (the texture the reader draws bind) and its resource, for the caller's frame memory.
    ID3D11ShaderResourceView* OutputView() const noexcept { return _outSrv.Get(); }
    ID3D11Texture2D* OutputTexture() const noexcept { return _out.Get(); }

    // The engine's wetness amount, copied GPU side for the frame whose material draw handed it over. One 32-byte copy, and a second one into a
    // staging buffer that a later frame maps without waiting, so the CPU can name the engine's own amount in the run line.
    bool NoteWetness(ID3D11DeviceContext* context, ID3D11Buffer* game);

    // The wetness the shader gates on, written by the caller: the wetness knob's forced amount while the knob is armed, the engine's own amount
    // otherwise (GlossPuddlesRules.h's WetnessFor). `{amount, 1 - amount/2, 0, 0}` goes into the same 32-byte buffer the shader reads at its own
    // slot, so the shape follows the amount the engine's own draws read even when the copy of the game's buffer was taken before the knob patched
    // it. No game resource is touched and there is no readback.
    bool SetWetness(ID3D11DeviceContext* context, float amount);

    // The pass's own 32-byte wetness buffer, as the shader reads it at t4. The tone-map stage binds the same object, so the two stages gate on one
    // value and a frame the gloss pass shaped wet is the frame the mirror is allowed to shade.
    ID3D11ShaderResourceView* WetnessView() const noexcept { return _wetnessView.Get(); }

    // The engine's own wetness amount as the CPU last read it (the no-wait map above): a frame behind, and used for the run line only - the shader
    // reads the pass's own buffer. `EngineWetnessKnown` says whether a copy has landed at all this session. A map that has not landed leaves the
    // previous figure in place (and `Known` is false until the first one does), so a miss is never a delay, a failure or a refusal: the pass runs
    // and the line says the figure is not known yet.
    float EngineWetness() const noexcept { return _engineWetness; }
    bool EngineWetnessKnown() const noexcept { return _engineWetnessKnown; }

    // ---- the frame's answers (GlossPuddlesRules.h's pairing) ----------------------------------------------------------------
    //
    // The pools' two stages share this pass's object, so the answers the two of them give to one frame are one thing
    // with it: the wetness latch the two gates read (WetEnoughDwell, answered once per frame on the frame's own
    // wetness copy), the frame whose composite holds the pass's shape, and the last good shape a refused attempt can
    // answer the frame's readers with instead of the unshaped engine target - the hold that keeps a skip from painting
    // the engine's dry picture in the middle of the ramp. Everything here lives and dies with the pass.

    // The one wetness latch both stages read. Updated wherever the frame's copy lands (the first draw that binds the
    // engine's buffer), so the two stages cannot disagree inside a frame the way two latches of their own could.
    void NoteWetLatch(bool latched) noexcept { _wetLatch = latched; }
    bool WetLatched() const noexcept { return _wetLatch; }

    // The frame token whose composite reads this pass's shape, and whether it does: the tone-map stage pairs its own
    // answer with it, so the darkening and the mirror run exactly on the frames whose gloss channel was shaped (or
    // held-shaped - a held shape is the frame's answer too). `shaped` is false for a frame the pass did not shape and
    // the hold did not answer; `Reset()` clears it (a publish-off, a teardown), and so leaves no answer behind.
    void NoteShapeFrame(uint64_t token, bool shaped) noexcept
    {
        _shapeToken = token;
        _shapeOnFrame = shaped;
    }
    bool ShapePresent(uint64_t token) const noexcept { return ShapePresentOnFrame(_shapeOnFrame, _shapeToken, token); }

    // The last good shape on its target's object, recorded after every pass that ran (a shape computed one frame is
    // the next frame's hold when that frame's own attempt is refused). The two `const void*`s are the fact the frame
    // cache compares the same way: the engine's resource, and its view. Cleared with the pass's own reset, so it can
    // never outlive the texture it names.
    void NoteHold(const void* engineResource, const void* engineView) noexcept
    {
        _holdEngine = engineResource;
        _holdEngineView = engineView;
    }
    const void* HoldEngineResource() const noexcept { return _holdEngine; }
    const void* HoldEngineView() const noexcept { return _holdEngineView; }
    // Whether the pass owns a whole picture a hold may answer with: the output texture is only the frame's answer
    // once a run has written it (a fresh target waiting for its first copy is not one).
    bool OutputReady() const noexcept { return _outValid; }
    // The sized form the gloss hold answers through: a hold bound across a resize would show the last size's measure
    // until the first run of the new one (the review's F5, the gloss side of it).
    bool OutputReady(uint32_t width, uint32_t height) const noexcept { return _outValid && _width == width && _height == height; }
    // Whether the pass's objects belong to `device`: a device-removed frame must not answer a hold with the dead device's texture
    // (the review's device-loss walk).
    bool OnDevice(ID3D11Device* device) const noexcept { return _device.Get() == device; }

    // Whether this session's camera table has been checked against the layout the shaders read (the one-time check, below).
    bool CameraChecked() const noexcept { return _cameraChecked; }

    // Whether the last camera check refused the table. The caller declines with its own reason (and does not count it towards the switch-off):
    // a loading screen's camera is not a build this module cannot serve.
    bool CameraRejected() const noexcept { return _cameraRejected; }

    // Whether the last camera check's copy had not landed on the CPU yet: a frame of delay, not a refusal. The caller declines with its own reason
    // and does not count it either.
    bool CameraNotReady() const noexcept { return _cameraNotReady; }

    // The one-time camera layout check: copies kCameraBytes of `game` into one of two staging buffers and reads a copy an earlier frame took with
    // a no-wait map, so the check never blocks the render thread inside one of the game's draws. It answers true once the table is checked, false
    // while it has not been read yet, while a refused table waits for its retry, and when the table is not a camera. The answer is cached, so a
    // session pays one copy and one map; a refused table is looked at again after kCameraCheckSeconds, because the first light draws of a session
    // can carry a loading camera.
    bool CheckCamera(ID3D11DeviceContext* context, ID3D11Buffer* game);

  private:
    bool Init(ID3D11Device* device);
    bool EnsureTarget(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& engineDesc);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params; // the scalars of the draw (GlossPuddlesRules.h's Constants)
    Microsoft::WRL::ComPtr<ID3D11Buffer> _camera; // kCameraBytes: g_CameraParameter +0x000..0x160
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _cameraView;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _wetness; // kWetnessBytes: g_WetnessParameter, .x the wetness amount
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _wetnessView;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _wetnessStaging[2]; // the engine's own amount, read on a later frame without waiting
    Microsoft::WRL::ComPtr<ID3D11Buffer> _cameraStaging[2]; // the camera check's copies: one frame's copy, read on a later frame
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _out;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _outRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _outSrv;
    uint32_t _width = 0, _height = 0;
    uint32_t _format = 0;
    uint32_t _targetsCreated = 0;
    bool _wetnessPending = false;     // a wetness copy is waiting to be read
    uint32_t _wetnessPendingSlot = 0; // and which of the two staging buffers holds it
    float _engineWetness = 0.0f;      // the engine's own amount as the CPU last read it (the log line's `engine` figure)
    bool _engineWetnessKnown = false;
    bool _cameraChecked = false;
    bool _cameraRejected = false;
    bool _cameraNotReady = false;      // the pending copy's map did not land this frame
    uint32_t _cameraNotReadyCount = 0; // frames the check waited for the GPU (diagnostics only)
    bool _cameraPending = false;       // a copy is waiting to be read
    uint32_t _cameraPendingSlot = 0;   // and which of the two buffers holds it
    double _cameraRetryAt = 0.0;       // the steady clock's seconds at which a refused table is looked at again
    // The frame's answers (above): the shared latch, the shaped frame, the hold and the output's own validity.
    bool _wetLatch = false;            // the one wetness latch both stages read
    uint64_t _shapeToken = 0;          // the frame token the shape (or the hold) is on
    bool _shapeOnFrame = false;        // the composite of that token reads the pass's shape
    const void* _holdEngine = nullptr; // the engine resource the last good shape was computed for
    const void* _holdEngineView = nullptr; // and the view that bound it (the same fact the frame cache compares)
    bool _outValid = false;            // the output texture holds a whole answer (a run has written it)
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine::gloss
