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

#include <cctype>
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

// The first word after the '#' of a preprocessor line ("if", "ifdef", "endif", ...), "" for any other line; `rest` receives what follows it.
std::string DirectiveWord(const std::string& line, std::string& rest)
{
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] != '#')
        return "";
    i = line.find_first_not_of(" \t", i + 1);
    if (i == std::string::npos)
        return "";
    size_t j = i;
    while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_'))
        ++j;
    rest = line.substr(j);
    return line.substr(i, j - i);
}

// `#if 0` ... `#endif` is dead text: removed (nested conditionals are counted, the live part after an `#else` or `#elif` of the dead block stays).
// A fragment inside such a block must not count as present.
std::string DropIfZero(const std::string& src)
{
    std::istringstream lines(src);
    std::string line, out;
    int depth = 0; // > 0: inside a dead block
    while (std::getline(lines, line))
    {
        std::string rest;
        const std::string word = DirectiveWord(line, rest);
        if (depth == 0)
        {
            const size_t zero = rest.find_first_not_of(" \t");
            if (word == "if" && zero != std::string::npos && rest[zero] == '0' &&
                (zero + 1 >= rest.size() || !std::isalnum(static_cast<unsigned char>(rest[zero + 1]))))
            {
                depth = 1;
                out += '\n';
                continue;
            }
            out += line + '\n';
            continue;
        }
        if (word == "if" || word == "ifdef" || word == "ifndef")
            ++depth;
        else if (word == "endif")
            --depth;
        else if ((word == "else" || word == "elif") && depth == 1)
            depth = 0; // the live part of the same conditional starts here
        out += '\n';
    }
    return out;
}

