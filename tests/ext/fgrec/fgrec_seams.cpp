// Every event tap writes the expected kind and fields through the recorder
// with stand-ins for the process reads (config key, State, game match,
// version). One session drives every seam: present begin/end pairs (with a
// missing begin reporting a zero duration, and willPresent=false emitting
// nothing), sleep, hidden present, AutoMFG, NR rebuild, provider activation
// (which bumps the generation) and teardown, preset switch, module loads by
// file name only and XeFG factor changes. A tap that writes the wrong kind,
// drops a field or leaks a path fails here.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecSeams.cpp" // the core links separately (see the recipe): its private queue state must not shadow the State stand-in

#include <cstdio>
#include <string>
#include <vector>

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

namespace ext::core
{
const char* ForkVersion() { return "fgrec-test"; }
} // namespace ext::core

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static std::wstring g_recDir;

// The begin slot of a present end belongs to one session. A key-off between a begin and its end left the slot
// set, so the first end of the next session paired with it and reported the whole pause as the present's
// duration (a false hitch of seconds). The end taps now take and clear the slot before anything can return
// early, so a tap that emits nothing still leaves no slot behind.
static void BeginSlotNeverCrossesASession()
{
    ext::Cfg().framePacing.fgRecorder = true;
    for (int i = 0; i < 20 && !fg::RecArmed(); ++i)
        fg::FgPresentBegin(false, 0, 0); // arm the cached key (skipped presents emit nothing)
    CHECK(fg::RecArmed());
    fg::FgPresentBegin(true, 1, 0);
    CHECK(fg::tFgBegin.QuadPart != 0); // the open begin of this frame
    fg::OutPresentBegin(true, 1, 0);
    CHECK(fg::tOutBegin.QuadPart != 0);

    ext::Cfg().framePacing.fgRecorder = false; // the key goes off between the begin and its end
    for (int i = 0; i < 20 && fg::RecArmed(); ++i)
        fg::FgPresentBegin(false, 0, 0); // the poll sees the key off and disarms
    CHECK(!fg::RecArmed());
    fg::FgPresentEnd(true, 4242); // off: emits nothing, and must clear its slot
    fg::OutPresentEnd(true, 4242);
    CHECK(fg::tFgBegin.QuadPart == 0);
    CHECK(fg::tOutBegin.QuadPart == 0);

    fg::EnsureStarted(false, fg::SessionInfo {}); // nothing keeps running after the test
    CHECK(ft::WaitUntil([]() { return !fg::Alive(); }, 30000));
}

