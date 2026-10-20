// NR runtime selection (ext/nr/runtime/NrRuntimeSelection.h): snapshots with a revision, the 250 ms coalescing of rapid requests, and
// the rule that the revision moves only when the chosen model changes. Driven with a fake catalog and a fake clock.
#include "ext/nr/runtime/NrRuntimeSelection.h"

#include <cstdio>

using namespace ext::nr::runtime;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static Entry Folder(const wchar_t* name)
{
    Entry e;
    e.id = name;
    e.path = std::filesystem::path(L"X:/g/OptiScaler") / name / L"nvngx_dlssnr.dll";
    e.folderVersion = *ParseFolderVersion(name);
    return e;
}
static Entry Legacy(VersionParts v)
{
    Entry e;
    e.id = L"legacy";
    e.legacy = true;
    e.path = L"X:/g/OptiScaler/nvngx_dlssnr.dll";
    e.fileVersion = std::move(v);
    return e;
}

struct Fake
{
    std::vector<Entry> entries;
    int calls = 0;
    Selector Make(uint64_t debounce = 250)
    {
        return Selector([this](std::wstring_view r) { ++calls; return MakeSnapshot(entries, r); }, debounce);
    }
};

static void Snapshots()
{
    std::vector<Entry> e { Legacy({ 310, 8, 0, 0 }), Folder(L"NR.310.9") };
    const auto automatic = MakeSnapshot(e, L"");
    CHECK(automatic.Available() && automatic.id == L"NR.310.9" && !automatic.legacy);
    CHECK((automatic.version == VersionParts { 310, 9 }));
    const auto legacy = MakeSnapshot(e, L"legacy");
    CHECK(legacy.Available() && legacy.legacy && legacy.path == e[0].path);
    const auto missing = MakeSnapshot(e, L"NR.310.7");
    CHECK(!missing.Available() && missing.status == Status::ExplicitMissing && missing.path.empty());
    CHECK(UnavailableReason(missing).find("NR.310.7") != std::string::npos);
    CHECK(UnavailableReason(missing).find("not installed") != std::string::npos);
    const auto bad = MakeSnapshot(e, L"..\\x");
    CHECK(!bad.Available() && bad.status == Status::InvalidName);
    CHECK(UnavailableReason(bad).find("not a valid NR runtime") != std::string::npos);
    const auto none = MakeSnapshot({}, L"");
    CHECK(!none.Available() && UnavailableReason(none) == "nvngx_dlssnr.dll was not found beside OptiScaler or the game");
    CHECK(UnavailableReason(automatic).empty());
}

static void FirstPollAndDebounce()
{
    Fake f;
    f.entries = { Legacy({ 310, 8, 0, 0 }), Folder(L"NR.310.9") };
    Selector s = f.Make();
    CHECK(s.Current() == nullptr);
    auto a = s.Poll(L"NR.310.8", 0); // the first request applies at once: nothing is loaded yet
    CHECK(a->revision == 1 && !a->Available()); // NR.310.8 is not installed
    CHECK(f.calls == 1);

    // a change waits for 250 ms without another change
    a = s.Poll(L"legacy", 1000);
    CHECK(a->revision == 1);
    CHECK(s.Pending());
    CHECK(s.Poll(L"legacy", 1249)->revision == 1);
    CHECK(s.Pending());
    const auto b = s.Poll(L"legacy", 1250);
    CHECK(b->revision == 2 && b->Available() && b->legacy);
    CHECK(!s.Pending());
    CHECK(f.calls == 2);
    // the snapshot handed out earlier is unchanged (immutable)
    CHECK(a->revision == 1 && !a->Available());

    // asking for what is already there changes nothing
    CHECK(s.Poll(L"legacy", 5000)->revision == 2);
    CHECK(f.calls == 2);
}

