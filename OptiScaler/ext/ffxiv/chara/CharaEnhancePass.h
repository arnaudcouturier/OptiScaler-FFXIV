#pragma once
// The spatial anti-aliasing pass of the character preview feature: runs chara_fxaa.hlsl over a source image into a private
// texture (1x or 2x), inside a private device-context state so the game's pipeline state is untouched. Knows nothing about
// the game: the integration (CharaEnhance.cpp) decides which draw gets the result.

#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::chara
{
class FxaaPass
{
  public:
    // Issues the full-screen triangle. Null = ctx->Draw(3, 0).
    using DrawFn = void (*)(ID3D11DeviceContext*);

    // Filters `source` (srcW x srcH) into an image of (srcW * scale) x (srcH * scale), scale 1 or 2. `srgb` selects the
    // sRGB views for sources that are sRGB views (the filter then works on linear values and the stored bytes stay sRGB).
    // `*out` receives the output shader resource view, owned by the pass and valid until the next Process or Reset.
    // Returns false (and leaves the context state as it found it) when the pass cannot run; Failure() then says why for
    // permanent failures. The pass must be used from one thread.
    bool Process(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, UINT srcW, UINT srcH, bool srgb, UINT scale,
                 ID3D11ShaderResourceView** out, DrawFn draw = nullptr, bool tint = false);

    // `tint` selects the debug variant of the filter: the output is mixed with magenta (config key PreviewDebugTint).

    // Releases every GPU object and clears a permanent failure.
    void Reset();

    // Text of a permanent failure (missing interface, shader creation failed), null while the pass is usable.
    const char* Failure() const { return _failure; }

    // The private output texture (the image of the last Process), or null before the first one. Owned by the pass; used by the
    // preview image dump.
    ID3D11Texture2D* OutputTexture() const { return _output.Get(); }

    UINT OutputWidth() const { return _outWidth; }
    UINT OutputHeight() const { return _outHeight; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureOutput(ID3D11Device* device, UINT width, UINT height, bool srgb);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _psTint;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _outputRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _outputSrv;
    UINT _outWidth = 0;
    UINT _outHeight = 0;
    bool _outSrgb = false;

    const char* _failure = nullptr;
};

// Binds `replacement` as pixel-shader resource 0 while `draw` runs, then puts back whatever was bound before.
template <class F> inline void WithSource0(ID3D11DeviceContext* context, ID3D11ShaderResourceView* replacement, F&& draw)
{
    ID3D11ShaderResourceView* previous = nullptr;
    context->PSGetShaderResources(0, 1, &previous); // adds a reference
    context->PSSetShaderResources(0, 1, &replacement);
    draw();
    context->PSSetShaderResources(0, 1, &previous);
    if (previous != nullptr)
        previous->Release();
}
} // namespace ext::ffxiv::chara
