#pragma once
// The debug tint (config key PreviewDebugTint) of the preview NR bring-up result.
//
// The bring-up result is a finished image that a UI draw samples as it is, and its image is written by the D3D12 queue's pass (a shared
// image the D3D11 side cannot draw into), so the cyan tint is one extra D3D11 pass: the result is drawn, tinted, into a private target
// of the same size, and the UI draw is given that target instead. The pass is made once per result (the module numbers its results), not
// once per UI draw, and the target is released when nobody asked for it for a while.
//
// Why a D3D11 pass and not a tint flag in the D3D12 resolve: the resolve is the main NR pass's shader and its constant block is full
// (256 bytes); a flag there would change the main pass for a debug aid, in the one place that is used by everybody. This pass lives next
// to the other preview helper passes (chara_guides.hlsl), costs nothing unless the tint is on, and is tested on its own (chara_nr_tint_warp).
// The production result needs no such pass: it goes through the alpha pass of the super-resolution path, which has the cyan variant.

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

#include "ext/ffxiv/chara/CharaGuides.h"

namespace ext::ffxiv::chara::nr
{
class TintTargets
{
  public:
    static constexpr size_t kCapacity = 16; // one per preview image, like the streams

    // The tinted copy of the bring-up result `source` (R16G16B16A16_FLOAT, `serial` identifies the result: the NR module never reuses one).
    // The pass is drawn when the target does not yet show this result; `drawn` (optional) says whether it was. Null when the tint could not
    // be made (no cyan shader, no target, the pass failed): the caller then gives the UI draw the untinted result and does not count a tint.
    // Draw thread only. `key` is the preview image's texture pointer (never dereferenced).
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Tinted(guides::Passes& passes, ID3D11Device* device, ID3D11DeviceContext* context,
                                                            uintptr_t key, ID3D11ShaderResourceView* source, uint64_t serial, uint64_t now,
                                                            bool* drawn = nullptr);

    // Releases the targets nobody asked for in the last `unusedMs`.
    void Release(uint64_t now, uint64_t unusedMs);
    void Clear();
    size_t Count() const;

  private:
    struct Entry
    {
        uintptr_t key = 0;
        uint64_t serial = 0;  // the result the target shows (0: nothing yet)
        uint64_t lastUse = 0;
        uint32_t width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    };
    std::array<Entry, kCapacity> _entries {};
};
} // namespace ext::ffxiv::chara::nr
