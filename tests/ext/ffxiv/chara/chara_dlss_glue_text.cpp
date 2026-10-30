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
#include <filesystem>
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
    // A focus pause (RunOutcome::Paused) is on purpose, not a fallback: it is neither counted (CountsAsFallback) nor logged under the generic cause text of the
    // two entries (the focus gate writes its own episode lines).
    // The test is "not Paused", NOT CountsAsFallback: Ran without an image (the module's own "cannot happen" case) is not counted either, but it must keep its
    // once-per-cause safety-net line.
    { "if (result.outcome != nr::RunOutcome::Paused && g_productionCauses.First(static_cast<unsigned>(result.outcome)))", 1, "a missing image is logged as a production cause for every outcome but a pause" },
    { "if (result.outcome != nr::RunOutcome::Paused && g_preSrCauses.First(static_cast<unsigned>(result.outcome)))", 1, "a missing image is logged as a pre-SR cause for every outcome but a pause" },
    { "CountsAsFallback(result.outcome) && g_", 0, "the cause-log conditions do not use the counting predicate (Ran without an image would be silenced)" },
    // The processed set's wiring: the pass is named by the camera constant buffer binding of its first geometry draw, every finisher teaches
    // that binding, the guides come from the draw the replay accepted, an image outside the set keeps no slot, and every NR request carries the
    // window label.
    { "const camera::Stage stage = camera::StageFor(view.kind == DrawKind::Prepass);", 1, "the stage of the camera block follows the draw that starts the pass" },
    { "g_pass.cameraKey = camera::BoundBlock(view.context, stage, g_pass.width, g_pass.height);", 1,
      "the binding of the first geometry draw names the image of the pass (no video memory is read)" },
    { "const WindowSet::PassName name = windows.NamePass(g_pass.cameraKey, view.now);", 1, "the pass asks the identity about the binding" },
    { "g_pass.nameImage = name.image;", 1, "the pass remembers the image the binding named, so its finisher can prove whether the name was right" },
    { "g_windowStats.NoteIdentity(name.name);", 1, "every pass the identity decided is counted, whatever the answer" },
    { "FollowPass(view, name.image, windows.FollowsPass(name.name));", 1, "the pass is followed by the identity's answer, not by a prediction" },
    { "if (!g_pass.identified && sr && Windows().On() && (view.kind == DrawKind::Prepass || view.kind == DrawKind::GBuffer))", 1,
      "the identity is asked only while the SR path runs, only with the budget on, and only at a geometry draw" },
    { "const WindowSet::Learned learned = Windows().NoteFinisher(key, g_pass.cameraKey, view.now);", 1,
      "every accepted finisher teaches which camera block binding belongs to which preview image" },
    { "if (Windows().NoteMismatch(g_pass.cameraKey, g_pass.nameImage, key, view.now))", 1,
      "a finisher of another image than the binding named is a mismatch, and the binding is held for that image" },
    { "g_windowStats.NoteIdentityMismatch();", 1, "a mismatch is counted, so the burst line says whether a pass was mis-named" },
    { "g_windowStats.NoteIdentityLearned();", 1, "a binding learned at a finisher is counted" },
    { "g_windowStats.NoteIdentityCleared();", 2, "an ambiguity that ended is counted where the pass saw it end and where the finisher did" },
    { "g_slots.ClearEvaluated(followed);", 0, "a mismatch clears nothing: the image the binding named is usually an active window, and its own next pass clears its composed picture when that pass is not evaluated" },
    { "if (motion::CapturesGuides((view.roles & RoleGeometry) != 0, replay.outcome == motion::Outcome::Replayed) && !g_pass.cameraCaptured)", 1,
      "a draw the replay accepted gives the pass its guides, so a wider-shape pass can finish" },
    { "g_slots.Cleanup(now, disabled, Windows().On(),", 1, "a cleanup forgets a slot's place in the render order only with the budget on" },
    { "if (Windows().On() && !Windows().Processed(key))", 1, "an image the set no longer processes gives its slot back and is never evaluated again" },
    { "if (index != kNoSlot && !windows.Processed(key))", 1, "a UI draw that samples an image outside the set is given the game's picture" },
    { "request.entry = view.facts.rtv0Entry;", 1, "the bring-up request carries the window label" },
    { "request.base.entry = view.facts.rtv0Entry;", 2, "the production and pre-SR requests carry the window label" },
    { "g_windowSet.FreshCount(GetTickCount64())", 1, "the menu count is the windows that are on screen, not every member" },
    { "g_windowSet.Reset();", 1, "a rebuilt device forgets the windows and bindings the set remembered" },
    { "g_windowStats.NoteSample(key, entry, view.now); windows.NoteDemand(key, entry, view.now);", 1,
      "demand is recorded right beside the sample count, for every sampled preview image and whatever the other options say" },
    { "g_pass.live = g_slots.JitterLive(view.now) && follows;", 1, "the pass gate is the live-slot rule AND the processed set's answer" },
    { "&& windows.Processed(key))", 1, "an image outside the set claims no slot" },
    { "if (index != kNoSlot && Windows().On() && g_windowStats.LastFinisherCollided())", 1,
      "the collision reset runs for a collided finisher only" },
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
    { "[](RunOutcome outcome) { return !CountsAsFallback(outcome); }", 1, "a call that ended in a focus pause is not counted as a render that kept the image" },
    { "RunCopiedImpl(request, Placement::PostSr)", 1, "the production entry runs the post-SR placement" },
    { "RunCopiedImpl(request, Placement::PreSr)", 1, "the pre-SR entry runs the pre-SR placement" },
    { "!ResultTextureServes(ResultKindOf(st))", 1, "the dump is not offered a result it cannot label" },
    // The processed set: only an image the set processes owns a stream, the eviction and the shortened linger run only with the budget on,
    // and the allocator of a list comes from the pure rule of the ring.
    { "if (!g_windowSet.Processed(key))", 1, "an image outside the set gets no NR stream" },
    { "if (index == kNoStream && g_windowSet.On())", 1, "a full stream table is only emptied with the budget on" },
    { "const bool pressure = g_windowSet.On() && g_windowSet.HasWaitingCandidate(now);", 1, "the linger is shortened only under pressure and only with the budget on" },
    { "const size_t free = FreeAllocators(m.allocatorValue, m.allocatorNext,", 1, "the allocator of the next list comes from the pure ring rule" },
};

