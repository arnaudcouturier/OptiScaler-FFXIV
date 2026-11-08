#pragma once
// Engine light controls: who owns the composite's ambient. While the engine light master is on, the card's ambient control (intensity and tint per
// channel) is the composite's ambient lever, and the shadow mapping module's night ambient lift yields: both scale the same SH fields, so letting
// both run would apply "the night's ambient" twice. A null-checked function pointer (the ShadowsHooks pattern), set by the family's own static
// registration (CbFamily.cpp) and read by the shadows module's ambient provider: neither module has a link dependency on the other, and a build
// without the family keeps the shadows lift exactly as it was.

namespace ext::ffxiv::enginelight
{
inline bool (*ownsAmbient)() = nullptr;
} // namespace ext::ffxiv::enginelight
