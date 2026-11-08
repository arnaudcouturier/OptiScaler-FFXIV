#pragma once
// Engine light controls: the blocking reader rule. While the moon's light draw runs with its g_LightParam swapped for the patched copy, every
// reader of the light colour that runs INSIDE that swap (the chara ambient's night feed, the contact shadows and the shadow mapping route's colour
// capture, which the night curve and its bright-light veto read) must see the ENGINE's value, otherwise moon x0.1 feeds back into the night weight.
//
// The readers that run before the swap (the seam modules: shafts, god rays, light bounce, night grade, atmosphere, depth of field, and the F9 dump)
// already read the game's buffer; the three readers inside the draw call map what they fetched with OwnedGameBuffer right after their
// PSGetConstantBuffers. Header only and free of any link dependency, so a test that compiles one of those readers links without the module.
//
// The game's buffer is never written (the patch works on GPU-side copies), so mapping back to it is always safe. Render thread only: the swap, the
// draw and the readers all run on the immediate context's thread.

#include <d3d11.h>

namespace ext::ffxiv::enginelight
{
namespace detail
{
inline ID3D11Buffer* g_patched = nullptr;  // the patched copy bound for the draw in flight (null = no swap in flight)
inline ID3D11Buffer* g_original = nullptr; // the game's buffer that copy stands in for
} // namespace detail

// Takes ONE reference to `bound` (what PSGetConstantBuffers returned) and returns ONE reference to the buffer the reader should look at: the game's
// buffer when `bound` is the patched copy of the draw in flight, `bound` itself otherwise. A null stays null.
inline ID3D11Buffer* OwnedGameBuffer(ID3D11Buffer* bound) noexcept
{
    if (bound == nullptr || detail::g_patched == nullptr || bound != detail::g_patched || detail::g_original == nullptr)
        return bound;
    detail::g_original->AddRef();
    bound->Release();
    return detail::g_original;
}

// Whether a swap is in flight (for the dump and the tests).
inline bool SwapInFlight() noexcept { return detail::g_patched != nullptr; }

// Marks the swap around the game's draw: the patched copy and the game's buffer it replaced. Nested scopes restore the previous pair; the
// destructor runs on every path, a throw included, so nothing dangles into the next draw.
class ActiveSwapScope
{
  public:
    ActiveSwapScope(ID3D11Buffer* patched, ID3D11Buffer* original) noexcept
        : _previousPatched(detail::g_patched), _previousOriginal(detail::g_original)
    {
        detail::g_patched = patched;
        detail::g_original = original;
    }
    ~ActiveSwapScope()
    {
        detail::g_patched = _previousPatched;
        detail::g_original = _previousOriginal;
    }
    ActiveSwapScope(const ActiveSwapScope&) = delete;
    ActiveSwapScope& operator=(const ActiveSwapScope&) = delete;

  private:
    ID3D11Buffer* _previousPatched;
    ID3D11Buffer* _previousOriginal;
};
} // namespace ext::ffxiv::enginelight
