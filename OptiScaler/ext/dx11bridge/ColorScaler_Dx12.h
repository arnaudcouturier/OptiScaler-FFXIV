#pragma once
// C4's own GPU scaler for the forced-quality colour scale (docs/SEAMS-catalog.md "dx11bridge" PrepareUpscalerColor).
//
// The oracle reuses upstream's OS_Dx12 directly ("Forced quality color input", Scaler::Lanczos3). This fork's
// OS_Dx12 cannot be reused as-is for that: its constructor has no per-instance Scaler override (it always reads the
// global Config::Instance()->OutputScalingDownscaler, which defaults to FSR1, not Lanczos3), and its Dispatch sizes
// the work from State::Instance().currentFeature->Target/DisplayWidth/Height -- the Output Scaling pass it was
// built for, not the arbitrary src/dst pair this feature needs to scale. ext/nr/NrResampler_Dx12 (A8) already solved
// exactly this problem for NR's supersample legs: a fixed-per-instance Scaler and a Dispatch sized by the resources
// passed to it. This class derives from NrResampler_Dx12 instead of duplicating it, adding only what that class
// does not need: a lazily-created, owned staging buffer. CreateBufferResource/SetBufferState/Buffer are copied
// verbatim from OS_Dx12 (Shader_Dx12::CreateBufferResource/SetBufferState are protected, reachable from any of its
// derived classes, NrResampler_Dx12 included). Upstream's OS_Dx12 and A8's NrResampler_Dx12 both stay untouched.
#include "ext/nr/NrResampler_Dx12.h"

#include <d3d12.h>

namespace ext::dx11bridge
{
class ColorScaler_Dx12 : public NrResampler_Dx12
{
  private:
    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, uint32_t InWidth, uint32_t InHeight,
                              D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);

    ID3D12Resource* Buffer() { return _buffer; }

    ColorScaler_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample, Scaler InScaler);

    ~ColorScaler_Dx12();
};
} // namespace ext::dx11bridge
