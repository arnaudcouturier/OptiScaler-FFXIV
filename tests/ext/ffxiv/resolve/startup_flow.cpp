// The startup decision (ext/ffxiv/resolve/StartupDecision.h) with a fake environment: every decision of ResolveStartup.cpp that used to be a text pin is
// a behaviour here. Always on, no executable, no Windows API.
//
//   * not the game, the rollback key: nothing is opened, scanned or built; the compile-time table is the published build (as before P3);
//   * a resolve that worked: the PUBLISHED build is the resolved one (a startup that published the table would silently end self-healing), the
//     table is offered to the resolver unless FfxivForceUnknownBuild, the registry is built once and only when a resolve runs, one log, one publish;
//   * the resolver failed (image unreadable, a throwing startup, a cached exception, a scan abort): the exact known build falls back to the legacy
//     decision and the table; any other build is off and publishes ZeroedBuild(), never the table;
//   * the log and the publish can fail without changing the decision; the keys map to their own flags; the exe path is read without the MAX_PATH cut.
#include "gate_fixture.h"

#include "ext/ffxiv/resolve/FieldTable.h"
#include "ext/ffxiv/resolve/ModulePath.h"
#include "ext/ffxiv/resolve/StartupDecision.h"

#include <cstring>
#include <stdexcept>
#include <string>

using namespace rt;
using namespace ext::ffxiv;
using namespace gf;

namespace
{
// every row of the field table, byte for byte, plus the stamp and size
bool SameBuild(const GameBuild& a, const GameBuild& b)
{
    for (const FieldRow& row : kFieldTable)
    {
        if (std::memcmp(FieldBytes(a, row), FieldBytes(b, row), row.size) != 0)
            return false;
    }
    return a.timeDateStamp == b.timeDateStamp && a.sizeOfImage == b.sizeOfImage;
}

// no address of the executable is claimed: every Rva and Guard row is zero
bool NoAddresses(const GameBuild& b)
{
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Rva && row.kind != FieldKind::Guard)
            continue;
        const std::uint8_t* p = FieldBytes(b, row);
        for (std::uint32_t i = 0; i < row.size; ++i)
            if (p[i] != 0)
                return false;
    }
    return true;
}

const wchar_t* const kGamePath = L"C:\\Games\\FFXIV\\game\\ffxiv_dx11.exe";

// an exception type whose what() is the empty string (a custom type can do this; the standard ones do not)
struct SilentError : std::exception
{
    const char* what() const noexcept override { return ""; }
};
// an exception whose what() is a null pointer (a custom type may do this; the standard names no rule for it)
struct NullWhatError : std::exception
{
    const char* what() const noexcept override { return nullptr; }
};

struct Fake
{
    GateSettings settings;
    std::wstring path = kGamePath;
    bool match = true; // the running image has the table's stamp and size
    const ResolvedBuild* rb = nullptr;
    bool resolveThrows = false, resolveThrowsSilent = false, resolveThrowsNullWhat = false, resolveThrowsNotStd = false, logThrows = false, settingsThrow = false, publishThrows = false;
    double ms = 12.5;

    int settingsCalls = 0, pathCalls = 0, matchCalls = 0, registryCalls = 0, resolveCalls = 0, logCalls = 0, publishCalls = 0;
    bool offeredSet = false;
    const GameBuild* offered = nullptr;
    GameBuild published;
    GateReport publishedGate;
    // the last log entry, copied (the entry holds pointers into RunStartup's locals)
    GateReport logGate;
    const ResolvedBuild* logRb = nullptr;
    const Registry* logRegistry = nullptr;
    std::uint32_t logStamp = 0, logSize = 0;
    double logMs = 0;
    std::string logExe, logFailure;