// CharaIdentify.cpp: what the burst log reads and prints. The NR part of the line is built by NrBurstText from the module's counters.
const std::vector<Fragment> kIdentify = {
    { "const nr::Status nr = nr::GetStatus();", 2, "the burst start and the burst end read the NR counters" },
    { "nrBurst.skipTableFull = nr.skipTableFull - g_state.startNrSkipTableFull;", 1, "the line prints why calls were skipped, per cause, for this burst" },
    { "nrBurst.skipSettling = nr.skipSettling - g_state.startNrSkipSettling;", 1, "... settling" },
    { "nrBurst.skipNoList = nr.skipNoList - g_state.startNrSkipNoList;", 1, "... no command list" },
    { "nrBurst.streamsPeak = nr.streamsPeak;", 1, "... the stream peak" },
    { "NrBurstText(nrBurst)", 1, "the line itself is the tested formatter" },
    { "if (NrBurstWorthLogging(nrBurst))", 1, "the NR part is written only when it says something (preview NR off prints nothing)" },
    { "IdentityBurstText(identity)", 1, "the identity part of the burst line is the tested formatter" },
    { "identity.known = static_cast<uint32_t>(g_windowSet.KnownBlocks());", 1, "the line counts the camera blocks that name a window now" },
    { "if (g_windowSet.On())", 1, "the identity part is written only with the limit on (off: nothing is learned or named)" },
    { "g_windowStats.Reset();", 1, "the several-window counters belong to one burst and start at zero in the next one" },
    { "s.startNrRuns = nr.runs;", 1, "the burst remembers the NR counters it started from, so the line shows that burst" },
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

// A Map of a staging copy that waits for the GPU: a read without D3D11_MAP_FLAG_DO_NOT_WAIT returns only once every command queued before the copy
// has executed, which drains the pipeline in the middle of a frame. The feature's readbacks retry on a later call instead, and a preview window is
// identified by the constant buffer binding it draws with, never by the content of that buffer.
bool BlockingReadback(const std::string& line)
{
    return line.find("D3D11_MAP_READ") != std::string::npos && line.find("D3D11_MAP_FLAG_DO_NOT_WAIT") == std::string::npos;
}

// How many lines of the preview sources map for read without DO_NOT_WAIT.
int BlockingReadbacks(const std::string& folder)
{
    int n = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".cpp" && path.extension() != ".h")
            continue;
        std::istringstream lines(ReadFile(path.string()));
        std::string line;
        while (std::getline(lines, line))
            n += BlockingReadback(line) ? 1 : 0;
    }
    return n;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string dir = repo + "/OptiScaler/ext/ffxiv/chara/";
    const std::string dlssSource = ReadFile(dir + "CharaDlss.cpp");
    const std::string nrSource = ReadFile(dir + "CharaNr.cpp");
    const std::string identifySource = ReadFile(dir + "CharaIdentify.cpp");
    CHECK(!dlssSource.empty() && !nrSource.empty() && !identifySource.empty());

    const std::string dlss = Code(dlssSource);
    const std::string nr = Code(nrSource);
    const std::string identify = Code(identifySource);
    Report("CharaDlss.cpp", dlss, kDlss);
    Report("CharaNr.cpp", nr, kNr);
    Report("CharaIdentify.cpp", identify, kIdentify);
    CHECK(PlanFieldAssignments(nr) == 0);
    CHECK(WrongOrder(dlss, kDlssOrder, true) == 0);
    // The two slot flags are pinned by their number of assignments too, not only by fragment and order: an unconditional second `g.lastColour = ...;` or
    // `g.preSrColour = ...;` elsewhere in the file would leave every count-1 fragment true.
    CHECK(AssignmentsTo(dlss, "g.lastColour") == 1);   // the store after a successful evaluate
    CHECK(AssignmentsTo(dlss, "g.preSrColour") == 2);  // the clear before the slot step and the copy-back after the alpha pass
    // the stripper does not understand raw strings: none may appear in the files it reads
    CHECK(dlssSource.find("R\"(") == std::string::npos && nrSource.find("R\"(") == std::string::npos &&
          identifySource.find("R\"(") == std::string::npos);

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

    // No readback of the preview path waits for the GPU: a blocking Map on the immediate context is a pipeline drain, and the identity of a
    // preview window is taken from the constant buffer binding alone (the negative control shows what the check catches).
    CHECK(BlockingReadback("context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);"));
    CHECK(!BlockingReadback("context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);"));
    CHECK(!BlockingReadback("context->Map(staging, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);"));
    CHECK(!BlockingReadback("// a read back with D3D11_MAP_FLAG_DO_NOT_WAIT on a later call"));
    CHECK(BlockingReadbacks(dir) == 0);

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
    // The processed set: a prediction instead of the image of the pass, a deleted learn at the finisher, an unlabelled NR request, and an NR
    // burst line that does not print the skip causes.
    const std::string predicted = Replace(dlss, "FollowPass(view, name.image, windows.FollowsPass(name.name));", "FollowPass(view, 0, true);");
    CHECK(predicted != dlss && Wrong(predicted, kDlss) > 0);
    const std::string noLearn = Replace(dlss, "const WindowSet::Learned learned = Windows().NoteFinisher(key, g_pass.cameraKey, view.now);", "");
    CHECK(noLearn != dlss && Wrong(noLearn, kDlss) > 0);
    const std::string noLabel = Replace(dlss, "request.base.entry = view.facts.rtv0Entry;", "");
    CHECK(noLabel != dlss && Wrong(noLabel, kDlss) > 0);
    // The processed set's call sites: demand under the NR option, the slot gate, the pass gate and the collision reset.
    const std::string noDemand = Replace(dlss, "g_windowStats.NoteSample(key, entry, view.now); windows.NoteDemand(key, entry, view.now);",
                                         "g_windowStats.NoteSample(key, entry, view.now); if (previewOutput) windows.NoteDemand(key, entry, view.now);");
    CHECK(noDemand != dlss && Wrong(noDemand, kDlss) > 0);
    const std::string openSlot = Replace(dlss, "&& windows.Processed(key))", ")");
    CHECK(openSlot != dlss && Wrong(openSlot, kDlss) > 0);
    const std::string alwaysFollow = Replace(dlss, "g_pass.live = g_slots.JitterLive(view.now) && follows;", "g_pass.live = g_slots.JitterLive(view.now);");
    CHECK(alwaysFollow != dlss && Wrong(alwaysFollow, kDlss) > 0);
    const std::string resetEvery = Replace(dlss, "if (index != kNoSlot && Windows().On() && g_windowStats.LastFinisherCollided())",
                                           "if (index != kNoSlot && Windows().On())");
    CHECK(resetEvery != dlss && Wrong(resetEvery, kDlss) > 0);
    const std::string noCause = Replace(identify, "nrBurst.skipTableFull = nr.skipTableFull - g_state.startNrSkipTableFull;", "");
    CHECK(noCause != identify && Wrong(noCause, kIdentify) > 0);
    // The identity's feed: the name the pass remembered, the mismatch, and the clearing of another window's picture the mismatch must not do.
    // Deleting any of them leaves every pure rule green and makes the burst line lie about what happened.
    const std::string noName = Replace(dlss, "g_pass.nameImage = name.image;", "");
    CHECK(noName != dlss && Wrong(noName, kDlss) > 0);
    const std::string noMismatch = Replace(dlss, "if (Windows().NoteMismatch(g_pass.cameraKey, g_pass.nameImage, key, view.now))", "if (false)");
    CHECK(noMismatch != dlss && Wrong(noMismatch, kDlss) > 0);
    const std::string noMismatchCount = Replace(dlss, "g_windowStats.NoteIdentityMismatch();", "");
    CHECK(noMismatchCount != dlss && Wrong(noMismatchCount, kDlss) > 0);
    const std::string noNameCount = Replace(dlss, "g_windowStats.NoteIdentity(name.name);", "");
    CHECK(noNameCount != dlss && Wrong(noNameCount, kDlss) > 0);
    const std::string noLearnedCount = Replace(dlss, "g_windowStats.NoteIdentityLearned();", "");
    CHECK(noLearnedCount != dlss && Wrong(noLearnedCount, kDlss) > 0);
    const std::string noClearedCount = Replace(dlss, "g_windowStats.NoteIdentityCleared();", "");
    CHECK(noClearedCount != dlss && Wrong(noClearedCount, kDlss) > 0); // both sites count
    const std::string identifyWithoutSr = Replace(dlss, "if (!g_pass.identified && sr && Windows().On()", "if (!g_pass.identified && Windows().On()");
    CHECK(identifyWithoutSr != dlss && Wrong(identifyWithoutSr, kDlss) > 0);
    const std::string clearFollowed = Replace(dlss, "g_windowStats.NoteIdentityMismatch();",
                                             "g_windowStats.NoteIdentityMismatch(); const int followed = g_slots.Find(g_pass.nameImage); if (followed != kNoSlot) g_slots.ClearEvaluated(followed);");
    CHECK(clearFollowed != dlss && Wrong(clearFollowed, kDlss) > 0); // a mismatch must not clear another window's picture
    const std::string noSetReset = Replace(dlss, "g_windowSet.Reset();", "");
    CHECK(noSetReset != dlss && Wrong(noSetReset, kDlss) > 0);

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
