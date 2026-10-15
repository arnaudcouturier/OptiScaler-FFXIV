#include "pch.h"
#include "ext/dx11bridge/ColorScaler_Dx12.h"

namespace ext::dx11bridge
{
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
}

ColorScaler_Dx12::~ColorScaler_Dx12()
{
    if (!IsInit() || State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_buffer);
}
} // namespace ext::dx11bridge
