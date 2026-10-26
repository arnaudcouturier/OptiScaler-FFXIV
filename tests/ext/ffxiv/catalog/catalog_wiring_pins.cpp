// Where the catalog accounting is wired in, and the literals it mirrors.
//
// The ledger (CatalogLedger.h) is pure and tested on its own. What a unit test cannot run is the game side: the CreatePixelShader hook, the
// CreateInputLayout tagging and the HUD-less provider live in headers and .cpp files that need D3D, Detours and the OptiScaler pch. So the
// properties that matter there are pinned against the SOURCE TEXT of those files (the pattern of ngx_lock_order_pins):
//   * the count happens in the always-run part of the creation hook: inside the DXBC test, reusing the CRC the hook already computed, with no
//     Armed()/Wanted()/Current() condition in between (counting only while armed is the S10-E3 late-arming bug);
//   * UiBoundaryProvider::Take() answers NotThisFrame for a partial UI catalog AFTER the Mode test and BEFORE it consults the live source
//     (consulting it would arm ReShade at a boundary that is wrong when the catalog is partial); LastReason() agrees; the guard is\n//     pinned as the exact statement if (!UiBoundaryUsable()) + eturn NotThisFrame; (whitespace and comments normalised), so a dropped\n//     ! fails here;
//   * the Present tick runs before the Armed() test of OnPresentBegin (default-ini sessions must get the catalog line);
//   * the time base is set before the creation detour is committed;
//   * the CRC literals the table mirrors from the heavy headers (lighting, companion, nameplate) are still the owners' values.
// It also runs the inline glue (FfxivCatalog.h) against the global ledger.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "check.h"

#include "ext/ffxiv/catalog/FfxivCatalog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ext::ffxiv::catalog;

namespace
{
std::string g_root;

std::string ReadText(const std::string& relative)
{
    std::ifstream f(g_root + "/" + relative, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end()); // the working tree may hold CRLF
    return s;
}

// The text between two markers (empty, with a failed check, when either is missing).
std::string Between(const std::string& text, const char* from, const char* to, const char* what)
{
    const size_t a = text.find(from);
    if (a == std::string::npos)
    {
        std::printf("pin: start marker for %s not found: %s\n", what, from);
        ++g_failures;
        return {};
    }
    const size_t b = text.find(to, a + 1);
    if (b == std::string::npos)
    {
        std::printf("pin: end marker for %s not found: %s\n", what, to);
        ++g_failures;
        return {};
    }
    return text.substr(a, b - a);
}

size_t CountOf(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

// ---- source text helpers for the exact pins -------------------------------------------------------------------------------------
// Comments removed (string and character literals are respected), so a word in a comment can neither satisfy nor break a pin.
std::string NoComments(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            i += 2;
            continue;
        }
        const bool digitSeparator = c == '\'' && i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]));
        if (c == '"' || (c == '\'' && !digitSeparator))
        {
            out += c;
            for (++i; i < s.size() && s[i] != c; ++i)
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i];
            }
            if (i < s.size())
                out += s[i++];
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Every whitespace character removed: a reformat (clang-format, a wrapped line) cannot change what a pin sees. Applied to the needle too.
std::string Squash(const std::string& s)
{
    std::string out;
    for (const char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)))
            out += c;
    return out;
}
std::string Code(const std::string& s) { return Squash(NoComments(s)); }

// `kw` as a whole word in comment-free text (so "specific" or "notified" is not an `if`).
bool HasKeyword(const std::string& text, const std::string& kw)
{
    for (size_t at = text.find(kw); at != std::string::npos; at = text.find(kw, at + 1))
    {
        const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        const bool before = at > 0 && ident(text[at - 1]);
        const bool after = at + kw.size() < text.size() && ident(text[at + kw.size()]);
        if (!before && !after)
            return true;
    }
    return false;
}

// Is there any construct in this stretch of code that could make what follows conditional?
bool HasCondition(const std::string& commentFree)
{
    for (const char* kw : { "if", "else", "switch", "while", "for", "return", "goto", "case", "break", "continue" })
        if (HasKeyword(commentFree, kw))
            return true;
    return commentFree.find('?') != std::string::npos || commentFree.find("&&") != std::string::npos || commentFree.find("||") != std::string::npos;
}

