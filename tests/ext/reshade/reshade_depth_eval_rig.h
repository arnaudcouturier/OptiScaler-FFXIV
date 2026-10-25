#pragma once
// The rig of the capture-engine tests (reshade_depth_eval.cpp, reshade_depth_loop.cpp): a WARP device with a depth texture and an Output,
// a fake NVSDK_NGX_Parameter, a fake host (registration, consumer, hooks), a recording sink and the real copy pass under the real
// CaptureEngine. No CHECK here: each test file has its own.
#include "pch.h"

#include "ext/reshade/ReShadeDepthEval.h"

#include <d3d11_1.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace evaltest
{
using Microsoft::WRL::ComPtr;
using namespace ext::reshade::depth;

inline size_t LogLines(const char* needle)
{
    size_t n = 0;
    for (const std::string& line : depthtest_log::Lines())
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}
inline std::string LastLine(const char* needle)
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
    std::map<std::string, float> floats;
    std::map<std::string, ID3D11Resource*> resources;
    mutable int gets = 0;
    mutable std::vector<std::string> asked;
    bool throwOnGet = false; // a parameter block that throws
    bool faultOnGet = false; // ... and one that faults (a hardware access violation, which /EHsc does not catch)

    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned int) override {}
    void Set(const char*, int) override {}
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}

    NVSDK_NGX_Result Get(const char* name, unsigned long long*) const override { return Miss(name); }
    NVSDK_NGX_Result Get(const char* name, float* out) const override
    {
        Note(name);
        const auto it = floats.find(name);
        if (it == floats.end())
            return NVSDK_NGX_Result_Fail;
        *out = it->second;
        return NVSDK_NGX_Result_Success;
    }
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
        if (faultOnGet)
            *static_cast<volatile int*>(nullptr) = 1;
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

inline int g_rawDraws = 0;
inline void CountingDraw(ID3D11DeviceContext* c)
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
    std::function<ConsumerInfo(int64_t)> consumerSource; // when set, the consumer comes from here (the real binder, in the loop test)
    ConsumerInfo GetConsumer(int64_t nowMs) override { return ++calls, consumerSource ? consumerSource(nowMs) : consumer; }
    ID3D11DeviceContext* HookedContext() override { return ++calls, hooked; }
    DrawTriangleFn RawDraw() override { return ++calls, raw; }
};

// the binder's side of the sink, as far as it matters here (the same as the copy pass test's)
inline ULONG RefCount(IUnknown* p)
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
    ICaptureSink* forward = nullptr; // when set, every capture goes on to this sink as well (the binder, in the loop test)
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
        if (forward != nullptr)
            forward->Publish(c);
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

} // namespace evaltest
