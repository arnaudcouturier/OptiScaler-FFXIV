#pragma once

#include "ext/gpu/GpuSubmission.h"

#include <functional>

namespace ext::gpu
{
class GpuLifetime
{
  public:
    void Collect() {}
    void Record(ID3D12GraphicsCommandList*) {}
    std::function<bool()> ReuseProbe(ID3D12GraphicsCommandList*) { return [] { return true; }; }
    GpuSubmission BeginSubmission(UINT, ID3D12CommandList* const*) { return {}; }
    void QuarantineSubmission(UINT, ID3D12CommandList* const*) {}
    void ResetRecording(ID3D12CommandList*) {}
};

struct Owner
{
    void* ctx = nullptr;
    GpuSubmission (*begin)(void*, UINT, ID3D12CommandList* const*) = nullptr;
    void (*quarantine)(void*, UINT, ID3D12CommandList* const*) = nullptr;
    void (*reset)(void*, ID3D12CommandList*) = nullptr;
    void (*after)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = nullptr;
};

inline int RegisterOwner(const Owner&) { return 0; }
} // namespace ext::gpu
