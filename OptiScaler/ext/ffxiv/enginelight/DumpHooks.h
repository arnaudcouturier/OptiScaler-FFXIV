#pragma once
// Engine light controls: the F9 draw diagnostic's two hooks (null-checked function pointers, the ShadowsHooks pattern), so ext/ffxiv/diag carries no
// link dependency on the family. `text` returns the capture's enginelight.csv body (empty when nothing was patched); `reset` starts the next capture.
// Both are set by the module's own static registration (CbFamily.cpp) and called on the render thread.

#include <string>

namespace ext::ffxiv::enginelight
{
inline constexpr const char* kDumpFile = "enginelight.csv"; // the capture folder's file of the patched buffers (before / after)

struct DumpHooks
{
    static inline std::string (*text)() = nullptr;
    static inline void (*reset)() = nullptr;
};
} // namespace ext::ffxiv::enginelight
