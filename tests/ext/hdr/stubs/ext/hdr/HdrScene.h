#pragma once

namespace ext::hdr
{
inline unsigned sceneHookInstallCalls = 0;

inline void InstallSceneHooks()
{
    ++sceneHookInstallCalls;
}
} // namespace ext::hdr