    StartupEnv Env()
    {
        StartupEnv e;
        e.readSettings = [this]
        {
            ++settingsCalls;
            if (settingsThrow)
                throw std::runtime_error("settings boom");
            return settings;
        };
        e.exePath = [this]
        {
            ++pathCalls;
            return path;
        };
        e.stampSizeMatchesKnown = [this]
        {
            ++matchCalls;
            return match;
        };
        e.registry = [this]() -> const Registry&
        {
            ++registryCalls;
            return Reg();
        };
        e.resolve = [this](const GameBuild* table, double* resolveMs) -> const ResolvedBuild*
        {
            ++resolveCalls;
            offeredSet = true;
            offered = table;
            if (resolveThrows)
                throw std::runtime_error("resolve boom");
            if (resolveThrowsSilent)
                throw SilentError();
            if (resolveThrowsNullWhat)
                throw NullWhatError();
            if (resolveThrowsNotStd)
                throw 42; // not derived from std::exception: only catch (...) sees it
            *resolveMs = ms;
            return rb;
        };
        e.log = [this](const StartupLog& entry)
        {
            ++logCalls;
            logGate = *entry.gate;
            logRb = entry.rb;
            logRegistry = entry.registry;
            logStamp = entry.stamp;
            logSize = entry.size;
            logMs = entry.resolveMs;
            logExe = entry.exe;
            logFailure = entry.failure;
            if (logThrows)
                throw std::runtime_error("log boom");
        };
        e.publish = [this](const GameBuild& build, const GateReport& gate)
        {
            ++publishCalls;
            published = build;
            publishedGate = gate;
            if (publishThrows)
                throw std::runtime_error("publish boom");
        };
        return e;
    }
    void Run()
    {
        const StartupEnv e = Env();
        RunStartup(e);
    }
};

bool SameDecisions(const GateReport& a, const GateReport& b)
{
    for (std::size_t i = 0; i < kFeatureCount; ++i)
        if (a.features[i].enabled != b.features[i].enabled)
            return false;
    return a.known == b.known && a.legacy == b.legacy && a.legacyFallback == b.legacyFallback;
}

// ---- the keys and the gate input ---------------------------------------------------------------------------------------------------------------

struct FakeOpt
{
    bool v = false;
    bool value_or_default() const { return v; }
};
struct FakeCfg
{
    FakeOpt legacyGameBuild, unknownBuildHooks, forceUnknownBuild;
};

void TestReadSettings()
{
    CHECK(!ReadSettings(FakeCfg {}).legacyGameBuild && !ReadSettings(FakeCfg {}).unknownBuildHooks && !ReadSettings(FakeCfg {}).forceUnknownBuild);
    FakeCfg a;
    a.legacyGameBuild.v = true;
    GateSettings s = ReadSettings(a);
    CHECK(s.legacyGameBuild && !s.unknownBuildHooks && !s.forceUnknownBuild);
    FakeCfg b;
    b.unknownBuildHooks.v = true;
    s = ReadSettings(b);
    CHECK(!s.legacyGameBuild && s.unknownBuildHooks && !s.forceUnknownBuild);
    FakeCfg c;
    c.forceUnknownBuild.v = true;
    s = ReadSettings(c);
    CHECK(!s.legacyGameBuild && !s.unknownBuildHooks && s.forceUnknownBuild);
}

void TestMakeGateInput()
{
    for (const bool match : { true, false })
    {
        const GateInput game = MakeGateInput(std::wstring_view(kGamePath), match);
        CHECK(game.exeIsFfxiv && game.exactKnownBuild == match);
        const GateInput narrow = MakeGateInput(std::string_view("D:\\x\\FFXIV_DX11.EXE"), match);
        CHECK(narrow.exeIsFfxiv && narrow.exactKnownBuild == match);
        // The exact build is identified by its PE stamp and size, whatever the file is called (a copy under another name, a per-exe driver profile, a
        // custom launcher is the game). The name only decides on a build the table does not know: another executable never counts there.
        const GateInput other = MakeGateInput(std::wstring_view(L"C:\\Windows\\notepad.exe"), match);
        CHECK(other.exeIsFfxiv == match && other.exactKnownBuild == match);
        const GateInput renamed = MakeGateInput(std::wstring_view(L"D:\\Games\\ffxiv_dx11_copy.exe"), match);
        CHECK(renamed.exeIsFfxiv == match && renamed.exactKnownBuild == match);
        const GateInput renamedNarrow = MakeGateInput(std::string_view("D:\\Games\\launcher.exe"), match);
        CHECK(renamedNarrow.exeIsFfxiv == match && renamedNarrow.exactKnownBuild == match);
        // the path could not be read at all (empty) or is a bare name: the same rule, no special case
        const GateInput empty = MakeGateInput(std::wstring_view(L""), match);
        CHECK(empty.exeIsFfxiv == match && empty.exactKnownBuild == match);
        const GateInput emptyNarrow = MakeGateInput(std::string_view(""), match);
        CHECK(emptyNarrow.exeIsFfxiv == match && emptyNarrow.exactKnownBuild == match);
        const GateInput readButOther = MakeGateInput(std::wstring_view(L"x"), match);
        CHECK(readButOther.exeIsFfxiv == match && readButOther.exactKnownBuild == match);
        CHECK(game.rb == nullptr && game.registry == nullptr && game.resolverFailure.empty());
    }
}

