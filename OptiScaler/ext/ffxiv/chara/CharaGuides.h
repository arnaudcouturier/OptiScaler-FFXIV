#pragma once
// The helper passes of the isolated DLSS path (shader: shaders/chara_guides.hlsl): Copy (native image -> colour input),
// Guides (depth + signed motion with the engine's camera offset removed) and Alpha (upscaled colour + native alpha).
//
// Every pass runs inside a private ID3DDeviceContextState (feature level 11_0): while a Scope is alive the context shows
// an empty pipeline, and leaving it swaps the game's state back untouched, whatever the passes bound. Nothing in this file
// knows the game: the caller supplies the shader bytecode, the camera copy and the full-screen draw.

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::chara::guides
{
struct Bytecode
{
    const void* data = nullptr;
    size_t size = 0;
};

struct Shaders
{
    Bytecode vs, copy, guides, alpha;
    Bytecode alphaTint;   // optional: the Alpha pass with the debug tint of a super-resolution image (magenta)
    Bytecode alphaTintNr; // optional: ... and of an image NR produced (cyan)
};

// The debug tint of the Alpha pass (PreviewDebugTint): which image the pass is making. A shader that was not supplied falls back to the
// plain Alpha pass.
enum class Tint : uint8_t
{
    None,
    Sr, // a super-resolution image: magenta
    Nr, // an image NR produced: cyan
};

// Layout of the Settings constant buffer (register b0): Jitter (pixels), InvSize (1 / render size). 16 bytes.
struct Settings
{
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float invWidth = 0.0f;
    float invHeight = 0.0f;
};
static_assert(sizeof(Settings) == 16, "must match cbuffer Settings in chara_guides.hlsl");

// Issues the full-screen triangle (Draw(3, 0)); in the game this is the original, unhooked Draw.
using DrawTriangleFn = void (*)(ID3D11DeviceContext*);

class Passes
{
  public:
    bool Init(ID3D11Device* device, const Shaders& shaders, DrawTriangleFn drawTriangle);
    void Release();
    bool Ready() const { return _state != nullptr; }

    // RAII: enters the private state, restores the game's state on destruction.
    class Scope
    {
      public:
        Scope(Passes& passes, ID3D11DeviceContext* context, const Settings& settings, ID3D11Buffer* camera, UINT first,
              UINT count);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        bool Ok() const { return _ok; }

        // Each returns false when the pass could not run. `tint` (Alpha): the debug variant, falls back to the plain one when the
        // tinted shader was not supplied.
        bool Copy(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height);
        bool Guides(ID3D11ShaderResourceView* motion, ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* depthTarget,
                    ID3D11RenderTargetView* motionTarget, uint32_t width, uint32_t height);
        bool Alpha(ID3D11ShaderResourceView* upscaled, ID3D11ShaderResourceView* original, ID3D11RenderTargetView* target,
                   uint32_t width, uint32_t height, Tint tint = Tint::None);
        // The cyan tint over a finished image (the bring-up NR result): colour and alpha both come from `source`. The Scope's settings
        // must have a zero jitter, or the alpha is read at a shifted position.
        bool TintResult(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width, uint32_t height);

      private:
        bool Draw(ID3D11PixelShader* ps, ID3D11ShaderResourceView* const (&srvs)[4], ID3D11RenderTargetView* const* targets,
                  UINT targetCount, uint32_t width, uint32_t height);

        Passes& _passes;
        ID3D11DeviceContext* _context = nullptr;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> _context1;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> _previous;
        bool _ok = false;
    };

  private:
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _copy, _guides, _alpha, _alphaTint, _alphaTintNr;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _settings;
    DrawTriangleFn _drawTriangle = nullptr;
};
} // namespace ext::ffxiv::chara::guides
