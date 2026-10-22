// Focus pause for two NR consumers (ext/nr/NrFocusTracker.h): the scene NR pass and the character preview NR share one window
// sample and have one FocusGate each. The existing single-gate rules stay in nr_focus_gate.cpp; here only what two consumers add:
// independence of the gates, the shared (rate-limited) sample, and the wiring of the preview run.
#include "ext/nr/NrFocusTracker.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using ext::nr::FocusConsumer;
using ext::nr::FocusGate;
using ext::nr::PauseEpisodes;
using ext::nr::WindowSample;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// A window that the test moves in and out of focus, and counts how often it is looked at.
struct FakeWindow
{
    bool focused = true;
    bool minimised = false;
    int looks = 0;
};

struct Sampler
{
    FakeWindow* w;
    WindowSample operator()() const
    {
        ++w->looks;
        return { w->focused, w->minimised };
    }
};

using Tracker = ext::nr::FocusTracker<Sampler>;
constexpr FocusConsumer kScene = FocusConsumer::Scene;
constexpr FocusConsumer kPreview = FocusConsumer::Preview;

// Scene NR on, preview NR off, 2 s unfocused with both asked every frame: the scene pauses after 1 s, the preview (switch
// off) is never paused, and the preview's "off" calls do not clear the scene's pause counting.
void IndependentGates()
{
    FakeWindow w;
    Tracker t { Sampler { &w } };
    w.focused = false;
    bool scenePaused = false;
    uint64_t pausedAt = 0;
    for (uint64_t now = 0; now <= 2000; now += 16)
    {
        const bool scene = t.Allows(kScene, true, now);
        const bool preview = t.Allows(kPreview, false, now);
        CHECK(preview);
        if (!scene && !scenePaused)
        {
            scenePaused = true;
            pausedAt = now;
        }
        CHECK(scene == !scenePaused);
    }
    CHECK(scenePaused);
    CHECK(pausedAt >= FocusGate::kPauseAfterMs && pausedAt < FocusGate::kPauseAfterMs + 100);

    // And the other way round: preview on, scene off.
    FakeWindow w2;
    Tracker t2 { Sampler { &w2 } };
    w2.focused = false;
    bool previewPaused = false;
    for (uint64_t now = 0; now <= 2000; now += 16)
    {
        const bool scene = t2.Allows(kScene, false, now);
        const bool preview = t2.Allows(kPreview, true, now);
        CHECK(scene);
        if (!preview)
            previewPaused = true;
    }
    CHECK(previewPaused);
}

// Both on: they pause together and resume together, with the gate's own timing.
void BothOn()
{
    FakeWindow w;
    Tracker t { Sampler { &w } };
    uint64_t now = 0;
    for (; now < 500; now += 16)
        CHECK(t.Allows(kScene, true, now) && t.Allows(kPreview, true, now));
    w.focused = false;
    bool sceneOut = false, previewOut = false;
    for (; now < 3000; now += 16)
    {
        sceneOut = !t.Allows(kScene, true, now);
        previewOut = !t.Allows(kPreview, true, now);
        CHECK(sceneOut == previewOut);
    }
    CHECK(sceneOut && previewOut);
    w.focused = true;
    const uint64_t back = now;
    bool resumed = false;
    for (; now < back + 1000; now += 16)
    {
        const bool s = t.Allows(kScene, true, now), p = t.Allows(kPreview, true, now);
        CHECK(s == p);
        if (s && !resumed)
        {
            resumed = true;
            CHECK(now - back >= FocusGate::kResumeAfterMs);
        }
    }
    CHECK(resumed);
}

// One window sample is shared: asking for both consumers in the same frame looks at the window once, and at most once per
// interval whatever the number of callers. With both switches off the window is never looked at.
void SharedSample()
{
    FakeWindow w;
    Tracker t { Sampler { &w } };
    for (uint64_t now = 0; now < 1000; now += 5) // 200 frames of 5 ms, both consumers each
    {
        t.Allows(kScene, true, now);
        t.Allows(kPreview, true, now);
    }
    // 1000 ms / 50 ms = 20 samples; two consumers must not double it
    CHECK(w.looks >= 19 && w.looks <= 21);

    FakeWindow idle;
    Tracker t2 { Sampler { &idle } };
    for (uint64_t now = 0; now < 1000; now += 16)
    {
        t2.Allows(kScene, false, now);
        t2.Allows(kPreview, false, now);
    }
    CHECK(idle.looks == 0);

    // A clock that steps backwards samples again instead of waiting for it to catch up.
    FakeWindow back;
    Tracker t3 { Sampler { &back } };
    t3.Allows(kScene, true, 10000);
    t3.Allows(kScene, true, 10010);
    CHECK(back.looks == 1);
    t3.Allows(kScene, true, 100);
    CHECK(back.looks == 2);
}

