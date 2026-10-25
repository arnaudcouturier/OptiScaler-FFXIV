// Automatic ReShade scene depth, the capture glue that runs at every NGX evaluate (ReShadeDepthEval.cpp) end to end on the WARP software
// adapter: a fake NVSDK_NGX_Parameter, a fake host (registration, consumer, hooks), a recording sink and the real copy pass.
//  - the option off costs one load of the gate word: the parameters are not read, the host not asked;
//  - the character-preview Output (a size that is not the consumer's) publishes nothing and never reads the depth;
//  - the render size is the per-axis minimum; the output size falls back from OutWidth/OutHeight to the Output texture to the display;
//  - the published capture carries the NATIVE device (NGX's own context is only logged), the frame counter and the clock;
//  - option off from another thread is carried out by the next evaluate (withdraw, then release); so is a shutdown of NGX;
//  - an exception from the parameter block never leaves the seam;
//  - a depth resource without a device (no native context) is reported to the status line and publishes nothing;
//  - the base is never clamped; a refusal is logged once per change; the capture line once per change; the identity line once;
//  - the raw draw goes with the hooked context only.
#include "pch.h"

#include "ext/reshade/ReShadeDepthEval.h"

#include <d3d11_1.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::reshade::depth;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

size_t LogLines(const char* needle)
{
    size_t n = 0;
    for (const std::string& line : depthtest_log::Lines())
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}
std::string LastLine(const char* needle)
{
    std::string found;
    for (const std::string& line : depthtest_log::Lines())
        if (line.find(needle) != std::string::npos)
            found = line;
    return found;
}

// ---- fakes -----------------------------------------------------------------------------------------------------------------------
struct FakeParams final : NVSDK_NGX_Parameter
{
    std::map<std::string, unsigned> uints;
    std::map<std::string, ID3D11Resource*> resources;
    mutable int gets = 0;
    mutable std::vector<std::string> asked;
    bool throwOnGet = false; // a parameter block that throws

    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned int) override {}
    void Set(const char*, int) override {}
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}

    NVSDK_NGX_Result Get(const char* name, unsigned long long*) const override { return Miss(name); }
    NVSDK_NGX_Result Get(const char* name, float*) const override { return Miss(name); }
    NVSDK_NGX_Result Get(const char* name, double*) const override { return Miss(name); }
    NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override
    {
        Note(name);
        const auto it = uints.find(name);
        if (it == uints.end())
            return NVSDK_NGX_Result_Fail;
        *out = it->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* name, int*) const override { return Miss(name); }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override
    {
        Note(name);
        const auto it = resources.find(name);
        if (it == resources.end())
            return NVSDK_NGX_Result_Fail;
        *out = it->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource**) const override { return Miss(name); }
    NVSDK_NGX_Result Get(const char* name, void** out) const override
    {
        Note(name);
        const auto it = resources.find(name);
        if (it == resources.end())
            return NVSDK_NGX_Result_Fail;
        *out = it->second;
        return NVSDK_NGX_Result_Success;
    }
    void Reset() override {}

    void Note(const char* name) const
    {
        ++gets;
        asked.push_back(name);
        if (throwOnGet)
            throw std::runtime_error("parameter block failure");
    }
    NVSDK_NGX_Result Miss(const char* name) const
    {
        Note(name);
        return NVSDK_NGX_Result_Fail;
    }
    bool Asked(const char* name) const
    {
        for (const std::string& a : asked)
            if (a == name)
                return true;
        return false;
    }
};

int g_rawDraws = 0;
void CountingDraw(ID3D11DeviceContext* c)
{
    ++g_rawDraws;
    c->Draw(3, 0);
}

struct FakeHost final : IEvalHost
{
    bool shutting = false;
    int64_t now = 100'000;
    uint32_t thread = GetCurrentThreadId();
    bool registered = true;
    bool lateResult = false;
    int lateCalls = 0, subscribes = 0, calls = 0;
    bool lateEnabledArg = false;
    int64_t lateNow = 0;
    ConsumerInfo consumer;
    ID3D11DeviceContext* hooked = nullptr;
    DrawTriangleFn raw = &CountingDraw;

