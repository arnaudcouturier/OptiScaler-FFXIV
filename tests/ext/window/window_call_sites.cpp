// Source-level check of the call sites (the guard test proves the header, not who calls it): the seam bodies and the ReShade
// handler reach the module only through the inline guards, and the ReShade event is subscribed only when the mode is not off.
// The checks are POSITIONAL, on the text with comments removed: the begin call sits in the PresentScope constructor, the end call
// in its destructor after the frame is closed, the resize and destroy calls are the first thing in their seam bodies (no early
// return before them), the install call is inside the braces of the mode check, and a commented-out call does not count.
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
static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Removes // and /* */ comments; string and character literals are copied as they are.
static std::string StripComments(const std::string& s)
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
static std::string Body(const std::string& text, const char* header)
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

static size_t Count(const std::string& text, const char* part)
{
    size_t n = 0;
    for (size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + 1))
        ++n;
    return n;
}

// Weak test 3: the call is a statement of the body itself (brace depth 0), directly after another statement or at its start:
// not under an `if`, `else`, a loop or a block, and not behind a preprocessor line (what precedes it is `;`, `}` or nothing).
static bool TopLevelStatement(const std::string& body, const char* call)
{
    const size_t at = body.find(call);
    if (at == std::string::npos)
        return false;
    int depth = 0;
    for (size_t i = 0; i < at; ++i)
        depth += body[i] == '{' ? 1 : body[i] == '}' ? -1 : 0;
    if (depth != 0 || body.find('#') != std::string::npos)
        return false;
    const size_t prev = body.find_last_not_of(" \t\r\n", at == 0 ? 0 : at - 1);
    return at == 0 || prev == std::string::npos || body[prev] == ';' || body[prev] == '}';
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: window_call_sites <repo root>\n");
        return 2;
    }
    const std::string root = argv[1];
    const std::string bridge = StripComments(Read(root + "/OptiScaler/ext/hdr/BridgeHdr.cpp"));
    const std::string events = StripComments(Read(root + "/OptiScaler/ext/reshade/ReShadeEvents.cpp"));
    const std::string addon = StripComments(Read(root + "/OptiScaler/ext/reshade/ReShadeAddon.cpp"));
    CHECK(!bridge.empty() && !events.empty() && !addon.empty());

    // The seam bodies call the inline guards and never an out-of-line body.
    CHECK(!Has(bridge, "Slow("));
    CHECK(!Has(bridge, "WindowModule"));

    // The present guards: begin in the constructor, end in the destructor (after the frame is closed), once each, never swapped.
    const std::string ctor = Body(bridge, "PresentScope::PresentScope()");
    const std::string dtor = Body(bridge, "PresentScope::~PresentScope()");
    CHECK(!ctor.empty() && !dtor.empty());
    CHECK(Count(ctor, "window::OnPresentBegin();") == 1);
    CHECK(!Has(ctor, "OnPresentEnd"));
    CHECK(Count(dtor, "window::OnPresentEnd();") == 1);
    CHECK(!Has(dtor, "OnPresentBegin"));
    CHECK(Count(bridge, "window::OnPresentBegin();") == 1 && Count(bridge, "window::OnPresentEnd();") == 1);
    {
        const size_t frameClosed = dtor.rfind("EndFrame()");
        const size_t endCall = dtor.find("window::OnPresentEnd();");
        CHECK(frameClosed != std::string::npos && endCall != std::string::npos && frameClosed < endCall); // last in the destructor
        CHECK(dtor.find("return") == std::string::npos);                                                    // no early exit around it
    }

    CHECK(TopLevelStatement(ctor, "window::OnPresentBegin();"));
    CHECK(TopLevelStatement(dtor, "window::OnPresentEnd();"));
    CHECK(!TopLevelStatement("if (a) window::OnPresentEnd();", "window::OnPresentEnd();")); // the helper itself
    CHECK(!TopLevelStatement("if (a) { b(); window::OnPresentEnd(); }", "window::OnPresentEnd();"));
    CHECK(!TopLevelStatement("b();\n#if 0\nwindow::OnPresentEnd();\n#endif", "window::OnPresentEnd();"));
    CHECK(TopLevelStatement("b();\nwindow::OnPresentEnd();", "window::OnPresentEnd();"));

    // The resize and destroy seams: the call is the first statement of the body, so no early return can skip it.
    const std::string resize = Body(bridge, "HRESULT OnResizeDone(Dx11wDx12SC& sc)");
    const std::string destroy = Body(bridge, "void OnDestroy(Dx11wDx12SC& sc)");
    CHECK(!resize.empty() && !destroy.empty());
    {
        const size_t call = resize.find("window::OnResizeDone(sc);");
        CHECK(call != std::string::npos && resize.find("return") > call);
        CHECK(call != std::string::npos && resize.find_first_not_of(" \t\r\n") == call); // the first statement
        CHECK(Count(bridge, "window::OnResizeDone(sc);") == 1);
    }
    {
        const size_t call = destroy.find("window::OnDestroy(sc);");
        CHECK(call != std::string::npos && destroy.find("return") > call);
        CHECK(call != std::string::npos && destroy.find_first_not_of(" \t\r\n") == call); // the first statement
        CHECK(Count(bridge, "window::OnDestroy(sc);") == 1);
    }

    // The ReShade handler calls the guarded entry only, after its null check, inside its try block.
    const std::string handler = Body(events, "bool WindowSetFullscreenState(swapchain* sc, bool fullscreen, void* hmonitor) noexcept");
    CHECK(!handler.empty());
    {
        const size_t guard = handler.find("sc == nullptr");
        const size_t call = handler.find("ext::window::OnSetFullscreenState(");
        const size_t tryAt = handler.find("try");
        CHECK(guard != std::string::npos && tryAt != std::string::npos && call != std::string::npos);
        CHECK(guard < tryAt && tryAt < call);
        CHECK(Has(handler, "catch (...)"));
    }
    CHECK(!Has(events, "SetFullscreenStateSlow"));

    // The event is registered inside InstallWindowEvent only, with that handler.
    const std::string install = Body(events, "bool InstallWindowEvent()");
    CHECK(Has(install, "register_event<::reshade::addon_event::set_fullscreen_state>(&WindowSetFullscreenState)"));
    CHECK(Count(events, "register_event<::reshade::addon_event::set_fullscreen_state>") == 1);

    // InstallWindowEvent() is called from exactly one place, and that place is inside the braces of the mode check.
    CHECK(Count(addon, "InstallWindowEvent()") == 1);
    const std::string modeBlock = Body(addon, "if (ext::window::CurrentMode() != ext::window::Mode::Off)");
    CHECK(Has(modeBlock, "InstallWindowEvent()"));

    // The helpers themselves: a commented-out call must not count (the same text, commented, is not found).
    CHECK(!Has(StripComments("// window::OnPresentEnd();\n/* window::OnPresentBegin(); */\nint x;"), "window::"));
    CHECK(Has(StripComments("window::OnPresentEnd(); // trailing"), "window::OnPresentEnd();"));
    CHECK(Body("void f() { if (a) { \"}\"; } return; } void g() {}", "void f()") == " if (a) { \"}\"; } return; ");

    return g_failed ? 1 : 0;
}
