#pragma once
// A same-frame pre-clipping scene image and the shared completion rule of every HDR work pool (C8, C10).
// Was shaders/hdr/SceneInput.h; the NR scene state enum and its text live in scene/NrSceneInput.h.

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace ext::hdr
{
// An absent callback must never make previously recorded GPU work look completed.
inline bool ReusableRecordedWork(bool recorded, const std::function<bool()>& probe)
{
    if (!recorded)
        return true; // Allocated only; no commands have referenced it.
    if (!probe)
        return false;
    const bool completedAndRecordingRetired = probe();
    if (!completedAndRecordingRetired)
        return false;
    return true;
}

// A same-frame pre-clipping scene and its original game reference. Both remain
// owned until the conversion command list finishes, including abandoned lists.
struct SceneInput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> hdr, reference;
    std::shared_ptr<void> owner;
    std::array<float, 4> rect { 0, 0, 1, 1 }; // Normalised offset/size of the rendered viewport.
    std::function<void(std::function<bool()>)> retire;
    bool referenceSrgb = false; // Native captures use gamma 2.2; DLSS compatibility output uses sRGB.
    Microsoft::WRL::ComPtr<ID3D12Resource> previewMask; // Native UI preview transmittance, in output coordinates.
    bool referenceHdrShoulder = false; // SDR reference was derived with the HDR compatibility shoulder.
};
} // namespace ext::hdr