    bool ShuttingDown() override { return ++calls, shutting; }
    int64_t NowMs() override { return ++calls, now; }
    uint32_t ThreadId() override { return ++calls, thread; }
    bool Registered() override { return ++calls, registered; }
    bool TryLateRegister(bool enabled, int64_t nowMs) override
    {
        ++calls;
        ++lateCalls;
        lateEnabledArg = enabled;
        lateNow = nowMs;
        if (lateResult)
            registered = true;
        return lateResult;
    }
    void EnsureSubscribed() override { ++calls, ++subscribes; }
    ConsumerInfo GetConsumer(int64_t) override { return ++calls, consumer; }
    ID3D11DeviceContext* HookedContext() override { return ++calls, hooked; }
    DrawTriangleFn RawDraw() override { return ++calls, raw; }
};

// the binder's side of the sink, as far as it matters here (the same as the copy pass test's)
ULONG RefCount(IUnknown* p)
{
    p->AddRef();
    return p->Release();
}
struct Recorder final : ICaptureSink
{
    struct Event
    {
        DepthCapture capture;
        ULONG heldRefs = 0;
    };
    std::vector<Event> events;
    ComPtr<ID3D11ShaderResourceView> held;
    void Publish(const DepthCapture& c) noexcept override
    {
        Event e;
        e.capture = c;
        e.heldRefs = held ? RefCount(held.Get()) : 0;
        events.push_back(e);
        ComPtr<ID3D11ShaderResourceView> next;
        if (c.valid && c.copyView != 0)
            next = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(c.copyView));
        held = next;
    }
    size_t Valid() const
    {
        size_t n = 0;
        for (const Event& e : events)
            n += e.capture.valid ? 1 : 0;
        return n;
    }
};

constexpr UINT kW = 128, kH = 72;
constexpr uintptr_t kBogusContext = 0x1234;

struct Rig
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> depth, output;
    FakeParams params;
    FakeHost host;
    Recorder sink;
    glue::EvalGate gate;
    std::atomic<uint64_t> frame { 5 };
    std::unique_ptr<CaptureEngine> engine;

    bool Create(UINT depthBind = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)
    {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, got {};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, &got, &context)))
            return false;
        D3D11_TEXTURE2D_DESC td {};
        td.Width = kW;
        td.Height = kH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = depthBind;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &depth)))
            return false;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &output)))
            return false;
        params.resources["Depth"] = depth.Get();
        params.resources["Output"] = output.Get();
        params.uints["DLSS.Render.Subrect.Dimensions.Width"] = kW;
        params.uints["DLSS.Render.Subrect.Dimensions.Height"] = kH;
        params.uints["Width"] = kW;
        params.uints["Height"] = kH;
        params.uints["OutWidth"] = kW;
        params.uints["OutHeight"] = kH;
        host.consumer.seen = true;
        host.consumer.seenMs = host.now;
        host.consumer.width = kW;
        host.consumer.height = kH;
        engine = std::make_unique<CaptureEngine>(gate, frame, sink, host);
        gate.SetEnabled(true);
        return true;
    }
    void Eval(const DisplaySource& display = {}) { engine->OnEvaluated(reinterpret_cast<ID3D11DeviceContext*>(kBogusContext), &params, display); }
    bool Capture() // one evaluate; true if it published a capture
    {
        const size_t before = sink.Valid();
        Eval();
        return sink.Valid() > before;
    }
};

// ---- the tests -------------------------------------------------------------------------------------------------------------------
void OptionOffIsOneLoad()
{
    Rig r;
    if (!r.Create())
        return;
    r.gate.SetEnabled(false);
    r.gate.TakeRequests(); // (nothing was ever published: the requests of the switch are carried out by hand here)
    CHECK(r.gate.Idle());
    r.host.calls = 0;
    for (int i = 0; i < 5; ++i)
        r.Eval();
    CHECK(r.params.gets == 0);         // the parameters are not read
    CHECK(r.host.calls == 0);          // the host is not asked: not the clock, not the registration, not the consumer
    CHECK(r.sink.events.empty() && !r.engine->PassReady());
    CHECK(r.host.lateCalls == 0 && r.host.subscribes == 0);
    CHECK(LogLines("contexts:") == 0);
}

