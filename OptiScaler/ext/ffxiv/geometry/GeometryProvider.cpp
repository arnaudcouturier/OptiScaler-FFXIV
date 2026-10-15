#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/geometry/FfxivGeometry.h"
#include "ext/game/GameInterfaces.h"
#include "ext/upscaler/RenderGeometry.h"

namespace ext::ffxiv::geometry
{
namespace
{
class RenderGeometryProvider final : public game::IRenderGeometry
{
  public:
    bool ResolveRenderExtent(unsigned int paramW, unsigned int paramH, unsigned int& w, unsigned int& h) override
    {
        const bool enabled = Cfg().upscaler.renderSizeFromWidthHeight.value_or_default();
        return upscaler::ResolveRenderExtentValue(enabled, paramW, paramH, w, h);
    }

    // C4 (next wave) implements the real "colour allocated at display size, render size smaller" logic -- do not
    // guess at it here.
    bool ColorIsDisplayAllocation() override { return false; }
};

RenderGeometryProvider provider;
} // namespace

game::IRenderGeometry& Provider() { return provider; }
} // namespace ext::ffxiv::geometry