// F5: the exe path is read without the MAX_PATH cut
void TestModulePath()
{
    // GetModuleFileNameW semantics: copies min(length, size) characters, returns that count; a truncated copy returns `size`
    auto reader = [](const std::wstring& full, int* calls)
    {
        return [full, calls](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
        {
            if (calls != nullptr)
                ++*calls;
            const std::uint32_t n = static_cast<std::uint32_t>(full.size() < size ? full.size() : size);
            for (std::uint32_t i = 0; i < n; ++i)
                buffer[i] = full[i];
            if (n < size)
                buffer[n] = 0;
            return n;
        };
    };
    auto pathOf = [](std::size_t chars)
    {
        const std::wstring tail = L"\\ffxiv_dx11.exe";
        std::wstring dir = L"C:";
        while (dir.size() + tail.size() < chars)
            dir += L"\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        dir.resize(chars - tail.size());
        return dir + tail;
    };
    for (const std::size_t chars : { std::size_t(20), std::size_t(100), std::size_t(259), std::size_t(260), std::size_t(261), std::size_t(300), std::size_t(1000),
                                     std::size_t(5000), std::size_t(32767) })
    {
        const std::wstring full = pathOf(chars);
        REQUIRE(full.size() == chars);
        int calls = 0;
        const std::wstring got = ReadModulePath(reader(full, &calls));
        if (got != full)
            std::printf("FAIL: a path of %zu characters was read as %zu characters\n", chars, got.size());
        CHECK(got == full);
        CHECK(IsFfxivExeName(std::wstring_view(got)));
        CHECK(calls <= 8);
        // a path that fits the first buffer is read in one call
        if (chars < 260)
            CHECK(calls == 1);
    }
    // longer than a Windows path can be, and a failure: empty (not "not ffxiv"), so the callers know it was not read
    CHECK(ReadModulePath(reader(pathOf(40000), nullptr)).empty());
    CHECK(ReadModulePath([](wchar_t*, std::uint32_t) -> std::uint32_t { return 0; }).empty());
    // the old fixed-buffer behaviour is what this fixes: a MAX_PATH-limited read of a 300-character path is truncated
    {
        const std::wstring full = pathOf(300);
        wchar_t buf[260] {};
        const std::uint32_t n = reader(full, nullptr)(buf, 260);
        CHECK(n == 260 && !IsFfxivExeName(std::wstring_view(buf, n)));
    }
    CHECK(NarrowAscii(L"a\\b") == "a\\b" && NarrowAscii(L"\x00e9x") == "?x");
    CHECK(FileNameOf("C:\\a\\b\\ffxiv_dx11.exe") == "ffxiv_dx11.exe" && FileNameOf("/x/y.exe") == "y.exe" && FileNameOf("plain.exe") == "plain.exe");
}

// ---- not the game, the rollback key ------------------------------------------------------------------------------------------------------------

void TestNotTheGameAndRollback()
{
    // not the game: nothing opened, scanned or built, nothing logged, the table published (as before P3), everything off
    for (const GateSettings& s : { Settings(), Settings(false, true, true), Settings(true) })
    {
        Fake f;
        f.settings = s;
        f.path = L"C:\\Windows\\notepad.exe";
        f.match = false; // another executable: not the table's stamp and size either (with them it would be the game under another name)
        f.Run();
        CHECK(f.resolveCalls == 0 && f.registryCalls == 0 && f.logCalls == 0 && f.publishCalls == 1);
        CHECK(AllGroup1(f.publishedGate, false) && AllGroup2(f.publishedGate, false));
        CHECK(SameBuild(f.published, kCurrentBuild));
    }
    // the rollback key on the exact known build: no image, no registry; the table is the build; everything on; one log line block
    {
        Fake f;
        f.settings = Settings(true);
        f.Run();
        CHECK(f.resolveCalls == 0 && f.registryCalls == 0 && f.logCalls == 1 && f.publishCalls == 1);
        CHECK(f.publishedGate.legacy && f.publishedGate.known && !f.publishedGate.legacyFallback && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true));
        CHECK(SameBuild(f.published, kCurrentBuild));
        CHECK(f.logRb == nullptr && f.logStamp == kCurrentBuild.timeDateStamp && f.logSize == kCurrentBuild.sizeOfImage && f.logExe == "ffxiv_dx11.exe");
        CHECK(f.logRegistry != nullptr && f.logRegistry != &Reg()); // the empty registry, not the product one
    }
    // the rollback key on another build: nothing runs, the other two keys are ignored
    {
        Fake f;
        f.settings = Settings(true, true, true);
        f.match = false;
        f.Run();
        CHECK(f.resolveCalls == 0 && f.registryCalls == 0 && f.publishCalls == 1);
        CHECK(f.publishedGate.legacy && !f.publishedGate.known && AllGroup1(f.publishedGate, false) && AllGroup2(f.publishedGate, false));
        CHECK(SameBuild(f.published, kCurrentBuild) && f.logStamp == 0);
    }
}

