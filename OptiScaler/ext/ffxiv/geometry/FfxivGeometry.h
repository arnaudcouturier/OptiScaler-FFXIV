#pragma once

namespace ext::game
{
struct IRenderGeometry;
}

namespace ext::ffxiv::geometry
{
ext::game::IRenderGeometry& Provider();
} // namespace ext::ffxiv::geometry
