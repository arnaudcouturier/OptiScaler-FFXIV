// DLSS-G render preset request model (ext/fg/dlssg/PresetRequest.h): the setting ids, the 0..3 mapping, what the
// process answers to the driver-settings query (restart path) and to NVIDIA's cached-settings reader (live path).
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetRequest.h"

#include <cstring>
#include <initializer_list>

using namespace ext::fg::preset;

namespace
{
const Baseline kNone { Baseline::Kind::Unknown, 0 };
const Baseline kMissing { Baseline::Kind::NotFound, 0 };
Baseline Has(uint32_t v) { return { Baseline::Kind::Value, v }; }

void Ids()
{
    CHECK(kRenderPresetSettingId == 0x10E41DF1u); // NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID
    CHECK(kOverrideSettingId == 0x10E41E03u);     // NGX_DLSS_FG_OVERRIDE_ID
    CHECK(kOverrideOn == 1u);
    CHECK(kAuto == 0 && kPresetA == 1 && kPresetB == 2 && kPresetC == 3);
}

void RequestRange()
{
    for (int r : { 0, 1, 2, 3 })
        CHECK_MSG(ValidRequest(r), "%d", r);
    for (int r : { -1, 4, 5, 13, 99, -100, 0x7fffffff })
        CHECK_MSG(!ValidRequest(r), "%d", r);
}

void Names()
{
    CHECK(std::strcmp(Name(0), "Auto") == 0);
    CHECK(std::strcmp(Name(1), "Preset A") == 0);
    CHECK(std::strcmp(Name(2), "Preset B") == 0);
    CHECK(std::strcmp(Name(3), "Preset C") == 0);
    CHECK(std::strcmp(Name(7), "Auto") == 0); // anything invalid reads as Auto
}

void DriverQueryAnswers()
{
    // Restart path: only an explicit A/B/C request changes the answer. The preset id gets the number, the override id
    // gets "on". Auto leaves the driver's own answer alone, whatever the baseline is.
    for (int r : { 1, 2, 3 })
    {
        const auto p = AnswerDriverQuery(kRenderPresetSettingId, r);
        CHECK(p.handled && p.found && p.value == (uint32_t) r);
        const auto o = AnswerDriverQuery(kOverrideSettingId, r);
        CHECK(o.handled && o.found && o.value == kOverrideOn);
    }
    CHECK(!AnswerDriverQuery(kRenderPresetSettingId, 0).handled);
    CHECK(!AnswerDriverQuery(kOverrideSettingId, 0).handled);
    for (int r : { -1, 4, 9 })
    {
        CHECK(!AnswerDriverQuery(kRenderPresetSettingId, r).handled);
        CHECK(!AnswerDriverQuery(kOverrideSettingId, r).handled);
    }
    // Other ids are never touched.
    for (uint32_t id : { 0x10E41DF3u /*SR preset*/, 0x10E41DF7u /*RR preset*/, 0x10308298u /*DLSSG mode*/, 0u })
        CHECK(!AnswerDriverQuery(id, 2).handled);
}

void CachedReaderAnswers()
{
    // Live path: the cached reader is asked for the preset id with the active request.
    for (int r : { 1, 2, 3 })
    {
        const auto a = AnswerCachedRead(kRenderPresetSettingId, r, kMissing);
        CHECK(a.handled && a.found && a.value == (uint32_t) r);
        const auto b = AnswerCachedRead(kRenderPresetSettingId, r, Has(2));
        CHECK(b.handled && b.found && b.value == (uint32_t) r); // the request wins over the driver value
    }
    // Auto gives back what the driver said: a value, or "not found".
    {
        const auto a = AnswerCachedRead(kRenderPresetSettingId, 0, Has(2));
        CHECK(a.handled && a.found && a.value == 2u);
        const auto b = AnswerCachedRead(kRenderPresetSettingId, 0, Has(0));
        CHECK(b.handled && b.found && b.value == 0u);
        const auto c = AnswerCachedRead(kRenderPresetSettingId, 0, kMissing);
        CHECK(c.handled && !c.found); // NOT the stale value of an earlier request
        const auto d = AnswerCachedRead(kRenderPresetSettingId, 0, kNone);
        CHECK(!d.handled); // unknown baseline: the reader's own cache decides
    }
    // Invalid request or another id: not handled.
    for (int r : { -1, 4, 77 })
        CHECK(!AnswerCachedRead(kRenderPresetSettingId, r, Has(1)).handled);
    CHECK(!AnswerCachedRead(0x10308298u, 2, Has(1)).handled);
    // The override-on id is not read through the cache by the DLL; the reader leaves it alone.
    CHECK(!AnswerCachedRead(kOverrideSettingId, 2, Has(1)).handled);
}

void BaselineFromDriver()
{
    CHECK(BaselineFromDriverReply(true, true, 5).kind == Baseline::Kind::Value);
    CHECK(BaselineFromDriverReply(true, true, 5).value == 5u);
    CHECK(BaselineFromDriverReply(false, false, 123).kind == Baseline::Kind::NotFound); // setting absent from the profile
    CHECK(BaselineFromDriverReply(false, true, 123).kind == Baseline::Kind::NotFound);
    CHECK(BaselineFromDriverReply(true, false, 5).kind == Baseline::Kind::Unknown); // an OK reply that is not a number
}

void StaleStartScenario()
{
    // Start at A with no driver value, switch live to Auto: the reader must say "not found", not the cached A.
    RequestState s;
    s.InitOnce(kPresetA);
    s.NoteDriverQuery(false, false, 0);
    CHECK(s.Boot() == kPresetA && s.Active() == kPresetA);
    CHECK(s.CanReturnToAuto());
    s.Commit(kAuto);
    const auto a = s.ReadCached(kRenderPresetSettingId);
    CHECK(a.handled && !a.found);
    s.Commit(kPresetC);
    const auto b = s.ReadCached(kRenderPresetSettingId);
    CHECK(b.handled && b.found && b.value == 3u);
}

void ReturnToAutoNeedsBaseline()
{
    // Booted with a request but the baseline was never seen: Auto cannot be restored live (restart needed).
    RequestState s;
    s.InitOnce(kPresetB);
    CHECK(!s.CanReturnToAuto());
    s.NoteDriverQuery(true, true, 1);
    CHECK(s.CanReturnToAuto());

    // Booted with Auto: the reader's own cache is the original, so Auto is always restorable.
    RequestState t;
    t.InitOnce(kAuto);
    CHECK(t.CanReturnToAuto());
    t.Commit(kPresetA);
    t.Commit(kAuto);
    CHECK(!t.ReadCached(kRenderPresetSettingId).handled); // no baseline: the untouched cache answers
}

void StateBasics()
{
    RequestState s;
    CHECK(s.Active() == kAuto && s.Boot() == kAuto);
    s.InitOnce(9); // out of range in the ini: Auto
    CHECK(s.Active() == kAuto && s.Boot() == kAuto);
    s.InitOnce(kPresetB); // once only
    CHECK(s.Active() == kAuto);

    RequestState u;
    u.InitOnce(kPresetB);
    CHECK(u.Active() == kPresetB && u.Boot() == kPresetB);
    u.Commit(kPresetC);
    CHECK(u.Active() == kPresetC && u.Boot() == kPresetB);
    u.Commit(42); // invalid commits are ignored
    CHECK(u.Active() == kPresetC);
    u.Commit(-1);
    CHECK(u.Active() == kPresetC);

    // The first driver reply is the baseline; later ones (another module asking) do not replace it.
    RequestState v;
    CHECK(!v.QuerySeen());
    v.NoteDriverQuery(true, true, 2);
    v.NoteDriverQuery(false, false, 0);
    CHECK(v.QuerySeen());
    CHECK(v.Base().kind == Baseline::Kind::Value && v.Base().value == 2u);
    v.ClearQuerySeen(); // a live switch recreates the endpoint, which asks again
    CHECK(!v.QuerySeen());
    CHECK(v.Base().kind == Baseline::Kind::Value);
}
} // namespace

int main()
{
    Ids();
    RequestRange();
    Names();
    DriverQueryAnswers();
    CachedReaderAnswers();
    BaselineFromDriver();
    StaleStartScenario();
    ReturnToAutoNeedsBaseline();
    StateBasics();
    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_request: ids, mapping 0..3, driver query, cached reader, Auto baseline, stale start passed");
    return 0;
}
