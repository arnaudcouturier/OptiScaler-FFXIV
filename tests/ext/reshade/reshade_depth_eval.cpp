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

#include "reshade_depth_eval_rig.h"

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
using namespace evaltest;

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
    r.frame = 77; // the depth-owned counter before this capture
    r.host.now = 123'456;
    const size_t identity = LogLines("contexts:");
    CHECK(r.Capture());
    CHECK(r.sink.events.size() == 1);
    const DepthCapture c = r.sink.events.back().capture;
    CHECK(c.valid && c.copyView != 0);
    CHECK(c.outputW == kW && c.outputH == kH && c.renderW == kW && c.renderH == kH);
    CHECK(c.frame == 78 && r.frame == 78 && c.tickMs == 123'456);                  // one tick for the capture, the stamp is the new value; the clock
    CHECK(c.device == reinterpret_cast<uintptr_t>(r.device.Get()));              // the NATIVE device
    CHECK(r.engine->PassReady());
    CHECK(r.engine->Stats().captured == 1 && r.engine->Stats().refused == 0 && r.engine->Stats().lastResult == 0);
    CHECK(r.host.subscribes == 1);                                                // the subscription is asked for at the first go (a no-op when it was made at load)
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

// The counter belongs to the capture side: one tick per capture, applied before the capture can be published (so a pass that reads the
// counter never sees a stamp from the future); refused captures, previews, a lost consumer and the option off do not move it.
void CounterTicksPerCapture()
{
    Rig r;
    if (!r.Create())
        return;
    r.frame = 40;
    for (int i = 1; i <= 4; ++i)
    {
        CHECK(r.Capture());
        CHECK(r.frame == 40 + i && r.sink.events.back().capture.frame == 40 + i); // consecutive stamps, each equal to the counter
    }
    CHECK(r.engine->Stats().frame == 44);
    // a preview-sized Output publishes nothing and moves nothing
    r.params.uints["OutWidth"] = 576;
    r.params.uints["OutHeight"] = 960;
    r.Eval();
    CHECK(r.frame == 44);
    r.params.uints["OutWidth"] = kW;
    r.params.uints["OutHeight"] = kH;
    // nobody reads the depth: withdrawn, not captured, not ticked
    r.host.consumer.seen = false;
    r.Eval();
    CHECK(r.frame == 44);
    r.host.consumer.seen = true;
    // a refused capture (no shader resource bind flag) is not a tick either, however often
    Rig q;
    if (!q.Create(D3D11_BIND_DEPTH_STENCIL))
        return;
    q.frame = 40;
    for (int i = 0; i < 3; ++i)
        CHECK(!q.Capture());
    CHECK(q.frame == 40 && q.engine->Stats().refused == 3 && q.engine->Stats().frame == 40);
    // the option off: nothing, not even a read
    r.gate.SetEnabled(false);
    r.gate.TakeRequests();
    r.Eval();
    CHECK(r.frame == 44);
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

void HalfAnOutputSizeIsNoSize()
{
    Rig r;
    if (!r.Create())
        return;
    r.params.uints["OutWidth"] = 64; // OutHeight is missing: the pair is not used, the Output texture (128x72) says the size
    r.params.uints.erase("OutHeight");
    CHECK(r.Capture());
    CHECK(r.sink.events.back().capture.outputW == kW && r.sink.events.back().capture.outputH == kH);
    r.params.uints.erase("OutWidth"); // and the other way round
    r.params.uints["OutHeight"] = 36;
    CHECK(r.Capture());
    CHECK(r.sink.events.back().capture.outputW == kW && r.sink.events.back().capture.outputH == kH);
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

    // the process is going down: nothing is touched (no release, no request)
    Rig s;
    if (s.Create())
    {
        CHECK(s.Capture());
        const size_t published = s.sink.events.size();
        s.host.shutting = true;
        s.engine->OnNgxShutdown();
        s.host.thread = GetCurrentThreadId() + 1;
        s.engine->OnNgxShutdown();
        s.host.thread = GetCurrentThreadId();
        CHECK(s.engine->PassReady() && s.sink.events.size() == published);
        CHECK((s.gate.Bits() & glue::EvalGate::kRequests) == 0);
    }
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

// A resource whose calls fault (a hardware access violation, which a C++ catch does not see).
struct FaultyResource final : ID3D11Resource
{
    enum class Where
    {
        QueryInterface,
        GetDevice,
    };
    explicit FaultyResource(Where w) : where(w) {}
    Where where;
    static void Fault() { *static_cast<volatile int*>(nullptr) = 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        if (where == Where::QueryInterface)
            Fault();
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override
    {
        if (where == Where::GetDevice)
            Fault();
        *device = nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* dimension) override { *dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
};

// The reads on the pointers the upscaler just used (the parameter block, the Output and Depth resources, the depth's device) are
// guarded: a hardware fault in one of them ends that evaluate and nothing else.
void HardwareFaultsStayInside()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    const size_t events = r.sink.events.size();

    const size_t faultLines = LogLines("failed with");
    r.params.faultOnGet = true; // the parameter block faults on the first read
    r.Eval();
    r.Eval(); // (and again: said once)
    r.params.faultOnGet = false;
    CHECK(r.sink.events.size() == events);
    CHECK(LogLines("failed with") == faultLines + 1);
    CHECK(LastLine("failed with").find("0xc0000005") != std::string::npos); // the access violation, named
    CHECK(r.Capture()); // the next evaluate works

    static FaultyResource faultyOutput(FaultyResource::Where::QueryInterface);
    r.params.uints.erase("OutWidth"); // the Output resource is what says the output size
    r.params.uints.erase("OutHeight");
    ID3D11Resource* const output = r.params.resources["Output"];
    r.params.resources["Output"] = &faultyOutput;
    r.Eval();
    CHECK(r.sink.events.size() == events + 1);
    r.params.resources["Output"] = output;
    CHECK(r.Capture());

    static FaultyResource faultyDepth(FaultyResource::Where::GetDevice);
    ID3D11Resource* const depth = r.params.resources["Depth"];
    r.params.resources["Depth"] = &faultyDepth; // the depth's device cannot be asked
    const size_t before = r.sink.events.size();
    r.Eval();
    CHECK(r.sink.events.size() == before);
    CHECK(!r.engine->NativeContextMissing()); // a fault is not a missing context
    r.params.resources["Depth"] = depth;
    CHECK(r.Capture());
}

void NullParameters()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    const size_t events = r.sink.events.size();
    const size_t faultLines = LogLines("failed with");
    r.engine->OnEvaluated(reinterpret_cast<ID3D11DeviceContext*>(kBogusContext), nullptr, {}); // no parameter block: nothing to read
    CHECK(r.sink.events.size() == events);
    CHECK(LogLines("failed with") == faultLines); // refused up front, not by a fault
    CHECK(r.Capture());
    // requests are still carried out with no parameter block
    std::thread([&] { r.gate.SetEnabled(false); }).join();
    r.engine->OnEvaluated(nullptr, nullptr, {});
    CHECK(!r.engine->PassReady() && !r.sink.events.back().capture.valid);
}

// The reference to the native immediate context that the depth read takes is given back at the end of every evaluate.
void NativeReferenceIsGivenBack()
{
    Rig r;
    if (!r.Create())
        return;
    CHECK(r.Capture());
    const ULONG settled = RefCount(r.context.Get());
    for (int i = 0; i < 5; ++i)
        CHECK(r.Capture());
    CHECK(RefCount(r.context.Get()) == settled);
    r.params.resources.erase("Depth"); // (and when the evaluate ends early)
    r.Eval();
    CHECK(RefCount(r.context.Get()) == settled);
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
    CounterTicksPerCapture();
    RenderSize();
    PreviewPublishesNothing();
    OutputFallbacks();
    HalfAnOutputSizeIsNoSize();
    NativeDeviceNotNgxContext();
    OptionOffFromAnotherThread();
    RequestsOneByOne();
    ConsumerLoss();
    Registration();
    NgxShutdown();
    NativeContextMissingIsReported();
    NoDepthResource();
    NativeReferenceIsGivenBack();
    ExceptionsDoNotEscape();
    HardwareFaultsStayInside();
    NullParameters();
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