// The preview's switch turned on while the window has been in the background for a long time starts the 1 s count then.
void LateSwitchOn()
{
    FakeWindow w;
    w.focused = false;
    Tracker t { Sampler { &w } };
    uint64_t now = 0;
    for (; now < 10000; now += 16)
        t.Allows(kScene, false, now); // preview off: nothing counted
    CHECK(t.Allows(kPreview, true, now));
    CHECK(t.Allows(kPreview, true, now + FocusGate::kPauseAfterMs - 100));
    CHECK(!t.Allows(kPreview, true, now + FocusGate::kPauseAfterMs + 100));
}

// The episode log of the preview (FocusAllowsPreviewNr writes one line per event): silent while the answer stays the same, one event
// when it flips each way, again for a second episode. First the bare state, then fed by the tracker the way NrFocus.cpp does.
void Episodes()
{
    using Event = PauseEpisodes::Event;
    PauseEpisodes e;
    CHECK(e.Note(true) == Event::None); // focused from the start: nothing to say
    CHECK(e.Note(true) == Event::None);
    CHECK(e.Note(false) == Event::Paused);
    CHECK(e.Note(false) == Event::None); // one line per episode
    CHECK(e.Note(false) == Event::None);
    CHECK(e.Note(true) == Event::Resumed);
    CHECK(e.Note(true) == Event::None);
    CHECK(e.Note(false) == Event::Paused); // a second episode
    CHECK(e.Note(true) == Event::Resumed);

    // Through the tracker: the preview is asked every 16 ms; the window leaves and returns twice.
    FakeWindow w;
    Tracker t { Sampler { &w } };
    PauseEpisodes episodes;
    int paused = 0, resumed = 0;
    uint64_t pausedAt = 0, resumedAt = 0, now = 0;
    const auto runUntil = [&](uint64_t until, bool switchOn)
    {
        for (; now < until; now += 16)
        {
            switch (episodes.Note(t.Allows(kPreview, switchOn, now)))
            {
            case Event::Paused: ++paused; pausedAt = now; break;
            case Event::Resumed: ++resumed; resumedAt = now; break;
            case Event::None: break;
            }
        }
    };
    runUntil(500, true);
    CHECK(paused == 0 && resumed == 0); // focused: silence
    w.focused = false;
    const uint64_t lost = now;
    runUntil(3000, true);
    CHECK(paused == 1 && resumed == 0);
    CHECK(pausedAt - lost >= FocusGate::kPauseAfterMs && pausedAt - lost < FocusGate::kPauseAfterMs + 100); // when the gate pauses, not before
    w.focused = true;
    const uint64_t back = now;
    runUntil(4000, true);
    CHECK(paused == 1 && resumed == 1);
    CHECK(resumedAt - back >= FocusGate::kResumeAfterMs);
    w.focused = false;
    runUntil(7000, true);
    w.focused = true;
    runUntil(8000, true);
    CHECK(paused == 2 && resumed == 2); // the second episode speaks again

    // The switch off (preview NR not wanted, or DisableOnFocusLoss off) while the window is away: nothing to report.
    w.focused = false;
    runUntil(12000, false);
    CHECK(paused == 2 && resumed == 2);
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // the sources are CRLF
    return text;
}

