#pragma once

#include <d3d12.h>

namespace ext::hdr::screenshot
{
inline void Reset(ID3D12CommandList*) noexcept {}
inline void AfterExecute(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept {}
} // namespace ext::hdr::screenshot
