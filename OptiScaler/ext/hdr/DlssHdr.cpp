#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/game/Game.h"
#include "ext/hdr/scene/DlssSceneInput.h"
#include "ext/nr/NgxResourceBinding.h"

#include <State.h>

// C10: the DLSS scene HDR input (blocks of the reference build's DLSSFeature_Dx12.cpp / IFeature_Dx11wDx12.cpp), behind
// seams; logic and log messages unchanged. Scene HDR is requested through the game module's ISceneHdr.
namespace ext::hdr
{
void AdjustDlssCreateFlags(DlssExt& ext, NVSDK_NGX_Parameter* params)
{
    auto& cfg = ext::Cfg().hdr;
    ext.sceneHdr = DlssSceneInput::bridgeInitialising && cfg.ffxivHdr.value_or_default() &&
                   cfg.mode.value_or_default() == 1 && cfg.dlssSceneInput.value_or_default();
    ext.originalFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &ext.originalFlags);

    if (ext.sceneHdr)
    {
        unsigned int flags = ext.originalFlags;
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                    flags | NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
        LOG_INFO("DLSS scene HDR input enabled: linear FP16 reconstruction, SDR output for ReShade");
    }
}

void RestoreDlssCreateFlags(DlssExt& ext, NVSDK_NGX_Parameter* params)
{
    if (ext.sceneHdr)
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, ext.originalFlags);
}

namespace
{
struct EvalState
{
    DlssExt* ext;
    NVSDK_NGX_Parameter* params;
    NgxResourceBinding::Binding<NVSDK_NGX_Parameter> colour, output, exposure;
    unsigned int reset = 0;
    std::shared_ptr<DlssSceneInput::Packet> packet;
    EvalState(DlssExt* e, NVSDK_NGX_Parameter* p)
        : ext(e), params(p), colour(p, NVSDK_NGX_Parameter_Color), output(p, NVSDK_NGX_Parameter_Output),
          exposure(p, NVSDK_NGX_Parameter_ExposureTexture)
    {
    }
};
} // namespace

DlssEvalScope::DlssEvalScope(DlssExt& ext, ID3D12Device* device, ID3D12GraphicsCommandList* commands,
                             NVSDK_NGX_Parameter* params, unsigned renderWidth, unsigned renderHeight, unsigned handleId)
    : _commands(commands)
{
    if (!ext.sceneHdr)
        return;

    auto state = std::make_shared<EvalState>(&ext, params);
    auto* colour = state->colour.original;
    auto* output = state->output.original;
    params->Get(NVSDK_NGX_Parameter_Reset, &state->reset);

    SceneInput scene;
    if (auto* provider = game::Current().hdr)
        scene = provider->Open(device, renderWidth, renderHeight, false);

    const char* failure = "unknown";
    state->packet = DlssSceneInput::Prepare(device, commands, colour, output, renderWidth, renderHeight, std::move(scene), &failure);

    if (!state->packet)
    {
        LOG_ERROR("DLSS scene HDR preparation failed: {}; render {}x{}. Recreating ordinary DLSS for this session.", failure,
                  renderWidth, renderHeight);
        for (auto* resource : { colour, output })
            if (resource)
            {
                auto desc = resource->GetDesc();
                LOG_ERROR("DLSS scene HDR resource: {}x{}, format {}, flags {}, mips {}, samples {}, array {}", desc.Width,
                          desc.Height, (UINT) desc.Format, (UINT) desc.Flags, desc.MipLevels, desc.SampleDesc.Count,
                          desc.DepthOrArraySize);
            }
        ext::Cfg().hdr.dlssSceneInput.set_volatile_value(false);
        State::Instance().changeBackend[handleId] = true;
        _ok = false;
        return;
    }

    const int available = state->packet->scene.hdr ? 1 : 0;
    if (available != ext.available)
    {
        params->Set(NVSDK_NGX_Parameter_Reset, 1u);
        ext.available = available;
        LOG_INFO("DLSS scene HDR: {}", available ? "native HDR highlights entering DLSS" : "linear SDR fallback (no same-frame scene)");
    }

    state->colour.Set(state->packet->input.Get());
    state->output.Set(state->packet->output.Get());
    state->exposure.Set(nullptr);
    _state = state;
}

void DlssEvalScope::Finish(bool success)
{
    if (!_state)
        return;

    auto state = std::static_pointer_cast<EvalState>(_state);
    _state.reset();
    state->colour.Restore();
    state->output.Restore();
    state->exposure.Restore();
    state->params->Set(NVSDK_NGX_Parameter_Reset, state->reset);
    DlssSceneInput::Finish(state->packet, _commands, success);

    if (success)
        if (auto* provider = game::Current().hdr)
            provider->PublishReconstructed(DlssSceneInput::Image(state->packet));
}

BridgeInitScope::BridgeInitScope() : _previous(DlssSceneInput::bridgeInitialising) { DlssSceneInput::bridgeInitialising = true; }
BridgeInitScope::~BridgeInitScope() { DlssSceneInput::bridgeInitialising = _previous; }

BridgeEvalScope::BridgeEvalScope(ID3D12Resource* colour, ID3D12Resource* output, D3D12_RESOURCE_STATES colourState)
{
    DlssSceneInput::bridgeColour = colour;
    DlssSceneInput::bridgeOutput = output;
    DlssSceneInput::bridgeColourState = colourState;
}

BridgeEvalScope::~BridgeEvalScope()
{
    DlssSceneInput::bridgeColour = nullptr;
    DlssSceneInput::bridgeOutput = nullptr;
}
} // namespace ext::hdr
