#pragma once
// The one write the VFX mask gate probe makes: the mask input of the evaluate in progress. Templated on the parameter block, so
// the write path is covered by a fake NVSDK_NGX_Parameter in tests/ext/ffxiv/vfxmask instead of only by a text pin, and so the
// product hands its own (the bridge's) block to exactly the code the test exercises.
//
// NVSDK_NGX_Parameter::Set returns void, so "the backend took it" can only be read back: the key is read again and the pointer
// compared. The block is the bridge's own and the runtime reads it later in the same evaluate (the DirectX 11 bridge copies every
// input the parameters name, and the FSR and XeSS backends convert this slot into their reactive and responsive masks).
//
// The probe writes and never clears: on a frame it declines, the key keeps whatever the game put there (NULL), and the game sets
// every key it uses again on every evaluate.

#include "ext/ffxiv/vfxmask/VfxMaskCore.h"

#include <d3d11.h>
#include <nvsdk_ngx_defs.h>

namespace ext::ffxiv::vfxmask
{
// The bias current colour mask, the key the DirectX 11 bridge copies as "ReactiveMask" and hands to DLSS, FSR (reactive and
// transparency) and XeSS (responsive). Spelled from the SDK header so a wrong literal cannot happen.
inline constexpr const char* kMaskParameterKey = NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask;

struct ApplyResult
{
    Outcome outcome = Outcome::Refused;
    unsigned writes = 0; // how many times the key was written this evaluate (0 or 1)
};

// Hands `mask` to this evaluate's parameter block when the probe is armed and everything about it checks out. `mode` off, a size
// the plan rejects, a null texture or a null block: nothing is written.
template <class Params>
ApplyResult ApplyBiasMask(Params* parameters, ID3D11Resource* mask, ProbeMode mode, unsigned width,
                          unsigned height) noexcept
{
    ApplyResult result;
    if (mode == ProbeMode::Off || mask == nullptr || parameters == nullptr)
        return result;

    const MaskPlan plan = PlanMask(width, height, mode);
    if (!plan.ok)
        return result;

    parameters->Set(kMaskParameterKey, mask);
    result.writes = 1;

    ID3D11Resource* readBack = nullptr;
    if (parameters->Get(kMaskParameterKey, &readBack) != NVSDK_NGX_Result_Success)
        parameters->Get(kMaskParameterKey, reinterpret_cast<void**>(&readBack));

    result.outcome = readBack == mask ? Outcome::Given : Outcome::Unconfirmed;
    return result;
}
} // namespace ext::ffxiv::vfxmask