// The helpers themselves, shown non-vacuous: what they must catch and what they must let through.
void HelperSelfTest()
{
    CHECK(Code("if (x)   {\n  y ; } // verify specific") == "if(x){y;}");
    CHECK(Code("/* if (a) */ b /* c */;") == "b;");
    CHECK(Code("s = \"a // b\"; // c") == "s=\"a//b\";"); // a comment marker inside a string literal is text
    CHECK(Code("n = 0xFFFF'FFFF; // c") == "n=0xFFFF'FFFF;");
    CHECK(HasCondition(NoComments("x; if (y) z;")));
    CHECK(HasCondition(NoComments("x; if(y) z;")));
    CHECK(HasCondition(NoComments("a ? b : c;")));
    CHECK(HasCondition(NoComments("a && b;")));
    CHECK(HasCondition(NoComments("x; else y;")));
    CHECK(!HasCondition(NoComments("// verify specific notified\n f(x); /* if (a) */ g();")));
    CHECK(!HasCondition(NoComments("specific(); notified(); verifyIf(); diff = 1;")));
    CHECK(CountOf("aXbXc", "X") == 2);
}

// position of `needle` in `text`, or npos with a failed check
size_t Pos(const std::string& text, const std::string& needle, const char* what)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos)
    {
        std::printf("pin: %s: \"%s\" not found\n", what, needle.c_str());
        ++g_failures;
    }
    return at;
}

void MirroredLiterals()
{
    // lighting: FfxivLightingCapture::shaders, "name", bytes, crc
    const std::string lighting = ReadText("OptiScaler/ext/ffxiv/lighting/FfxivLightingCapture.h");
    CHECK(lighting.find("std::array<Shader,9> shaders") != std::string::npos);
    for (size_t i = 0; i < Info(Catalog::Lighting).count; ++i)
    {
        const Entry& e = Info(Catalog::Lighting).entries[i];
        const std::string needle = "{\"" + std::string(e.name) + "\", " + std::to_string(e.bytes) + "u, " + std::to_string(e.crc) + "u,";
        if (lighting.find(needle) == std::string::npos)
        {
            std::printf("lighting literal not found in FfxivLightingCapture.h: %s\n", needle.c_str());
            ++g_failures;
        }
    }

    // companion: CompanionGpu::Identify, four `case <crc>u:`
    const std::string companion = ReadText("OptiScaler/ext/ffxiv/companion/CompanionGpu.h");
    const std::string identify = Between(companion, "inline UINT Identify(", "inline void TagShader", "companion Identify");
    CHECK_EQ(CountOf(identify, "case "), Info(Catalog::Companion).count);
    for (size_t i = 0; i < Info(Catalog::Companion).count; ++i)
        CHECK(identify.find("case " + std::to_string(Info(Catalog::Companion).entries[i].crc) + "u:") != std::string::npos);

    // nameplate: FfxivNameplateRenderer::TagLayout membership and the variant check in CaptureMaterial
    const std::string nameplate = ReadText("OptiScaler/ext/ffxiv/companion/replay/FfxivNameplateRenderer.h");
    const auto& np = Info(Catalog::Nameplate);
    CHECK(nameplate.find("crc!=" + std::to_string(np.entries[0].crc) + "u && crc!=" + std::to_string(np.entries[1].crc) + "u && crc!=" +
                         std::to_string(np.entries[2].crc) + "u") != std::string::npos);
    CHECK(nameplate.find("variant==0?" + std::to_string(np.entries[0].crc) + "u:variant==4?" + std::to_string(np.entries[1].crc) + "u:" +
                         std::to_string(np.entries[2].crc) + "u") != std::string::npos);
    // ... and the UI/companion CRCs of Variant()
    const std::string variant = Between(nameplate, "inline int Variant(", "inline constexpr GUID layoutTag", "nameplate Variant");
    CHECK_EQ(CountOf(variant, "case "), Info(Catalog::Companion).count);
    for (size_t i = 0; i < Info(Catalog::Companion).count; ++i)
        CHECK(variant.find("case " + std::to_string(Info(Catalog::Companion).entries[i].crc) + "u:") != std::string::npos);
}

