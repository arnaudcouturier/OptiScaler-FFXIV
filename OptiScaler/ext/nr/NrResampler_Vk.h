#pragma once
// NR's own resampler for the supersample up/down legs (SEAMS section 7, A8), derived from upstream's OS_Vk.
// The filter is fixed per instance (NrConfig::scalingDownscaler) and the work is sized by the images passed to Dispatch.
#include "SysUtils.h"

#include <shaders/Shader_Vk.h>

#include <Config.h>

class NrResampler_Vk : public Shader_Vk
{
    bool _upsample = false;
    Scaler _scaler;

  public:
    NrResampler_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice, bool InUpsample,
                   Scaler InScaler);
    ~NrResampler_Vk() = default;

    bool Dispatch(VkCommandBuffer InCmdList, const VkImageInfo& InResourceView, const VkImageInfo& OutResourceView);
};
