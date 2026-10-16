#pragma once
// Test replacement of ext/gpu/Restrack.h: the queue / reset hooks need the product (QueueHook.cpp).
#include <d3d12.h>

namespace ext::restrack
{
inline int ensureHooksCalls = 0;
inline bool EnsureHooks(ID3D12Device*)
{
    ++ensureHooksCalls;
    return true;
}
} // namespace ext::restrack