// Dead `#if 0` blocks out, comments out, white space out. The CONTENT of string and character literals is dropped (the quotes stay): a fragment that
// is only text in a log line or a message must not count as wiring. A "//" inside a literal is not a comment. Raw strings are not understood (none
// is allowed in the files read, see main).
std::string Code(const std::string& text)
{
    const std::string src = DropIfZero(text);
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
            if (c == '\\' && n != '\0')
                ++i; // an escaped character (an escaped quote does not end the literal)
            else if ((state == S::Str && c == '"') || (state == S::Chr && c == '\''))
            {
                out += c; // the closing quote; the content in between is not kept
                state = S::Code;
            }
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
    { "nr::ConsumerRecentFor(g_consumed, view.rtvTexture, view.now, kNrLiveWindowMs)", 3, "liveness from the preview image of this render: pre-SR step, slot step and finisher step" },
    { "nr::DecideFinisherRender(facts, view.finisherAccepted, live, srInCharge, g_nrProductionThisRender,", 1, "the finisher step decides on the snapshot taken at the top of Around and carries the slot step's flag" },
    { "}nr::PlumbingFacts facts = PlumbingNow();", 1, "the snapshot taken at the top of Around (the other PlumbingNow() copy is const and follows a statement)" },
    { "g_nrProductionThisRender = render.composedFromNr;", 1, "the flag that carries 'composed from NR' to the finisher step" },
    { "g.composedFromNr = render.composedFromNr;", 1, "the slot remembers whether its picture came from NR" },
    { "nrOutput = std::move(call.view);", 1, "the production image is handed to the alpha pass" },
    { "nr::AlphaColourFor<ID3D11ShaderResourceView*>(input, nrOutput.Get(), g.output.srv.Get())", 1, "the alpha pass reads the NR image for NrOutput and the SR image otherwise" },
    { "RunNrProduction(view, g, s, ngx::WithReset(in, ev.resetUsed))", 1, "the production entry is given the evaluate's own inputs with the history reset the evaluate ran with (a source change resets too)" },
    { "nr::GuidesOf(in, s.reversedZ)", 2, "the NR guides come from the evaluate's inputs and the slot's depth convention (post-SR and pre-SR entries)" },
    { "request.superResolutionReset = in.reset;", 2, "the SR reset reaches the NR model (post-SR and pre-SR entries)" },
    { "nr::DecideSrEvaluate(PlumbingNow(),", 1, "the SR evaluate and the pre-SR step are decided on a fresh snapshot" },
    { "preSrColour = std::move(call.texture);", 1, "the pre-SR NR image is handed to the evaluate" },
    { "nr::SrHistory { in.reset, g.lastColour, g.sourceResets.Due(view.now) }", 1,
      "the rule is told the slot's own reset, the colour source of its last successful evaluate and whether the 500 ms debounce of a source-change reset allows one" },
    { "ngx::ApplyEvaluate<ParamSetter, ID3D11Resource*>(setter, ngx::WithReset(in, reset),", 1, "the evaluate gets the rule's reset (the first call's, and the retry's after a refusal), not the slot's own" },
    { "if (ev.ok) g.lastColour = ev.colour;", 1, "a successful evaluate records its colour source for the next render's reset rule" },
    { "if (ev.ok && ev.sourceReset)", 1, "only a successful evaluate that ran with a source-change reset starts the debounce and is counted" },
    { "NoteSourceReset(index, g, view.now);", 1, "the debounce is started with the evaluate's own time" },
    { "g.sourceResets.Note(now);", 1, "NoteSourceReset starts the slot's debounce clock" },
    { "++g_stats.srSourceResets", 0, "the counter goes up through NoteSourceReset's fetch_add only" },
    { "g_stats.srSourceResets.fetch_add(1, std::memory_order_relaxed)", 1, "one counter increment for a source-change reset" },
    { "nr::SrColourFor<ID3D11Resource*>(source, preSrColour.Get(), g.color.texture.Get())", 1, "the evaluate's Color is the NR image or g.color, by the pure choice" },
    { "tinted, ev.placement, ev.usedPreSrColour,", 1, "the slot step is told the placement and whether the evaluate used the NR image" },
    { "if (ev.latchPreSr && nr::SharedLatches().LatchPreSr())", 1, "a refused NR image latches NR before SR once, with one log line" },
    { "if (!ev.ok)", 1, "only a failed plain evaluate fails the slot" },
    { "g.preSrColour = false;", 1, "every render clears the pre-SR flag before the slot step, so an alpha failure never leaves a stale bit" },
    { "g.preSrColour = render.preSrColour;", 1, "the slot remembers whether its picture's colour was the pre-SR image" },
    { "if (nr::MadeFromPostSrNr(g.composedFromNr, g.preSrColour))", 1, "the run statistic does not count pre-SR images" },
    { "if (nr::MadeFromPostSrNr(g->composedFromNr, g->preSrColour))", 1, "the UI-draw statistic does not count pre-SR images either" },
    { "if (anyPostSrNr)", 1, "the production swap counter and its log line follow the post-SR-only flag" },
    { "request.input = ev.usedPreSrColour ? preSrColour.Get() : g.color.texture.Get();", 1, "a probe capture labels the colour the evaluate really read as the DLSS input" },
    { "request.colour = g.color.texture.Get();", 1, "the pre-SR colour is the slot's render-size colour" },
    // The three requests that carry the slot's guides, each as one contiguous block (a swap of depth and motion in one of them compiles, passes every
    // pure test and is only caught in game by the format check, which then switches all preview NR off), and the total of each line.
    { "request.colour = g.color.texture.Get(); request.depth = g.depth.texture.Get(); request.motion = g.motion.texture.Get(); request.guides = nr::GuidesOf(in, s.reversedZ);", 1, "the pre-SR request: colour, depth, motion and guides in this order" },
    { "request.colour = g.output.texture.Get(); request.depth = g.depth.texture.Get(); request.motion = g.motion.texture.Get(); request.guides = nr::GuidesOf(in, s.reversedZ);", 1, "the production request: colour, depth, motion and guides in this order" },
    { "request.dlss = g.output.texture.Get(); request.composed = g.composed.texture.Get(); request.depth = g.depth.texture.Get(); request.motion = g.motion.texture.Get();", 1, "the probe capture request: depth and motion" },
    { "request.depth = g.depth.texture.Get();", 3, "depth is the slot's depth in all three requests" },
    { "request.motion = g.motion.texture.Get();", 3, "motion is the slot's motion in all three requests" },
    { "const nr::ProductionAnswer answer { result.outcome, result.view != nullptr && result.texture != nullptr };", 1, "the pre-SR answer needs the view and the texture the evaluate takes" },
    { "const nr::ProductionAnswer answer { result.outcome, result.view != nullptr };", 1, "the production answer needs the view the alpha pass reads" },
    { "return { std::move(result.texture), answer };", 1, "the pre-SR step hands back the texture, not the view" },
    { "g.output.texture.Get(), g.depth.texture.Get(), g.motion.texture.Get());", 1, "the first evaluate and its retry are given the same output, depth and motion (one lambda, one call)" },
    { "result = evaluate(context, g.handle, g.params, nullptr);", 1, "one NGX evaluate call in RunSlot, inside the lambda of the decision" },
    { "request.base.width = in.width;", 1, "the pre-SR stream's size is the render size" },
    { "nr::RunPreSr(request)", 1, "the pre-SR step calls the pre-SR entry" },
    { "PreSrCall call = RunNrPreSr(view, g, s, in);", 1, "the pre-SR step is given the evaluate's own inputs" },
    { "f.preSrWanted = ext::Cfg().ffxiv.chara.previewNrBeforeSr.value_or_default();", 1, "the key reaches the rules" },
    { "f.preSrLatched = nr::SharedLatches().PreSrLatched();", 1, "the latch reaches the rules" },
    { "const uint32_t outW = in.width * ngx::kOutputScale, outH = in.height * ngx::kOutputScale;", 1, "the production colour is the SR output size" },
    { "request.base.width = outW;", 1, "the production stream's size is the colour image's" },
    { "request.colour = g.output.texture.Get();", 1, "the production colour is the SR output" },
};

