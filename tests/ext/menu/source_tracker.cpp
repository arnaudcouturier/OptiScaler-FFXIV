// ext::menu::SourceTracker (draw each shared menu function once per tab pass) and the one place upstream's tab loop uses it.
//
//  1. The tracker: first claim true, repeats false, interleaved sources, null source, a fresh tracker per pass, capacity.
//  2. A model of upstream's tab loop, old logic (lastSource) and new logic (tracker): the old logic draws a shared function
//     twice when a fork card sits inside its group (the bug of BACKLOG B7), the new logic draws it once.
//  3. Text pins on menu_common.cpp (comments stripped): the loop declares the tracker, asks it before calling a source
//     function, and no `lastSource` is left.
#include "ext/core/SourceTracker.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

using Fn = void (*)(int&);
// distinct bodies: identical functions may be folded into one address by the linker
void SrcA(int& x)
{
    x = 1;
}
void SrcB(int& x)
{
    x = 2;
}
void SrcC(int& x)
{
    x = 3;
}

// ---- 1. the tracker -----------------------------------------------------------------------------------------------
void TrackerBasics()
{
    ext::menu::SourceTracker<Fn> t;
    CHECK(t.Claim(SrcA));
    CHECK(!t.Claim(SrcA));
    CHECK(!t.Claim(SrcA));

    // interleaved sources: each one is claimed once, whatever the order
    ext::menu::SourceTracker<Fn> u;
    CHECK(u.Claim(SrcA));
    CHECK(u.Claim(SrcB));
    CHECK(!u.Claim(SrcA));
    CHECK(!u.Claim(SrcB));
    CHECK(u.Claim(SrcC));
    CHECK(!u.Claim(SrcA));
    CHECK(!u.Claim(SrcC));

    // a null source has nothing to draw: never claimed, and it does not disturb the others
    ext::menu::SourceTracker<Fn> n;
    CHECK(!n.Claim(nullptr));
    CHECK(!n.Claim(nullptr));
    CHECK(n.Claim(SrcA));
    CHECK(!n.Claim(nullptr));
    CHECK(!n.Claim(SrcA));

    // a fresh tracker per pass: the next pass draws the function again
    ext::menu::SourceTracker<Fn> pass2;
    CHECK(pass2.Claim(SrcA));
    CHECK(pass2.Claim(SrcB));
}

// distinct bodies, so the linker cannot fold the functions into one address
template <int N> void Gen(int& x)
{
    x = N;
}

template <int... I> void Overflow(std::integer_sequence<int, I...>)
{
    constexpr size_t cap = ext::menu::SourceTracker<Fn>::kCapacity;
    static_assert(sizeof...(I) >= cap + 2, "need more distinct functions than the capacity");
    const Fn fns[] = { Gen<I>... };

    ext::menu::SourceTracker<Fn> t;
    for (size_t i = 0; i < cap; i++)
        CHECK(t.Claim(fns[i])); // the first kCapacity distinct functions are tracked
    for (size_t i = 0; i < cap; i++)
        CHECK(!t.Claim(fns[i]));

    // beyond the capacity the tracker fails open: the function is drawn, never hidden
    CHECK(t.Claim(fns[cap]));
    CHECK(t.Claim(fns[cap]));
    CHECK(t.Claim(fns[cap + 1]));
    // and the tracked ones are still remembered
    CHECK(!t.Claim(fns[0]));
    CHECK(!t.Claim(fns[cap - 1]));
}

// ---- 2. model of the upstream tab loop ----------------------------------------------------------------------------
struct Box
{
    const char* id;
    const char* tab;
    bool hasDraw; // fork cards and lambda cards draw themselves
    Fn source;    // shared function when !hasDraw
};

struct Drawn
{
    int a = 0, b = 0;
    std::vector<std::string> own; // boxes that drew themselves, in order
};

void Call(Drawn& d, Fn f)
{
    if (f == SrcA)
        d.a++;
    else if (f == SrcB)
        d.b++;
}

// The loop as it was: `lastSource` is set after every box, a box with its own draw resets it to null
Drawn LegacyPass(const std::vector<Box>& boxes, const char* tab)
{
    Drawn d;
    Fn lastSource = nullptr;
    for (const Box& box : boxes)
    {
        if (std::string(box.tab) != tab)
            continue;
        if (!box.hasDraw)
        {
            if (box.source != lastSource)
                Call(d, box.source);
        }
        else
            d.own.push_back(box.id);
        lastSource = box.hasDraw ? nullptr : box.source;
    }
    return d;
}

