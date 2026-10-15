#pragma once
// The only fork header upstream files may include (docs/SEAMS.md section 1).
// Every seam is a free function in `ext::<group>`; the catalog is docs/SEAMS-catalog.md.
// Hot-path seams are inline wrappers testing a feature bit, see ExtFeatures.h.

#include "ext/ExtFeatures.h"
#include "ext/core/MenuRegistry.h"
#include "ext/gpu/Gpu.h"
#include "ext/gpu/Restrack.h"

#include <cstddef>

// CSimpleIniA is a typedef of a template, it cannot be forward declared
#include <SimpleIni.h>

// Upstream classes named by the seams below (the full types come with the host file's own includes)
class IFeature_Dx11;
class IFGFeature_Dx12;
struct ID3D11Device;

namespace ext
{
// Friend of the upstream classes whose private state the fork has to reach (`friend struct ext::Access;`).
// It is the only fork code that names private upstream members. Accessors are added next to the seam that needs
// them; the definitions live in Seams.cpp. (The menu seam needs none: it is a template, see ext/core/MenuRegistry.h.)
struct Access
{
    // framegen/IFGFeature_Dx12.h (friend line). C6: close the unsubmitted UI/SC lists, drop the frame resources and
    // Deactivate, so nothing queued references an upscaler that is about to be destroyed (ext::dx11bridge::
    // BeforeReleaseFeature). Not a seam: ext-internal.
    static void CancelPendingUpscalerWork(IFGFeature_Dx12& fg);

    // resource_tracking/ResTrack_dx12.h (friend line). C6, ext-internal (ext::restrack::EnsureHooks): attach upstream's
    // own hkReset to ResTrack_Dx12::o_Reset. AttachResetHook is a no-op returning true when o_Reset is already set;
    // `target` is slot 10 of a real ID3D12GraphicsCommandList vtable. Failure leaves o_Reset null.
    static bool ResetHookAttached();
    static bool AttachResetHook(void* target);
};

// ini load/save hooks in Config.cpp (state: ext/core/ExtConfig)
namespace config
{
void Load(CSimpleIniA& ini);    // end of Config::Reload, after upstream read its keys
void Save(CSimpleIniA& ini);    // Config::SaveIni, before ini.SaveFile
void SaveXeFG(CSimpleIniA& ini); // Config::SaveXeFG, before ini.SaveFile
} // namespace config

// menu_common.cpp (state: ext/core/MenuRegistry). ext::menu::MergeBoxes(base, count) is a template declared in
// ext/core/MenuRegistry.h: `base` is upstream's static MenuBox array, the result is `base` itself while no fork card is
// registered, otherwise a merged array (count is updated).

// core services (state: ext/core)
namespace core
{
void Banner(); // once, right after upstream logs its version line
} // namespace core

// ext::gpu (command-list Reset/Execute wrappers, owners, Scope) is declared in ext/gpu/Gpu.h and ext::restrack
// (OnCommandListReset, WantsCommandListReset, OnDeviceHooked, ReleaseQueueHook) in ext/gpu/Restrack.h; both are
// included above.

// NGX shutdown / feature release on the D3D11 entry points (C6). State: ext/dx11bridge/NgxLifecycle.cpp
namespace dx11bridge
{
// NVSDK_NGX_D3D11_Shutdown, replaces the legacy proxy Shutdown call. True = handled (`out` holds the result): the
// game isolates NGX shutdown (ext::game INgxLifecycle) and the bridge is up, so only the device-specific Shutdown1 runs
// and Streamline's live DX12 NGX instance survives. False = upstream calls the legacy Shutdown.
bool ShutdownNgx(ID3D11Device* device, NVSDK_NGX_Result& out);
// NVSDK_NGX_D3D11_Shutdown1, first statement. True = the caller must `return NVSDK_NGX_D3D11_Shutdown();`.
// `recordedDevice` is the translation unit's D3D11Device, set to `inDevice` when handled.
bool RedirectShutdown1(ID3D11Device* inDevice, ID3D11Device*& recordedDevice);
// NVSDK_NGX_D3D11_ReleaseFeature, before the feature is reset. False -> return FAIL_PlatformError.
bool BeforeReleaseFeature(IFeature_Dx11& feature, bool shuttingDown);
} // namespace dx11bridge

// Groups of later series. Declarations are added here together with the SEAMS-catalog.md row.
namespace upscaler {}
namespace nr {}
namespace fg {}
namespace present {}
namespace streamline {}
namespace hdr {}
namespace hudless {}
namespace game {}
} // namespace ext
