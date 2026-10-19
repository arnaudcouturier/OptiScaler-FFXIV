// The thin interface of the live Env (WindowModule.cpp) on fakes: the order of the frame generation request and the arguments of
// the replay. The module's two out-calls are templates (WindowLiveOps.h) that LiveEnv calls, so this test runs the production
// code; a source check pins that LiveEnv really goes through them. ("Release skipped at shutdown" is gone: the held swapchain
// reference was removed in fix round 1, M1.)
#include "window_test.h"
#include "ext/window/WindowLiveOps.h"

#include <fstream>
#include <sstream>

using namespace ext::window;

namespace
{
// An IFGFeature stand-in that records the order of the calls and the flag as it was at each call.
struct FakeFeature
{
    bool* flag = nullptr;
    std::vector<std::string> calls;
    void UpdateTarget() { calls.push_back(std::string("UpdateTarget(fgChanged=") + (*flag ? "1" : "0") + ")"); }
    void Deactivate() { calls.push_back(std::string("Deactivate(fgChanged=") + (*flag ? "1" : "0") + ")"); }
};

// An IDXGISwapChain stand-in: SetFullscreenState(BOOL, IDXGIOutput*).
struct FakeChain
{
    int calls = 0;
    int lastFullscreen = -1;
    const void* lastTarget = reinterpret_cast<const void*>(1); // anything but null until a call overwrites it
    int32_t result = 0;
    int32_t SetFullscreenState(int fullscreen, void* target)
    {
        ++calls;
        lastFullscreen = fullscreen;
        lastTarget = target;
        return result;
    }
};

std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
} // namespace

int main(int argc, char** argv)
{
    // The frame generation request: fgChanged first, then UpdateTarget, then Deactivate (what upstream's hook does first).
    {
        bool fgChanged = false;
        FakeFeature fg;
        fg.flag = &fgChanged;
        live::RequestOff(&fg, fgChanged);
        CHECK(fgChanged);
        CHECK(fg.calls.size() == 2);
        if (fg.calls.size() == 2)
        {
            CHECK(fg.calls[0] == "UpdateTarget(fgChanged=1)"); // the flag is already set when the target is updated
            CHECK(fg.calls[1] == "Deactivate(fgChanged=1)");   // and the target is updated before the feature is deactivated
        }
    }
    // No feature object: nothing is touched, not even the flag.
    {
        bool fgChanged = false;
        live::RequestOff(static_cast<FakeFeature*>(nullptr), fgChanged);
        CHECK(!fgChanged);
    }

    // The replay: a null target, the requested state as a BOOL, the HRESULT passed back unchanged.
    {
        FakeChain chain;
        CHECK(live::ReplaySetFullscreen(&chain, true) == 0);
        CHECK(chain.calls == 1);
        CHECK(chain.lastFullscreen == 1);
        CHECK(chain.lastTarget == nullptr);
        chain.result = static_cast<int32_t>(0x887A0001u); // a failing HRESULT comes back as it is
        CHECK(live::ReplaySetFullscreen(&chain, false) == static_cast<int32_t>(0x887A0001u));
        CHECK(chain.calls == 2);
        CHECK(chain.lastFullscreen == 0);
        CHECK(chain.lastTarget == nullptr);
    }
    // No swapchain: E_POINTER, nothing is called.
    {
        CHECK(live::ReplaySetFullscreen(static_cast<FakeChain*>(nullptr), true) == static_cast<int32_t>(0x80004003u));
    }

    // LiveEnv goes through exactly these two templates (it must not grow its own copy of the order).
    if (argc >= 2)
    {
        const std::string module = Read(std::string(argv[1]) + "/OptiScaler/ext/window/WindowModule.cpp");
        CHECK(!module.empty());
        CHECK(Has(module, "live::RequestOff(State::Instance().currentFG, State::Instance().fgChanged);"));
        CHECK(Has(module, "live::ReplaySetFullscreen("));
        CHECK(!Has(module, "->Deactivate()"));
        CHECK(!Has(module, "->UpdateTarget()"));
        CHECK(!Has(module, "->SetFullscreenState("));
    }
    else
    {
        std::printf("usage: window_live_ops <repo root>\n");
        ++wt::g_failed;
    }

    return wt::g_failed ? 1 : 0;
}