// The wiring: NrFocus.cpp feeds the tracker, the scene entry is the same as before, the preview run asks before it does any work.
void Wiring(const std::string& repo)
{
    const std::string nr = repo + "/OptiScaler/ext/nr/";
    const std::string header = ReadFile(nr + "NrFocus.h");
    const std::string source = ReadFile(nr + "NrFocus.cpp");
    CHECK(!header.empty() && !source.empty());

    CHECK(header.find("bool FocusAllowsNr(bool switchOn);") != std::string::npos);
    CHECK(header.find("bool FocusAllowsPreviewNr(bool switchOn);") != std::string::npos);
    CHECK(source.find("#include \"ext/nr/NrFocusTracker.h\"") != std::string::npos);
    CHECK(source.find("Tracker().Allows(FocusConsumer::Scene, switchOn, GetTickCount64())") != std::string::npos);
    CHECK(source.find("Tracker().Allows(FocusConsumer::Preview, switchOn, GetTickCount64())") != std::string::npos);
    // one episode, one line, each way: the flip is PauseEpisodes::Note (tested above); the line follows the event it names
    CHECK(source.find("LOG_INFO(\"CharaView NR: paused, window not focused\");") != std::string::npos);
    CHECK(source.find("LOG_INFO(\"CharaView NR: resumed\");") != std::string::npos);
    std::string compact; // white space removed: the pins below are about order, not layout
    for (const char c : source)
        if (c != ' ' && c != '\t' && c != '\n')
            compact += c;
    CHECK(compact.find("staticPauseEpisodesepisodes;") != std::string::npos);
    CHECK(compact.find("switch(episodes.Note(allowed))") != std::string::npos);
    CHECK(source.find("case PauseEpisodes::Event::Paused:") != std::string::npos);
    CHECK(source.find("case PauseEpisodes::Event::Resumed:") != std::string::npos);
    const size_t casePaused = source.find("case PauseEpisodes::Event::Paused:");
    const size_t caseResumed = source.find("case PauseEpisodes::Event::Resumed:");
    const size_t linePaused = source.find("LOG_INFO(\"CharaView NR: paused, window not focused\");");
    const size_t lineResumed = source.find("LOG_INFO(\"CharaView NR: resumed\");");
    CHECK(casePaused < linePaused && linePaused < caseResumed && caseResumed < lineResumed); // each line sits under the event it names

    // the preview run: the question comes after the attempt is counted (a paused run still counts as an attempt, so the stale
    // result is not shown for it) and before the availability, the stream table and every GPU step
    const std::string chara = ReadFile(repo + "/OptiScaler/ext/ffxiv/chara/CharaNr.cpp");
    CHECK(!chara.empty());
    const size_t entry = chara.find("static RunOutcome RunCoreSteps(");
    CHECK(entry != std::string::npos);
    const size_t attempts = chara.find("++m.table.At(known).attempts;", entry);
    const size_t ask = chara.find("if (!ext::nr::FocusAllowsPreviewNr(Wanted() && ext::Cfg().nr.disableOnFocusLoss.value_or_default()))", entry);
    const size_t refresh = chara.find("if (Refresh() != Availability::Available", entry);
    CHECK(attempts != std::string::npos && ask != std::string::npos && refresh != std::string::npos);
    CHECK(attempts < ask && ask < refresh);
    // ... and it ends in its own outcome, Paused, not in the generic Skipped: a pause is not counted as a render that kept the image
    // (CountsAsFallback) and is not logged under the "no stream or list was free" cause text; the focus gate's episode lines report it
    const size_t paused = chara.find("return RunOutcome::Paused;", ask);
    CHECK(paused != std::string::npos && paused - ask < 400);
    // a paused run keeps the open window's stream alive: Maintain releases a stream whose lastLive is older than the linger
    // time (5 s by default), so without this refresh any longer alt-tab would free the model and rebuild it on return
    const size_t live = chara.find("m.table.At(openStream).lastLive = req.now;", ask);
    CHECK(live != std::string::npos && paused != std::string::npos && live < paused);
    CHECK(chara.find("if (const int openStream = m.table.Find(key); openStream != kNoStream)", ask) < live);

    // the key now covers the preview too, and the two places that describe it say so
    const std::string ini = ReadFile(repo + "/OptiScaler/ext/ini/DlssNr.ini");
    const size_t keyAt = ini.find("\nDisableOnFocusLoss=auto\n");
    CHECK(keyAt != std::string::npos);
    CHECK(ini.substr(0, keyAt).rfind("the character preview NR") != std::string::npos);
    CHECK(ini.substr(0, keyAt).rfind("native Vulkan is not paused") != std::string::npos);
    const std::string menu = ReadFile(nr + "menu/DlssNr_Menu.cpp");
    const size_t box = menu.find("Pause NR while the game is not focused (experimental)");
    CHECK(box != std::string::npos);
    CHECK(menu.find("The character preview NR pauses and resumes with it.", box) != std::string::npos);

    // the scene pass keeps asking the old entry point with its own switch
    const std::string pass = ReadFile(nr + "pass/DlssNr_Dx12.cpp");
    CHECK(pass.find("ext::nr::FocusAllowsNr(switchOn)") != std::string::npos);
    CHECK(pass.find("FocusAllowsPreviewNr") == std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    IndependentGates();
    BothOn();
    SharedSample();
    LateSwitchOn();
    Episodes();
    Wiring(argc > 1 ? argv[1] : ".");

    if (failures == 0)
        std::printf("nr_focus_tracker: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
