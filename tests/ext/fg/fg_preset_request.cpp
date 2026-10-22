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
    // Restart path: only an explicit A/B/C request changes the answer, and only for the preset id. The DLSS override id
    // (NGX_DLSS_FG_OVERRIDE_ID) is passed through to the driver: answering it "on" made NGX load the model-store module at boot
    // (proven in game 2026-10-22, PRESET-BOOT). Auto leaves the driver's own answer alone, whatever the baseline is.
    for (int r : { 1, 2, 3 })
    {
        const auto p = AnswerDriverQuery(kRenderPresetSettingId, r);
        CHECK(p.handled && p.found && p.value == (uint32_t) r);
        CHECK(!AnswerDriverQuery(kOverrideSettingId, r).handled);
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
        const auto a = AnswerCachedRead(kRenderPresetSettingId, r, kMissing, kPresetA);
        CHECK(a.handled && a.found && a.value == (uint32_t) r);
        const auto b = AnswerCachedRead(kRenderPresetSettingId, r, Has(2), kPresetA);
        CHECK(b.handled && b.found && b.value == (uint32_t) r); // the request wins over the driver value
    }
    // Auto gives back what the driver said: a value, or "not found".
    {
        const auto a = AnswerCachedRead(kRenderPresetSettingId, 0, Has(2), kPresetA);
        CHECK(a.handled && a.found && a.value == 2u);
        const auto b = AnswerCachedRead(kRenderPresetSettingId, 0, Has(0), kPresetA);
        CHECK(b.handled && b.found && b.value == 0u);
        const auto c = AnswerCachedRead(kRenderPresetSettingId, 0, kMissing, kPresetA);
        CHECK(c.handled && !c.found); // NOT the stale value of an earlier request
        const auto d = AnswerCachedRead(kRenderPresetSettingId, 0, kNone, kPresetA);
        CHECK(!d.handled); // unknown baseline: the reader's own cache decides
    }
    // Invalid request or another id: not handled.
    for (int r : { -1, 4, 77 })
        CHECK(!AnswerCachedRead(kRenderPresetSettingId, r, Has(1), kPresetA).handled);
    CHECK(!AnswerCachedRead(0x10308298u, 2, Has(1), kPresetA).handled);
    // The override-on id is not read through the cache by the DLL; the reader leaves it alone.
    CHECK(!AnswerCachedRead(kOverrideSettingId, 2, Has(1), kPresetA).handled);
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
    CHECK(v.Base().kind == Baseline::Kind::Value);
}
void DriverQuerySeam()
{
    // The hook records the driver reply of the preset id BEFORE it answers, then answers for the active request.
    {
        RequestState s; // Auto
        s.InitOnce(kAuto);
        const auto e = HandleDriverReply(s, kRenderPresetSettingId, true, true, 2);
        CHECK(!e.replace);
        CHECK(s.QuerySeen() && s.Base().kind == Baseline::Kind::Value && s.Base().value == 2u);
    }
    {
        RequestState s; // A, driver profile holds B
        s.InitOnce(kPresetA);
        const auto e = HandleDriverReply(s, kRenderPresetSettingId, true, true, 2);
        CHECK(e.replace && e.value == 1u);
        CHECK(s.Base().kind == Baseline::Kind::Value && s.Base().value == 2u); // the original, not the request
    }
    {
        RequestState s; // C, no such setting in the driver profile: the reply becomes a found number
        s.InitOnce(kPresetC);
        const auto e = HandleDriverReply(s, kRenderPresetSettingId, false, false, 0);
        CHECK(e.replace && e.value == 3u);
        CHECK(s.Base().kind == Baseline::Kind::NotFound);
    }
    {
        RequestState s; // the override id is passed through for every request, found or not, and is not the preset query
        for (int r : { 1, 2, 3 })
        {
            s.Commit(r);
            CHECK(!HandleDriverReply(s, kOverrideSettingId, false, false, 0).replace);
            CHECK(!HandleDriverReply(s, kOverrideSettingId, true, true, 0).replace);
            CHECK(!HandleDriverReply(s, kOverrideSettingId, true, true, 1).replace);
        }
        CHECK(!s.QuerySeen() && s.Base().kind == Baseline::Kind::Unknown);
    }
    {
        RequestState s; // other ids are none of our business
        s.InitOnce(kPresetB);
        CHECK(!HandleDriverReply(s, 0x10308298u, true, true, 1).replace);
        CHECK(!s.QuerySeen());
    }
    {
        RequestState s; // live switch later: the same hook answers the new request
        s.InitOnce(kPresetA);
        s.Commit(kPresetB);
        CHECK(HandleDriverReply(s, kRenderPresetSettingId, true, true, 0).value == 2u);
        s.Commit(kAuto);
        CHECK(!HandleDriverReply(s, kRenderPresetSettingId, true, true, 0).replace);
    }
}
void AutoPassesThroughWhenBootedOnAuto()
{
    // M3: a process that booted on Auto never had its cache touched, so Auto must reach the module's own reader, whatever the first
    // driver reply was (the runtime may have asked several profiles; the first reply is not necessarily what it cached).
    for (const Baseline& b : { kNone, kMissing, Has(2), Has(0) })
        CHECK(!AnswerCachedRead(kRenderPresetSettingId, kAuto, b, kAuto).handled);
    // an explicit request is still answered after a boot on Auto
    CHECK(AnswerCachedRead(kRenderPresetSettingId, kPresetB, kMissing, kAuto).value == 2u);

    // the same through the state: boot Auto, the first reply was "not found" (app profile), the cache holds a value (base profile)
    RequestState s;
    s.InitOnce(kAuto);
    s.NoteDriverQuery(false, false, 0);
    s.Commit(kPresetA);
    CHECK(s.ReadCached(kRenderPresetSettingId).value == 1u);
    s.Commit(kAuto);
    CHECK(!s.ReadCached(kRenderPresetSettingId).handled); // passes through to the real reader

    // booted on a request: the recorded reply is the only original there is
    RequestState t;
    t.InitOnce(kPresetB);
    t.NoteDriverQuery(true, true, 1);
    t.Commit(kAuto);
    const auto a = t.ReadCached(kRenderPresetSettingId);
    CHECK(a.handled && a.found && a.value == 1u);
}

void InitIsAtomicForReaders()
{
    // L1: once InitOnce returned, both values are visible together; a second call (another thread) changes nothing
    RequestState s;
    s.InitOnce(kPresetC);
    CHECK(s.Boot() == kPresetC && s.Active() == kPresetC);
    s.InitOnce(kPresetA);
    CHECK(s.Boot() == kPresetC && s.Active() == kPresetC);
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
    DriverQuerySeam();
    AutoPassesThroughWhenBootedOnAuto();
    InitIsAtomicForReaders();
    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_request: ids, mapping 0..3, driver query, cached reader, Auto baseline, stale start passed");
    return 0;
}