// The loop with the tracker
Drawn TrackedPass(const std::vector<Box>& boxes, const char* tab)
{
    Drawn d;
    ext::menu::SourceTracker<Fn> drawn;
    for (const Box& box : boxes)
    {
        if (std::string(box.tab) != tab)
            continue;
        if (!box.hasDraw)
        {
            if (drawn.Claim(box.source))
                Call(d, box.source);
        }
        else
            d.own.push_back(box.id);
    }
    return d;
}

void LoopModel()
{
    // Frame Gen today: a fork card (auto MFG, window mode) between two boxes of the runtime function
    const std::vector<Box> insideGroup = {
        { "fg_selection", "Frame Gen", true, nullptr },
        { "fg_fsrfg", "Frame Gen", false, SrcA },
        { "fg_xefg", "Frame Gen", false, SrcA },
        { "fg_dlssg", "Frame Gen", false, SrcA },
        { "fg_auto_mfg", "Frame Gen", true, nullptr },
        { "window_mode", "Frame Gen", true, nullptr },
        { "fg_reprojection", "Frame Gen", false, SrcA },
        { "fg_sl_inputs", "Frame Gen", false, SrcA },
        { "mipmap", "Textures", false, SrcB },
        { "mipmap_auto", "Textures", true, nullptr },
        { "anisotropy", "Textures", false, SrcB },
    };
    CHECK(LegacyPass(insideGroup, "Frame Gen").a == 2); // the double draw of BACKLOG B7
    CHECK(LegacyPass(insideGroup, "Textures").b == 2);
    CHECK(TrackedPass(insideGroup, "Frame Gen").a == 1);
    CHECK(TrackedPass(insideGroup, "Textures").b == 1);
    // the cards with their own draw still draw once each
    const Drawn fg = TrackedPass(insideGroup, "Frame Gen");
    CHECK(fg.own.size() == 3 && fg.own[0] == "fg_selection" && fg.own[1] == "fg_auto_mfg" && fg.own[2] == "window_mode");

    // fork cards before or after the group: one draw with either logic
    const std::vector<Box> aroundGroup = {
        { "fg_selection", "Frame Gen", true, nullptr },
        { "fg_fsrfg", "Frame Gen", false, SrcA },
        { "fg_xefg", "Frame Gen", false, SrcA },
        { "fg_auto_mfg", "Frame Gen", true, nullptr },
    };
    CHECK(LegacyPass(aroundGroup, "Frame Gen").a == 1);
    CHECK(TrackedPass(aroundGroup, "Frame Gen").a == 1);

    // two groups of different functions, each once, even when they alternate
    const std::vector<Box> alternating = {
        { "x1", "T", false, SrcA }, { "y1", "T", false, SrcB }, { "x2", "T", false, SrcA }, { "y2", "T", false, SrcB },
    };
    const Drawn alt = TrackedPass(alternating, "T");
    CHECK(alt.a == 1 && alt.b == 1);

    // boxes of other tabs are skipped and do not matter
    CHECK(TrackedPass(insideGroup, "Menu").a == 0);
}

// ---- 3. text pins on upstream's loop ------------------------------------------------------------------------------
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removes // and /* */ comments; string and character literals are kept as they are
std::string StripComments(const std::string& s)
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
            out += s[i++];
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            if (i < s.size())
                out += s[i++];
        }
        else
            out += s[i++];
    }
    return out;
}