// The exact build is identified by its PE stamp and size: a copy of the game under another name is the game, with the default keys and under the
// rollback key (which is exactly the old rule: the stamp and size, nothing else; the old selector never read the name).
void TestRenamedExecutable()
{
    const wchar_t* const renamedPath = L"D:\\Games\\ffxiv_dx11_copy.exe";
    ResolvedBuild rb = AllFound(true);
    // default keys, the exact stamp and size: the resolver runs, Group 1 and Group 2 are on, the exe name in the log is the file's own
    {
        Fake f;
        f.path = renamedPath;
        f.rb = &rb;
        f.Run();
        CHECK(f.resolveCalls == 1 && f.registryCalls == 1 && f.logCalls == 1 && f.publishCalls == 1);
        CHECK(f.publishedGate.exeIsFfxiv && f.publishedGate.known && !f.publishedGate.legacy && !f.publishedGate.legacyFallback);
        CHECK(AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true));
        CHECK(f.logExe == "ffxiv_dx11_copy.exe");
        // the game selector (Game.cpp) asks Group 1 with the same two flags
        const GateInput in = MakeGateInput(std::wstring_view(renamedPath), true);
        CHECK(DecideGroup1(Feature::Lighting, in.exeIsFfxiv, in.exactKnownBuild, Settings()).enabled);
    }
    // the rollback key, the exact stamp and size: exactly the pre-P3 path: nothing opened or scanned, the table published, everything on
    {
        Fake f;
        f.path = renamedPath;
        f.settings = Settings(true);
        f.Run();
        CHECK(f.resolveCalls == 0 && f.registryCalls == 0 && f.publishCalls == 1);
        CHECK(f.publishedGate.legacy && f.publishedGate.known && !f.publishedGate.legacyFallback);
        CHECK(AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true));
        CHECK(SameBuild(f.published, kCurrentBuild));
        const GateInput in = MakeGateInput(std::wstring_view(renamedPath), true);
        CHECK(DecideGroup1(Feature::Lighting, in.exeIsFfxiv, in.exactKnownBuild, Settings(true)).enabled);
    }
    // the rollback key, another stamp or size: nothing runs, whatever the name (as before P3)
    for (const wchar_t* path : { renamedPath, kGamePath })
    {
        Fake f;
        f.path = path;
        f.match = false;
        f.settings = Settings(true);
        f.Run();
        CHECK(f.resolveCalls == 0 && f.registryCalls == 0 && f.publishCalls == 1);
        CHECK(f.publishedGate.legacy && !f.publishedGate.known && AllGroup1(f.publishedGate, false) && AllGroup2(f.publishedGate, false));
        const GateInput in = MakeGateInput(std::wstring_view(path), false);
        CHECK(!DecideGroup1(Feature::Lighting, in.exeIsFfxiv, in.exactKnownBuild, Settings(true)).enabled);
    }
    // another stamp or size under another name, default keys: not the game, nothing runs
    {
        Fake f;
        f.path = renamedPath;
        f.match = false;
        f.Run();
        CHECK(f.resolveCalls == 0 && f.registryCalls == 0 && f.logCalls == 0 && f.publishCalls == 1);
        CHECK(!f.publishedGate.exeIsFfxiv && AllGroup1(f.publishedGate, false) && AllGroup2(f.publishedGate, false));
    }
}

