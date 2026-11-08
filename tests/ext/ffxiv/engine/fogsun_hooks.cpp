// The fog sun accessor (ext/ffxiv/engine/FogSun.h): the one thing the god rays module (the fog-post capture) and the shadow mapping module (the night
// curve) share. Two inline atomics and one hook; no module includes the other.
//
// What is pinned: the published sun and its publish count (the consumer's freshness clock watches the count), the consumer's "wanted" flag, the
// hook that tells the capture to re-evaluate its arming when the flag changes - called once per change, never per call, and independent of which
// module's config load ran first (the capture registers its hook at start-up, the consumer sets the flag at a config load, in either order).
#include "check.h"

#include "ext/ffxiv/engine/FogSun.h"

#include <limits>

namespace fogsun = ext::ffxiv::fogsun;

namespace
{
int g_calls = 0;
void Hook() { ++g_calls; }
} // namespace

int main()
{
    // ---- the published sun and its count -------------------------------------------------------------------------------------------------
    fogsun::ResetForTest();
    {
        const fogsun::Reading before = fogsun::Read();
        CHECK(!before.have && before.count == 0);
        fogsun::Publish(0.183f);
        fogsun::Reading now = fogsun::Read();
        CHECK(now.have && now.y == 0.183f && now.count == 1);
        fogsun::Publish(-0.109f);
        fogsun::Publish(-0.109f);
        now = fogsun::Read();
        CHECK(now.have && now.y == -0.109f && now.count == 3); // every landed read counts: the consumer's clock restarts on each
        fogsun::Publish(std::numeric_limits<float>::quiet_NaN()); // a NaN is not a sun: neither stored nor counted
        now = fogsun::Read();
        CHECK(now.y == -0.109f && now.count == 3);
    }

    // ---- the wanted flag and the hook: the shadow module's config load first, then the capture registers ------------------------------------
    fogsun::ResetForTest();
    g_calls = 0;
    {
        CHECK(!fogsun::Wanted()); // the shipped default: nothing asks for the fog sun
        fogsun::SetWanted(fogsun::kShadowMapping, true);  // no hook yet: the flag is stored, nothing is called
        CHECK(fogsun::Wanted() && g_calls == 0);
        fogsun::SetWantedChangedHook(&Hook); // registering with the flag already up calls the hook once (the capture's arming needs to see it)
        CHECK_EQ(g_calls, 1);
        fogsun::SetWanted(fogsun::kShadowMapping, true);  // no change: no call
        CHECK_EQ(g_calls, 1);
        fogsun::SetWanted(fogsun::kShadowMapping, false); // a change: one call
        CHECK(!fogsun::Wanted() && g_calls == 2);
        fogsun::SetWanted(fogsun::kShadowMapping, false);
        CHECK_EQ(g_calls, 2);
    }

    // ---- the other order: the capture registers first, the consumer's config load comes after -------------------------------------------------
    fogsun::ResetForTest();
    g_calls = 0;
    {
        fogsun::SetWantedChangedHook(&Hook);
        CHECK_EQ(g_calls, 0); // registering with the flag down calls nothing
        fogsun::SetWanted(fogsun::kShadowMapping, true);
        CHECK(fogsun::Wanted() && g_calls == 1);
        fogsun::SetWanted(fogsun::kShadowMapping, true);
        CHECK_EQ(g_calls, 1);
        fogsun::SetWanted(fogsun::kShadowMapping, false);
        CHECK(g_calls == 2 && !fogsun::Wanted());
        // A hook cleared again (a test or a teardown) is not called.
        fogsun::SetWantedChangedHook(nullptr);
        fogsun::SetWanted(fogsun::kShadowMapping, true);
        CHECK_EQ(g_calls, 2);
    }

    // ---- two consumers (2026-11-07 integration: the character ambient reads the same curve as shadow mapping): the capture is wanted while EITHER
    // asks, and one consumer letting go never drops the other's request; the hook still fires only when the overall answer changes.
    fogsun::ResetForTest();
    g_calls = 0;
    {
        fogsun::SetWantedChangedHook(&Hook);
        fogsun::SetWanted(fogsun::kShadowMapping, true);
        CHECK(fogsun::Wanted() && g_calls == 1);
        fogsun::SetWanted(fogsun::kCharaAmbient, true); // a second consumer joins: still wanted, no change, no call
        CHECK(fogsun::Wanted() && g_calls == 1);
        fogsun::SetWanted(fogsun::kShadowMapping, false); // the first lets go: the second still wants it
        CHECK(fogsun::Wanted() && g_calls == 1);
        fogsun::SetWanted(fogsun::kShadowMapping, false); // letting go twice changes nothing
        CHECK(fogsun::Wanted() && g_calls == 1);
        fogsun::SetWanted(fogsun::kCharaAmbient, false); // the last lets go: one call
        CHECK(!fogsun::Wanted() && g_calls == 2);
        fogsun::SetWanted(fogsun::kCharaAmbient, true); // the character ambient alone arms the capture
        CHECK(fogsun::Wanted() && g_calls == 3);
    }
    fogsun::ResetForTest();
    return Finish("fogsun_hooks");
}