static void RapidClicks()
{
    Fake f;
    f.entries = { Legacy({ 310, 8, 0, 0 }), Folder(L"NR.310.9"), Folder(L"NR.310.10") };
    Selector s = f.Make();
    s.Poll(L"legacy", 0);
    // a click every 100 ms keeps restarting the quiet period: nothing applies while it goes on
    uint64_t now = 1000;
    const wchar_t* names[] = { L"NR.310.9", L"legacy", L"NR.310.10" };
    for (int i = 0; i < 20; ++i)
    {
        CHECK(s.Poll(names[i % 3], now)->revision == 1);
        now += 100;
    }
    CHECK(f.calls == 1);
    // the last click was names[19 % 3] = names[1] = legacy; the same model as the loaded one: no switch at all
    CHECK(s.Poll(L"legacy", now + 250)->revision == 1);
    CHECK(f.calls == 2); // resolved once, found equal

    // 20 clicks 10 ms apart that end somewhere else: exactly one switch, to the last request
    for (int i = 0; i < 20; ++i)
    {
        now += 10;
        s.Poll(names[(i + 1) % 3], now); // the 20th click is names[2] = NR.310.10
    }
    const auto last = s.Poll(names[2], now + 250);
    CHECK(last->revision == 2 && last->id == L"NR.310.10");
    CHECK(f.calls == 3);
    CHECK(s.Poll(names[2], now + 100000)->revision == 2);
}

static void ClicksRestartTheTimer()
{
    Fake f;
    f.entries = { Legacy({ 1 }), Folder(L"NR.2") };
    Selector s = f.Make();
    s.Poll(L"legacy", 0);
    s.Poll(L"NR.2", 100);
    CHECK(s.Poll(L"NR.2", 300)->revision == 1);
    s.Poll(L"legacy", 300); // another click: the quiet period starts again
    s.Poll(L"NR.2", 349);   // and another
    CHECK(s.Poll(L"NR.2", 598)->revision == 1);
    CHECK(s.Poll(L"NR.2", 599)->revision == 2);
}

static void ExplicitMissingAndRescan()
{
    Fake f;
    f.entries = { Legacy({ 310, 8, 0, 0 }) };
    Selector s = f.Make();
    CHECK(s.Poll(L"NR.310.9", 0)->status == Status::ExplicitMissing);
    CHECK(s.Current()->revision == 1);
    // the folder is added; nothing happens until the card asks for a rescan (no surprise switches)
    f.entries.push_back(Folder(L"NR.310.9"));
    CHECK(s.Poll(L"NR.310.9", 10000)->revision == 1);
    s.Rescan();
    const auto after = s.Poll(L"NR.310.9", 10001);
    CHECK(after->revision == 2 && after->Available() && after->id == L"NR.310.9");
    // a rescan that changes nothing keeps the revision
    s.Rescan();
    CHECK(s.Poll(L"NR.310.9", 10002)->revision == 2);
    // the folder disappears: the next rescan makes the runtime unavailable (a new revision: the card shows it)
    f.entries.pop_back();
    s.Rescan();
    const auto gone = s.Poll(L"NR.310.9", 10003);
    CHECK(gone->revision == 3 && !gone->Available());
    // two different missing names are two different states (the message names the request)
    s.Poll(L"NR.310.7", 20000);
    const auto other = s.Poll(L"NR.310.7", 20250);
    CHECK(other->revision == 4 && UnavailableReason(*other).find("NR.310.7") != std::string::npos);
}

static void AutomaticFollowsRescanOnly()
{
    Fake f;
    f.entries = { Legacy({ 310, 8, 0, 0 }) };
    Selector s = f.Make();
    CHECK(s.Poll(L"", 0)->legacy);
    f.entries.push_back(Folder(L"NR.310.9"));
    CHECK(s.Poll(L"", 50000)->legacy); // Automatic does not move by itself when a folder appears
    s.Rescan();
    CHECK(s.Poll(L"", 50001)->id == L"NR.310.9");
}

int main()
{
    Snapshots();
    FirstPollAndDebounce();
    RapidClicks();
    ClicksRestartTheTimer();
    ExplicitMissingAndRescan();
    AutomaticFollowsRescanOnly();
    if (failures == 0)
        std::puts("PASS: NR runtime selection");
    return failures == 0 ? 0 : 1;
}
