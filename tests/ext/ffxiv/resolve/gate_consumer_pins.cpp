// Consumer conversion, pinned on the sources: every reader of the compile-time table (kCurrentBuild), of the exact-build match (MatchBuild,
// MatchCurrentProcess) and every consumer of the gate is one of the files listed here; a new reader, or a converted consumer that goes back to the
// table, fails this test. (The behaviour of the converted code is covered by gate_rules, gate_real_exe, jitter_source_constants
// and chara_accessor; the product build compiles the files the unit tests cannot include.)
//
// The decisions of the startup and of the game selector are PURE functions with behaviour tests (startup_flow, gate_rules, gate_keys):
// ReadSettings, MakeGateInput, DecideStartup / RunStartup, ReadModulePath, BuildFor. What no unit test can compile is the product line that CALLS them
// with the right argument (the feature a consumer asks for, the key a selector reads, the stamp and size match). Those lines are pinned here PER
// FUNCTION, from the comment-stripped body of that function and nowhere else in the file: a token that merely occurs somewhere in the file (what the
// first version of this test checked) does not notice a consumer that asks for another consumer's feature.
#include "../chara/check.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
std::string ReadFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// code only: line and block comments and the contents of string literals removed (a comment may name the old API on purpose)
std::string Code(const std::string& text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size();)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
        }
        else if (text.compare(i, 2, "/*") == 0)
        {
            const std::size_t e = text.find("*/", i + 2);
            i = e == std::string::npos ? text.size() : e + 2;
        }
        else if (text[i] == '"')
        {
            out += '"';
            for (++i; i < text.size() && text[i] != '"'; ++i)
                if (text[i] == '\\')
                    ++i;
            if (i < text.size())
                ++i;
            out += '"';
        }
        else
            out += text[i++];
    }
    return out;
}

bool Has(const std::string& code, const std::string& token) { return code.find(token) != std::string::npos; }

// whitespace collapsed to single spaces, trimmed: a body or a line compares independent of the formatting
std::string Norm(const std::string& s)
{
    std::string out;
    bool space = false;
    for (const char c : s)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            space = !out.empty();
        else
        {
            if (space)
                out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

// The brace-balanced body (without the braces) of the function whose declaration contains `signature`, from comment-stripped code. Empty when not found.
std::string BodyOf(const std::string& code, const std::string& signature)
{
    const std::size_t at = code.find(signature);
    if (at == std::string::npos)
        return {};
    const std::size_t open = code.find('{', at + signature.size());
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return Norm(code.substr(open + 1, i - open - 1));
    }
    return {};
}

// every distinct `Feature::Name` token of a text
std::set<std::string> FeatureTokens(const std::string& text)
{
    std::set<std::string> out;
    for (std::size_t at = 0; (at = text.find("Feature::", at)) != std::string::npos;)
    {
        std::size_t end = at + 9;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            ++end;
        out.insert(text.substr(at + 9, end - at - 9));
        at = end;
    }
    return out;
}

// a function body must be exactly `expected` and name only the features in `features`
void PinFunction(const std::string& code, const char* file, const char* signature, const std::string& expected, const std::set<std::string>& features)
{
    const std::string body = BodyOf(code, signature);
    if (body != Norm(expected))
        std::printf("FAIL: %s: %s\n    body:     %s\n    expected: %s\n", file, signature, body.c_str(), Norm(expected).c_str());
    CHECK(!body.empty() && body == Norm(expected));
    if (FeatureTokens(body) != features)
        std::printf("FAIL: %s: %s names the wrong feature set\n", file, signature);
    CHECK(FeatureTokens(body) == features);
}

// a function body must contain every line (normalised) and no other `Feature::` token than `features`
void PinLines(const std::string& code, const char* file, const char* signature, const std::vector<std::string>& lines, const std::set<std::string>& features)
{
    const std::string body = BodyOf(code, signature);
    CHECK(!body.empty());
    for (const std::string& l : lines)
    {
        if (body.find(Norm(l)) == std::string::npos)
            std::printf("FAIL: %s: %s does not contain: %s\n", file, signature, Norm(l).c_str());
        CHECK(body.find(Norm(l)) != std::string::npos);
    }
    if (FeatureTokens(body) != features)
        std::printf("FAIL: %s: %s names the wrong feature set\n", file, signature);
    CHECK(FeatureTokens(body) == features);
}

std::string Rel(const fs::path& base, const fs::path& p)
{
    std::string s = fs::relative(p, base).generic_string();
    return s;
}
} // namespace