void HappyPath()
{
    Rig r;
    if (!r.Create())
        return;
    r.frame = 77;
    r.host.now = 123'456;
    const size_t identity = LogLines("contexts:");
    CHECK(r.Capture());
    CHECK(r.sink.events.size() == 1);
    const DepthCapture c = r.sink.events.back().capture;
    CHECK(c.valid && c.copyView != 0);
    CHECK(c.outputW == kW && c.outputH == kH && c.renderW == kW && c.renderH == kH);
    CHECK(c.frame == 77 && c.tickMs == 123'456);                                   // the depth-owned counter and the clock
    CHECK(c.device == reinterpret_cast<uintptr_t>(r.device.Get()));              // the NATIVE device
    CHECK(r.engine->PassReady());
    CHECK(r.engine->Stats().captured == 1 && r.engine->Stats().refused == 0 && r.engine->Stats().lastResult == 0);
    CHECK(r.host.subscribes == 1);                                                // the lazy subscription is asked for at the first go
    CHECK(r.host.lateCalls == 0);
    // the identity line: NGX's context, the native one, the hooked one, the thread; once
    CHECK(LogLines("contexts:") == identity + 1);
    const std::string line = LastLine("contexts:");
    CHECK(line.find("0x1234") != std::string::npos);                                                        // what NGX gave
    CHECK(line.find(std::format("{:#x}", reinterpret_cast<uintptr_t>(r.context.Get()))) != std::string::npos); // the native immediate context
    CHECK(line.find("not the native one") != std::string::npos);
    CHECK(line.find("evaluate thread " + std::to_string(GetCurrentThreadId())) != std::string::npos);
    CHECK(r.Capture() && r.Capture());
    CHECK(LogLines("contexts:") == identity + 1);
    // a capture line for the first capture, none for the same capture again
    CHECK(LogLines("capture: the game's depth is") >= 1);
    const size_t lines = LogLines("capture: the game's depth is");
    CHECK(r.Capture());
    CHECK(LogLines("capture: the game's depth is") == lines);
}

void RenderSize()
{
    Rig r;
    if (!r.Create())
        return;
    r.params.uints["Width"] = 64; // FFXIV reports the buffer as the subrect and the real render size as Width/Height
    r.params.uints["Height"] = 36;
    CHECK(r.Capture());
    CHECK(r.sink.events.back().capture.renderW == 64 && r.sink.events.back().capture.renderH == 36);
    r.params.uints["Height"] = kH; // per axis: letterboxing changes one
    CHECK(r.Capture());
    CHECK(r.sink.events.back().capture.renderW == 64 && r.sink.events.back().capture.renderH == kH);
    const size_t lines = LogLines("capture: the game's depth is");
    r.params.uints["Width"] = 96; // a change of the render size is a new line
    CHECK(r.Capture());
    CHECK(LogLines("capture: the game's depth is") == lines + 1);
    r.params.uints.erase("DLSS.Render.Subrect.Dimensions.Width"); // no subrect: the NGX render size
    r.params.uints.erase("DLSS.Render.Subrect.Dimensions.Height");
    r.params.uints["Width"] = 48;
    r.params.uints["Height"] = 27;
    CHECK(r.Capture());
    CHECK(r.sink.events.back().capture.renderW == 48 && r.sink.events.back().capture.renderH == 27);
    r.params.uints.erase("Width");
    r.params.uints.erase("Height");
    const size_t before = r.sink.events.size();
    r.Eval(); // no render size at all
    CHECK(r.sink.events.size() == before);
}

void PreviewPublishesNothing()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    const size_t events = r.sink.events.size();
    r.params.uints["OutWidth"] = 576; // the character preview's evaluate
    r.params.uints["OutHeight"] = 960;
    r.params.asked.clear();
    r.Eval();
    CHECK(r.sink.events.size() == events); // not a capture, and not a withdrawal either
    CHECK(!r.params.Asked("Depth"));       // the depth was not even looked at
    CHECK(r.engine->PassReady());
    r.params.uints["OutWidth"] = kW;
    r.params.uints["OutHeight"] = kH;
    CHECK(r.Capture()); // the real one works again
}

