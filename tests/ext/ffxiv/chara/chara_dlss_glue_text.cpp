// Source-text guard of the glue that no recipe links: the lines of CharaDlss.cpp and CharaNr.cpp that pass a fact, a colour or a flag between the
// pure decision functions (CharaNrPlumbing.h) and the effects. The pure functions are tested with fakes, but a swapped argument, a deleted hand-off
// or a flag that is no longer carried to the finisher step compiles and passes every other test; these fragments fail on exactly those edits.
//
// The files are read from disk, comments are removed (a commented-out copy of a line does not count), all white space is removed from both the text
// and the fragments, and every fragment must be found the stated number of times. A fragment is a statement of how the glue is wired TODAY: a
// legitimate rewrite of a line has to change the fragment in the same commit, which is the point.
//
// Negative control: the same checks run on a copy of the text with one edit (the colour pair swapped, the hand-off deleted, the flag line deleted)
// and must report the edit.
#include "check.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Comments out, white space out. String and character literals are kept whole (a "//" inside one is not a comment).
std::string Code(const std::string& src)
{
    enum class S { Code, Line, Block, Str, Chr } state = S::Code;
    std::string out;
    for (size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        const char n = i + 1 < src.size() ? src[i + 1] : '\0';
        switch (state)
        {
        case S::Code:
            if (c == '/' && n == '/')
                state = S::Line;
            else if (c == '/' && n == '*')
            {
                state = S::Block;
                ++i;
            }
            else if (c == '"')
            {
                state = S::Str;
                out += c;
            }
            else if (c == '\'')
            {
                state = S::Chr;
                out += c;
            }
            else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                out += c;
            break;
        case S::Line:
            if (c == '\n')
                state = S::Code;
            break;
        case S::Block:
            if (c == '*' && n == '/')
            {
                state = S::Code;
                ++i;
            }
            break;
        case S::Str:
        case S::Chr:
            out += c;
            if (c == '\\' && n != '\0')
            {
                out += n;
                ++i;
            }
            else if ((state == S::Str && c == '"') || (state == S::Chr && c == '\''))
                state = S::Code;
            break;
        }
    }
    return out;
}