// Order. Counts alone keep every fragment present when a statement is moved: each chain below occurs once per step, in this order, in the
// comment-free text (positions are first occurrences, and every step is also counted above or by the check itself).
struct Chain
{
    std::vector<const char*> steps;
    const char* why;
};

const std::vector<Chain> kDlssOrder = {
    { { "in.reset = g_slots.ShouldReset(index, view.now);",
        "nr::DecideSrEvaluate(PlumbingNow(),",
        "PreSrCall call = RunNrPreSr(view, g, s, in);",
        "ngx::ApplyEvaluate<ParamSetter, ID3D11Resource*>(setter, ngx::WithReset(in, reset),",
        "nr::SrColourFor<ID3D11Resource*>(source, preSrColour.Get(), g.color.texture.Get())",
        "g.output.texture.Get(), g.depth.texture.Get(), g.motion.texture.Get());",
        "result = evaluate(context, g.handle, g.params, nullptr);",
        "if (ev.ok) g.lastColour = ev.colour;",
        "if (ev.ok && ev.sourceReset)",
        "if (ev.latchPreSr && nr::SharedLatches().LatchPreSr())",
        "if (!ev.ok)",
        "g.composedFromNr = false;",
        "g.preSrColour = false;",
        "nr::DecideSlotRender(PlumbingNow(),",
        "g.composedFromNr = render.composedFromNr;",
        "request.input = ev.usedPreSrColour ? preSrColour.Get() : g.color.texture.Get();" },
      "RunSlot: the slot's reset is taken before the decision, the pre-SR run and the evaluate (with its retry) happen inside the decision, then the latch, "
      "the failure, the slot step and the capture" },
};

// How many chains are broken in `code`: a step missing, found more than once, or out of order.
int WrongOrder(const std::string& code, const std::vector<Chain>& chains, bool report = false)
{
    int broken = 0;
    for (const Chain& chain : chains)
    {
        size_t last = 0;
        bool ok = true;
        for (const char* step : chain.steps)
        {
            const std::string f = NoSpace(step);
            const size_t pos = code.find(f);
            if (pos == std::string::npos || code.find(f, pos + f.size()) != std::string::npos || pos < last)
            {
                if (report)
                    std::fprintf(stderr, "order: step '%s' missing, repeated or out of order (%s)\n", step, chain.why);
                ok = false;
                break;
            }
            last = pos;
        }
        if (!ok)
            ++broken;
    }
    return broken;
}