int main(int argc, char** argv)
{
    const fs::path repo = argc > 1 ? argv[1] : ".";
    const fs::path ext = repo / "OptiScaler" / "ext";

    // ---- 1. who may read the table or the exact-build match: exactly these files
    const std::set<std::string> mayRead = {
        "ffxiv/GameBuild.h",                  // the definitions
        "ffxiv/GameBuild.cpp",
        "ffxiv/ActiveBuild.h",                // ActiveBuild() before the startup: the table
        "ffxiv/resolve/ResolveStartup.cpp",   // the only product caller of the resolver: asks the exact match for the gate input (MatchCurrentProcess)
        "ffxiv/resolve/StartupDecision.h",    // RunStartup: offers the table to the resolver, publishes it for the rollback key and the legacy fallback
        "game/Game.cpp",                      // Group 1 under the rollback key: the exact stamp and size
        "ffxiv/resolve/ResolveTypes.h",       // the resolver's own docs / types
        "ffxiv/resolve/Entry.h",
        "ffxiv/resolve/AnchorsJitter.h",
    };
    std::set<std::string> found;
    for (const auto& entry : fs::recursive_directory_iterator(ext))
    {
        const std::string e = entry.path().extension().string();
        if (!entry.is_regular_file() || (e != ".cpp" && e != ".h"))
            continue;
        const std::string code = Code(ReadFile(entry.path()));
        if (Has(code, "kCurrentBuild") || Has(code, "MatchCurrentProcess") || Has(code, "MatchBuild"))
            found.insert(Rel(ext, entry.path()));
    }
    // (the comment-only mentions of the resolve headers are stripped by Code(): they are not in `found`)
    for (const std::string& f : found)
    {
        if (mayRead.count(f) == 0)
            std::printf("FAIL: %s reads the compile-time table or the exact-build match directly\n", f.c_str());
        CHECK(mayRead.count(f) != 0);
    }
    // the files that must still read it (a pin that the allow-list is not stale)
    for (const char* f : { "ffxiv/GameBuild.h", "ffxiv/GameBuild.cpp", "ffxiv/ActiveBuild.h", "ffxiv/resolve/ResolveStartup.cpp", "ffxiv/resolve/StartupDecision.h",
                           "game/Game.cpp" })
        CHECK(found.count(f) == 1);

    // ---- 2. every converted consumer asks the gate for its own feature and reads ActiveBuild()
    struct Consumer
    {
        const char* file;
        std::vector<const char*> tokens;
    };
    const std::vector<Consumer> consumers = {
        { "ffxiv/quality/FfxivNativeQualityHooks.h",
          { "ext::ffxiv::BuildFor(feature)", "ActiveStarted()", "SettingsSnapshotBytes", "kMaxSettingsSnapshotBytes", "DlaaRequiredFromSnapshot", "SettingsOffsetsInBounds",
            "Outcome::Transient" } },
        { "ffxiv/jitter/FfxivJitterSourceHooks.cpp",
          { "ext::ffxiv::BuildFor(", "ActiveGate()", "ActiveStarted()", "Outcome::Transient", "strengthFloorBits", "ndcSignMaskBits", "VerifyAnchors" } },
        { "ffxiv/resolve/ResolveStartup.cpp",
          { "ReadSettings(ext::Cfg().ffxiv)", "MakeDefaultSink()", "ResolveOnce(", "LiveImage::OpenMainModule()", "PublishActive(", "call_once", "RunStartup(", "ReadModulePath(" } },
        { "ffxiv/resolve/StartupDecision.h",
          { "settings.forceUnknownBuild ? nullptr : &kCurrentBuild", "MakeGateInput(", "DecideStartup(", "FailedStartup(", "env.publish(" } },
        { "ffxiv/jitter/FfxivJitterProbe.cpp", { "ext::ffxiv::BuildFor(" } },
        { "ffxiv/chara/CharaTable.cpp", { "ext::ffxiv::BuildFor(", "ActiveGate()", "ActiveStarted()" } },
    };
    for (const Consumer& c : consumers)
    {
        const std::string code = Code(ReadFile(ext / c.file));
        CHECK(!code.empty());
        for (const char* t : c.tokens)
        {
            if (!Has(code, t))
                std::printf("FAIL: %s does not contain %s\n", c.file, t);
            CHECK(Has(code, t));
        }
    }

    // ---- 2b. PER FUNCTION: every consumer asks for ITS OWN feature, through the one shared accessor (a file-wide token cannot tell)
    {
        const std::string quality = Code(ReadFile(ext / "ffxiv/quality/FfxivNativeQualityHooks.h"));
        PinFunction(quality, "quality", "inline const NativeQualityBuild* BuildFor(ext::ffxiv::resolve::Feature feature)",
                    "const GameBuild* build = ext::ffxiv::BuildFor(feature); return build != nullptr ? &build->nativeQuality : nullptr;", {});
        PinFunction(quality, "quality", "inline const NativeQualityBuild* Build()", "return BuildFor(ext::ffxiv::resolve::Feature::QualityRead);", { "QualityRead" });
        PinFunction(quality, "quality", "inline const NativeQualityBuild* HookBuild()", "return BuildFor(ext::ffxiv::resolve::Feature::QualityHooks);", { "QualityHooks" });
        // the INSTALL gate of the two detours asks for the hooks, not for the settings read path (a swap would install the detours whenever only the
        // DLAA read passes)
        const std::string attempt = BodyOf(quality, "inline retry::StepResult Attempt()");
        CHECK(Has(attempt, "const auto* build = HookBuild();"));
        std::string rest = attempt;
        for (std::size_t at; (at = rest.find("HookBuild()")) != std::string::npos;)
            rest.erase(at, 11);
        CHECK(!Has(rest, "Build()") && FeatureTokens(attempt).empty());
        const std::string jitter = Code(ReadFile(ext / "ffxiv/jitter/FfxivJitterSourceHooks.cpp"));
        PinFunction(jitter, "jitter source", "const GameBuild* Matched()", "return ext::ffxiv::BuildFor(ext::ffxiv::resolve::Feature::JitterSource);", { "JitterSource" });
        const std::string probe = Code(ReadFile(ext / "ffxiv/jitter/FfxivJitterProbe.cpp"));
        PinFunction(probe, "jitter probe", "const GameBuild* Build()", "return ext::ffxiv::BuildFor(ext::ffxiv::resolve::Feature::JitterProbe);", { "JitterProbe" });
        const std::string chara = Code(ReadFile(ext / "ffxiv/chara/CharaTable.cpp"));
        PinLines(chara, "chara", "Resolution DoResolve()",
                 { "const GameBuild* build = ext::ffxiv::BuildFor(ext::ffxiv::resolve::Feature::CharaViewAccessor);", "if (build == nullptr)",
                   "ext::ffxiv::ActiveGate().Reason(ext::ffxiv::resolve::Feature::CharaViewAccessor)" },
                 { "CharaViewAccessor" });
    }

    // ---- 2c. PER FUNCTION: the game selector and the startup shell pass the right values to the pure functions (startup_flow / gate_keys test those)
    {
        const std::string game = Code(ReadFile(ext / "game/Game.cpp"));
        PinLines(game, "Game.cpp", "const Game& SelectGame()",
                 { "const std::wstring exePath = ext::ffxiv::resolve::ReadModulePath([](wchar_t* buffer, std::uint32_t size) { return GetModuleFileNameW(nullptr, buffer, size); });",
                   "const bool exactBuild = ext::ffxiv::MatchBuild(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage) != nullptr;",
                   "const ext::ffxiv::resolve::GateInput gateInput = ext::ffxiv::resolve::MakeGateInput(exePath, exactBuild);",
                   "const ext::ffxiv::resolve::GateSettings settings = ext::ffxiv::resolve::ReadSettings(ext::Cfg().ffxiv);",
                   "return ext::ffxiv::resolve::DecideGroup1(f, gateInput.exeIsFfxiv, gateInput.exactKnownBuild, settings).enabled;",
                   "if (!group1(Feature::Lighting)) return none;",
                   "ffxiv.hdr = group1(Feature::SceneHdr) ? &ext::hdr::SceneProvider() : nullptr;",
                   "ffxiv.lighting = group1(Feature::Lighting) ? &ext::ffxiv::lighting::Provider() : nullptr;",
                   "ffxiv.geometry = group1(Feature::Geometry) ? &ext::ffxiv::geometry::Provider() : nullptr;",
                   "ffxiv.fgMarkers = group1(Feature::Markers) ? &ext::ffxiv::markers::Provider() : nullptr;",
                   "ffxiv.hudless = group1(Feature::UiHudless) ? &ext::ffxiv::ui::Provider() : nullptr;",
                   "ffxiv.ngx = group1(Feature::QuirkIsolatesShutdown) ? &quirks : nullptr;",
                   "ffxiv.nrQuirks = group1(Feature::QuirkBridgedNativeDx11) ? &quirks : nullptr;" },
                 { "Lighting", "SceneHdr", "Geometry", "Markers", "UiHudless", "QuirkIsolatesShutdown", "QuirkBridgedNativeDx11" });
        // F5: no exe path is read into a MAX_PATH buffer any more
        CHECK(!Has(game, "MAX_PATH"));
        const std::string startup = Code(ReadFile(ext / "ffxiv/resolve/ResolveStartup.cpp"));
        CHECK(!Has(startup, "MAX_PATH"));
        PinLines(startup, "ResolveStartup.cpp", "StartupEnv MakeProductEnv()",
                 { "env.readSettings = [] { return ReadSettings(ext::Cfg().ffxiv); };",
                   "env.exePath = [] { return ReadModulePath([](wchar_t* buffer, std::uint32_t size) { return GetModuleFileNameW(nullptr, buffer, size); }); };",
                   "env.stampSizeMatchesKnown = [] { return MatchCurrentProcess() != nullptr; };",
                   "env.registry = []() -> const Registry& { return ProductRegistry(); };",
                   "options.knownTable = table;", "options.sink = MakeDefaultSink();", "const auto image = LiveImage::OpenMainModule();",
                   "ResolveOnce(*image, ProductRegistry(), options);",
                   "env.publish = [](const GameBuild& build, const GateReport& gate) { PublishActive(build, gate); };" },
                 {});
        const std::string once = BodyOf(startup, "void StartOnce()");
        CHECK(Has(once, "RunStartup(env);") && Has(once, "static const StartupEnv env = MakeProductEnv();"));
    }

    // ---- 2d. the published state is read ONCE: BuildFor(feature) takes the gate and the build from the same state. ActiveGate() and ActiveBuild() are two
    // separate acquire loads, and the order in which two function arguments are evaluated is unspecified, so BuildFor(ActiveGate(), ActiveBuild(), f)
    // could pair the compile-time table with a gate published a moment later. (The behaviour of BuildForState is a gate_rules test.)
    {
        const std::string active = Code(ReadFile(ext / "ffxiv/ActiveBuild.h"));
        const char* const oneState = "inline const GameBuild* BuildFor(resolve::Feature feature)";
        PinFunction(active, "ActiveBuild.h", oneState, "return BuildForState(detail::g_active.load(std::memory_order_acquire), feature);", {});
        PinFunction(active, "ActiveBuild.h", "inline const GameBuild* BuildForState(const detail::ActiveState* state, resolve::Feature feature)",
                    "return state != nullptr ? BuildFor(state->gate, state->build, feature) : nullptr;", {});
        const std::string body = BodyOf(active, oneState);
        std::size_t loads = 0;
        for (std::size_t at = 0; (at = body.find("g_active.load", at)) != std::string::npos; at += 13)
            ++loads;
        if (loads != 1 || Has(body, "ActiveGate()") || Has(body, "ActiveBuild()"))
            std::printf("FAIL: ActiveBuild.h: BuildFor(feature) must load g_active exactly once and not call ActiveGate() / ActiveBuild(): %s\n", body.c_str());
        CHECK(loads == 1 && !Has(body, "ActiveGate()") && !Has(body, "ActiveBuild()"));
    }

    // ---- 3. the startup: called once, at the top of the first device hook, before anything installs a Detour; never from DllMain
    const std::string seams = Code(ReadFile(ext / "Seams.cpp"));
    const std::size_t hook = seams.find("void OnD3D11DeviceHooked(ID3D11Device* device)");
    const std::size_t start = seams.find("StartOnce()");
    const std::size_t lightingInstall = seams.find("lighting::OnDeviceHooked(device)");
    CHECK(hook != std::string::npos && start != std::string::npos && lightingInstall != std::string::npos);
    CHECK(hook < start && start < lightingInstall);
    CHECK(seams.find("StartOnce()", start + 1) == std::string::npos); // exactly one call
    int callSites = 0;
    for (const auto& entry : fs::recursive_directory_iterator(repo / "OptiScaler"))
    {
        const std::string e = entry.path().extension().string();
        if (!entry.is_regular_file() || (e != ".cpp" && e != ".h"))
            continue;
        const std::string code = Code(ReadFile(entry.path()));
        std::size_t at = 0;
        while ((at = code.find("StartOnce()", at)) != std::string::npos)
        {
            const std::string name = entry.path().filename().string();
            if (name != "ResolveStartup.cpp" && name != "ResolveStartup.h" && name != "Seams.cpp")
                std::printf("FAIL: StartOnce() is referenced from %s\n", name.c_str());
            CHECK(name == "ResolveStartup.cpp" || name == "ResolveStartup.h" || name == "Seams.cpp");
            ++callSites;
            at += 11;
        }
    }
    CHECK(callSites >= 2);
    CHECK(!Has(Code(ReadFile(repo / "OptiScaler" / "dllmain.cpp")), "StartOnce"));

    // ---- 4. Group 1 on the exe name; the old exact rule only under the rollback key
    const std::string game = Code(ReadFile(ext / "game" / "Game.cpp"));
    CHECK(Has(game, "MakeGateInput") && Has(game, "ReadSettings") && Has(game, "DecideGroup1")); // the name check and the key mapping are pure functions now
    CHECK(Has(game, "return ext::game::Current()") == false); // (the predicates live in the menu files)
    // the predicate text the five menu tests count is unchanged
    for (const char* f : { "ffxiv/jitter/FfxivJitterMenu.cpp", "ffxiv/FfxivAdvancedMenu.cpp", "ffxiv/chara/CharaMenu.cpp", "window/WindowMenu.cpp" })
        CHECK(ReadFile(ext / f).find("return ext::game::Current().lighting != nullptr;") != std::string::npos);

    // ---- 5. the constants that stay compile-time are Constant rows of the field table (never resolved)
    const std::string table = ReadFile(ext / "ffxiv" / "resolve" / "FieldTable.h");
    for (const char* row : { "FFXIV_U32(jitterSource, JitterSourceBuild, strengthFloorBits, Constant)", "FFXIV_U32(jitterSource, JitterSourceBuild, ndcSignMaskBits, Constant)" })
        CHECK(table.find(row) != std::string::npos);
    return TEST_RESULT("gate_consumer_pins");
}
