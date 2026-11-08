// Engine light controls: the shader stamp read from the REAL bytecode of the 20261107-164135 capture (fixtures/): which of the game's shaders the family
// recognises, and which it must never touch.
//   moon_main (0x9DC59665)        the main view's directional light: directional + fake specular, g_LightParam at slot 2
//   moon_water_pass (0xABA675C4)  the water pass's copy: directional, NO fake specular
//   shadow_reader (0x1257B24B)    one of the 10 cascade-atlas readers: declares g_LightParam and g_DirectionalShadowParameter and no mask sampler
//   water_surface (0xA86A2431)    the water: g_AmbientParam at slot 3 AND g_WaterParameter
//   chara_ambient (0xB428E5B6)    a character shader: g_AmbientParam, never g_WaterParameter
#include "check.h"

#include "ext/ffxiv/enginelight/CbFamilyRules.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace ext::ffxiv::enginelight;
using namespace ext::ffxiv::enginelight::cb; // the constant-buffer family's own namespace (look3 integration: its names collided with the lamps / exposure module's)

namespace
{
std::string Read(const std::string& repo, const char* name)
{
    std::ifstream file(repo + "/tests/ext/ffxiv/enginelight/fixtures/" + name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

ShaderKind Kind(const std::string& repo, const char* name, bool* found = nullptr)
{
    const std::string code = Read(repo, name);
    if (found)
        *found = !code.empty();
    return KindOfShader(code.data(), code.size());
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    bool found = false;

    const ShaderKind moon = Kind(repo, "moon_main_0x9DC59665.dxbc", &found);
    CHECK(found);
    CHECK(moon.directional && moon.fakeSpecular && !moon.water);
    CHECK_EQ(moon.lightSlot, 2u);

    const ShaderKind water = Kind(repo, "moon_water_pass_0xABA675C4.dxbc", &found);
    CHECK(found);
    CHECK(water.directional && !water.fakeSpecular && !water.water);
    CHECK_EQ(water.lightSlot, 2u);

    const ShaderKind reader = Kind(repo, "shadow_reader_0x1257B24B.dxbc", &found);
    CHECK(found);
    CHECK(!reader.directional && !reader.fakeSpecular && !reader.water); // the 10 mask readers are NotAsked

    const ShaderKind surface = Kind(repo, "water_surface_0xA86A2431.dxbc", &found);
    CHECK(found);
    CHECK(surface.water && !surface.directional);
    CHECK_EQ(surface.ambientSlot, 3u);

    const ShaderKind chara = Kind(repo, "chara_ambient_0xB428E5B6.dxbc", &found);
    CHECK(found);
    CHECK(!chara.water && !chara.directional);

    // Garbage and empty bytecode are nothing.
    CHECK(PackShaderKind(KindOfShader(nullptr, 0)) == 0);
    const std::string junk(64, '\x7f');
    CHECK(PackShaderKind(KindOfShader(junk.data(), junk.size())) == 0);
    return Finish("enginelight_reflect");
}