std::string NoSpace(const std::string& s)
{
    std::string out;
    for (const char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

size_t CountOf(const std::string& code, const std::string& fragment)
{
    const std::string f = NoSpace(fragment);
    size_t n = 0;
    for (size_t pos = code.find(f); pos != std::string::npos; pos = code.find(f, pos + f.size()))
        ++n;
    return n;
}

struct Fragment
{
    const char* text;
    size_t times; // exactly this many occurrences
    const char* why;
};

// Assignments `name = ...` (not `name == ...`) in comment-free, white-space-free code.
size_t AssignmentsTo(const std::string& code, const std::string& name)
{
    const std::string f = NoSpace(name) + "=";
    size_t n = 0;
    for (size_t pos = code.find(f); pos != std::string::npos; pos = code.find(f, pos + f.size()))
        if (pos + f.size() >= code.size() || code[pos + f.size()] != '=')
            ++n;
    return n;
}

// How many fragments are wrong in `code`.
int Wrong(const std::string& code, const std::vector<Fragment>& list)
{
    int wrong = 0;
    for (const Fragment& f : list)
        if (CountOf(code, f.text) != f.times)
            ++wrong;
    return wrong;
}

void Report(const std::string& file, const std::string& code, const std::vector<Fragment>& list)
{
    for (const Fragment& f : list)
    {
        const size_t n = CountOf(code, f.text);
        if (n != f.times)
            std::fprintf(stderr, "%s: expected %zu x '%s' (%s), found %zu\n", file.c_str(), f.times, f.text, f.why, n);
        CHECK(n == f.times);
    }
}

// CharaDlss.cpp: the wiring of the two moments of a render.
const std::vector<Fragment> kDlss = {
    { "nr::DecideSlotRender(PlumbingNow(),", 1, "the slot step decides on a fresh snapshot of the facts" },
    { "nr::ConsumerRecentFor(g_consumed, view.rtvTexture, view.now, kNrLiveWindowMs)", 2, "liveness from the preview image of this render, slot step and finisher step" },
    { "nr::DecideFinisherRender(facts, view.finisherAccepted, live, srInCharge, g_nrProductionThisRender,", 1, "the finisher step decides on the snapshot taken at the top of Around and carries the slot step's flag" },
    { "}nr::PlumbingFacts facts = PlumbingNow();", 1, "the snapshot taken at the top of Around (the other PlumbingNow() copy is const and follows a statement)" },
    { "g_nrProductionThisRender = render.composedFromNr;", 1, "the flag that carries 'composed from NR' to the finisher step" },
    { "g.composedFromNr = render.composedFromNr;", 1, "the slot remembers whether its picture came from NR" },
    { "nrOutput = std::move(call.view);", 1, "the production image is handed to the alpha pass" },
    { "nr::AlphaColourFor<ID3D11ShaderResourceView*>(input, nrOutput.Get(), g.output.srv.Get())", 1, "the alpha pass reads the NR image for NrOutput and the SR image otherwise" },
    { "RunNrProduction(view, g, s, in)", 1, "the production entry is given the evaluate's own inputs" },
    { "nr::GuidesOf(in, s.reversedZ)", 1, "the NR guides come from the evaluate's inputs and the slot's depth convention" },
    { "request.superResolutionReset = in.reset;", 1, "the SR reset reaches the NR model" },
    { "const uint32_t outW = in.width * ngx::kOutputScale, outH = in.height * ngx::kOutputScale;", 1, "the production colour is the SR output size" },
    { "request.base.width = outW;", 1, "the production stream's size is the colour image's" },
    { "request.colour = g.output.texture.Get();", 1, "the production colour is the SR output" },
};

// CharaNr.cpp: the plan is made in one place (assignments to these fields are only allowed inside MakeFramePlan).
const char* const kPlanFields[] = { "plan.reset", "plan.initGuides", "plan.guides", "plan.width", "plan.height", "plan.colour", "plan.result" };
const std::vector<Fragment> kNr = {
    { "MakeFramePlan(st, planInputs, &reset)", 1, "Run() records the plan the tests check" },
};

// The edit of a negative control, made on the comment-free text (white space is not part of the match).
std::string Replace(std::string code, const std::string& from, const std::string& to)
{
    const std::string f = NoSpace(from);
    const size_t pos = code.find(f);
    if (pos != std::string::npos)
        code.replace(pos, f.size(), NoSpace(to));
    return code;
}

int PlanFieldAssignments(const std::string& code)
{
    int n = 0;
    for (const char* field : kPlanFields)
        n += static_cast<int>(AssignmentsTo(code, field));
    return n;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string dir = repo + "/OptiScaler/ext/ffxiv/chara/";
    const std::string dlssSource = ReadFile(dir + "CharaDlss.cpp");
    const std::string nrSource = ReadFile(dir + "CharaNr.cpp");
    CHECK(!dlssSource.empty() && !nrSource.empty());

    const std::string dlss = Code(dlssSource);
    const std::string nr = Code(nrSource);
    Report("CharaDlss.cpp", dlss, kDlss);
    Report("CharaNr.cpp", nr, kNr);
    CHECK(PlanFieldAssignments(nr) == 0);

    // The comment stripper: a fragment inside a comment or after "//" does not count, a "//" inside a string does not start a comment.
    CHECK(Code("a /* b */ c // d\ne") == "ace");
    CHECK(Code("x = \"a // b\"; y") == "x=\"a // b\";y");
    CHECK(CountOf(Code("// nr::PlumbingFacts facts = PlumbingNow();\n"), "nr::PlumbingFacts facts = PlumbingNow();") == 0);
    CHECK(AssignmentsTo(Code("if (plan.colour == Tex::A) plan.colour = B;"), "plan.colour") == 1);

    // Negative control: each edit the guard exists for is reported.
    const std::string swapped = Replace(dlss, "AlphaColourFor<ID3D11ShaderResourceView*>(input, nrOutput.Get(), g.output.srv.Get())",
                                        "AlphaColourFor<ID3D11ShaderResourceView*>(input, g.output.srv.Get(), nrOutput.Get())");
    CHECK(swapped != dlss && Wrong(swapped, kDlss) > 0);
    const std::string noHandOff = Replace(dlss, "nrOutput = std::move(call.view);", "");
    CHECK(noHandOff != dlss && Wrong(noHandOff, kDlss) > 0);
    const std::string noFlag = Replace(dlss, "g_nrProductionThisRender = render.composedFromNr;", "");
    CHECK(noFlag != dlss && Wrong(noFlag, kDlss) > 0);
    const std::string staleFacts = Replace(dlss, "nr::DecideSlotRender(PlumbingNow(),", "nr::DecideSlotRender(facts,");
    CHECK(staleFacts != dlss && Wrong(staleFacts, kDlss) > 0);
    const std::string wrongSnapshot = Replace(dlss, "nr::DecideFinisherRender(facts,", "nr::DecideFinisherRender(PlumbingNow(),");
    CHECK(wrongSnapshot != dlss && Wrong(wrongSnapshot, kDlss) > 0);
    const std::string loosePlan = Replace(nr, "FramePlan plan = MakeFramePlan(st, planInputs, &reset);", "FramePlan plan; plan.reset = true;");
    CHECK(loosePlan != nr && (Wrong(loosePlan, kNr) > 0 || PlanFieldAssignments(loosePlan) > 0));
    return TEST_RESULT("chara_dlss_glue_text");
}
