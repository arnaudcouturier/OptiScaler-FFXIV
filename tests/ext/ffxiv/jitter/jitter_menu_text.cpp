// The "Game jitter (FFXIV)" menu card is ImGui glue and cannot run offline: this pins what it must do by reading the source
// (comments and string literals stripped): both keys are assigned from their own checkbox inside Draw, the card is registered
// in the FFXIV tab with the FFXIV visibility rule, and it holds no decision of its own.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removes // comments, /* */ comments and the CONTENT of string literals (the quotes stay).
std::string Strip(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
        if (s.compare(i, 2, "//") == 0)
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (s.compare(i, 2, "/*") == 0)
        {
            const size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? s.size() : e + 2;
        }
        else if (s[i] == '"')
        {
            out += '"';
            ++i;
            while (i < s.size() && s[i] != '"')
                i += (s[i] == '\\' && i + 1 < s.size()) ? 2 : 1;
            out += '"';
            ++i;
        }
        else
            out += s[i++];
    }
    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

std::string Squash(const std::string& s)
{
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string raw = ReadFile(repo + "/OptiScaler/ext/ffxiv/jitter/FfxivJitterMenu.cpp");
    CHECK(!raw.empty());
    const std::string code = Squash(Strip(raw));

    // each key is read from and written back to its own checkbox, once
    CHECK(Count(code, "boolprobe=cfg.jitterProbe.value_or_default();") == 1);
    CHECK(Count(code, "if(ImGui::Checkbox(\"\",&probe))cfg.jitterProbe=probe;") == 1);
    CHECK(Count(code, "boolcorrect=cfg.jitterCorrection.value_or_default();") == 1);
    CHECK(Count(code, "if(ImGui::Checkbox(\"\",&correct))cfg.jitterCorrection=correct;") == 1);
    // the source-fix switch: read from and written back to its own checkbox, once, with plain-English text
    CHECK(Count(code, "boolsource=cfg.jitterSource.value_or_default();") == 1);
    CHECK(Count(code, "if(ImGui::Checkbox(\"\",&source))cfg.jitterSource=source;") == 1);
    CHECK(Count(code, "cfg.jitterSource=") == 1);
    CHECK(raw.find("\"Fix the game's jitter at the source (experimental)\"") != std::string::npos);
    CHECK(raw.find("full +-0.5 pixel jitter") != std::string::npos && raw.find("repeating cycle (32 phases at Performance") != std::string::npos);
    CHECK(raw.find("exactly the offset that was drawn") != std::string::npos);
    CHECK(raw.find("the correction below stands down") != std::string::npos && raw.find("switches itself off") != std::string::npos);
    CHECK(raw.find("Jitter source: active") != std::string::npos && raw.find("Jitter source: verified") != std::string::npos);
    CHECK(raw.find("Takes effect on the next frame") != std::string::npos);
    // no other writes to the keys
    CHECK(Count(code, "cfg.jitterProbe=") == 1);
    CHECK(Count(code, "cfg.jitterCorrection=") == 1);
    // the manual scale: read from and written back to its own slider, once, greyed out while the correction switch is off
    CHECK(Count(code, "floatscale=cfg.jitterScaleOverride.value_or_default();") == 1);
    CHECK(Count(code, "if(ImGui::SliderFloat(\"\",&scale,0.0f,1.0f,scale>0.0f?\"\":\"\"))cfg.jitterScaleOverride=SnapManualScale(scale);") == 1);
    CHECK(Count(code, "cfg.jitterScaleOverride=") == 1);
    CHECK(Count(code, "ImGui::BeginDisabled(!correct);") == 1 && Count(code, "ImGui::EndDisabled();") == 1);
    // the plain-English text: the label, what 0 means, the valid range, that it is an experiment and what it needs
    CHECK(raw.find("\"Manual jitter scale (0 = learn it)\"") != std::string::npos);
    CHECK(raw.find("Experiment") != std::string::npos && raw.find("0.25 to 1.0") != std::string::npos);
    CHECK(raw.find("1.0 (changes nothing), 0.8, 0.6 and 0.5") != std::string::npos);
    CHECK(raw.find("0 learns the ratio") != std::string::npos);
    CHECK(raw.find("only used while the correction switch above is ticked") != std::string::npos);
    // the correction tooltip describes the calibrate-and-scale behaviour, not the old per-frame replacement
    CHECK(raw.find("hands DLSS the jitter the game really applied") == std::string::npos);
    CHECK(raw.find("Learns how much of the jitter the game reports it really applies") != std::string::npos);
    CHECK(raw.find("measured 0.6 in this game") != std::string::npos && raw.find("scaled by that ratio") != std::string::npos);
    CHECK(raw.find("Jitter correction: calibrated ratio") != std::string::npos);
    CHECK(raw.find("how many evaluates were replaced") == std::string::npos);
    CHECK(raw.find("how many evaluates were scaled") != std::string::npos);
    // registered in the FFXIV tab (its place comes from the layout table), visible only for the FFXIV build entry
    CHECK(Count(code, "ext::menu::AutoRegisterregistration{{\"\",\"\",\"\",Draw,\"\",Visible}};") == 1);
    CHECK(raw.find("{ \"ffxiv_jitter\", \"FFXIV\", ") != std::string::npos);
    CHECK(raw.find("\"output_scaling\"") != std::string::npos);
    CHECK(raw.find("\"ffxiv_jitter\"") != std::string::npos);
    CHECK(Count(code, "returnext::game::Current().lighting!=nullptr;") == 1);
    // the card decides nothing: no calls into the engine or the hook
    CHECK(code.find("Engine") == std::string::npos);
    CHECK(code.find("OnUpscaleBoundary") == std::string::npos);

    // the wrapper hands the engine the value of the manual-scale key at every evaluate (read like the two switches, so the menu acts at once)
    const std::string wrapper = Squash(Strip(ReadFile(repo + "/OptiScaler/ext/ffxiv/jitter/FfxivJitterProbe.cpp")));
    CHECK(Count(wrapper, "S().engine.Step(env,probe,correctNow,cfg.jitterScaleOverride.value_or_default(),SourceNote{src.active,src.maxJitter,src.standDown});") == 1);
    // the source fix runs first, and it alone decides whether the correction runs in this evaluate
    // asked only when the key is on or has been on: with the key off all session the controller is never reached (the cost is the early return)
    CHECK(Count(wrapper, "source::StepResultsrc;") == 1);
    CHECK(Count(wrapper, "if(sourceKey||g_sourceEverOn.load(std::memory_order_relaxed))src=RunSourceFix(env,sourceKey,correct);") == 1);
    CHECK(Count(wrapper, "constboolcorrectNow=correct&&!src.standDown;") == 1);
    CHECK(wrapper.find("src=RunSourceFix(env,sourceKey,correct)") < wrapper.find("engine.Step("));
    CHECK(Count(wrapper, "controller.Step(env,source::hooks::State(),correct)") == 1);
    // all keys off, and the fix never switched on: the early return is the whole cost
    CHECK(Count(wrapper, "if((!probe&&!correct&&!sourceKey&&!g_sourceEverOn.load(std::memory_order_relaxed))||parameters==nullptr)return;") == 1);
    CHECK(Count(wrapper, "constboolsourceKey=cfg.jitterSource.value_or_default();") == 1);
    // an exception inside the controller leaves the engine out of an evaluate the fix may have touched
    CHECK(Count(wrapper, "r.standDown=r.active=g.controller.WasActive();") == 1);
    CHECK(Count(wrapper, "engine.Step(") == 1);

    // the stripper itself: a pin kept alive only by a comment or a string must not count
    CHECK(Squash(Strip("a // cfg.jitterProbe = x;\nb")).find("jitterProbe") == std::string::npos);
    CHECK(Squash(Strip("a /* cfg.jitterProbe = x; */ b")).find("jitterProbe") == std::string::npos);
    CHECK(Squash(Strip("a \"cfg.jitterProbe = x;\" b")).find("jitterProbe") == std::string::npos);

    if (failures)
    {
        std::printf("jitter_menu_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("jitter_menu_text: all passed\n");
    return 0;
}
