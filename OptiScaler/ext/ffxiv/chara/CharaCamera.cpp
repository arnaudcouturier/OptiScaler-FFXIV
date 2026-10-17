#include "pch.h"

#include "ext/ffxiv/chara/CharaCamera.h"
#include <d3d11_1.h>
#include "ext/ffxiv/chara/CharaDispatch.h"

#include <atomic>

namespace ext::ffxiv::chara::camera
{
using Microsoft::WRL::ComPtr;

namespace
{
ComPtr<ID3D11Buffer> g_copy;
uint32_t g_copyBytes = 0;
UINT g_first = 0;
UINT g_count = 0;
bool g_ready = false;
std::atomic<uint32_t> g_rejectLogs { 0 };
uint32_t g_lastCapturedBytes = 0;
UINT g_lastFirst = ~0u;
UINT g_lastCount = ~0u;
} // namespace

Result Capture(ID3D11DeviceContext* context)
{
    if (context == nullptr)
        return Result::Failed;

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
        return Result::Failed;

    ComPtr<ID3D11Buffer> source;
    UINT first = 0, count = 0;
    context1->PSGetConstantBuffers1(kSlot, 1, &source, &first, &count);
    if (source == nullptr)
        return Result::Rejected;

    D3D11_BUFFER_DESC bd {};
    source->GetDesc(&bd);
    if (!Accept(bd.ByteWidth, first, count))
    {
        if (g_rejectLogs.fetch_add(1) < 4)
            LOG_INFO("CharaView camera capture rejected: native buffer {} bytes", bd.ByteWidth);
        return Result::Rejected;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
        return Result::Failed;

    if (g_copy == nullptr || !CanReuseCopy(g_copyBytes, bd.ByteWidth))
    {
        D3D11_BUFFER_DESC cd {};
        cd.ByteWidth = bd.ByteWidth;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> created;
        if (FAILED(device->CreateBuffer(&cd, nullptr, &created)))
        {
            g_ready = false;
            return Result::Failed;
        }
        g_copy = created;
        g_copyBytes = bd.ByteWidth;
    }

    {
        ReplayScope scope; // the copy goes through the hooked CopyResource
        context->CopyResource(g_copy.Get(), source.Get());
    }
    g_first = first;
    g_count = count;
    g_ready = true;
    if (g_lastCapturedBytes != bd.ByteWidth || g_lastFirst != first || g_lastCount != count)
    {
        g_lastCapturedBytes = bd.ByteWidth;
        g_lastFirst = first;
        g_lastCount = count;
        LOG_INFO("CharaView camera captured: buffer {} bytes, first constant {}, range {} constants", bd.ByteWidth, first, count);
    }
    return Result::Captured;
}

bool Ready() { return g_ready && g_copy != nullptr; }
ID3D11Buffer* Buffer() { return g_copy.Get(); }
UINT FirstConstant() { return g_first; }
UINT ConstantCount() { return g_count; }

void Release()
{
    g_copy.Reset();
    g_copyBytes = 0;
    g_ready = false;
    g_lastCapturedBytes = 0;
    g_lastFirst = ~0u;
    g_lastCount = ~0u;
}
} // namespace ext::ffxiv::chara::camera