// CharaNr.cpp: the plan is made in one place (assignments to these fields are only allowed inside MakeFramePlan).
const char* const kPlanFields[] = { "plan.reset", "plan.initGuides", "plan.guides", "plan.width", "plan.height", "plan.colour", "plan.result" };
const std::vector<Fragment> kNr = {
    { "MakeFramePlan(st, planInputs, &reset)", 1, "Run() records the plan the tests check" },
    { "CopyPlanFor(placement)", 1, "the hand-off copies exactly what the pure copy plan lists, all before the ready signal" },
    { "ShapeFor(placement, req.width, req.height, tuning.preset, tuning.style, Late(),", 1, "the stream shape comes from the pure ShapeFor (PreviewNRLate only for the bring-up)" },
    { "CopiedInputsFit(wanted,", 1, "the copied images are checked against the placement's own fit" },
    { "ColourFormatFor(placement, static_cast<uint32_t>(nativeFormat))", 1, "the colour image has the placement's format" },
    { "m.timerRing.NoteSubmitted(out.done, placement);", 1, "the timer attributes a measurement to the placement" },
    { "CountedCall(M().fallbacks, placement,", 1, "a call without a result is counted for its own placement" },
    { "RunCopiedImpl(request, Placement::PostSr)", 1, "the production entry runs the post-SR placement" },
    { "RunCopiedImpl(request, Placement::PreSr)", 1, "the pre-SR entry runs the pre-SR placement" },
    { "!ResultTextureServes(ResultKindOf(st))", 1, "the dump is not offered a result it cannot label" },
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
    CHECK(WrongOrder(dlss, kDlssOrder, true) == 0);
    // The two slot flags are pinned by their number of assignments too, not only by fragment and order: an unconditional second `g.lastColour = ...;` or
    // `g.preSrColour = ...;` elsewhere in the file would leave every count-1 fragment true.
    CHECK(AssignmentsTo(dlss, "g.lastColour") == 1);   // the store after a successful evaluate
    CHECK(AssignmentsTo(dlss, "g.preSrColour") == 2);  // the clear before the slot step and the copy-back after the alpha pass
    // the stripper does not understand raw strings: none may appear in the files it reads
    CHECK(dlssSource.find("R\"(") == std::string::npos && nrSource.find("R\"(") == std::string::npos);

    // The stripper: a fragment inside a comment, after "//", in a string or character literal, or in an `#if 0` block does not count; a "//" inside a
    // string does not start a comment.
    CHECK(Code("a /* b */ c // d\ne") == "ace");
    CHECK(Code("x = \"a // b\"; y") == "x=\"\";y");
    CHECK(Code("c = '\"'; d = \"\\\"q\"; e") == "c='';d=\"\";e"); // a quote inside a character literal, an escaped quote inside a string
    CHECK(CountOf(Code("// nr::PlumbingFacts facts = PlumbingNow();\n"), "nr::PlumbingFacts facts = PlumbingNow();") == 0);
    CHECK(CountOf(Code("LOG_INFO(\"nr::PlumbingFacts facts = PlumbingNow();\");"), "nr::PlumbingFacts facts = PlumbingNow();") == 0);
    CHECK(CountOf(Code("a();\n#if 0\nnr::PlumbingFacts facts = PlumbingNow();\n#endif\nb();\n"), "nr::PlumbingFacts facts = PlumbingNow();") == 0);
    CHECK(Code("a();\n#if 0\nx();\n#endif\nb();\n") == "a();b();");
    {
        // nested conditionals inside the dead block, and the live part after its #else
        const std::string text = Code("#if 0 // dead\n#if X\nfoo(1);\n#endif\nfoo(2);\n#else\nbar(3);\n#endif\nbaz(4);\n");
        CHECK(CountOf(text, "foo(1);") == 0 && CountOf(text, "foo(2);") == 0 && CountOf(text, "bar(3);") == 1 && CountOf(text, "baz(4);") == 1);
        CHECK(CountOf(Code("#if 01\nlive();\n#endif\n"), "live();") == 1); // not a literal zero
        CHECK(CountOf(Code("#ifdef X\nlive();\n#endif\n"), "live();") == 1);
    }
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

    // Depth and motion swapped in one of the three requests (each is a separate edit; a swap keeps the file compiling and every pure test green).
    const std::string swapPre = Replace(dlss, "request.colour = g.color.texture.Get(); request.depth = g.depth.texture.Get(); request.motion = g.motion.texture.Get();",
                                        "request.colour = g.color.texture.Get(); request.depth = g.motion.texture.Get(); request.motion = g.depth.texture.Get();");
    CHECK(swapPre != dlss && Wrong(swapPre, kDlss) > 0);
    const std::string swapProduction = Replace(dlss, "request.colour = g.output.texture.Get(); request.depth = g.depth.texture.Get(); request.motion = g.motion.texture.Get();",
                                               "request.colour = g.output.texture.Get(); request.depth = g.motion.texture.Get(); request.motion = g.depth.texture.Get();");
    CHECK(swapProduction != dlss && Wrong(swapProduction, kDlss) > 0);
    const std::string swapCapture = Replace(dlss, "request.composed = g.composed.texture.Get(); request.depth = g.depth.texture.Get(); request.motion = g.motion.texture.Get();",
                                            "request.composed = g.composed.texture.Get(); request.depth = g.motion.texture.Get(); request.motion = g.depth.texture.Get();");
    CHECK(swapCapture != dlss && Wrong(swapCapture, kDlss) > 0);
    // the pre-SR answer without the texture the evaluate takes
    const std::string viewOnly = Replace(dlss, "result.view != nullptr && result.texture != nullptr", "result.view != nullptr");
    CHECK(viewOnly != dlss && Wrong(viewOnly, kDlss) > 0);

    // Statements moved out of their place keep every count and break the order: the slot's reset taken after the decision, the colour source stored
    // before the decision, the evaluate called after the failure check.
    const std::string resetLate = Replace(Replace(dlss, "in.reset = g_slots.ShouldReset(index, view.now);", ""), "nr::DecideSrEvaluate(PlumbingNow(),",
                                          "nr::DecideSrEvaluate(PlumbingNow(), in.reset = g_slots.ShouldReset(index, view.now);");
    CHECK(resetLate != dlss && Wrong(resetLate, kDlss) == 0 && WrongOrder(resetLate, kDlssOrder) > 0);
    const std::string storeEarly = Replace(Replace(dlss, "if (ev.ok) g.lastColour = ev.colour;", ""), "nr::DecideSrEvaluate(PlumbingNow(),",
                                           "if (ev.ok) g.lastColour = ev.colour; nr::DecideSrEvaluate(PlumbingNow(),");
    CHECK(storeEarly != dlss && Wrong(storeEarly, kDlss) == 0 && WrongOrder(storeEarly, kDlssOrder) > 0);
    const std::string evaluateLate = Replace(Replace(dlss, "result = evaluate(context, g.handle, g.params, nullptr);", ""), "if (!ev.ok)",
                                             "result = evaluate(context, g.handle, g.params, nullptr); if (!ev.ok)");
    CHECK(evaluateLate != dlss && Wrong(evaluateLate, kDlss) == 0 && WrongOrder(evaluateLate, kDlssOrder) > 0);
    // a fragment kept alive only by an `#if 0` block or a log string does not satisfy its count
    const std::string deadBlock = Replace(dlss, "g_nrProductionThisRender = render.composedFromNr;", "");
    CHECK(deadBlock != dlss);
    CHECK(Wrong(Code(dlssSource + "\n#if 0\n g_nrProductionThisRender = render.composedFromNr;\n#endif\n"), kDlss) == 0);
    CHECK(Wrong(Code(dlssSource + "\nconst char* kText = \"g.preSrColour = render.preSrColour;\";\n"), kDlss) == 0);
    {
        std::string withoutLine = dlssSource;
        const size_t at = withoutLine.find("g.preSrColour = render.preSrColour;");
        CHECK(at != std::string::npos);
        if (at != std::string::npos)
        {
            withoutLine.replace(at, std::string("g.preSrColour = render.preSrColour;").size(), "");
            CHECK(Wrong(Code(withoutLine), kDlss) > 0); // the pin is real ...
            CHECK(Wrong(Code(withoutLine + "\nconst char* kText = \"g.preSrColour = render.preSrColour;\";\n"), kDlss) > 0); // ... and a literal does not stand in for it
            CHECK(Wrong(Code(withoutLine + "\n#if 0\ng.preSrColour = render.preSrColour;\n#endif\n"), kDlss) > 0); // ... nor does a dead block
        }
    }
    return TEST_RESULT("chara_dlss_glue_text");
}
