// [DLSSG] UIRecomposition: the combo "UI recomposition" in HudlessMenu.cpp (Auto / On / Off) against the config.
//
// The bug this pins: the member was declared `CustomOptional<bool> uiRecomposition { false }`, so Off (false == the declared
// default) was written to the ini as `auto` by Save Settings, and the engine reads an unset key as ON (HudlessEngine.cpp
// QueryRecomposition): Off came back as On after a restart. The declared default has to be what the engine does with an
// unset key, so Off is stored as an explicit `false`.
//
// What runs: the real FgConfig.cpp binding, the real CustomOptional (sliced from OptiScaler\Config.h by the Prepare script)
// and a Binder copy of the two real steps (recomp_stubs\ext\core\ExtConfig.h); the test pins those two real lines by text.
// What is read as text: the combo's two mappings (HudlessMenu.cpp, ImGui cannot run here), the engine's fallback for an
// unset key and the declared default. A negative control repeats the round trip with the old default `false` and must
// lose Off, so the test proves it can see the bug.
#include "menu_scan.h"

#include "ext/core/ExtConfig.h"
#include "ext/fg/FgConfig.h"

#include <cstdio>
#include <optional>
#include <string>

namespace
{
int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

std::string g_repo;

// A product file with comments stripped and all blanks removed
std::string Code(const char* rel)
{
    return menuscan::Squash(menuscan::StripComments(menuscan::ReadFile(g_repo + "/" + rel)));
}

// The text after `from` up to (not including) the next `to`; "" when either is missing
std::string Between(const std::string& hay, const std::string& from, const std::string& to)
{
    const size_t a = hay.find(from);
    if (a == std::string::npos)
        return {};
    const size_t b = hay.find(to, a + from.size());
    return b == std::string::npos ? std::string {} : hay.substr(a + from.size(), b - a - from.size());
}

// The combo as HudlessMenu.cpp has it, mirrored from the two pinned expressions (the test fails when the text changes)
int ComboIndex(const std::optional<bool>& v)
{
    return !v.has_value() ? 0 : (v.value() ? 1 : 2);
}
std::optional<bool> ComboWrite(int choice)
{
    return choice == 0 ? std::optional<bool> {} : std::optional<bool>(choice == 1);
}

ext::config::Entry FindEntry(const ext::config::Binder& binder, const char* section, const char* key)
{
    for (const auto& e : binder.Table())
        if (std::string(e.section) == section && std::string(e.key) == key)
            return e;
    return {};
}

// What the engine does with an unset key, taken from its text; ok = the literal was found
bool EngineFallback(bool& ok)
{
    const std::string eng = Code("OptiScaler/ext/hudless/HudlessEngine.cpp");
    const std::string tail = Between(eng, "(configured.has_value()?configured.value():", ");");
    ok = tail == "true" || tail == "false";
    return tail == "true";
}

// The default the member is declared with, taken from FgConfig.h
bool DeclaredDefault(bool& ok)
{
    const std::string h = Code("OptiScaler/ext/fg/FgConfig.h");
    const std::string lit = Between(h, "CustomOptional<bool>uiRecomposition{", "}");
    ok = lit == "true" || lit == "false";
    return lit == "true";
}

// Save Settings after picking `choice`, then a restart: the value the engine sees; `written` = the ini text
bool EngineValueAfterRestart(int choice, bool engineFallback, std::string& written)
{
    ext::FgConfig session;
    ext::config::Binder binder;
    session.Bind(binder);
    session.uiRecomposition = ComboWrite(choice);
    written = FindEntry(binder, "DLSSG", "UIRecomposition").save(&session.uiRecomposition);

    ext::FgConfig restarted; // a new run: the field starts unset, then the ini value is loaded
    ext::config::Binder binder2;
    restarted.Bind(binder2);
    FindEntry(binder2, "DLSSG", "UIRecomposition").load(&restarted.uiRecomposition, written);
    const auto& m = restarted.uiRecomposition;
    return m.has_value() ? m.value() : engineFallback;
}

void RealStepsArePinned()
{
    // The Binder copy models these two real lines; the real bool parser and formatter are pinned by their bodies
    const std::string cfgH = Code("OptiScaler/ext/core/ExtConfig.h");
    CHECK(cfgH.find("ifconstexpr(std::is_same_v<T,bool>)out=FormatBool(f.value_for_config());") != std::string::npos);
    CHECK(cfgH.find("ifconstexpr(std::is_same_v<T,bool>)f.set_from_config(ParseBool(ReadRaw(ini,b.section,b.key,true)));") !=
          std::string::npos);
    const std::string cfgCpp = Code("OptiScaler/ext/core/ExtConfig.cpp");
    CHECK(cfgCpp.find("std::stringFormatBool(std::optional<bool>v){if(!v)return\"auto\";return*v?\"true\":\"false\";}") !=
          std::string::npos);
    CHECK(cfgCpp.find("if(raw==\"true\")returntrue;if(raw==\"false\")returnfalse;returnstd::nullopt;") != std::string::npos);
    CHECK(cfgCpp.find("if(lower==\"auto\")returnstd::nullopt;") != std::string::npos); // "auto" (any case) is unset
}

void ComboMappingIsPinned()
{
    const std::string menu = Code("OptiScaler/ext/hudless/HudlessMenu.cpp");
    CHECK(menu.find("auto&recompositionSetting=ext::Cfg().fg.uiRecomposition;") != std::string::npos);
    CHECK(menu.find("intrecomposition=!recompositionSetting.has_value()?0:(recompositionSetting.value()?1:2);") !=
          std::string::npos);
    CHECK(menu.find("recompositionSetting=recomposition==0?std::optional<bool>{}:std::optional<bool>(recomposition==1);") !=
          std::string::npos);
    CHECK(menu.find("ImGui::Combo(\"UIrecomposition\",&recomposition,\"Auto(on)\\0On\\0Off\\0\")") != std::string::npos);
}

void DeclaredDefaultEqualsTheEngineFallback()
{
    bool okE = false, okD = false;
    const bool engine = EngineFallback(okE);
    const bool declared = DeclaredDefault(okD);
    CHECK(okE && okD); // both literals found
    CHECK(declared == engine);

    ext::FgConfig cfg;
    CHECK(cfg.uiRecomposition.value_or_default() == engine); // the real object agrees with the text
    CHECK(!cfg.uiRecomposition.has_value());                  // unset until the ini says otherwise
}

void AutoOnOffSurviveSaveAndRestart()
{
    bool ok = false;
    const bool fallback = EngineFallback(ok);
    CHECK(ok);

    struct Row
    {
        int choice;
        const char* name;
        bool wantedAfterRestart;
    };
    for (const Row& r : { Row { 0, "Auto", fallback }, Row { 1, "On", true }, Row { 2, "Off", false } })
    {
        std::string written;
        const bool got = EngineValueAfterRestart(r.choice, fallback, written);
        if (got != r.wantedAfterRestart)
            std::printf("  %s: wrote '%s', the engine sees %d after the restart, wanted %d\n", r.name, written.c_str(), got,
                        r.wantedAfterRestart);
        CHECK(got == r.wantedAfterRestart);

        // What the combo shows after the restart: Off stays Off; Auto and On both read as the default (the same effect)
        ext::FgConfig again;
        ext::config::Binder b;
        again.Bind(b);
        FindEntry(b, "DLSSG", "UIRecomposition").load(&again.uiRecomposition, written);
        CHECK((ComboIndex(again.uiRecomposition) == 2) == (r.choice == 2));
    }

    // Off is stored as an explicit value (not `auto`), Auto is `auto`
    std::string w;
    EngineValueAfterRestart(2, fallback, w);
    CHECK(w == "false");
    EngineValueAfterRestart(0, fallback, w);
    CHECK(w == "auto");

    // A hand-written `false` survives the next Save Settings, `auto` stays `auto`
    for (const char* hand : { "false", "auto" })
    {
        ext::FgConfig cfg;
        ext::config::Binder b;
        cfg.Bind(b);
        auto e = FindEntry(b, "DLSSG", "UIRecomposition");
        e.load(&cfg.uiRecomposition, hand);
        CHECK(e.save(&cfg.uiRecomposition) == hand);
    }
}

// The same round trip with the member declared the old way: Off must be lost. If this ever stops being true, the checks
// above no longer see the bug they are for.
void NegativeControlOldDefaultLosesOff()
{
    CustomOptional<bool> oldStyle { false }; // the old declaration
    oldStyle = ComboWrite(2);                // the user picks Off
    const auto saved = oldStyle.value_for_config();
    CHECK(!saved.has_value()); // written as `auto`, so the key is unset after the restart
    CustomOptional<bool> restarted { false };
    restarted.set_from_config(saved);
    CHECK((restarted.has_value() ? restarted.value() : true) == true); // the engine turns it on again: the bug

    CustomOptional<bool> fixedStyle { true }; // the declaration the product has to have
    fixedStyle = ComboWrite(2);
    CHECK(fixedStyle.value_for_config().has_value() && !*fixedStyle.value_for_config());
}
} // namespace

int main(int argc, char** argv)
{
    g_repo = argc > 1 ? argv[1] : ".";
    RealStepsArePinned();
    ComboMappingIsPinned();
    DeclaredDefaultEqualsTheEngineFallback();
    AutoOnOffSurviveSaveAndRestart();
    NegativeControlOldDefaultLosesOff();
    if (g_failures)
    {
        std::printf("ui_recomposition_roundtrip: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ui_recomposition_roundtrip: all passed\n");
    return 0;
}
