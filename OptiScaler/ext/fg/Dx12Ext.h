#pragma once
// Per-object fork state of an IFGFeature_Dx12 (docs/SEAMS.md 3.2): the one `_ext` member of the upstream class.
// It covers DLSSG_Dx12, XeFG_Dx12 and FSRFG_Dx12 through the base class, so no member is added to them or to
// DLSSG_Common. Later series (B5 guide matcher, D2 observed multiplier, D3 XeFG dynamic state) add their state here
// behind out-of-line types, so this header stays free of upstream types.

#include "ext/gpu/Gpu.h"

#include <atomic>
#include <cstdint>
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

    // D2 (DLSS-G on the Dx12 output): the latest observed output frames per real frame (tick count << 8 | multiplier,
    // 0 = none), the DLSS-G mode of the last sampled query (-1 = none) and the result/mode of the last SetOptions.
    std::atomic<uint64_t> observedMultiplier { 0 };
    int sampleMode = -1;
    bool setOptionsOk = false;
    uint32_t optionsMode = 0; // sl::DLSSGMode, eOff = 0
};
} // namespace ext::fg
