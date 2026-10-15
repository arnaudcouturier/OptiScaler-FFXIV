#pragma once

#include <d3d12.h>

namespace ext
{
enum class F
{
    Hdr,
    Gpu
};

inline bool activeHdr = false;

inline bool Active(F feature)
{
    return feature == F::Hdr && activeHdr;
}

inline void SetActive(F feature, bool active)
{
    if (feature == F::Hdr)
        activeHdr = active;
}

namespace restrack
{
inline void EnsureHooks(ID3D12Device*) {}
} // namespace restrack
} // namespace ext
