#include "pch.h"

#include "ext/Seams.h"
#include "ext/present/CompositionWindow.h"

// E5: the composition-swapchain half of the oracle's swapchain size-to-window feature (the 0x0 resolve half is not
// carried, SEAMS.md section 5). This file holds the wrapper-side decision seam; the factory hook joins it in the
// commit that installs it.

namespace ext::present
{
bool ResolveCompositionHandle(HWND& handle)
{
    if (!IsCompositionWindow(handle))
        handle = FindCompositionWindow();

    return handle != nullptr;
}
} // namespace ext::present
