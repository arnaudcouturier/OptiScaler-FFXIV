// Pins the lock order of the product's LoadLibrary hook (docs/notes/streamline-loader-lock-hazard.md): a plugin hook mutex
// is never held across a DLL load, and the catch-up for modules already in memory never waits for one, because its caller
// may hold the Windows loader lock. A runtime test can show the load side on this machine; none can reach the catch-up's
// nested load, so the rule is read from the source, with comments and literals blanked first.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
int g_failed = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

std::string Read(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Code only: comments and the contents of string/char literals replaced by spaces, so no remark can satisfy or break a pin
// and no literal brace confuses the block scan. Indices and line breaks of the code are unchanged.
std::string CodeOnly(const std::string& source)
{
    std::string out = source;

    for (size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] == '"' || source[i] == '\'')
        {
            const char quote = source[i];
            for (++i; i < source.size() && source[i] != quote; ++i)
            {
                if (source[i] == '\\')
                    ++i;
                out[i] = ' ';
            }
        }
        else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/')
        {
            for (; i < source.size() && source[i] != '\n'; ++i)
                out[i] = ' ';
        }
        else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*')
        {
            for (i += 2; i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'); ++i)
                out[i] = ' ';
            if (i + 1 < source.size())
                out[i] = out[i + 1] = ' ';
            ++i;
        }
    }

    return out;
}

// Whitespace runs collapse to one space, so a pin survives re-wrapping and CRLF/LF.
std::string Flat(const std::string& text)
{
    std::string out;
    bool space = false;

    for (char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            space = true;
            continue;
        }

        if (space && !out.empty())
            out += ' ';
        space = false;
        out += c;
    }

    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t found = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size()))
        ++found;

    return found;
}

bool Before(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t a = text.find(first);
    const size_t b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

// The whole block that follows `marker` (an if condition or a function signature), braces matched over code-only text.
std::string BlockAfter(const std::string& code, const std::string& marker)
{
    const size_t at = code.find(marker);
    if (at == std::string::npos)
        return {};

    const size_t open = code.find('{', at);
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

// The catch-up runs on whatever thread made the load, with the loader lock held when that load is nested, so it must not
// wait for a plugin mutex: a busy one means another thread is loading or hooking that plugin right now, and that thread
// hooks what it finds.
void CatchUpNeverWaits(const std::string& code)
{
    const std::string body = Flat(BlockAfter(code, "void LibraryLoadHooks::CheckModulesInMemory()"));
    CHECK(!body.empty());

    CHECK(Count(body, "std::scoped_lock") == 0);
    CHECK(Count(body, "std::lock_guard") == 0);
    CHECK(Count(body, "std::unique_lock lock(StreamlineHooks::mutexHookDlss, std::try_to_lock);") == 1);
    CHECK(Count(body, "std::unique_lock lock1(StreamlineHooks::mutexHookDlssg, std::try_to_lock);") == 1);
    CHECK(Count(body, "std::unique_lock lock2(StreamlineHooks::mutexHookLocalDlssg, std::try_to_lock);") == 1);
    CHECK(Count(body, "std::unique_lock lock(StreamlineHooks::mutexHookReflex, std::try_to_lock);") == 1);
    CHECK(Count(body, "std::unique_lock lock(StreamlineHooks::mutexHookPcl, std::try_to_lock);") == 1);
    CHECK(Count(body, "std::unique_lock lock(StreamlineHooks::mutexHookCommon, std::try_to_lock);") == 1);
    // the six try-locks are the only place a plugin mutex is named, and each one's result is required before the work
    CHECK(Count(body, "mutexHook") == 6);
    CHECK(Count(body, "owns_lock()") == 6);
    CHECK(body.find("if (lock.owns_lock() && !StreamlineHooks::isDlssHooked())") != std::string::npos);
    CHECK(body.find("if (lock1.owns_lock() && lock2.owns_lock() && (!StreamlineHooks::isDlssgHooked() || "
                    "!StreamlineHooks::isLocalDlssgHooked()))") != std::string::npos);
    CHECK(body.find("if (lock.owns_lock() && !StreamlineHooks::isReflexHooked())") != std::string::npos);
    CHECK(body.find("if (lock.owns_lock() && !StreamlineHooks::isPclHooked())") != std::string::npos);
    CHECK(body.find("if (lock.owns_lock() && !StreamlineHooks::isCommonHooked())") != std::string::npos);
}

struct Branch
{
    const char* condition;
    std::vector<const char*> locks;
    const char* hookCall;
};

// The five plugin branches load the module first and take their mutex only for the hook, so no thread ever waits for the
// loader lock while holding one. The mutex is still taken before the hook call: hooking a module that has just been loaded
// must not be skipped.
const Branch kLoadBranches[] = {
    { "if (shouldHookSl && (CheckDllNameW(&libName, &slDlssNamesW)",
      { "std::scoped_lock lock(StreamlineHooks::mutexHookDlss);" }, "StreamlineHooks::hookDlss(dlssModule)" },
    { "if ((CheckDllNameW(&libName, &slDlssgNamesW)",
      { "std::scoped_lock lock1(StreamlineHooks::mutexHookDlssg);",
        "std::scoped_lock lock2(StreamlineHooks::mutexHookLocalDlssg);" },
      "StreamlineHooks::hookDlssg(dlssgModule)" },
    { "if (shouldHookSl && (CheckDllNameW(&libName, &slReflexNamesW)",
      { "std::scoped_lock lock(StreamlineHooks::mutexHookReflex);" }, "StreamlineHooks::hookReflex(reflexModule)" },
    { "if (shouldHookSl && (CheckDllNameW(&libName, &slPclNamesW)",
      { "std::scoped_lock lock(StreamlineHooks::mutexHookPcl);" }, "StreamlineHooks::hookPcl(pclModule)" },
    { "if (shouldHookSl && (CheckDllNameW(&libName, &slCommonNamesW)",
      { "std::scoped_lock lock(StreamlineHooks::mutexHookCommon);" }, "StreamlineHooks::hookCommon(commonModule)" },
};

void LoadsComeBeforeTheLock(const std::string& code)
{
    for (const auto& branch : kLoadBranches)
    {
        const std::string body = Flat(BlockAfter(code, branch.condition));
        CHECK(!body.empty());

        const std::string load = "NtdllProxy::LoadLibraryExW_Ldr(lpLibFullPath, NULL, 0);";
        CHECK(Count(body, load) == 1);
        CHECK(Before(body, load, branch.locks.front()));

        for (const char* lock : branch.locks)
            CHECK(Count(body, lock) == 1);

        // the branch mutexes are the only ones it names, they always wait (a branch that skips its hook on a busy mutex
        // would leave the plugin it just loaded unhooked), and they cover the hook call
        CHECK(Count(body, "mutexHook") == (int) branch.locks.size());
        CHECK(Count(body, "try_to_lock") == 0);
        CHECK(Before(body, branch.locks.back(), branch.hookCall));
    }
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string source = Read(repo + "/OptiScaler/hooks/LibraryLoad_Hooks.cpp");
    CHECK(!source.empty());

    const std::string code = CodeOnly(source);
    CatchUpNeverWaits(code);
    LoadsComeBeforeTheLock(code);

    if (g_failed != 0)
    {
        std::printf("library_load_lock_order_pin: %d check(s) failed\n", g_failed);
        return 1;
    }

    std::puts("library_load_lock_order_pin: no plugin mutex across a load, the catch-up never waits passed");
    return 0;
}
