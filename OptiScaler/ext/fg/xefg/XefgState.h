#pragma once
// D3: per-XeFG_Dx12 fork state (Dx12Ext::xefg): dynamic multi frame generation. These were XeFG_Dx12::_dynamic,
// _dynamicWasEnabled and _dynamicLastTarget in the reference build. Touched only from XeFG_Dx12::Dispatch.

#include "ext/fg/xefg/XeFGDynamic.h"

namespace ext::fg
{
struct XefgState
{
    XeFGDynamic dynamic;
    bool wasEnabled = false;
    float lastTarget = 0;
};
} // namespace ext::fg
