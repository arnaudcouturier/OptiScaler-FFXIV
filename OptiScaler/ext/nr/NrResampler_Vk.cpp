// NR's own resampler for the supersample up/down legs (SEAMS section 7, A8). Derived from upstream's OS_Vk (shader
// choice, constants, dispatch): the filter is fixed per instance (NrConfig::scalingDownscaler) and the work is sized by
// the images passed to Dispatch. Upstream's OS_Vk stays untouched.
#include "pch.h"
#include "ext/nr/NrResampler_Vk.h"
#include <shaders/output_scaling/OS_Common.h>
#include <Config.h>

#define A_CPU
#include <shaders/output_scaling/precompile/BCDS_bicubic_Shader_Vk.h>
#include <shaders/output_scaling/precompile/BCDS_catmull_Shader_Vk.h>
#include <shaders/output_scaling/precompile/bcds_lanczos2_Shader_Vk.h>
#include <shaders/output_scaling/precompile/bcds_lanczos3_Shader_Vk.h>
#include <shaders/output_scaling/precompile/bcds_kaiser2_Shader_Vk.h>
#include <shaders/output_scaling/precompile/bcds_kaiser3_Shader_Vk.h>
#include <shaders/output_scaling/precompile/BCDS_magc_Shader_Vk.h>
#include <shaders/output_scaling/precompile/BCUS_Shader_Vk.h>
#include <shaders/output_scaling/fsr1/ffx_fsr1.h>
#include <shaders/output_scaling/fsr1/FSR_EASU_Shader_Vk.h>

static Constants constants {};
static UpscaleShaderConstants fsr1Constants {};

#pragma warning(disable : 4244)

NrResampler_Vk::NrResampler_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice, bool InUpsample,
                               Scaler InScaler)
    : Shader_Vk(InName, InDevice, InPhysicalDevice), _upsample(InUpsample), _scaler(InScaler)
{
    if (InDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_FUNC();

    // 1. Create Base Resources
    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    uint32_t constantSize = (_scaler == Scaler::FSR1)
                                ? sizeof(UpscaleShaderConstants)
                                : sizeof(Constants);
    CreateConstantBuffer(constantSize);

    // 2. Setup Layouts & Pools
    std::vector<VkDescriptorSetLayoutBinding> bindings = { CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                                           CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                                           CreateBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                                           CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLER) };
    CreateLayouts(bindings);

    std::vector<VkDescriptorPoolSize> poolSizes = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_SAMPLER, _maxFramesInFlight } };

    CreateDescriptorPool(poolSizes, _maxFramesInFlight);
    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);

    // 3. Load Pipeline
    std::vector<char> shaderCode;
    if (_scaler == Scaler::FSR1)
    {
        shaderCode = std::vector<char>(FSR_EASU_spv, FSR_EASU_spv + sizeof(FSR_EASU_spv));
    }
    else
    {
        if (_upsample)
        {
            shaderCode = std::vector<char>(bcus_spv, bcus_spv + sizeof(bcus_spv));
        }
        else
        {
            switch (_scaler)
            {
            case Scaler::Bicubic:
                shaderCode = std::vector<char>(bcds_bicubic_spv, bcds_bicubic_spv + sizeof(bcds_bicubic_spv));
                break;
            case Scaler::CatmullRom:
                shaderCode = std::vector<char>(bcds_catmull_spv, bcds_catmull_spv + sizeof(bcds_catmull_spv));
                break;
            case Scaler::Lanczos2:
                shaderCode = std::vector<char>(bcds_lanczos2_spv, bcds_lanczos2_spv + sizeof(bcds_lanczos2_spv));
                break;
            case Scaler::Lanczos3:
                shaderCode = std::vector<char>(bcds_lanczos3_spv, bcds_lanczos3_spv + sizeof(bcds_lanczos3_spv));
                break;
            case Scaler::Kaiser2:
                shaderCode = std::vector<char>(bcds_kaiser2_spv, bcds_kaiser2_spv + sizeof(bcds_kaiser2_spv));
                break;
            case Scaler::Kaiser3:
                shaderCode = std::vector<char>(bcds_kaiser3_spv, bcds_kaiser3_spv + sizeof(bcds_kaiser3_spv));
                break;
            case Scaler::Magic:
                shaderCode = std::vector<char>(bcds_magc_spv, bcds_magc_spv + sizeof(bcds_magc_spv));
                break;
            default:
                shaderCode = std::vector<char>(bcds_bicubic_spv, bcds_bicubic_spv + sizeof(bcds_bicubic_spv));
                break;
            }
        }
    }

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, shaderCode))
    {
        LOG_ERROR("Failed to create pipeline for NrResampler_Vk");
        _init = false;
        return;
    }

    _init = true;
}

bool NrResampler_Vk::Dispatch(VkCommandBuffer InCmdList, const VkImageInfo& InResourceView, const VkImageInfo& OutResourceView)
{
    if (!_init || InCmdList == VK_NULL_HANDLE)
        return false;

    ScopedGpuTime_Vk scopedGpuTime(GpuTime.get(), InCmdList);

    // Sized by the images passed in (NR's supersample legs), not by the current feature
    const uint32_t srcW = InResourceView.Width;
    const uint32_t srcH = InResourceView.Height;
    const uint32_t dstW = OutResourceView.Width;
    const uint32_t dstH = OutResourceView.Height;

    // Update Constants
    FsrEasuCon(fsr1Constants.const0, fsr1Constants.const1, fsr1Constants.const2, fsr1Constants.const3, srcW, srcH, srcW,
               srcH, dstW, dstH);

    constants.srcWidth = srcW;
    constants.srcHeight = srcH;
    constants.destWidth = dstW;
    constants.destHeight = dstH;

    if (_mappedConstantBuffer)
    {
        if (_scaler == Scaler::FSR1)
            memcpy(_mappedConstantBuffer, &fsr1Constants, sizeof(UpscaleShaderConstants));
        else
            memcpy(_mappedConstantBuffer, &constants, sizeof(Constants));
    }

    // Advance Frame Index
    _currentSetIndex = (_currentSetIndex + 1) % _maxFramesInFlight;
    VkDescriptorSet currentSet = _descriptorSets[_currentSetIndex];

    // Build Descriptor Writes
    VkDescriptorBufferInfo bufferInfo { _constantBuffer, 0, sizeof(Constants) };
    VkDescriptorImageInfo sourceInfo { VK_NULL_HANDLE, InResourceView.ImageView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo destInfo { VK_NULL_HANDLE, OutResourceView.ImageView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo samplerInfo { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

    std::vector<VkWriteDescriptorSet> writes = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 0, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 1, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &sourceInfo, nullptr, nullptr },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 2, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destInfo, nullptr, nullptr },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 3, 0, 1,
                                                   VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr, nullptr } };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &currentSet, 0, nullptr);

    // Dispatch
    if (_scaler == Scaler::FSR1 || _upsample)
    {
        uint32_t groupX = (OutResourceView.Width + 15) / 16;
        uint32_t groupY = (OutResourceView.Height + 15) / 16;
        vkCmdDispatch(InCmdList, groupX, groupY, 1);
    }
    else
    {
        uint32_t groupX = (OutResourceView.Width + 7) / 8;
        uint32_t groupY = (OutResourceView.Height + 7) / 8;
        vkCmdDispatch(InCmdList, groupX, groupY, 1);
    }

    return true;
}