// ImGui stack balance of every menu function of the fork (OptiScaler/ext), as source text (ImGui glue cannot run offline).
//
// A BeginDisabled without its EndDisabled, a PushItemWidth without its Pop, a Push/PopStyleColor count that does not match
// or an Indent without its Unindent corrupt the ImGui stack for the rest of the frame (an assert in a debug build, a wrong
// look or a crash in the field). The older text pins only checked TreeNode/TreePop. This test checks, for EVERY function
// body of EVERY ext file that calls ImGui (comments, #if 0 blocks stripped; braces in strings and character literals
// skipped): the calls of each family are as many as their closes (PopStyleColor(2) counts two), and no `return` can run
// between an open and its close (textual order; returns inside lambdas are ignored). A scanned family that is opened in one
// function and closed in another is a finding too: keep the pair in one function, or say so here (kSplitPairs).
//
// The check is textual: it does not follow branches (a conditional push with a conditional pop of a different condition
// passes), it counts calls, and it cannot see a pair that a helper opens for its caller. The named card functions below
// must be found in their files (so a parser miss cannot make the test vacuous) and each is also run against broken copies:
// the last close of every family removed, and a `return` injected right after the first open.
#include "menu_scan.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void Check(bool ok, const std::string& what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what.c_str());
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)
#define CHECKM(x, msg) Check((x), (msg), __LINE__)

namespace fs = std::filesystem;

struct Named
{
    const char* file; // under OptiScaler/ext
    const char* func;
};

// Card functions edited by the menu redesign (WP2-WP4): they must exist, balance, and their breakages must be caught
const Named kCards[] = {
    { "ffxiv/chara/CharaMenu.cpp", "DrawCharaCard" },
    { "ffxiv/chara/CharaMenu.cpp", "DrawDebugSection" },
    { "ffxiv/chara/CharaMenu.cpp", "DrawNrToggles" },
    { "ffxiv/chara/CharaMenu.cpp", "DrawNrStatusLine" },
    { "ffxiv/chara/CharaMenu.cpp", "DrawNrCounters" },
    { "ffxiv/FfxivAdvancedMenu.cpp", "Draw" },
    { "hudless/HudlessMenu.cpp", "DrawHudless" },
    { "ffxiv/ui/UiMenu.cpp", "DrawDiagBody" },
    { "reshade/ReShadeMenu.cpp", "DrawReShadeBody" },
    { "fg/MenuFgUnlocks.cpp", "DrawAmpereRuntime" },
    { "fg/MenuFgUnlocks.cpp", "DrawAmpereAdvanced" },
    { "fg/xefg/XefgSeams.cpp", "XeFgExtras" },
    { "fg/MenuAutoMfg.cpp", "DrawAutoMfg" },
    { "nr/menu/DlssNr_Menu.cpp", "RenderMenu" },
};

// Pairs a function is allowed to leave open for another function of the same file ("file:function:open call")
const char* const kSplitPairs[] = {
    "",
};

std::string Stripped(const std::string& path)
{
    std::string raw = menuscan::ReadFile(path);
    raw.erase(std::remove(raw.begin(), raw.end(), '\r'), raw.end());
    return menuscan::StripComments(raw);
}

// Problems of one body: unbalanced families and returns inside a stack
std::vector<std::string> BodyProblems(const std::string& body)
{
    std::vector<std::string> p;
    for (const auto& pair : menuscan::StackPairs())
    {
        std::string why;
        if (!menuscan::StackBalanced(body, pair, why))
            p.push_back("unbalanced: " + why);
        size_t at = 0;
        if (menuscan::ReturnInsideStack(body, pair, at))
            p.push_back(std::string("a return can run between ") + pair.open + " and " + pair.close);
    }
    return p;
}

// `body` with the LAST call of `call` (up to its ";") removed: the last close is the one of the normal path (an earlier one may
// belong to a branch that leaves); unchanged when there is none
std::string DropLast(std::string body, const char* call)
{
    const size_t at = body.rfind(call);
    if (at == std::string::npos)
        return body;
    const size_t end = body.find(';', at);
    body.erase(at, end == std::string::npos ? std::string::npos : end + 1 - at);
    return body;
}

// `body` with `if (x) return;` right after the first call of `call`
std::string ReturnAfterFirst(std::string body, const char* call)
{
    const size_t at = body.find(call);
    const size_t end = at == std::string::npos ? at : body.find(';', at);
    if (end != std::string::npos)
        body.insert(end + 1, " if (x) return; ");
    return body;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const fs::path root = fs::path(repo) / "OptiScaler" / "ext";

    size_t files = 0, functions = 0, familiesSeen = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h"))
            continue;
        const std::string code = Stripped(entry.path().string());
        if (code.find("ImGui::") == std::string::npos)
            continue;
        ++files;
        const std::string rel = fs::relative(entry.path(), root).generic_string();
        for (const auto& f : menuscan::FunctionBodies(code))
        {
            ++functions;
            const std::string body = code.substr(f.open, f.close - f.open + 1);
            for (const auto& pair : menuscan::StackPairs())
                if (body.find(pair.open) != std::string::npos)
                    ++familiesSeen;
            for (const std::string& problem : BodyProblems(body))
            {
                const std::string key = rel + ":" + f.name;
                bool allowed = false;
                for (const char* s : kSplitPairs)
                    allowed = allowed || (*s != '\0' && key.rfind(s, 0) == 0);
                CHECKM(allowed, key + ": " + problem);
            }
        }
    }
    std::printf("menu_stack_balance: scanned %zu files, %zu functions, %zu family uses\n", files, functions, familiesSeen);
    // the scan saw the menu: a parser that finds nothing must not pass
    CHECK(files >= 15);
    CHECK(functions >= 100);
    CHECK(familiesSeen >= 15);

    // the named card functions: found exactly once, balanced, and every breakage is caught
    for (const Named& n : kCards)
    {
        const std::string code = Stripped((root / n.file).string());
        const std::string body = menuscan::FunctionBody(code, n.func);
        CHECKM(!body.empty(), std::string("not found exactly once: ") + n.file + ":" + n.func);
        if (body.empty())
            continue;
        CHECKM(BodyProblems(body).empty(), std::string("problems in ") + n.file + ":" + n.func);

        for (const auto& pair : menuscan::StackPairs())
        {
            if (body.find(pair.open) == std::string::npos)
                continue;
            // a family that is opened in this function: removing its first close or returning after its first open is caught
            const std::string noClose = DropLast(body, pair.close);
            CHECKM(noClose != body && !BodyProblems(noClose).empty(),
                   std::string("a removed ") + pair.close + " is not caught in " + n.func);
            const std::string early = ReturnAfterFirst(body, pair.open);
            bool caught = false;
            size_t at = 0;
            caught = menuscan::ReturnInsideStack(early, pair, at);
            CHECKM(early != body && caught, std::string("a return after ") + pair.open + " is not caught in " + n.func);
        }
    }

    if (failures)
    {
        std::printf("menu_stack_balance: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("menu_stack_balance: all passed (%zu files, %zu functions, %zu family uses)\n", files, functions, familiesSeen);
    return 0;
}