glue::Extent DisplaySize(void* feature) { return *static_cast<glue::Extent*>(feature); }

void OutputFallbacks()
{
    Rig r;
    if (!r.Create())
        return;
    r.params.uints.erase("OutWidth"); // the Output texture says kW x kH
    r.params.uints.erase("OutHeight");
    CHECK(r.Capture());
    CHECK(r.sink.events.back().capture.outputW == kW);
    r.params.resources.erase("Output"); // nothing else: the display size the feature has
    glue::Extent display { kW, kH };
    const DisplaySource source { &DisplaySize, &display };
    const size_t before = r.sink.Valid();
    r.Eval(source);
    CHECK(r.sink.Valid() == before + 1);
    r.Eval(); // no display source: the output is unknown
    CHECK(r.sink.Valid() == before + 1);
    display = glue::Extent { 576, 960 }; // ... and a display that is not the consumer's size is the preview filter, too
    r.Eval(source);
    CHECK(r.sink.Valid() == before + 1);
}

void NativeDeviceNotNgxContext()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    // the pass drew on the native context: its private-state swaps are what a bogus context could not have done
    CHECK(r.sink.events.back().capture.device != 0 && r.sink.events.back().capture.device == reinterpret_cast<uintptr_t>(r.device.Get()));
    // a context that IS the native one is said so
    Rig q;
    if (!q.Create())
        return;
    q.engine->OnEvaluated(q.context.Get(), &q.params, {});
    CHECK(LastLine("contexts:").find("NGX's context is the native one") != std::string::npos);
}

void OptionOffFromAnotherThread()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture() && r.Capture());
    CHECK(r.engine->PassReady());
    const size_t events = r.sink.events.size();
    std::thread([&] { r.gate.SetEnabled(false); }).join(); // the menu thread
    // nothing happened on that thread: the pass is still there, nothing was published
    CHECK(r.engine->PassReady() && r.sink.events.size() == events);
    CHECK(!r.gate.Idle()); // the requests are pending
    r.params.gets = 0;
    r.Eval(); // the next evaluate carries them out: withdraw first (the pass still holds its view), then the resources go
    CHECK(r.sink.events.size() == events + 1 && !r.sink.events.back().capture.valid);
    CHECK(r.sink.events.back().heldRefs == 2);
    CHECK(!r.engine->PassReady());
    CHECK(r.params.gets == 0); // (and nothing else: the option is off)
    CHECK(r.gate.Idle());
    r.Eval();
    CHECK(r.params.gets == 0 && r.sink.events.size() == events + 1);
    // switched on again: a new pass is set up
    r.gate.SetEnabled(true);
    CHECK(r.Capture() && r.engine->PassReady());
}

void RequestsOneByOne()
{
    { // a withdraw request alone: the published capture is withdrawn, the pass stays, this evaluate publishes anew
        Rig r;
        if (!r.Create())
            return;
        CHECK(r.Capture());
        const size_t events = r.sink.events.size();
        r.gate.Request(glue::EvalGate::kRequestWithdraw);
        CHECK(r.Capture());
        CHECK(r.sink.events.size() == events + 2);
        CHECK(!r.sink.events[events].capture.valid && r.sink.events[events + 1].capture.valid);
        CHECK(r.engine->PassReady() && (r.gate.Bits() & glue::EvalGate::kRequests) == 0);
    }
    { // a release request alone: the pass goes (it publishes valid = false first), a new one is made
        Rig r;
        if (!r.Create())
            return;
        CHECK(r.Capture());
        const size_t events = r.sink.events.size();
        r.gate.Request(glue::EvalGate::kRequestRelease);
        r.host.consumer.seen = false; // nobody reads depth: no new pass is made
        r.Eval();
        CHECK(r.sink.events.size() == events + 1 && !r.sink.events.back().capture.valid);
        CHECK(!r.engine->PassReady() && (r.gate.Bits() & glue::EvalGate::kRequests) == 0);
    }
}

