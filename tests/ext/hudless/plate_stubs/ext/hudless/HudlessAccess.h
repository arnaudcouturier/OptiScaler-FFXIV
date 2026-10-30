#pragma once
// Test replacement of ext/hudless/HudlessAccess.h: the same accessors over a test-owned bridge struct, so the engine's
// slot creation, produce signal and pass run against real WARP devices without a game.
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <vector>

namespace plate_test
{
// The frame generator: the engine only asks it for the queue its produce fence lives on.
struct FakeFg
{
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* GetCommandQueue() { return queue; }
};

// The bridge extension the engine reads for the output encoding.
struct FakeBridgeExt
{
    bool hdrOutput = false;
};
} // namespace plate_test

// What Dx11wDx12SC exposes to ext/hudless; the test fills it in.
class Dx11wDx12SC
{
  public:
    ID3D11Device* _dx11Device = nullptr;
    ID3D11DeviceContext* _dx11Context = nullptr;
    ID3D12Device* _dx12Device = nullptr;
    plate_test::FakeFg* _fg = nullptr;
    plate_test::FakeBridgeExt* _ext = nullptr;
    DXGI_FORMAT _bufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT _currentFakeIndex = 0;
    std::vector<ID3D11Texture2D*> _sharedDx11BackBufferCopies;
    ID3D12Fence* _copyFence = nullptr;
    uint64_t _lastInteropCopyFenceValue = 0;
};

namespace ext
{
struct Access
{
    struct Hudless
    {
        static ID3D11Device* Dx11Device(Dx11wDx12SC& sc) { return sc._dx11Device; }
        static ID3D11DeviceContext* Dx11Context(Dx11wDx12SC& sc) { return sc._dx11Context; }
        static ID3D12Device* Dx12Device(Dx11wDx12SC& sc) { return sc._dx12Device; }
        static DXGI_FORMAT BufferFormat(const Dx11wDx12SC& sc) { return sc._bufferFormat; }
        static bool HdrOutput(const Dx11wDx12SC& sc) { return sc._ext != nullptr && sc._ext->hdrOutput; }
        static UINT FakeIndex(const Dx11wDx12SC& sc) { return sc._currentFakeIndex; }
        static plate_test::FakeFg* Fg(Dx11wDx12SC& sc) { return sc._fg; }
        static ID3D11Texture2D* SharedBackBuffer(Dx11wDx12SC& sc, UINT slot)
        {
            return slot < sc._sharedDx11BackBufferCopies.size() ? sc._sharedDx11BackBufferCopies[slot] : nullptr;
        }
        static ID3D12Fence* CopyFence(Dx11wDx12SC& sc) { return sc._copyFence; }
        static uint64_t LastCopyValue(const Dx11wDx12SC& sc) { return sc._lastInteropCopyFenceValue; }
    };
};
} // namespace ext
