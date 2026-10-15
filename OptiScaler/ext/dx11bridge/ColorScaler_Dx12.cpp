#include "pch.h"
#include "ext/dx11bridge/ColorScaler_Dx12.h"

namespace ext::dx11bridge
{
namespace
{
// Mirrors OS_Dx12's own "OS: <filter>" naming (ext/dx11bridge/ColorScaler_Dx12.h's header comment explains why this
// class derives from NrResampler_Dx12 instead of OS_Dx12 itself). NrResampler_Dx12's base constructor already set
// _name to its own "NR resample: <filter>" string -- correct for NR's own instances, but misleading here: the
// oracle's PrepareForcedQualityColor literally constructs an OS_Dx12, so every per-frame "[{name}] Start!" debug
// line and the pipeline/constants-buffer error lines should read "OS: <filter>" for a byte-identical Phase 4 log
// diff, not "NR resample: <filter>" (crediting this feature to NR) and not the caller-supplied InName either --
// the oracle's own OS_Dx12 constructor discards its InName parameter the same way, always deriving _name from the
// scaler/upsample choice instead.
std::string OsStyleName(bool upsample, Scaler scaler)
{
    if (scaler == Scaler::FSR1)
        return "OS: FSR1";
    if (upsample)
        return "OS: BicubicUp";

    switch (scaler)
    {
    case Scaler::CatmullRom: return "OS: CatmullRom";
    case Scaler::Lanczos2: return "OS: Lanczos2";
    case Scaler::Lanczos3: return "OS: Lanczos3";
    case Scaler::Kaiser2: return "OS: Kaiser2";
    case Scaler::Kaiser3: return "OS: Kaiser3";
    case Scaler::Magic: return "OS: Magic";
    case Scaler::Bicubic:
    default: return "OS: Bicubic";
    }
}
} // namespace

bool ColorScaler_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, uint32_t InWidth,
                                            uint32_t InHeight, D3D12_RESOURCE_STATES InState)
{
    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    // Shader_Dx12::CreateBufferResource/SetBufferState are protected: reachable here because ColorScaler_Dx12
    // derives (through NrResampler_Dx12) from Shader_Dx12, same as OS_Dx12 does.
    auto result =
        Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &_buffer, resourceFlags, InWidth, InHeight);

    if (result)
    {
        _buffer->SetName(L"ColorScaler_Buffer");
        _bufferState = InState;
    }

    return result;
}

void ColorScaler_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState)
{
    return Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

ColorScaler_Dx12::ColorScaler_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample, Scaler InScaler)
    : NrResampler_Dx12(InName, InDevice, InUpsample, InScaler)
{
    // Overrides NrResampler_Dx12's own "NR resample: <filter>" _name -- see OsStyleName's comment above.
    _name = OsStyleName(InUpsample, InScaler);
}

ColorScaler_Dx12::~ColorScaler_Dx12()
{
    if (!IsInit() || State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_buffer);
}
} // namespace ext::dx11bridge