void CreationHook()
{
    const std::string text = ReadText("OptiScaler/ext/ffxiv/lighting/FfxivLightingCapture.h");
    const std::string hook = Between(text, "inline HRESULT STDMETHODCALLTYPE CreateShader(", "template<class F> inline void Around", "CreateShader");
    CHECK_EQ(CountOf(hook, "ext::ffxiv::catalog::OnPixelShaderCreated(code,size,crc)"), 1u);
    CHECK_EQ(CountOf(hook, "Crc(code,size)"), 1u); // the CRC the hook computed once is the one handed over

    // From the DXBC test to the call there is nothing that depends on the detector being armed, wanted, or the game being matched.
    const std::string hookCode = NoComments(hook); // comments do not count: a word in one can neither pass nor fail a pin
    const std::string guarded = Between(hookCode, "!std::memcmp(code,\"DXBC\",4)", "OnPixelShaderCreated", "path from the DXBC test to the count");
    CHECK(!guarded.empty());
    CHECK(guarded.find("Armed(") == std::string::npos);
    CHECK(guarded.find("Wanted(") == std::string::npos);
    CHECK(guarded.find("Current()") == std::string::npos);
    CHECK(guarded.find("registry") == std::string::npos);
    CHECK(!HasCondition(guarded)); // no if/else/switch/loop/return/?:/&&/|| between the DXBC test and the call (a real `if (` is caught, a comment word is not)
    // ... and the path is exactly: the DXBC test closes, the block opens, the CRC is computed once, the count follows. Nothing else.
    CHECK_EQ(CountOf(Code(hook), Squash("!std::memcmp(code,\"DXBC\",4)) { const UINT crc=Crc(code,size); ext::ffxiv::catalog::OnPixelShaderCreated(code,size,crc);")), 1u);
    // The count sits in front of the consumers that can be switched off (it must not depend on them).
    CHECK(Pos(hook, "OnPixelShaderCreated", "count") < Pos(hook, "ui::OnShaderCreated", "ui consumer"));

    // The time base is set before the creation detour is committed.
    const std::string install = Between(text, "inline void Install(ID3D11Device* d)", "inline void DetachTransaction()", "Install");
    const size_t base = Pos(install, "ext::ffxiv::catalog::OnHooksInstalled()", "time base");
    const size_t commit = Pos(install, "DetourTransactionCommit()", "commit");
    CHECK(base != std::string::npos && commit != std::string::npos && base < commit);
}

void NameplateTagging()
{
    const std::string text = ReadText("OptiScaler/ext/ffxiv/companion/replay/FfxivNameplateRenderer.h");
    const std::string tag = Between(text, "inline void TagLayout(", "struct Material", "TagLayout");
    const size_t seen = Pos(tag, "ext::ffxiv::catalog::OnLayoutSeen()", "layout seen");
    const size_t crc = Pos(tag, "const auto crc=Crc(code,size);", "crc");
    const size_t membership = Pos(tag, "crc!=3153679471u) return;", "membership");
    const size_t note = Pos(tag, "ext::ffxiv::catalog::OnNameplateVertexShader(code,size,crc)", "note");
    const size_t layout = Pos(tag, "Layout layout;", "layout");
    CHECK(seen < crc);                      // counted for every layout, whatever the early outs below do
    CHECK(crc < membership && membership < note && note < layout); // only the catalog members reach the note; before the element validation that can return
    CHECK_EQ(CountOf(tag, "Crc(code,size)"), 1u); // no second hash of the bytecode
}

void UiProvider()
{
    const std::string text = ReadText("OptiScaler/ext/ffxiv/ui/FfxivUiBoundary.cpp");

    const std::string take = Between(text, "ext::hudless::TakeResult Take(ext::hudless::FrameImage& out) override", "const char* LastReason() override", "Take");
    const size_t wanted = Pos(take, "ext::hudless::Wanted()", "Mode test");
    const size_t guard = Pos(take, "ext::ffxiv::catalog::UiBoundaryUsable()", "catalog guard");
    const size_t live = Pos(take, "Live().Take(out)", "live source");
    CHECK(wanted < guard && guard < live); // after Mode, before the live source: Live().Take arms ReShade at the boundary for the next frame
    const std::string answer = Between(take, "ext::ffxiv::catalog::UiBoundaryUsable()", "Live().Take(out)", "guard answer");
    CHECK(answer.find("return ext::hudless::TakeResult::NotThisFrame;") != std::string::npos);
    CHECK(answer.find("NoSource") == std::string::npos);
    CHECK(answer.find("Rejected") == std::string::npos);

    // The polarity: the guard is `if (!UiBoundaryUsable())` and it answers NotThisFrame, directly, and directly before the live source. Dropping the
    // `!` would switch HUD-less OFF on every healthy build and leave it running on the broken one, and no other test can see that (Take() needs D3D).
    const std::string takeCode = Code(take);
    const std::string usable = "ext::ffxiv::catalog::UiBoundaryUsable()";
    const std::string notUsable = Squash("if (!" + usable + ")");
    CHECK_EQ(CountOf(takeCode, Squash(usable)), 1u);       // the one and only use in Take()
    CHECK_EQ(CountOf(takeCode, notUsable), 1u);            // ... and it is the negated test
    CHECK_EQ(CountOf(takeCode, Squash("if (" + usable + ")")), 0u);
    CHECK_EQ(CountOf(takeCode, Squash("if (!" + usable + ") return ext::hudless::TakeResult::NotThisFrame; try { return Live().Take(out); }")), 1u);
    CHECK_EQ(CountOf(takeCode, Squash("if (!ext::hudless::Wanted()) return ext::hudless::TakeResult::NotThisFrame;") + notUsable), 1u); // directly after the Mode test

    const std::string reason = Between(text, "const char* LastReason() override", "void EndFrame() override", "LastReason");
    CHECK(Pos(reason, "ext::ffxiv::catalog::UiBoundaryUsable()", "reason guard") < Pos(reason, "Live().LastReason()", "live reason"));
    const std::string reasonCode = Code(reason);
    CHECK_EQ(CountOf(reasonCode, Squash(usable)), 1u);
    CHECK_EQ(CountOf(reasonCode, notUsable + "return\""), 1u); // `if (!UiBoundaryUsable()) return "<text>";`
    CHECK_EQ(CountOf(reasonCode, Squash("if (" + usable + ")")), 0u);
    CHECK(Pos(reasonCode, notUsable, "reason guard polarity") < Pos(reasonCode, Squash("Live().LastReason()"), "live reason"));

    const std::string present = Between(text, "void OnPresentBegin()", "} // namespace ext::ffxiv::ui", "OnPresentBegin");
    const size_t tick = Pos(present, "ext::ffxiv::catalog::OnPresent()", "present tick");
    CHECK(tick < Pos(present, "RefreshArmed()", "refresh"));
    CHECK(tick < Pos(present, "if (!Armed())", "armed test")); // default-ini sessions (nothing armed) still count Presents and log the catalog
}

