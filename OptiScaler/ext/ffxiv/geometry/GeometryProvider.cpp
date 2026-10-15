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

    // S7 C4: reads the same RenderSizeFromWidthHeight flag as ResolveRenderExtent above (coupling this scale to the
    // same gate as the geometry correction, see ext/upscaler/RenderGeometry.h's ColorIsDisplayAllocationValue
    // doc comment) and delegates the actual decision math to it.
    bool ColorIsDisplayAllocation(unsigned int renderWidth, unsigned int renderHeight, uint64_t allocWidth,
                                  unsigned int allocHeight, unsigned int subrectWidth,
                                  unsigned int subrectHeight) override
    {
        const bool enabled = Cfg().upscaler.renderSizeFromWidthHeight.value_or_default();
        return upscaler::ColorIsDisplayAllocationValue(enabled, renderWidth, renderHeight, allocWidth, allocHeight,
                                                        subrectWidth, subrectHeight);
    }
};

RenderGeometryProvider provider;
} // namespace

game::IRenderGeometry& Provider() { return provider; }
} // namespace ext::ffxiv::geometry
