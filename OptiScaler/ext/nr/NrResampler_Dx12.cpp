// NR's own resampler for the supersample up/down legs (SEAMS section 7, A8). Derived from upstream's OS_Dx12 (shader
// choice, constants, dispatch) so Output Scaling keeps nightly's code untouched: the filter is fixed per instance
// (NrConfig::scalingDownscaler, not the global OutputScalingDownscaler) and the work is sized by the resources passed
// to Dispatch, not by the current feature.
#include "pch.h"
#include "ext/nr/NrResampler_Dx12.h"

#include <shaders/output_scaling/OS_Common.h>

using Microsoft::WRL::ComPtr;

#define A_CPU
// FSR compute shader is from : https://github.com/fholger/vrperfkit/

#include <shaders/output_scaling/precompile/BCDS_bicubic_Shader.h>
#include <shaders/output_scaling/precompile/BCDS_catmull_Shader.h>
#include <shaders/output_scaling/precompile/BCDS_lanczos2_Shader.h>
#include <shaders/output_scaling/precompile/BCDS_lanczos3_Shader.h>
#include <shaders/output_scaling/precompile/BCDS_kaiser2_Shader.h>
#include <shaders/output_scaling/precompile/BCDS_kaiser3_Shader.h>
#include <shaders/output_scaling/precompile/BCDS_magc_Shader.h>

#include <shaders/output_scaling/precompile/BCUS_Shader.h>

#include <shaders/output_scaling/fsr1/ffx_fsr1.h>
#include <shaders/output_scaling/fsr1/FSR_EASU_Shader.h>

#include <Config.h>

static Constants constants {};
static UpscaleShaderConstants fsr1Constants {};

#pragma warning(disable : 4244)

bool NrResampler_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    _counter++;
    _counter = _counter % NR_RESAMPLER_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    // The work is sized by the resources passed in: NR's supersample legs resample native <-> super-native, which
    // has nothing to do with the current feature's render/display sizes.
    const auto srcDesc = InResource->GetDesc();
    const auto dstDesc = OutResource->GetDesc();
    const auto srcW = (uint32_t) srcDesc.Width;
    const auto srcH = (uint32_t) srcDesc.Height;
    const auto dstW = (uint32_t) dstDesc.Width;
    const auto dstH = (uint32_t) dstDesc.Height;

    FsrEasuCon(fsr1Constants.const0, fsr1Constants.const1, fsr1Constants.const2, fsr1Constants.const3, srcW, srcH,
               srcW, srcH, dstW, dstH);

    constants.srcWidth = srcW;
    constants.srcHeight = srcH;
    constants.destWidth = dstW;
    constants.destHeight = dstH;

    // fsr upscaling
    bool createdConstantsBuffer = false;
    if (_scaler == Scaler::FSR1)
    {
        createdConstantsBuffer =
            CreateConstantsBuffer(_device, _constantBuffer, fsr1Constants, currentHeap.GetCbvCPU(0));
    }
    else
    {
        createdConstantsBuffer = CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0));
    }

    if (!createdConstantsBuffer)
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = 0;
    UINT dispatchHeight = 0;

    dispatchWidth = (dstW + InNumThreadsX - 1) / InNumThreadsX;
    dispatchHeight = (dstH + InNumThreadsY - 1) / InNumThreadsY;

    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

NrResampler_Dx12::NrResampler_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample, Scaler InScaler)
    : Shader_Dx12(InName, InDevice), _upsample(InUpsample), _scaler(InScaler)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0);
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; // no sampler.AddressW ???

    if (!SetupRootSignature(InDevice, 1, 1, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      nullptr, IID_PPV_ARGS(&_constantBuffer));

    const auto downscalerConfig = _scaler;

    const void* csoData = nullptr;
    size_t csoSize = 0;
    const char* sourceCode = nullptr;

    std::string name = "NR resample: ";

    if (downscalerConfig == Scaler::FSR1)
    {
        csoData = fsr_easu_cso;
        csoSize = sizeof(fsr_easu_cso);
        sourceCode = nullptr; // FSR1 is precompiled only
        name += "FSR1";
    }
    else if (_upsample)
    {
        csoData = bcus_cso;
        csoSize = sizeof(bcus_cso);
        sourceCode = upsampleCode.c_str();
        name += "BicubicUp";
    }
    else
    {
        InNumThreadsY = 8;
        InNumThreadsX = 8;

        switch (downscalerConfig)
        {
        case Scaler::CatmullRom:
            csoData = bcds_catmull_cso;
            csoSize = sizeof(bcds_catmull_cso);
            sourceCode = downsampleCodeCatmull.c_str();
            name += "CatmullRom";
            break;
        case Scaler::Lanczos2:
            csoData = bcds_lanczos2_cso;
            csoSize = sizeof(bcds_lanczos2_cso);
            sourceCode = downsampleCodeLanczos2.c_str();
            name += "Lanczos2";
            break;
        case Scaler::Lanczos3:
            csoData = bcds_lanczos3_cso;
            csoSize = sizeof(bcds_lanczos3_cso);
            sourceCode = downsampleCodeLanczos3.c_str();
            name += "Lanczos3";
            break;
        case Scaler::Kaiser2:
            csoData = bcds_kaiser2_cso;
            csoSize = sizeof(bcds_kaiser2_cso);
            sourceCode = downsampleCodeKaiser2.c_str();
            name += "Kaiser2";
            break;
        case Scaler::Kaiser3:
            csoData = bcds_kaiser3_cso;
            csoSize = sizeof(bcds_kaiser3_cso);
            sourceCode = downsampleCodeKaiser3.c_str();
            name += "Kaiser3";
            break;
        case Scaler::Magic:
            csoData = bcds_magc_cso;
            csoSize = sizeof(bcds_magc_cso);
            sourceCode = downsampleCodeMAGIC.c_str();
            name += "Magic";
            break;
        case Scaler::Bicubic:
        default:
            csoData = bcds_bicubic_cso;
            csoSize = sizeof(bcds_bicubic_cso);
            sourceCode = downsampleCodeBC.c_str();
            name += "Bicubic";
            break;
        }
    }

    _name = name;

    if (!Shader_Dx12::CreateComputePipeline(InDevice, &_pipelineState, csoData, csoSize, sourceCode))
    {
        LOG_ERROR("[{0}] CreateComputePipeline error!", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, NR_RESAMPLER_NUM_OF_HEAPS);
}

NrResampler_Dx12::~NrResampler_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < NR_RESAMPLER_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }

}
