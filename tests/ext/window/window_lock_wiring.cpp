// R3-L2: the lock wiring of the live module (WindowModule.cpp), as a source check on the text with comments removed. The scope
// logic itself is tested on a fake mutex (window_lock_scope); this pins its USE: each of the three calls out (ApplyExStyle,
// RequestFgOff, Replay) gives the module lock up around the call, each of the five entry points takes the lock and counts it.
// Dropping one of them keeps every other test green and runs a call out (which can wait for another thread) locked.
#include "window_test.h"

#include <fstream>
#include <sstream>

namespace
{
std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
size_t Count(const std::string& text, const char* part)
{
    size_t n = 0;
    for (size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + 1))
        ++n;
    return n;
}

// Removes // and /* */ comments; string and character literals are copied as they are.
std::string StripComments(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
        if (s[i] == '"' || s[i] == '\'')
        {
            const char quote = s[i];
            out += s[i++];
            while (i < s.size() && s[i] != quote)
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            if (i < s.size())
                out += s[i++];
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            i += 2;
        }
        else
        {
            out += s[i++];
        }
    }
    return out;
}

// The text between the braces that follow `header` (first occurrence), braces in literals ignored; empty when not found.
std::string Body(const std::string& text, const char* header)
{
    const size_t at = text.find(header);
    if (at == std::string::npos)
        return {};
    const size_t open = text.find('{', at);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '"' || c == '\'')
        {
            const char quote = c;
            for (++i; i < text.size() && text[i] != quote; ++i)
                if (text[i] == '\\')
                    ++i;
        }
        else if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            if (--depth == 0)
                return text.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

// R3-L2: `Unlocked unlocked;` comes before the call out and the call sits in the same braces (no closing brace in between).
void CheckCallOut(const std::string& module, const char* header, const char* call)
{
    const std::string body = Body(module, header);
    CHECK(!body.empty());
    CHECK(Count(body, "Unlocked unlocked;") == 1);
    CHECK(Count(body, call) == 1);
    const size_t scope = body.find("Unlocked unlocked;");
    const size_t at = body.find(call);
    CHECK(scope != std::string::npos && at != std::string::npos && scope < at);
    if (scope != std::string::npos && at != std::string::npos && scope < at)
        CHECK(body.find('}', scope) == std::string::npos || body.find('}', scope) > at);
}

// R3-L2: an entry point takes the module lock and then counts it, both in its own braces.
void CheckEntry(const std::string& module, const char* header)
{
    const std::string body = Body(module, header);
    CHECK(!body.empty());
    CHECK(Count(body, "std::lock_guard lock(g.lock);") == 1);
    CHECK(Count(body, "LockDepth depth;") == 1);
    const size_t lock = body.find("std::lock_guard lock(g.lock);");
    const size_t depth = body.find("LockDepth depth;");
    CHECK(lock != std::string::npos && depth != std::string::npos && lock < depth);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: window_lock_wiring <repo root>\n");
        return 2;
    }
    const std::string module = Read(std::string(argv[1]) + "/OptiScaler/ext/window/WindowModule.cpp");
    CHECK(!module.empty());
    {
        // points holds it and counts it. Dropping one of them keeps every other test green and runs the call out locked.
        const std::string code = StripComments(module);
        CheckCallOut(code, "bool ApplyExStyle(uint64_t hwnd, uint32_t exstyle) override", "win32::ApplyExStyle(");
        CheckCallOut(code, "void RequestFgOff() override", "live::RequestOff(");
        CheckCallOut(code, "int32_t Replay(uint64_t native, bool fullscreen) override", "live::ReplaySetFullscreen(");
        CheckEntry(code, "void ResizeDoneSlow(Dx11wDx12SC& sc) noexcept");
        CheckEntry(code, "void DestroySlow(Dx11wDx12SC& sc) noexcept");
        CheckEntry(code, "void PresentBeginSlow() noexcept");
        CheckEntry(code, "void PresentEndSlow() noexcept");
        CheckEntry(code, "bool SetFullscreenStateSlow(uint64_t native, uint64_t hwnd, bool fullscreen, uint64_t target) noexcept");
        CHECK(Count(code, "Unlocked unlocked;") == 3);
        CHECK(Count(code, "LockDepth depth;") == 5);
        CHECK(Count(code, "std::lock_guard lock(g.lock);") == 5);
        CHECK(Count(code, "ReportLockDepth();") == 3); // a violation is reported after each call out (R3-L5)
    }
    return wt::g_failed ? 1 : 0;
}
