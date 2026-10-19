// Source-level check of the call sites (the guard test proves the header, not who calls it): the seam bodies and the ReShade
// handler reach the module only through the inline guards, and the ReShade event is subscribed only when the mode is not off.
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

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: window_call_sites <repo root>\n");
        return 2;
    }
    const std::string root = argv[1];
    const std::string bridge = Read(root + "/OptiScaler/ext/hdr/BridgeHdr.cpp");
    const std::string events = Read(root + "/OptiScaler/ext/reshade/ReShadeEvents.cpp");
    const std::string addon = Read(root + "/OptiScaler/ext/reshade/ReShadeAddon.cpp");
    CHECK(!bridge.empty() && !events.empty() && !addon.empty());

    // The seam bodies call the four inline guards and never an out-of-line body.
    CHECK(Has(bridge, "window::OnResizeDone(sc);"));
    CHECK(Has(bridge, "window::OnPresentBegin();"));
    CHECK(Has(bridge, "window::OnPresentEnd();"));
    CHECK(Has(bridge, "window::OnDestroy(sc);"));
    CHECK(!Has(bridge, "Slow("));
    CHECK(!Has(bridge, "WindowModule"));

    // The ReShade handler calls the guarded entry only.
    CHECK(Has(events, "ext::window::OnSetFullscreenState("));
    CHECK(!Has(events, "SetFullscreenStateSlow"));

    // The event is subscribed from exactly one place, and only under a mode check.
    size_t calls = 0;
    for (size_t pos = addon.find("InstallWindowEvent()"); pos != std::string::npos; pos = addon.find("InstallWindowEvent()", pos + 1))
        ++calls;
    CHECK(calls == 1);
    const size_t install = addon.find("InstallWindowEvent()");
    const size_t guard = addon.rfind("ext::window::CurrentMode() != ext::window::Mode::Off", install);
    CHECK(install != std::string::npos && guard != std::string::npos && install - guard < 200);

    // Nothing else in the fork subscribes the event.
    CHECK(Has(events, "register_event<::reshade::addon_event::set_fullscreen_state>"));

    return g_failed ? 1 : 0;
}
