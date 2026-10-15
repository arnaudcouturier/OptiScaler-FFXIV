#pragma once
// Per-object fork state of an IFGFeature_Dx12 (docs/SEAMS.md 3.2): the one `_ext` member of the upstream class.
// It covers DLSSG_Dx12, XeFG_Dx12 and FSRFG_Dx12 through the base class, so no member is added to them or to
// DLSSG_Common. Later series (B5 guide matcher, D2 observed multiplier, D3 XeFG dynamic state) add their state here
// behind out-of-line types, so this header stays free of upstream types.

#include "ext/gpu/Gpu.h"

#include <memory>

namespace ext::fg
{
class Dx12Ext
{
  public:
    Dx12Ext();
    ~Dx12Ext();
    Dx12Ext(const Dx12Ext&) = delete;
    Dx12Ext& operator=(const Dx12Ext&) = delete;

    // C6: the SC list is submitted in order with a monotonic fence value (was IFGFeature_Dx12::_scFenceValue and
    // _scSubmissionFailed). `failed` is permanent for this object: further SC rendering is disabled.
    gpu::OrderedFence sc;
};
} // namespace ext::fg
