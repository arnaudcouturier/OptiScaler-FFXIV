// The module's RequestFgOff (WindowLiveOps.h live::RequestOff) copies the first thing upstream's SetFullscreenState hook does while
// frame generation is active: fgChanged = true, then UpdateTarget(), then Deactivate(). window_live_ops proves the module's order
// against itself; this proves it against the upstream hook, read-only: if an upstream merge reorders or changes the hook, the
// copy must be revisited and this test says so. The upstream file is never edited by the fork (footprint).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

static std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The body of the function whose definition line contains `header`: from the line to the next line that is exactly "}" (a
// function definition ends at column 0 in this file). Empty when not found.
static std::string FunctionBody(const std::string& text, const char* header)
{
    const size_t at = text.find(header);
    if (at == std::string::npos)
        return {};
    size_t end = at;
    for (;;)
    {
        end = text.find("\n}", end + 1);
        if (end == std::string::npos)
            return {};
        const size_t next = end + 2;
        if (next >= text.size() || text[next] == '\n' || text[next] == '\r')
            break; // "}" alone on its line, not "}};" or "} else"
    }
    return text.substr(at, end - at);
}

static size_t Count(const std::string& text, const char* part)
{
    size_t n = 0;
    for (size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + 1))
        ++n;
    return n;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: window_hook_order <repo root>\n");
        return 2;
    }
    const std::string source = Read(std::string(argv[1]) + "/OptiScaler/hooks/FG_Hooks.cpp");
    CHECK(!source.empty());

    const std::string body = FunctionBody(source, "FGHooks::hkSetFullscreenState(");
    CHECK(!body.empty());

    // The three steps, once each, in this order ...
    const size_t active = body.find("IsActive()");
    const size_t changed = body.find("fgChanged = true");
    const size_t update = body.find("UpdateTarget()");
    const size_t deactivate = body.find("Deactivate()");
    CHECK(active != std::string::npos && changed != std::string::npos && update != std::string::npos &&
          deactivate != std::string::npos);
    CHECK(Count(body, "fgChanged = true") == 1 && Count(body, "UpdateTarget()") == 1 && Count(body, "Deactivate()") == 1);
    CHECK(active < changed && changed < update && update < deactivate);

    // ... and before anything else the hook does (the ForceBorderless handling and the call into the original function).
    const size_t forceBorderless = body.find("FGXeFGForceBorderless");
    const size_t original = body.find("o_FGSCSetFullscreenState(");
    CHECK(forceBorderless != std::string::npos && original != std::string::npos);
    CHECK(deactivate < forceBorderless && deactivate < original);

    // The module's copy is only made while frame generation is active (the verdict needs it, and a re-request only happens when
    // the snapshot says active), which is the condition upstream guards the three steps with.
    CHECK(body.find("fg != nullptr") != std::string::npos && body.find("fg != nullptr") < active);

    // Weak test 5: all three steps sit inside the braces of that guard, and the guard is exactly this condition (no extra
    // condition between the `)` and the `{`, none added to the test), so a refactor that moves one step out or under another
    // condition is noticed.
    {
        const char* guardText = "if (fg != nullptr && fg->IsActive())";
        const size_t guard = body.find(guardText);
        CHECK(guard != std::string::npos && Count(body, guardText) == 1);
        const size_t open = guard == std::string::npos ? std::string::npos : body.find('{', guard);
        CHECK(open != std::string::npos);
        if (open != std::string::npos)
        {
            const std::string between = body.substr(guard + std::string(guardText).size(), open - guard - std::string(guardText).size());
            CHECK(between.find_first_not_of(" \t\r\n") == std::string::npos); // nothing between the condition and the brace
            int depth = 0;
            size_t close = std::string::npos;
            for (size_t i = open; i < body.size(); ++i)
            {
                if (body[i] == '{')
                    ++depth;
                else if (body[i] == '}' && --depth == 0)
                {
                    close = i;
                    break;
                }
            }
            CHECK(close != std::string::npos);
            if (close != std::string::npos)
            {
                const std::string block = body.substr(open + 1, close - open - 1);
                CHECK(Count(block, "fgChanged = true") == 1 && Count(block, "UpdateTarget()") == 1 && Count(block, "Deactivate()") == 1);
                CHECK(block.find("fgChanged = true") < block.find("UpdateTarget()") && block.find("UpdateTarget()") < block.find("Deactivate()"));
                CHECK(close < body.find("FGXeFGForceBorderless"));
            }
        }
    }

    // The helper itself: a function body ends at the first "}" alone on a line.
    CHECK(FunctionBody("void f()\n{\n  if (a) {\n  }\n  return;\n}\nvoid g()\n{\n}\n", "void f()") ==
          "void f()\n{\n  if (a) {\n  }\n  return;");

    return g_failed ? 1 : 0;
}
