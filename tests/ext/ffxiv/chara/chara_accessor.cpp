#include "check.h"
#include "pch.h"

#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/chara/CharaTable.h"

#include <string>

using namespace ext::ffxiv::chara::table;

int main()
{
    // This test executable is not the FFXIV game: the build gate must refuse it.
    CHECK(ext::ffxiv::MatchCurrentProcess() == nullptr);

    // Nothing is resolved (and nothing is scanned) until the first request; the menu may ask for the reason before that.
    CHECK(std::string(InertReason()).empty());
    CHECK(g_testLogLines == 0);

    CHECK(!Ready());
    const Resolution& r = Resolve();
    CHECK(r.state == AccessorState::Inert);
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
    CHECK(cv.accessorRva == 0x26AB40 && cv.managerPointerRva == 0x28F9490);
    CHECK(cv.pattern.size() == 18 && cv.pattern[0] == 0x48 && cv.pattern[17] == 0xC3);
    return TEST_RESULT("chara_accessor");
}
