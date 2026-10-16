#pragma once
// A minimal IDXGISwapChain3 for the S10 paste tests: it only knows its current backbuffer (index 0, a D3D12 texture the
// test provides). Everything else is E_NOTIMPL. UiPaste::Paste needs exactly QueryInterface(IDXGISwapChain3),
// GetCurrentBackBufferIndex and GetBuffer, so no desktop session / window / composition setup is needed.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>

namespace ui_test
{
class FakeSwapChain final : public IDXGISwapChain3
{
  public:
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) || iid == __uuidof(IDXGIDeviceSubObject) ||
            iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IDXGISwapChain1) || iid == __uuidof(IDXGISwapChain2) ||
            iid == __uuidof(IDXGISwapChain3))
        {
            *out = static_cast<IDXGISwapChain3*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; } // owned by the test, never deleted here

    // IDXGISwapChain3: what the module uses
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID iid, void** surface) override
    {
        if (index != 0 || !buffer)
            return DXGI_ERROR_INVALID_CALL;
        return buffer->QueryInterface(iid, surface);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT, UINT, const DXGI_PRESENT_PARAMETERS*) override { return E_NOTIMPL; }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION*) override { return E_NOTIMPL; }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT*) override { return E_NOTIMPL; }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return nullptr; }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F*) override { return E_NOTIMPL; }
    // IDXGISwapChain3
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*) override
    {
        return E_NOTIMPL;
    }


  private:
    std::atomic<ULONG> references { 1 };
};
} // namespace ui_test
