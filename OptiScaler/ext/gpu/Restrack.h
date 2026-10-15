#pragma once
// ext::restrack seams of the single command-list Reset hook and the fork's queue hook (docs/SEAMS.md section 5,
// catalog group `restrack`). Implementation: QueueHook.cpp. Subscribers register an ext::gpu::Owner (Gpu.h); the
// resource-tracking host (ResTrack_dx12.cpp) only sees these free functions.

#include "ext/gpu/Gpu.h"

#include <d3d12.h>

namespace ext::restrack
{
// ResTrack_dx12.cpp hkReset, after a successful o_Reset. Called on game recording threads, many times per frame.
inline void OnCommandListReset(ID3D12GraphicsCommandList* commands)
{
    if (gpu::OwnersWantReset())
        gpu::detail::NotifyReset(commands);
}

// ResTrack_dx12.cpp: install condition of the one Reset hook (`persistentBindings || WantsCommandListReset()`).
inline bool WantsCommandListReset() { return gpu::OwnersWantReset(); }

// ResTrack_dx12.cpp HookDevice, once the device methods are hooked. Installs the queue hook when owners are active
// (and the Reset hook when an owner wants it). Idempotent, mutexed.
void OnDeviceHooked(ID3D12Device* device);

// ResTrack_dx12.cpp ReleaseDeviceHooks / ReleaseHooks, before upstream's own detach transaction. The only detach of
// the fork's ExecuteCommandLists detour. Idempotent.
void ReleaseQueueHook();

// ext-internal (not an upstream seam): called by a feature right after it registers an owner, and by the bridges
// for devices ResTrack never hooked. Installs whatever the registered owners need: the queue hook when
// SubmissionOwnersActive(), the Reset hook (ResTrack_Dx12::EnsureResetHook) when WantsCommandListReset().
// Idempotent and mutexed; replaces the reference build's HookLateNrQueue. Returns true when everything wanted is hooked.
bool EnsureHooks(ID3D12Device* device);
} // namespace ext::restrack