void ConsumerLoss()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    const size_t events = r.sink.events.size();
    r.host.consumer.seen = false;
    r.Eval();
    CHECK(r.sink.events.size() == events + 1 && !r.sink.events.back().capture.valid);
    CHECK(r.engine->PassReady()); // withdrawn, not released
    r.Eval();
    r.Eval();
    CHECK(r.sink.events.size() == events + 1); // once
    r.host.consumer.seen = true;
    CHECK(r.Capture());
}

void Registration()
{
    { // ReShade is not there: the late attempt is asked for (with the option and the clock), nothing is captured
        Rig r;
        if (!r.Create())
            return;
        r.host.registered = false;
        r.host.now = 4242;
        r.params.gets = 0;
        r.Eval();
        CHECK(r.host.lateCalls == 1 && r.host.lateEnabledArg && r.host.lateNow == 4242);
        CHECK(r.params.gets == 0 && r.sink.events.empty() && r.host.subscribes == 0);
        // it registers: this very evaluate goes on
        r.host.lateResult = true;
        CHECK(r.Capture());
        CHECK(r.host.lateCalls == 2 && r.host.subscribes == 1);
        r.Eval(); // registered now: no attempt
        CHECK(r.host.lateCalls == 2);
    }
    { // shutting down: nothing
        Rig r;
        if (!r.Create())
            return;
        r.host.shutting = true;
        r.Eval();
        CHECK(r.params.gets == 0 && r.sink.events.empty() && r.host.lateCalls == 0);
    }
}

void NgxShutdown()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    const size_t events = r.sink.events.size();
    // on the evaluate thread: at once
    r.engine->OnNgxShutdown();
    CHECK(!r.engine->PassReady() && r.sink.events.size() == events + 1 && !r.sink.events.back().capture.valid);
    CHECK(r.sink.events.back().heldRefs == 2);
    CHECK((r.gate.Bits() & glue::EvalGate::kRequests) == 0); // done here, nothing left for the next evaluate

    // from another thread: left for the next evaluate
    CHECK(r.Capture() && r.engine->PassReady());
    const size_t before = r.sink.events.size();
    r.host.thread = GetCurrentThreadId() + 1;
    r.engine->OnNgxShutdown();
    r.host.thread = GetCurrentThreadId();
    CHECK(r.engine->PassReady() && r.sink.events.size() == before);
    CHECK(!r.gate.Idle());
    CHECK(r.Capture()); // the request is carried out first (withdraw, release), then this evaluate captures anew
    bool withdrew = false;
    for (size_t i = before; i < r.sink.events.size(); ++i)
        withdrew = withdrew || !r.sink.events[i].capture.valid;
    CHECK(withdrew);
    CHECK(r.sink.events.back().capture.valid);

    // ... and the request is a RELEASE, not just a withdrawal: with nobody reading depth the next evaluate leaves no pass behind
    CHECK(r.engine->PassReady());
    r.host.thread = GetCurrentThreadId() + 1;
    r.engine->OnNgxShutdown();
    r.host.thread = GetCurrentThreadId();
    r.host.consumer.seen = false;
    r.Eval();
    CHECK(!r.engine->PassReady());

    // before any evaluate there is nothing to release, and nothing breaks
    Rig q;
    if (q.Create())
        q.engine->OnNgxShutdown();
}

// A depth resource whose device is gone (GetDevice answers nothing): the immediate context cannot be found.
struct DeviceLessResource final : ID3D11Resource
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11DeviceChild) || iid == __uuidof(ID3D11Resource))
        {
            *out = static_cast<ID3D11Resource*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; } // static lifetime
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override { *dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
};

void NativeContextMissingIsReported()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    CHECK(!r.engine->NativeContextMissing());
    static DeviceLessResource lost;
    ID3D11Resource* const real = r.params.resources["Depth"];
    r.params.resources["Depth"] = &lost;
    const size_t events = r.sink.events.size();
    r.Eval();
    CHECK(r.engine->NativeContextMissing()); // said to the status line, nothing published
    CHECK(r.sink.events.size() == events);
    r.params.resources["Depth"] = real;
    CHECK(r.Capture());
    CHECK(!r.engine->NativeContextMissing()); // and cleared by the next good evaluate
}