// The inline glue against the process-wide ledger.
void Glue()
{
    Ledger& g = Global();
    CHECK_EQ(g.UiMask(), 0u);
    CHECK(!UiBoundaryUsable());

    OnHooksInstalled();
    const uint32_t t0 = ElapsedMs();
    CHECK(t0 < 5000); // the base was set just now
    CHECK(ElapsedMs() >= t0); // monotonic
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const uint32_t tStart = ElapsedMs(); // the creations below happen after this
    CHECK(tStart >= t0 + 20); // the clock moved: without it `ms >= tStart` below would also hold for a hard-coded 0

    const std::vector<uint8_t> zeros(2000, 0);
    for (size_t i = 0; i < 5; ++i)
        OnPixelShaderCreated(zeros.data(), 100, Info(Catalog::Ui).entries[i].crc);
    CHECK_EQ(g.Count(Catalog::Ui), 5u);
    CHECK(!UiBoundaryUsable());
    OnPixelShaderCreated(zeros.data(), 100, Info(Catalog::Ui).entries[6].crc); // 6 of 7, any six
    CHECK(UiBoundaryUsable());
    CHECK_EQ(g.SlotAt(Catalog::Ui, 6).present, 0u);
    const uint32_t ms = g.SlotAt(Catalog::Ui, 6).ms;
    CHECK(ms >= tStart && ms <= ElapsedMs()); // the time passed to the ledger is the elapsed time since the base, taken at creation
    CHECK(g.SlotAt(Catalog::Ui, 0).ms >= tStart);

    OnLayoutSeen();
    CHECK_EQ(g.LayoutsSeen(), 1u);
    OnNameplateVertexShader(zeros.data(), 100, Info(Catalog::Nameplate).entries[1].crc);
    CHECK_EQ(g.Mask(Catalog::Nameplate), 2u);
    CHECK(g.SlotAt(Catalog::Nameplate, 1).ms >= tStart && g.SlotAt(Catalog::Nameplate, 1).ms <= ElapsedMs());

    // PresentTick: counts, and hands over lines once settled (local ledger, injected clock)
    Ledger l;
    std::vector<std::string> lines;
    const auto sink = [&lines](const std::string& s) { lines.push_back(s); };
    for (uint32_t i = 0; i < 599; ++i)
        PresentTick(l, kSettleMs + 1, sink);
    CHECK_EQ(l.Present(), 599u);
    CHECK(lines.empty());
    PresentTick(l, kSettleMs + 1, sink);
    CHECK_EQ(l.Present(), 600u);
    CHECK(!lines.empty());
    CHECK_HAS(lines[0], "[ffxiv-catalog] UI 0/7");
    const size_t before = lines.size();
    PresentTick(l, kSettleMs + 2, sink);
    CHECK_EQ(lines.size(), before); // logged once
}
} // namespace

int main(int argc, char** argv)
{
    g_root = argc > 1 ? argv[1] : ".";
    HelperSelfTest();
    MirroredLiterals();
    CreationHook();
    NameplateTagging();
    UiProvider();
    Glue();
    return Finish("catalog_wiring_pins");
}
