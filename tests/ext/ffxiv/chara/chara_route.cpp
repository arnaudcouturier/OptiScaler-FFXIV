#include "check.h"

#include "ext/ffxiv/chara/CharaRoute.h"

using namespace ext::ffxiv::chara;

namespace
{
constexpr DrawApi kApis[] = { DrawApi::Draw,
                              DrawApi::DrawIndexed,
                              DrawApi::DrawInstanced,
                              DrawApi::DrawIndexedInstanced,
                              DrawApi::DrawInstancedIndirect,
                              DrawApi::DrawIndexedInstancedIndirect };
}

int main()
{
    for (const DrawApi api : kApis)
    {
        // The regression of the first in-game test: the equipment display draw arrives as an indexed draw and must get the AA.
        CHECK(ChooseRoute(false, false, true, RoleEquipDisplay, api) == Route::Enhance);
        CHECK(ChooseRoute(false, true, true, RoleEquipDisplay, api) == Route::Enhance); // an active DLSS path alone does not matter
        CHECK(ChooseRoute(false, false, true, RoleEquipDisplay | RoleUi, api) == Route::Enhance);

        // AA off: native
        CHECK(ChooseRoute(false, false, false, RoleEquipDisplay, api) == Route::Native);

        // other shaders never get the AA
        CHECK(ChooseRoute(false, false, true, RoleNone, api) == Route::Native);
        CHECK(ChooseRoute(false, false, true, RoleUi, api) == Route::Native);
        CHECK(ChooseRoute(false, false, true, RoleGeometry | RoleFinisher, api) == Route::Native);

        // PreviewDLSS on: the DLSS path when it can run, never the AA
        CHECK(ChooseRoute(true, true, true, RoleEquipDisplay, api) == Route::Dlss);
        CHECK(ChooseRoute(true, true, false, RoleNone, api) == Route::Dlss);
        // PreviewDLSS on but the DLSS path cannot run (NGX not initialised, latched after a failure, slots only draining): the spatial AA
        // takes the equipment display draw when PreviewAA is on, otherwise the draw stays native. This deliberately differs from the
        // specification of the reference build, where the draw stayed native.
        CHECK(ChooseRoute(true, false, true, RoleEquipDisplay, api) == Route::Enhance);
        CHECK(ChooseRoute(true, false, true, RoleEquipDisplay | RoleUi, api) == Route::Enhance);
        CHECK(ChooseRoute(true, false, true, RoleUi, api) == Route::Native); // other shaders never get the AA
        CHECK(ChooseRoute(true, false, true, RoleNone, api) == Route::Native);
        CHECK(ChooseRoute(true, false, false, RoleEquipDisplay, api) == Route::Native); // AA off: native
        CHECK(ChooseRoute(true, false, false, RoleNone, api) == Route::Native);

        // the deferred-context counter follows the same rule
        CHECK(CountsAsDeferredEquip(true, RoleEquipDisplay, api));
        CHECK(!CountsAsDeferredEquip(false, RoleEquipDisplay, api));
        CHECK(!CountsAsDeferredEquip(true, RoleUi, api));
    }
    return TEST_RESULT("chara_route");
}
