#pragma once
// NR's own resampler for the supersample up/down legs (SEAMS section 7, A8), derived from upstream's OS_Dx12.
// The filter is fixed per instance (NrConfig::scalingDownscaler) and the work is sized by the resources passed to
// Dispatch. Upstream's OS_Dx12 stays untouched.
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <Config.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>

#define NR_RESAMPLER_NUM_OF_HEAPS 2

class NrResampler_Dx12 : public Shader_Dx12
{
  private:
    bool _upsample = false;
    Scaler _scaler;

    FrameDescriptorHeap _frameHeaps[NR_RESAMPLER_NUM_OF_HEAPS];

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource);

    bool IsUpsampling() const { return _upsample; }

    NrResampler_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample, Scaler InScaler);

    ~NrResampler_Dx12();
};