// ---- a resolve that worked ---------------------------------------------------------------------------------------------------------------------

void TestKnownBuild()
{
    ResolvedBuild rb = AllFound(true);
    rb.build.jitterSource.generatorRva = 0x123456; // a value no table row has: proves WHICH build is published
    Fake f;
    f.rb = &rb;
    f.Run();
    CHECK(f.settingsCalls == 1 && f.pathCalls == 1 && f.matchCalls == 1 && f.registryCalls == 1 && f.resolveCalls == 1 && f.logCalls == 1 && f.publishCalls == 1);
    CHECK(f.offeredSet && f.offered == &kCurrentBuild); // the table is offered
    CHECK(f.published.jitterSource.generatorRva == 0x123456 && SameBuild(f.published, rb.build)); // the RESOLVED build, not the table
    CHECK(!SameBuild(f.published, kCurrentBuild));
    CHECK(AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true) && f.publishedGate.known && !f.publishedGate.legacy && !f.publishedGate.legacyFallback);
    CHECK(SameDecisions(f.publishedGate, f.logGate));
    CHECK(f.logRb == &rb && f.logRegistry == &Reg() && f.logMs == 12.5 && f.logStamp == rb.timeDateStamp && f.logSize == rb.sizeOfImage);
    CHECK(f.logExe == "ffxiv_dx11.exe" && f.logFailure.empty());
    // the key to allow an unknown build changes nothing on the known one; the keys are read once
    Fake g;
    g.rb = &rb;
    g.settings = Settings(false, true);
    g.Run();
    CHECK(AllGroup2(g.publishedGate, true) && g.offered == &kCurrentBuild && g.settingsCalls == 1);
}

void TestUnknownBuildAndForcedUnknown()
{
    ResolvedBuild rb = AllFound(false);
    rb.build.companion.batch16Rva = 0x777777;
    // an unknown build, the key off: Group 1 on, Group 2 off; the published build is still the resolved one (consumers may read it, the gate says no)
    {
        Fake f;
        f.match = false;
        f.rb = &rb;
        f.Run();
        CHECK(f.offered == &kCurrentBuild && f.resolveCalls == 1 && f.publishCalls == 1);
        CHECK(AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, false) && !f.publishedGate.known);
        CHECK(f.published.companion.batch16Rva == 0x777777 && SameBuild(f.published, rb.build));
        CHECK(f.logStamp == rb.timeDateStamp);
    }
    // the key on: Group 2 on
    {
        Fake f;
        f.match = false;
        f.rb = &rb;
        f.settings = Settings(false, true);
        f.Run();
        CHECK(AllGroup2(f.publishedGate, true) && SameBuild(f.published, rb.build));
    }
    // the debug key: the table is NOT offered (the resolver runs on its bytes alone), Group 2 needs the unknown-build key as well
    for (const bool hooks : { false, true })
    {
        Fake f;
        f.rb = &rb;
        f.settings = Settings(false, hooks, true);
        f.Run();
        CHECK(f.resolveCalls == 1 && f.offeredSet && f.offered == nullptr);
        CHECK(f.publishedGate.forcedUnknown && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, hooks));
        CHECK(SameBuild(f.published, rb.build));
    }
}

// ---- the resolver failed -----------------------------------------------------------------------------------------------------------------------

