#include "pch.h"

#include "ext/ffxiv/quality/FfxivNativeQualityHooks.h"
#include "ext/ffxiv/quality/FfxivQuality.h"
#include "ext/game/GameInterfaces.h"

namespace ext::ffxiv::quality
{
namespace
{
class NativeQualityProvider final : public game::INativeQuality
{
  public:
    bool EnsureHooks() override { return FfxivNativeQualityHooks::Install(); }
    bool Live() override { return FfxivNativeQualityHooks::available.load(); }
    bool DlaaRequired() override { return FfxivNativeQualityHooks::DlaaRequiredCached(); }
    void Request(int preset) override { FfxivNativeQualityHooks::Request(preset); }
};

NativeQualityProvider provider;
} // namespace

game::INativeQuality& Provider() { return provider; }
} // namespace ext::ffxiv::quality