// The session file is settled once it holds the terminal record: the writer
// emits session.end last, so a parsed end line means every event is on disk.
// (This binary links the core instead of including it, so it cannot watch the
// worker flag directly; see fgrec_record.cpp for why Running() alone is not
// enough after a stop.)
static bool FileSettled()
{
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((g_recDir + L"\\fg-rec-*.jsonl").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const std::string bytes = ft::ReadBytes(g_recDir + L"\\" + f.cFileName);
    FindClose(h);
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    if (lines.empty())
        return false;
    bool ok = false;
    ft::JsonValue v = ft::ParseLine(lines.back(), ok);
    return ok && ft::IsText(ft::Find(v, "kind"), "session.end");
}

static void MappingUnits()
{
    CHECK(fg::MapProvider(static_cast<uint32_t>(FGOutput::NoFG)) == fg::kProvNone);
    CHECK(fg::MapProvider(static_cast<uint32_t>(FGOutput::FSRFG)) == fg::kProvFsr);
    CHECK(fg::MapProvider(static_cast<uint32_t>(FGOutput::DLSSG)) == fg::kProvDlssG);
    CHECK(fg::MapProvider(static_cast<uint32_t>(FGOutput::XeFG)) == fg::kProvXeFg);
    CHECK(fg::MapProvider(static_cast<uint32_t>(FGOutput::Reprojection)) == fg::kProvNone);
    CHECK(fg::MapProvider(99) == fg::kProvNone); // unknown output is none, never a made-up id
    CHECK(std::string(fg::ProviderLabel(static_cast<uint32_t>(FGOutput::DLSSG))) == "dlssg");
    CHECK(std::string(fg::ProviderLabel(static_cast<uint32_t>(FGOutput::XeFG))) == "xefg");
    CHECK(std::string(fg::ProviderLabel(static_cast<uint32_t>(FGOutput::FSRFG))) == "fsr");
    CHECK(std::string(fg::ProviderLabel(static_cast<uint32_t>(FGOutput::NoFG))) == "none");
    CHECK(std::string(fg::ProviderLabel(99)) == "none");
}

static void TapsThroughTheRecorder()
{
    CHECK(!fg::RecArmed());
    ext::Cfg().framePacing.fgRecorder = true;
    State::Instance().gameExe = "ffxiv_dx11.exe";
    State::Instance().fgLastFrame = 1001;
    State::Instance().frameCount = 2000;
    State::Instance().activeFgOutput = FGOutput::DLSSG;
    ext::game::TestGame().lighting = reinterpret_cast<const void*>(1);
    for (int i = 0; i < 20; ++i)
        fg::FgPresentBegin(false, 0, 0); // prime the cached key (skipped presents emit nothing)
    CHECK(fg::RecArmed());

    fg::FgPresentBegin(true, 1, 0);
    fg::FgPresentEnd(true, 0);
    fg::FgPresentBegin(false, 1, 0); // skipped presents emit nothing
    fg::FgPresentEnd(false, 0);
    fg::tFgBegin.QuadPart = 0; // a missing begin reports a zero duration, never a wrong one
    fg::FgPresentEnd(true, 5);
    State::Instance().activeFgOutput = FGOutput::FSRFG; // the provider id follows the live output
    fg::OutPresentBegin(true, 1, 2);
    fg::OutPresentEnd(true, 0);
    fg::OutPresentBegin(false, 1, 2);
    fg::OutPresentEnd(false, 0);
    fg::tOutBegin.QuadPart = 0;
    fg::OutPresentEnd(true, 0);

    fg::OnReflexSleep(777);
    const HRESULT hiddenHr = static_cast<HRESULT>(0x887A0005L);
    fg::OnHiddenPresent(hiddenHr, 42);
    fg::OnAutoMfg(true, "switch 2x -> 3x now");
    fg::OnAutoMfg(false, "");
    fg::OnAutoMfg(true, nullptr);
    fg::OnNrRebuild(); // probe A: generation g
    fg::OnProviderOn("xefg"); // a new generation starts on activation
    State::Instance().frameCount = 2001;
    fg::OnNrRebuild(); // probe B: g + 1 (a new frame, so it is kept)
    fg::OnPresetSwitch("Quality", "Balanced"); // a switch starts one too
    fg::OnReflexSleep(1); // probe C: g + 2
    fg::OnProviderOff("dlssg"); // teardown bumps nothing
    fg::OnReflexSleep(2); // probe D: still g + 2
    State::Instance().frameCount = 3000;
    fg::OnNrRebuild(); // park and build of one rebuild share the frame: only one is kept
    fg::OnNrRebuild();
    fg::OnModule(L"C:\\Games\\mods\\nvngx_dlssg.dll");
    fg::OnModule(L"\\\\MACHINE\\share\\lib.xefg.dll");
    fg::OnModule(L"bare.dll");
    fg::OnModule(L"lib-\u00e9 Revered.dll");
    fg::OnModule(nullptr);
    fg::OnModule(L"");
    fg::OnXeFactor(2, 2); // unchanged factors emit nothing
    fg::OnXeFactor(2, 4);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring dir = exe;
    if (const size_t slash = dir.find_last_of(L"\\/"); slash != std::wstring::npos)
        dir.resize(slash);
    const std::wstring rec = dir + L"\\fg-recordings";
    g_recDir = rec;
    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(FileSettled, 30000));
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((rec + L"\\fg-rec-*.jsonl").c_str(), &f);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE)
        return;
    const std::string bytes = ft::ReadBytes(rec + L"\\" + f.cFileName);
    FindClose(h);
    CHECK(bytes.find('\\') == std::string::npos); // stripped modules: no backslash left anywhere
    CHECK(bytes.find("Users") == std::string::npos);
    CHECK(bytes.find("MACHINE") == std::string::npos);

    std::vector<ft::JsonValue> rows;
    for (const auto& line : ft::SplitLines(bytes))
    {
        bool ok = false;
        ft::JsonValue v = ft::ParseLine(line, ok);
        CHECK(ok);
        rows.push_back(std::move(v));
    }
    if (g_failed != 0)
        return;
    auto kind = [&](size_t i) {
        const ft::JsonValue* k = ft::Find(rows[i], "kind");
        return k != nullptr && k->type == ft::JsonValue::Type::Text ? k->text : std::string();
    };
    auto count = [&](const char* k) {
        int n = 0;
        for (const auto& r : rows)
            if (ft::IsText(ft::Find(r, "kind"), k))
                ++n;
        return n;
    };
    auto first = [&](const char* k) -> const ft::JsonValue* {
        for (const auto& r : rows)
            if (ft::IsText(ft::Find(r, "kind"), k))
                return &r;
        return nullptr;
    };
    auto num = [](const ft::JsonValue* r, const char* key) {
        const ft::JsonValue* v = ft::Find(*r, key);
        return v != nullptr && v->type == ft::JsonValue::Type::Number ? std::stoll(v->text) : -1;
    };

    CHECK(kind(0) == "session.begin");
    CHECK(ft::IsText(ft::Find(rows[0], "version"), "fgrec-test"));
    CHECK(ft::IsText(ft::Find(rows[0], "exe"), "ffxiv_dx11.exe"));
    CHECK(ft::IsText(ft::Find(rows[0], "build"), "matched"));
    CHECK(kind(rows.size() - 1) == "session.end");

    CHECK(count("fg.present.begin") == 1 && count("fg.present.end") == 2);
    const ft::JsonValue* fgBegin = first("fg.present.begin");
    CHECK(num(fgBegin, "frame") == 1001 && num(fgBegin, "a") == 1 && num(fgBegin, "b") == 0);
    CHECK(num(fgBegin, "prov") == 1); // DLSS-G at tap time
    const ft::JsonValue* fgEnd = first("fg.present.end");
    CHECK(num(fgEnd, "frame") == 1001 && num(fgEnd, "a") == 0 && num(fgEnd, "b") >= 0);
    CHECK(count("out.present.begin") == 1 && count("out.present.end") == 2);
    const ft::JsonValue* outBegin = first("out.present.begin");
    CHECK(num(outBegin, "frame") == 2000 && num(outBegin, "a") == 1 && num(outBegin, "b") == 2);
    CHECK(num(outBegin, "prov") == 3); // FSR FG at tap time
    {
        // an end without its begin reports a zero duration, never a wrong one
        bool orphanFg = false, orphanOut = false;
        for (const auto& r : rows)
        {
            if (ft::IsText(ft::Find(r, "kind"), "fg.present.end") && num(&r, "a") == 5)
                orphanFg = num(&r, "b") == 0;
            if (ft::IsText(ft::Find(r, "kind"), "out.present.end") && num(&r, "frame") == 2000)
                orphanOut = orphanOut || num(&r, "b") == 0;
        }
        CHECK(orphanFg && orphanOut);
    }
    const ft::JsonValue* outEnd = first("out.present.end");
    CHECK(num(outEnd, "frame") == 2000 && num(outEnd, "a") == 0);

    const ft::JsonValue* sleep = first("sleep");
    CHECK(num(sleep, "a") == 777 && ft::IsText(ft::Find(*sleep, "msg"), "reflex"));
    const ft::JsonValue* hidden = first("hidden.present");
    CHECK(hidden != nullptr &&
          num(hidden, "a") == static_cast<long long>(static_cast<int64_t>(hiddenHr)) && num(hidden, "b") == 42);
    const ft::JsonValue* automfg = first("automfg");
    CHECK(count("automfg") == 1); // null and empty texts emit nothing
    CHECK(num(automfg, "a") == 1 && ft::IsText(ft::Find(*automfg, "msg"), "switch 2x -> 3x now"));

    CHECK(count("nr.rebuild") == 3); // A, B and one of the same-frame pair
    CHECK(count("provider.activated") == 1 && count("provider.deactivating") == 1);
    CHECK(count("dlssg.preset.switch") == 1);
    CHECK(ft::IsText(ft::Find(*first("provider.activated"), "msg"), "xefg"));
    CHECK(ft::IsText(ft::Find(*first("provider.deactivating"), "msg"), "dlssg"));
    CHECK(ft::IsText(ft::Find(*first("dlssg.preset.switch"), "msg"), "Quality -> Balanced"));
    const long long g = num(first("nr.rebuild"), "gen");
    CHECK(g == 0);
    {
        int seen = 0;
        long long second = -1;
        for (const auto& r : rows)
            if (ft::IsText(ft::Find(r, "kind"), "nr.rebuild"))
            {
                if (++seen == 2)
                    second = num(&r, "gen");
            }
        CHECK(second == g + 1); // activation bumped the generation
    }
    CHECK(num(first("dlssg.preset.switch"), "gen") == g + 2); // the switch record itself already carries the new generation
    {
        long long c = -1, d = -1;
        int n = 0;
        for (const auto& r : rows)
            if (ft::IsText(ft::Find(r, "kind"), "sleep") &&
                (ft::Find(r, "msg")->text == "reflex" && (num(&r, "a") == 1 || num(&r, "a") == 2)))
            {
                ++n;
                (num(&r, "a") == 1 ? c : d) = num(&r, "gen");
            }
        CHECK(n == 2 && c == g + 2 && d == g + 2); // switch bumped once, teardown not at all
    }

    CHECK(count("provider.module") == 4); // null and empty emit nothing
    {
        std::vector<std::string> mods;
        for (const auto& r : rows)
            if (ft::IsText(ft::Find(r, "kind"), "provider.module"))
                mods.push_back(ft::Find(r, "msg")->text);
        CHECK(mods.size() == 4 && mods[0] == "nvngx_dlssg.dll" && mods[1] == "lib.xefg.dll" &&
              mods[2] == "bare.dll" && mods[3] == "lib-\xC3\xA9 Revered.dll"); // UTF-8, never a path
    }
    CHECK(count("xefg.factor") == 1); // the unchanged pair emits nothing
    CHECK(num(first("xefg.factor"), "a") == 2 && num(first("xefg.factor"), "b") == 4);
}

int main()
{
    MappingUnits();
    TapsThroughTheRecorder();
    BeginSlotNeverCrossesASession();
    if (g_failed != 0)
    {
        std::printf("fgrec_seams: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_seams: all passed\n");
    return 0;
}