void TestResolverFailureOnTheKnownBuild()
{
    // the image could not be opened (the resolve step returns nullptr)
    {
        Fake f;
        f.rb = nullptr;
        f.Run();
        CHECK(f.resolveCalls == 1 && f.registryCalls == 1 && f.logCalls == 1 && f.publishCalls == 1);
        CHECK(f.publishedGate.legacyFallback && f.publishedGate.known && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true));
        CHECK(SameBuild(f.published, kCurrentBuild)); // the table: the old byte guards decide
        CHECK(f.logRb == nullptr && f.logStamp == kCurrentBuild.timeDateStamp && f.logSize == kCurrentBuild.sizeOfImage);
        CHECK(f.logGate.legacyFallback && Contains(f.logGate.fallbackWhy, "image"));
    }
    // the startup threw: the same fallback, the text of the exception reaches the log
    {
        Fake f;
        f.resolveThrows = true;
        f.Run();
        CHECK(f.publishCalls == 1 && f.publishedGate.legacyFallback && AllGroup2(f.publishedGate, true) && SameBuild(f.published, kCurrentBuild));
        CHECK(Contains(f.logFailure, "resolve boom") && Contains(f.publishedGate.fallbackWhy, "resolve boom") && f.logRb == nullptr);
    }
    // a resolve result that carries a failure (cached exception, scan abort): the fallback, and the TABLE is published, not the half-empty result
    {
        ResolvedBuild exception = AllFound(true);
        exception.registryErrors.push_back("resolver exception: bad_alloc");
        exception.build = ZeroedBuild();
        ResolvedBuild scan = AllFound(true);
        scan.scanFailed = true;
        scan.scanError = "window unreadable";
        scan.build = ZeroedBuild();
        for (ResolvedBuild* r : { &exception, &scan })
        {
            Fake f;
            f.rb = r;
            f.Run();
            CHECK(f.publishedGate.legacyFallback && AllGroup2(f.publishedGate, true) && SameBuild(f.published, kCurrentBuild) && f.logRb == r);
        }
    }
    // the debug key asked for the resolver's own values: no fallback to the table, the build is zero
    {
        Fake f;
        f.settings = Settings(false, false, true);
        f.rb = nullptr;
        f.Run();
        CHECK(!f.publishedGate.legacyFallback && AllGroup2(f.publishedGate, false) && SameBuild(f.published, ZeroedBuild()) && NoAddresses(f.published));
    }
}

// F6: on any other build a failure publishes zeros, never the table's addresses
void TestResolverFailureOnAnUnknownBuild()
{
    // the image could not be opened
    {
        Fake f;
        f.match = false;
        f.settings = Settings(false, true);
        f.rb = nullptr;
        f.Run();
        CHECK(f.publishCalls == 1 && !f.publishedGate.legacyFallback && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, false));
        CHECK(SameBuild(f.published, ZeroedBuild()) && NoAddresses(f.published) && !SameBuild(f.published, kCurrentBuild));
        CHECK(f.logStamp == 0 && f.logSize == 0);
    }
    // the startup threw
    {
        Fake f;
        f.match = false;
        f.resolveThrows = true;
        f.Run();
        CHECK(f.publishCalls == 1 && AllGroup2(f.publishedGate, false) && SameBuild(f.published, ZeroedBuild()) && NoAddresses(f.published));
        CHECK(Contains(f.logFailure, "resolve boom"));
    }
    // a failed result that still holds addresses: not published
    {
        ResolvedBuild scan = AllFound(false);
        scan.scanFailed = true;
        scan.scanError = "window unreadable";
        scan.build.jitterSource.generatorRva = 0x123456;
        Fake f;
        f.match = false;
        f.settings = Settings(false, true);
        f.rb = &scan;
        f.Run();
        CHECK(AllGroup2(f.publishedGate, false) && SameBuild(f.published, ZeroedBuild()) && f.published.jitterSource.generatorRva == 0);
    }
    // the startup could not even read the settings: off, zeros, one publish
    {
        Fake f;
        f.settingsThrow = true;
        f.Run();
        CHECK(f.publishCalls == 1 && AllGroup2(f.publishedGate, false) && SameBuild(f.published, ZeroedBuild()) && f.resolveCalls == 0);
        CHECK(Contains(f.logFailure, "settings boom"));
    }
}

// An exception whose what() is empty is still a failure of the startup: the decision cannot depend on the text of the message.
void TestExceptionWithEmptyMessage()
{
    // the exact known build: the legacy fallback (the table, the old byte guards), the failure reaches the log with a text
    {
        Fake f;
        f.resolveThrowsSilent = true;
        f.Run();
        CHECK(f.resolveCalls == 1 && f.logCalls == 1 && f.publishCalls == 1);
        CHECK(f.publishedGate.started && f.publishedGate.legacyFallback && f.publishedGate.known && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true));
        CHECK(SameBuild(f.published, kCurrentBuild));
        CHECK(Contains(f.publishedGate.fallbackWhy, "startup exception"));
        CHECK(!f.logFailure.empty() && f.logRb == nullptr);
        CHECK(SameDecisions(f.publishedGate, f.logGate));
    }
    // any other build: Group 2 off, ZERO addresses (never the table, never a default GameBuild), the failure logged
    for (const bool hooks : { false, true })
    {
        Fake f;
        f.match = false;
        f.settings = Settings(false, hooks);
        f.resolveThrowsSilent = true;
        f.Run();
        CHECK(f.publishCalls == 1 && f.logCalls == 1 && f.publishedGate.started && !f.publishedGate.legacyFallback);
        CHECK(AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, false));
        CHECK(SameBuild(f.published, ZeroedBuild()) && NoAddresses(f.published) && !SameBuild(f.published, kCurrentBuild));
        CHECK(!f.logFailure.empty());
    }
    // the forced-unknown key asked for the resolver's own values: no fallback to the table
    {
        Fake f;
        f.settings = Settings(false, true, true);
        f.resolveThrowsSilent = true;
        f.Run();
        CHECK(!f.publishedGate.legacyFallback && AllGroup2(f.publishedGate, false) && SameBuild(f.published, ZeroedBuild()));
    }
}