std::string Squash(const std::string& s)
{
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The scope of the tracker in upstream's tab loop, on whitespace-free, comment-free text. It must be a plain local that is
// declared ONCE per tab pass, as the statement right before the box loop (a `static` or a tracker declared inside the
// loop body would be wrong in opposite ways: shared functions drawn only on the first frame, or drawn again for every
// box), and the loop body must claim a shared function exactly once, with the box's own draw as the other branch.
std::vector<std::string> TrackerScopeProblems(const std::string& code)
{
    std::vector<std::string> p;
    const std::string decl = "ext::menu::SourceTracker<DrawFn>drawnSources;";
    const std::string loop = "for(size_ti=0;i<boxCount;i++){";

    if (Count(code, decl) != 1)
        p.push_back("the tracker is not declared exactly once");
    // `;` + declaration + loop: nothing (no static, thread_local, constexpr) in front of the type, nothing between it and the loop
    if (Count(code, ";" + decl + loop) != 1)
        p.push_back("the tracker is not an unqualified statement right before the box loop");

    const size_t at = code.find(loop);
    if (at == std::string::npos)
    {
        p.push_back("the box loop was not found");
        return p;
    }

    // the loop body, by brace matching
    const size_t open = at + loop.size() - 1;
    size_t depth = 0, close = std::string::npos;
    for (size_t i = open; i < code.size() && close == std::string::npos; i++)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            close = i;
    }
    if (close == std::string::npos)
    {
        p.push_back("the box loop body is not closed");
        return p;
    }
    const std::string body = code.substr(open, close - open + 1);

    if (body.find("SourceTracker") != std::string::npos)
        p.push_back("a tracker is declared inside the loop body");
    if (Count(code, "drawnSources.Claim(") != 1)
        p.push_back("the tracker is claimed other than exactly once");
    if (Count(body, "if(boxes[i].draw==nullptr){if(drawnSources.Claim(boxes[i].source))boxes[i].source(ctx);}"
                    "else{boxes[i].draw(ctx);}") != 1)
        p.push_back("the loop body does not claim a shared function once, with the box's own draw as the other branch");
    return p;
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
    const size_t at = s.find(from);
    if (at != std::string::npos)
        s.replace(at, from.size(), to);
    return s;
}

void UpstreamLoopPins(const std::string& repo)
{
    const std::string raw = ReadFile(repo + "/OptiScaler/menu/menu_common.cpp");
    CHECK(!raw.empty());
    const std::string code = Squash(StripComments(raw));

    // the tracker is declared once per tab pass, outside the loop, not static; the loop claims each shared function once
    const auto problems = TrackerScopeProblems(code);
    for (const auto& s : problems)
        std::printf("  tracker scope: %s\n", s.c_str());
    CHECK(problems.empty());

    // negative controls: the same text with the scope broken must complain
    const std::string decl = "ext::menu::SourceTracker<DrawFn>drawnSources;";
    const std::string loop = "for(size_ti=0;i<boxCount;i++){";
    CHECK(!TrackerScopeProblems(Replace(code, decl, "static" + decl)).empty());
    CHECK(!TrackerScopeProblems(Replace(code, decl, "thread_local" + decl)).empty());
    CHECK(!TrackerScopeProblems(Replace(Replace(code, decl, ""), loop, loop + decl)).empty()); // inside the loop body
    CHECK(!TrackerScopeProblems(Replace(Replace(code, decl, ""), loop, loop + "staticconstintx=0;" + decl)).empty());
    CHECK(!TrackerScopeProblems(Replace(code, "if(drawnSources.Claim(boxes[i].source))", "if(true)")).empty());
    CHECK(!TrackerScopeProblems(Replace(code, "else{boxes[i].draw(ctx);}", "")).empty());
    CHECK(!TrackerScopeProblems(Replace(code, decl, "")).empty());
    CHECK(!TrackerScopeProblems(Replace(code, loop, "for(size_ti=0;i<boxCount;i++){drawnSources=ext::menu::SourceTracker<DrawFn>();")).empty());
    CHECK(TrackerScopeProblems("junk").size() >= 1);

    // the old run logic is gone
    CHECK(code.find("lastSource") == std::string::npos);
    // the Custom tab path (one card, filtered) is untouched
    CHECK(Count(code, "ScopedCard::SetFilter(box.id);") == 1);

    // the pin helpers themselves: a pin kept alive only by a comment must not count
    CHECK(Squash(StripComments("a // lastSource = x;\nb")).find("lastSource") == std::string::npos);
    CHECK(Squash(StripComments("a /* lastSource */ b")).find("lastSource") == std::string::npos);
    CHECK(Squash(StripComments("a \"//x\" b")).find("\"//x\"") != std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    TrackerBasics();
    Overflow(std::make_integer_sequence<int, 20> {});
    LoopModel();
    UpstreamLoopPins(repo);

    if (failures)
    {
        std::printf("source_tracker: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("source_tracker: all passed\n");
    return 0;
}
