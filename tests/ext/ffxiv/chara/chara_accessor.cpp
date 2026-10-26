#include "check.h"
#include "pch.h"

#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/chara/CharaTable.h"

#include <string>

using namespace ext::ffxiv::chara::table;

int main()
{
    // This test executable is not the FFXIV game: the exact-build match refuses it (the rollback path) ...
    CHECK(ext::ffxiv::MatchCurrentProcess() == nullptr);

    // Nothing is resolved (and nothing is scanned) until the first request; the menu may ask for the reason before that.
    CHECK(std::string(InertReason()).empty());
    CHECK(g_testLogLines == 0);

    // Before the startup has published the build gate there is no verdict. The answer is "not decided", it is not latched and nothing is logged,
    // however often it is asked (a refusal computed this early would stick for the whole session).
    CHECK(!ext::ffxiv::ActiveStarted());
    for (int i = 0; i < 10; ++i)
    {
        CHECK(!Ready());
        CHECK(Resolve().state == AccessorState::Unresolved);
        Current(100 + i);
    }
    CHECK(g_testLogLines == 0 && std::string(InertReason()).empty());

    // ... and the gate of the startup refuses it too (not ffxiv_dx11.exe): the feature goes inert with the gate's reason.
    ext::ffxiv::resolve::GateReport gate;
    gate.started = true;
    for (auto& d : gate.features)
        d.reason = "the executable is not ffxiv_dx11.exe";
    CHECK(ext::ffxiv::PublishActive(ext::ffxiv::kCurrentBuild, gate));

    CHECK(!Ready());
    const Resolution& r = Resolve();
    CHECK(r.state == AccessorState::Inert);
    CHECK(r.reason.find("the executable is not ffxiv_dx11.exe") != std::string::npos);
    CHECK(r.managerPointerAddress == 0);
    CHECK(!r.reason.empty());
    CHECK(std::string(InertReason()) == r.reason);
    CHECK(g_testLastLog.find(r.reason) != std::string::npos);

    // exactly one log line for the failure, however often it is asked
    CHECK(g_testLogLines == 1);
    for (int i = 0; i < 100; ++i)
    {
        CHECK(!Ready());
        Current(1000 + i * 1000);
    }
    CHECK(g_testLogLines == 1);

    // the table stays empty and never matches
    const Snapshot& s = Current(5000);
    CHECK(!s.ready && !s.AnyValid() && s.ValidCount() == 0);
    CHECK(Lookup(s, 0x1234, 576, 960) == -1);

    // the documentation values of the matched build are present and consistent
    const ext::ffxiv::CharaViewBuild& cv = ext::ffxiv::kCurrentBuild.charaView;
    CHECK(cv.accessorRva == 0x26ABD0 && cv.managerPointerRva == 0x290E490);
    CHECK(cv.pattern.size() == 18 && cv.pattern[0] == 0x48 && cv.pattern[17] == 0xC3);
    return TEST_RESULT("chara_accessor");
}