// A startup that throws something that is not a std::exception, or an exception whose what() is a null pointer, fails the startup like any other
// exception: the fallback depends on the `threw` flag, and a null what() must not reach a std::string.
void TestExceptionOfAnyType()
{
    for (const int kind : { 0, 1 }) // 0: what() returns nullptr, 1: not derived from std::exception
    {
        // the exact known build: the legacy fallback, the failure reaches the log with a text
        {
            Fake f;
            f.resolveThrowsNullWhat = kind == 0;
            f.resolveThrowsNotStd = kind == 1;
            f.Run();
            CHECK(f.resolveCalls == 1 && f.logCalls == 1 && f.publishCalls == 1);
            CHECK(f.publishedGate.started && f.publishedGate.legacyFallback && f.publishedGate.known && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, true));
            CHECK(SameBuild(f.published, kCurrentBuild) && f.logRb == nullptr && !f.logFailure.empty());
        }
        // any other build: Group 2 off, ZERO addresses, the failure logged
        {
            Fake f;
            f.match = false;
            f.resolveThrowsNullWhat = kind == 0;
            f.resolveThrowsNotStd = kind == 1;
            f.Run();
            CHECK(f.publishCalls == 1 && f.logCalls == 1 && !f.publishedGate.legacyFallback && AllGroup1(f.publishedGate, true) && AllGroup2(f.publishedGate, false));
            CHECK(SameBuild(f.published, ZeroedBuild()) && NoAddresses(f.published) && !f.logFailure.empty());
        }
    }
}

// the log and the publish can fail without changing what is decided
void TestLogAndPublishFailuresDecideNothing()
{
    ResolvedBuild rb = AllFound(true);
    rb.build.jitterSource.generatorRva = 0x123456;
    Fake f;
    f.rb = &rb;
    f.logThrows = true;
    f.Run();
    CHECK(f.logCalls == 1 && f.publishCalls == 1 && AllGroup2(f.publishedGate, true) && !f.publishedGate.legacyFallback && SameBuild(f.published, rb.build));
    Fake g;
    g.rb = &rb;
    g.publishThrows = true;
    g.Run(); // must not throw
    CHECK(g.publishCalls == 1 && g.logCalls == 1);
}

// every path publishes exactly once
void TestEveryPathPublishesOnce()
{
    ResolvedBuild rb = AllFound(true);
    for (int mode = 0; mode < 7; ++mode)
    {
        Fake f;
        f.rb = mode == 3 ? nullptr : &rb;
        f.resolveThrows = mode == 4;
        f.settingsThrow = mode == 5;
        f.resolveThrowsSilent = mode == 6;
        f.settings = Settings(mode == 1);
        if (mode == 2)
        {
            f.path = L"x.exe";
            f.match = false; // not the game
        }
        f.Run();
        if (f.publishCalls != 1)
            std::printf("FAIL: mode %d published %d times\n", mode, f.publishCalls);
        CHECK(f.publishCalls == 1);
    }
}
} // namespace

int main()
{
    TestReadSettings();
    TestMakeGateInput();
    TestModulePath();
    TestNotTheGameAndRollback();
    TestRenamedExecutable();
    TestKnownBuild();
    TestUnknownBuildAndForcedUnknown();
    TestResolverFailureOnTheKnownBuild();
    TestResolverFailureOnAnUnknownBuild();
    TestExceptionWithEmptyMessage();
    TestExceptionOfAnyType();
    TestLogAndPublishFailuresDecideNothing();
    TestEveryPathPublishesOnce();
    return TEST_RESULT("startup_flow");
}
