#pragma once

#include "SysUtils.h"
#include <proxies/NVNGX_Proxy.h>
#include <upscalers/IFeature.h>

class DLSSDFeature : public virtual IFeature
{
  private:
    feature_version _version = { 0, 0, 0 };

  protected:
    NVSDK_NGX_Handle _dlssdHandle = {};
    NVSDK_NGX_Handle* _p_dlssdHandle = nullptr;
    // One flag per API, not one shared by all three. With a single flag, a D3D11 game that initialised
    // native Ray Reconstruction and is then moved onto the D3D12 bridge finds the flag already set,
    // skips NVNGXProxy::InitDx12 and every D3D12 accessor on the proxy returns nullptr ("_CreateFeature
    // is nullptr"), silently falling back to another upscaler. The proxy's own Init* functions are
    // idempotent and keep a flag apiece; these only skip the deliberate delay between init and feature
    // creation, so they have to be counted per API as well.
    inline static bool _dlssdInitedDx11 = false;
    inline static bool _dlssdInitedDx12 = false;
    inline static bool _dlssdInitedVk = false;

    void ProcessEvaluateParams(NVSDK_NGX_Parameter* InParameters);
    void ProcessInitParams(NVSDK_NGX_Parameter* InParameters);
    void ReadVersion();

    float GetSharpness(const NVSDK_NGX_Parameter* InParameters);

  public:
    feature_version Version() override { return feature_version { _version.major, _version.minor, _version.patch }; }
    Upscaler GetUpscalerType() const override { return Upscaler::DLSSD; }

    DLSSDFeature(unsigned int handleId, NVSDK_NGX_Parameter* InParameters);

    ~DLSSDFeature();
};