void ExceptionsDoNotEscape()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    r.params.throwOnGet = true;
    r.Eval(); // the seam is noexcept: whatever the parameter block does, nothing comes out
    r.params.throwOnGet = false;
    CHECK(r.Capture()); // and the next evaluate works
}

void NoDepthResource()
{
    Rig r;
    if (!r.Create())
        return;
    r.params.resources.erase("Depth"); // the game gave no depth: not a capture, not a refusal, not the context's fault
    r.Eval();
    CHECK(r.sink.events.empty() && r.engine->Stats().captured == 0 && r.engine->Stats().refused == 0);
    CHECK(!r.engine->NativeContextMissing() && !r.engine->PassReady());
}

ComPtr<ID3D11Texture2D> MakeDepth(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    ComPtr<ID3D11Texture2D> texture;
    device->CreateTexture2D(&td, nullptr, &texture);
    return texture;
}

// The capture line is said once per change of ANY field of what it reports: the game's depth (size, format, bind flags), the base, the
// render size, the output size.
void CaptureLineChangesWithEachField()
{
    Rig r;
    if (!r.Create())
        return;
    r.params.uints["Width"] = 64;
    r.params.uints["Height"] = 36;
    CHECK(r.Capture());
    size_t lines = LogLines("capture: the game's depth is");
    CHECK(lines >= 1);
    const auto step = [&](const char* what, auto change) {
        change();
        const bool captured = r.Capture();
        const size_t now = LogLines("capture: the game's depth is");
        if (!captured || now != lines + 1)
            std::printf("  the change of '%s' did not make exactly one new line (captured %d, lines %zu -> %zu)\n", what, captured ? 1 : 0, lines, now);
        CHECK(captured && now == lines + 1);
        CHECK(r.Capture() && LogLines("capture: the game's depth is") == now); // the same again: no line
        lines = now;
    };
    constexpr UINT ds = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> wider = MakeDepth(r.device.Get(), 160, kH, DXGI_FORMAT_R32_TYPELESS, ds);
    ComPtr<ID3D11Texture2D> taller = MakeDepth(r.device.Get(), 160, 90, DXGI_FORMAT_R32_TYPELESS, ds);
    ComPtr<ID3D11Texture2D> half = MakeDepth(r.device.Get(), 160, 90, DXGI_FORMAT_R16_TYPELESS, ds);
    ComPtr<ID3D11Texture2D> renderable = MakeDepth(r.device.Get(), 160, 90, DXGI_FORMAT_R16_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (wider == nullptr || taller == nullptr || half == nullptr || renderable == nullptr)
        return;
    step("the depth width", [&] { r.params.resources["Depth"] = wider.Get(); });
    step("the depth height", [&] { r.params.resources["Depth"] = taller.Get(); });
    step("the depth format", [&] { r.params.resources["Depth"] = half.Get(); });
    step("the depth bind flags", [&] { r.params.resources["Depth"] = renderable.Get(); });
    step("the base x", [&] { r.params.uints["DLSS.Input.Depth.Subrect.Base.X"] = 8; });
    step("the base y", [&] { r.params.uints["DLSS.Input.Depth.Subrect.Base.Y"] = 4; });
    step("the render width", [&] { r.params.uints["Width"] = 48; });
    step("the render height", [&] { r.params.uints["Height"] = 27; });
    step("the output width", [&] {
        r.params.uints["OutWidth"] = 96;
        r.host.consumer.width = 96;
    });
    step("the output height", [&] {
        r.params.uints["OutHeight"] = 54;
        r.host.consumer.height = 54;
    });
    // and the line says what changed
    const std::string line = LastLine("capture: the game's depth is");
    CHECK(line.find("160x90") != std::string::npos && line.find("base 8,4") != std::string::npos && line.find("render 48x27") != std::string::npos &&
          line.find("output 96x54") != std::string::npos);
}

void RefusalsAreLoggedOncePerChange()
{
    Rig r;
    if (!r.Create(D3D11_BIND_DEPTH_STENCIL)) // no shader resource bind flag: the pass refuses
        return;
    const size_t before = LogLines("capture refused");
    CHECK(!r.Capture());
    CHECK(!r.Capture());
    CHECK(!r.Capture());
    CHECK(LogLines("capture refused") == before + 1);
    CHECK(LastLine("capture refused").find("no shader resource bind flag") != std::string::npos);
    CHECK(r.engine->Stats().refused == 3 && r.engine->Stats().captured == 0);
    CHECK(r.engine->Stats().lastResult == static_cast<uint32_t>(Result::NoShaderResource));
    // later than a minute: said again
    r.host.now += 61'000;
    r.host.consumer.seenMs = r.host.now;
    CHECK(!r.Capture());
    CHECK(LogLines("capture refused") == before + 2);
}

void BaseIsNeverClamped()
{
    Rig r;
    if (!r.Create())
        return;
    r.params.uints["Width"] = 64;
    r.params.uints["Height"] = 36;
    r.params.uints["DLSS.Input.Depth.Subrect.Base.X"] = 32; // inside the source
    r.params.uints["DLSS.Input.Depth.Subrect.Base.Y"] = 18;
    CHECK(r.Capture());
    CHECK(LastLine("capture: the game's depth is").find("base 32,18") != std::string::npos); // x first, y second
    r.params.uints["DLSS.Input.Depth.Subrect.Base.X"] = 0xFFFFFFFFu; // a negative base (an int that came as unsigned)
    const size_t valid = r.sink.Valid();
    r.Eval();
    CHECK(r.sink.Valid() == valid);                                                 // refused, not read from the corner
    CHECK(r.engine->Stats().lastResult == static_cast<uint32_t>(Result::NotContained));
    r.params.uints["DLSS.Input.Depth.Subrect.Base.X"] = 100; // 100 + 64 > 128
    r.Eval();
    CHECK(r.sink.Valid() == valid && r.engine->Stats().lastResult == static_cast<uint32_t>(Result::NotContained));
}

void RawDrawGoesWithTheHookedContext()
{
    Rig r;
    if (!r.Create())
        return;
    g_rawDraws = 0;
    r.host.hooked = nullptr; // the lighting hooks are not installed: the context's own Draw
    CHECK(r.Capture());
    CHECK(g_rawDraws == 0);
    r.host.hooked = r.context.Get(); // they are, on this very context: the raw trampoline
    const size_t events = r.sink.events.size();
    CHECK(r.Capture());
    CHECK(g_rawDraws == 1);
    bool withdrew = false;
    for (size_t i = events; i < r.sink.events.size(); ++i)
        withdrew = withdrew || !r.sink.events[i].capture.valid;
    CHECK(withdrew); // the pass was rebuilt for the other draw: the old one published valid = false first
    CHECK(r.Capture() && g_rawDraws == 2);
    // hooks on another context: not valid here
    int other = 0;
    r.host.hooked = reinterpret_cast<ID3D11DeviceContext*>(&other);
    CHECK(r.Capture());
    CHECK(g_rawDraws == 2);
}
} // namespace

int main()
{
    {
        Rig probe;
        if (!probe.Create())
        {
            std::puts("SKIP: no WARP D3D11 device");
            return 0;
        }
    }
    OptionOffIsOneLoad();
    HappyPath();
    RenderSize();
    PreviewPublishesNothing();
    OutputFallbacks();
    NativeDeviceNotNgxContext();
    OptionOffFromAnotherThread();
    RequestsOneByOne();
    ConsumerLoss();
    Registration();
    NgxShutdown();
    NativeContextMissingIsReported();
    NoDepthResource();
    ExceptionsDoNotEscape();
    CaptureLineChangesWithEachField();
    RefusalsAreLoggedOncePerChange();
    BaseIsNeverClamped();
    RawDrawGoesWithTheHookedContext();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_eval: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_eval: all checks passed");
    return 0;
}
