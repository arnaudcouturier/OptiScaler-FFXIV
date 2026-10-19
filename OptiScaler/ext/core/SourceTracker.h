#pragma once
// Draw each shared menu function once per tab pass (docs/SEAMS-catalog.md, group `menu`).
//
// Some upstream cards have no draw function of their own: they name a shared `source` function that draws several
// cards at once (the Frame Gen runtime cards, Mipmap Bias + Anisotropic Filtering, the Compatibility advanced cards,
// ...). The tab loop in MenuCommon::RenderMainMenuTabs used to call such a function for the first box of a run of
// ADJACENT boxes with the same source and forgot it after any box that was not of that run, fork cards included. A fork
// card between two boxes of one function therefore made the function run, and draw all its cards, a second time.
//
// The loop now keeps a SourceTracker for the pass and calls the function only when Claim() says so. Consequence: a shared
// function is drawn at the position of its FIRST box, so a fork card can sit before or after a group of boxes of one
// function, never inside it (ext/core/MenuLayout.h and the menu manifest test keep it that way).
//
// A template on the function-pointer type so this header names no upstream type (upstream's DrawFn is local to the loop).
// Fixed storage: it is built every menu frame.

#include <array>
#include <cstddef>

namespace ext::menu
{
template <class Fn> class SourceTracker
{
  public:
    // Distinct shared functions seen in one tab pass; upstream has 5 in total (frame generation runtime, API and
    // texture, advanced, quirks, status)
    static constexpr size_t kCapacity = 16;

    // True when the caller must draw `source` now: the first time it is seen in this pass. A null source has nothing to
    // draw. If more than kCapacity distinct sources were seen, the extra ones are drawn for EVERY box of their group
    // (fail open: a function may draw several times, it is never hidden).
    bool Claim(Fn source)
    {
        if (source == nullptr)
            return false;

        for (size_t i = 0; i < _count; i++)
        {
            if (_seen[i] == source)
                return false;
        }

        if (_count < kCapacity)
            _seen[_count++] = source;

        return true;
    }

  private:
    std::array<Fn, kCapacity> _seen {};
    size_t _count = 0;
};
} // namespace ext::menu
