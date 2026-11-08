// Atmosphere: the timed counts line's wiring pins (INT2), read as text from the sources (no device). The rules half is atmo_counts_rules.
//
// What a wrong wiring would break, and what is pinned here:
//   every entry that reaches the stage ticks the line and is counted: the three entries (own seam, chained hook, owner hook) reach the pass through the ONE
//     function StageAtToneMap, whose first two statements tick the line and count the entry that reached it, and which counts the run; the three call sites
//     pass three different entries (so the chained hook, the entry the stage is on whenever the night grade, the light bounce or the march has the chain, is
//     not counted as the seam);
//   the declines are counted per reason and the skips per reason where they are noted (NoteDecline, NoteSkip);
//   the line is written by exactly one place, from the timed rules, on the wall clock, and nothing else writes it; the old once-a-minute frame cadence is
//     gone (a rendered-frame cadence is silent whenever no entry runs it);
//   the heartbeat: the once-an-output-frame sample ticks the line too, and the engine pass reaches that sample while the stage is active, so an armed stage
//     that no entry reaches still writes its line ("reached 0 times");
//   the counts are reset with the stage's other counters (key off, key back on after a stand-down);
//   the stage names its entries by hook, never by caller (the chain's order is written only in ChainHooks.h).
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                   \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string Comments(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string Raw(const std::string& repo, const char* relative) { return ReadFile(repo + "/OptiScaler/ext/" + relative); }

std::string Strip(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Code(const std::string& repo, const char* relative) { return Strip(Comments(Raw(repo, relative))); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }

std::string Body(const std::string& code, const std::string& signature)
{
    const size_t at = code.find(signature);
    if (at == std::string::npos)
        return {};
    const size_t open = code.find('{', at + signature.size());
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return code.substr(open, i - open + 1);
    }
    return {};
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

bool Ordered(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t firstAt = text.find(first);
    const size_t secondAt = text.find(second);
    return firstAt != std::string::npos && secondAt != std::string::npos && firstAt < secondAt;
}

// The number a whitespace-free code text assigns to a constant `name` ("...kName=0.12f;" / "...kName=1.0e-6;"), or NaN when it is not defined once.
double ConstantOf(const std::string& code, const std::string& name)
{
    const std::string needle = name + "=";
    const size_t at = code.find(needle);
    if (at == std::string::npos || code.find(needle, at + needle.size()) != std::string::npos)
        return std::nan("");
    return std::strtod(code.c_str() + at + needle.size(), nullptr);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: atmo_counts_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string draw = Code(repo, "ffxiv/engine/AtmoDraw.cpp");
    const std::string counts = Code(repo, "ffxiv/engine/AtmoCounts.h");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    EXPECT(!draw.empty() && !counts.empty() && !enginePass.empty(), "every file the pins read is found\n");

    // ---- the one function every entry passes through ----------------------------------------------------------------------------------
    {
        const std::string stage = Body(draw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,OutcomedeclineOutcome,Entryentry)");
        EXPECT(!stage.empty(), "StageAtToneMap is found, with the entry as its last parameter\n");
        EXPECT(stage.rfind("{LogCountsIfDue(state);state.counts.NoteReached(entry);", 0) == 0, "its first two statements tick the line and count the entry that reached it\n");
        EXPECT(Has(stage, "state.runs++;state.counts.NoteRan(entry);state.streak.NoteRun();"), "and a run is counted for the entry that made it, with the module's own run counter\n");
        EXPECT(Count(draw, "StageAtToneMap(state,") == 3, "the pass is reached from exactly three call sites (the definition aside)\n");
        const std::string chain = Body(draw, "ID3D11ShaderResourceView*ChainSceneInternal(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        const std::string owner = Body(draw, "boolRunToneMapAsOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        const std::string seam = Body(draw, "boolSeamDraw(ID3D11DeviceContext*context,Anchorsanchors,DrawFndraw,void*user)");
        EXPECT(!chain.empty() && !owner.empty() && !seam.empty(), "the three entries are found\n");
        EXPECT(Has(chain, "StageAtToneMap(state,context,facts,scene,Outcome::HoldStands,Entry::Chain)") && Count(chain, "Entry::") == 1,
               "the chained hook passes Entry::Chain (and no other entry)\n");
        EXPECT(Has(owner, "StageAtToneMap(state,context,facts,scene,Outcome::EngineOwn,Entry::Owner)") && Count(owner, "Entry::") == 1,
               "the owner hook passes Entry::Owner\n");
        EXPECT(Has(seam, "StageAtToneMap(state,context,facts,scene.Get(),Outcome::EngineOwn,Entry::Seam)") && Count(seam, "Entry::") == 1,
               "the own seam entry passes Entry::Seam\n");
        EXPECT(Count(draw, "LogCountsIfDue(state);") == 2, "the line is ticked from exactly two places: StageAtToneMap and the heartbeat\n");
    }

    // ---- the declines and the skips, counted per reason ----------------------------------------------------------------------------------
    {
        const std::string decline = Body(draw, "voidNoteDecline(State&state,Declinereason,Outcomeoutcome)");
        const std::string skip = Body(draw, "voidNoteSkip(State&state,SkipReasonreason)");
        EXPECT(Has(decline, "state.declined++;state.counts.NoteDeclined(reason);"), "a decline is counted by reason beside the total\n");
        EXPECT(Has(skip, "CountSkip(state.skips,reason);state.counts.NoteSkipped(reason);"), "a skip is counted by reason beside the card's own skip counters\n");
    }

    // ---- the line itself ---------------------------------------------------------------------------------------------------------------------
    {
        const std::string tick = Body(draw, "voidLogCountsIfDue(State&state)");
        EXPECT(tick == "{constuint64_tnow=GetTickCount64();if(!state.counts.Due(now,state.frames))return;LOG_INFO(\"{}\",state.counts.TakeLine(now,state.frames,state.params));}",
               "the tick reads the wall clock, asks the timed rules and writes the line they return, nothing else\n");
        EXPECT(Count(draw, "TakeLine(") == 1, "the line is taken in one place\n");
        EXPECT(!Has(draw, "nextCountsLog") && !Has(draw, "kCountsLogFrames"), "the once-a-minute rendered-frame cadence is gone\n");
        EXPECT(ConstantOf(counts, "kCountsLogMs") == 10000.0, "the cadence is ten seconds of wall clock\n");
        EXPECT(Has(counts, "enumclassEntry:uint8_t{Seam=0,Chain,Owner,};"), "the entries are the own seam, the chained hook and the owner hook, in that order\n");
        EXPECT(Has(counts, "++_window.declined[uint32_t(reason)];"), "the rules count a decline by its reason\n");
    }

    // ---- the heartbeat -------------------------------------------------------------------------------------------------------------------------
    {
        const std::string sample = Body(draw, "voidSampleSceneHdr()");
        EXPECT(!sample.empty() && Has(sample, "ObserveSceneHdr(state);LogCountsIfDue(state);}"), "the once-an-output-frame sample ticks the line after it has taken its sample\n");
        EXPECT(Has(sample, "if(state.haveHdrSample&&state.hdrSampleFrame==token)return;"), "and only once an output frame (the sample's own gate comes first)\n");
        EXPECT(Has(enginePass, "if(ext::Active(ext::F::Atmosphere))ext::ffxiv::engine::atmo::SampleSceneHdr();"), "the engine pass reaches the sample while the stage is active\n");
    }

    // ---- the reset -----------------------------------------------------------------------------------------------------------------------------
    {
        const std::string apply = Body(draw, "voidApply(boolon)");
        EXPECT(Count(apply, "state.counts.Reset();") == 2, "the counts are reset when the key goes off and when it comes back on after a stand-down\n");
        EXPECT(Has(apply, "state.runs=0;state.declined=0;state.skips=SkipCounts{};"), "beside the module's other counters\n");
    }

    // ---- by hook, never by caller -----------------------------------------------------------------------------------------------------------------
    {
        EXPECT(!Has(draw, "nightgrade") && !Has(draw, "ng::") && !Has(counts, "nightgrade") && !Has(counts, "Night"), "the stage names its entries by hook; the chain's order is written only in ChainHooks.h\n");
    }

    if (g_failures)
    {
        std::printf("atmo_counts_pins: %d pin(s) failed\n", g_failures);
        return 1;
    }
    std::printf("atmo_counts_pins: all pins hold\n");
    return 0;
}
