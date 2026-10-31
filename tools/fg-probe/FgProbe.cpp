// FgProbe: the release-model measurement for a frame-generation library, in its own process.
//
// Why a separate executable: deciding whether a library's swapchain destroy call already ends the COM object needs a
// release of a reference that may be freed, and it needs chains of its own. Doing that inside the game would (a) release
// process-global state a live chain depends on (the XeLL context behind XeFG's destroy), (b) create a second frame
// generation chain next to the game's, and (c) run a use-after-free on purpose in the game's heap. None of that is
// acceptable in a player's process, so the destructive half lives here: the repository builds this exe, it loads the same
// library files from a folder given on the command line, creates its own D3D12 device, command queue, hidden window and
// chains, and prints the classification as data. The product never runs it; the in-game probe (ext/fg/outswitch/Probe.cpp)
// only reads the live chain.
//
// The configuration matters: the product always attaches a XeLL context to its XeFG context, and a XeLL context is the
// obvious holder of the reference that makes a destroy behave differently. So the XeFG leg creates and attaches one
// exactly as the product does, and every row carries `config=xell` (XeFG) or `config=direct` (FSR FG: the library called
// directly, without the product's loader or its release patch, which only change who calls Release).
//
// What it measures, per class and per library build, three times:
//   run B  create a chain, put a private-data witness on it, destroy it through the library, then poll whether the window
//          is free (a plain flip chain can be created on it) until the same answer holds for kQuietMs without a change,
//          up to kReadBudgetMs. The hold is 3 s: a library that finishes its teardown on its own thread (a join with a
//          timeout, a fence wait) must not be misread as M2 because the poll stopped asking after 500 ms. A window that
//          is free means the destroy ended the object (M1); a window that stays bound means the context held a reference
//          (M2), and then exactly one release of the chain must free it, which the second poll confirms - but only after
//          the witness agreed that the object was still there.
//   the second destroy, the other order, and video memory, for the same run:
//          after the pin release, destroy the context again and record the code (XeFG answers POINTER_STILL_IN_USE while
//          the chain is held, so a success afterwards is the proof that its context was leaked, not destroyed, on the
//          switch path); create a second chain and do the reverse order (release the pin, then destroy) to see whether
//          that order leaves the window free; and read local video memory in use before the chain and after the teardown,
//          because a leaked context shows up there and nowhere else.
//   the standstill count: AddRef, read, Release on the fresh chain, so the row carries the reference count the preflight
//          compares a live chain against.
//   run A  only when run B said M2 (so the extra release is safe by definition): keep one reference, destroy, release the
//          kept reference. A clean return corroborates M2; under a page heap on this exe an M1 library would stop here,
//          which is the second signal for the classification.
// Three runs have to agree, the window polls have to settle, and the witness has to agree, before a row is printed: a
// library that finishes its teardown on its own thread must never be classified M2 on one early read. A noisy result
// prints verdict=unknown, and the switch refuses that build.
//
// Running it:      tools\fg-probe\build.ps1   then   x64\fg-probe.exe --folder <dir with the library DLLs>
// Page heap:       gflags /p /enable fg-probe.exe /full   (or Application Verifier) before a run, for run A's evidence.
//                  gflags needs an elevated shell and writes the Image File Execution Options key for that image name; the
//                  host prints `pageheap=on|off` so a saved run says whether it was active.
// Then:            tools\fg-probe\Update-ModelTable.ps1 -Source <saved output>
//
// Library files: amd_fidelityfx_framegeneration_dx12.dll for FSR FG, libxess_fg.dll + libxell.dll for XeFG. Nothing is
// written next to them and nothing is installed.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell.h>
#include <xell/xell_d3d12.h>

#include "ext/fg/outswitch/LibraryModel.h"
#include "ext/fg/outswitch/QueueRules.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace ext::fg::outswitch;

namespace
{
constexpr uint32_t kWindowSize = 64;
constexpr int kReadStepMs = 20;
// The window has to hold one answer for this long before anything is released or a run is accepted: a library that
// finishes its teardown on its own thread keeps the window bound for a moment, and a release in that moment is exactly
// the wrong M2 the model table must never contain.
constexpr int kQuietMs = 3000;
constexpr int kReadBudgetMs = 9000;
// A library call that blocks must not keep the process alive: the host prints what it has and stops. The clock is
// silence, not total runtime: a full three-run measurement of both classes takes minutes on purpose (the window has to
// hold an answer for three seconds before every release), and the limit exists for a call that never returns.
constexpr int kWatchdogMs = 90000;
constexpr int kWatchdogStepMs = 5000;
// The destructive second-destroy experiment, off by default: see step 3 in MeasureClass.
bool g_secondDestroy = false;
// The D3D12 debug layer with DRED (--debug, and always with --cycle).
bool g_debugLayer = false;
// The presenting cycle presents to a window the desktop composes: a hidden window may answer DXGI_STATUS_OCCLUDED, and a
// library may then skip its own present path, which is the path the cycle exists to exercise.
constexpr uint32_t kCycleWindowSize = 256;

std::string Narrow(const wchar_t* text)
{
    char buffer[MAX_PATH * 4] {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, static_cast<int>(sizeof(buffer)), nullptr, nullptr);
    return buffer;
}

// The command line is UTF-8 on this machine; a folder outside ASCII must still open.
std::wstring Widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring(text.begin(), text.end());
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

// The output file and stdout are the only record of a run, so every line is written through here and flushed at once: a
// crash or a kill must not lose the runs that already happened.
FILE* g_out = nullptr;

// The last time a line was printed: the watchdog measures silence, not the runtime of a measurement that is supposed to
// take minutes.
std::atomic<uint64_t> g_lastOutputMs { 0 };

void Emit(const char* text)
{
    g_lastOutputMs.store(GetTickCount64(), std::memory_order_relaxed);
    std::fputs(text, stdout);
    std::fflush(stdout);
    if (g_out != nullptr)
    {
        std::fputs(text, g_out);
        std::fflush(g_out);
    }
}

void Emitf(const char* format, ...)
{
    char line[512] {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Emit(line);
}

// Is a page heap plausibly active for this image? gflags stores the flag in the Image File Execution Options key of the
// image name, and Application Verifier loads its verifier DLL into the process. Both are read only.
bool PageHeapActive()
{
    wchar_t path[MAX_PATH] {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return false;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;

    wchar_t key[512] {};
    std::swprintf(key, sizeof(key) / sizeof(key[0]),
                  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%s", name);
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &handle) == ERROR_SUCCESS)
    {
        DWORD flags = 0;
        DWORD size = sizeof(flags);
        DWORD type = 0;
        const LONG read = RegQueryValueExW(handle, L"GlobalFlag", nullptr, &type, reinterpret_cast<LPBYTE>(&flags), &size);
        RegCloseKey(handle);
        constexpr DWORD kHeapPageAllocs = 0x02000000;
        if (read == ERROR_SUCCESS && type == REG_DWORD && (flags & kHeapPageAllocs) != 0)
            return true;
    }
    return GetModuleHandleW(L"verifier.dll") != nullptr || GetModuleHandleW(L"vfbasics.dll") != nullptr;
}

// ---- the private-data witness ------------------------------------------------------------------------------------

struct WitnessState
{
    volatile LONG released = 0;
    volatile LONG alive = 0;
};

// The interface the chain is asked to hold. It reports only when its last reference is dropped, which is what "the
// interface died with the object" means. It is a diagnostic, never a decision.
class Witness final : public IUnknown
{
  public:
    explicit Witness(WitnessState& state) : _state(state) { InterlockedIncrement(&_state.alive); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown))
        {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&_refs)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG left = InterlockedDecrement(&_refs);
        if (left == 0)
        {
            InterlockedIncrement(&_state.released);
            InterlockedDecrement(&_state.alive);
            delete this;
        }
        return static_cast<ULONG>(left);
    }

  private:
    LONG _refs = 1;
    WitnessState& _state;
};

const GUID kWitnessGuid = { 0x6f2a3c9b, 0x3f6d, 0x4a3e, { 0x9a, 0x1c, 0x2b, 0x7e, 0x55, 0x14, 0x8f, 0x31 } };

// ---- the two libraries, loaded from the folder on the command line -----------------------------------------------

class Module
{
  public:
    ~Module()
    {
        if (_handle != nullptr)
            FreeLibrary(_handle);
    }

    bool Load(const std::wstring& folder, const wchar_t* file, char (&error)[256])
    {
        const std::wstring path = folder + L"\\" + file;
        // Search the library's own folder first: libxess_fg.dll pulls libxell.dll and libxess.dll from beside it.
        _handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (_handle == nullptr)
        {
            std::snprintf(error, sizeof(error), "load %s failed error=%lu", Narrow(path.c_str()).c_str(), GetLastError());
            return false;
        }
        _path = path;
        return true;
    }

    template <class T> T Get(const char* name)
    {
        if (_handle == nullptr)
            return nullptr;
        return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(_handle, name)));
    }

    // The library's identity: the file on disk, the module's PE timestamp and size.
    bool Identify(LibraryIdentity& identity) const
    {
        HANDLE file = CreateFileW(_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size {};
        const bool sized = GetFileSizeEx(file, &size) != 0;
        CloseHandle(file);
        if (!sized)
            return false;

        const wchar_t* name = std::wcsrchr(_path.c_str(), L'\\');
        name = name != nullptr ? name + 1 : _path.c_str();
        WideCharToMultiByte(CP_UTF8, 0, name, -1, identity.file, static_cast<int>(sizeof(identity.file)), nullptr, nullptr);
        identity.size = static_cast<uint64_t>(size.QuadPart);

        const auto* base = static_cast<const uint8_t*>(static_cast<const void*>(_handle));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (pe->Signature == IMAGE_NT_SIGNATURE)
                identity.stamp = pe->FileHeader.TimeDateStamp;
        }
        return !identity.Empty();
    }

  private:
    HMODULE _handle = nullptr;
    std::wstring _path;
};

// ---- the factory the libraries create their inner chain through ---------------------------------------------------

// A frame-generation library creates its real chain through the factory it is handed, on a queue of its own choosing.
// The product sees that queue in its hooked factory and points the D3D11 interop at it; this wrapper records it the same
// way, so the host knows which queue the library's chain presents on without asking the library (FSR FG's chain does not
// answer GetDevice for a queue, XeFG's answers with the device). Every call is forwarded unchanged.
class RecordingFactory final : public IDXGIFactory7
{
  public:
    explicit RecordingFactory(IDXGIFactory2* real) : real_(real) { real_->AddRef(); }
    ~RecordingFactory()
    {
        Forget();
        real_->Release();
    }

    // The queue of the last chain created through this factory, with one reference of the caller's (null when the
    // device the library passed was not a queue). The recording is cleared.
    ID3D12CommandQueue* TakeQueue()
    {
        ID3D12CommandQueue* queue = queue_;
        queue_ = nullptr;
        return queue;
    }
    void Forget()
    {
        if (queue_ != nullptr)
            queue_->Release();
        queue_ = nullptr;
    }
    int Creates() const { return creates_; }

    // IUnknown: the wrapper answers every factory interface itself, so a library that asks for a newer one keeps
    // creating through it. Anything else is the real factory's.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
            riid == __uuidof(IDXGIFactory7))
        {
            void* real = nullptr;
            if (FAILED(real_->QueryInterface(riid, &real)))
            {
                *out = nullptr;
                return E_NOINTERFACE; // the real factory does not have it either
            }
            static_cast<IUnknown*>(real)->Release();
            *out = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG left = InterlockedDecrement(&refs_);
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override
    {
        return real_->SetPrivateData(name, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override
    {
        return real_->SetPrivateDataInterface(name, unknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override
    {
        return real_->GetPrivateData(name, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return real_->GetParent(riid, parent); }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** adapter) override
    {
        return real_->EnumAdapters(index, adapter);
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override
    {
        return real_->MakeWindowAssociation(window, flags);
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* window) override { return real_->GetWindowAssociation(window); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** chain) override
    {
        Record(device);
        return real_->CreateSwapChain(device, desc, chain);
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) override
    {
        return real_->CreateSoftwareAdapter(module, adapter);
    }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override
    {
        return real_->EnumAdapters1(index, adapter);
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return real_->IsCurrent(); }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return real_->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                     IDXGIOutput* output, IDXGISwapChain1** chain) override
    {
        Record(device);
        return real_->CreateSwapChainForHwnd(device, window, desc, fullscreen, output, chain);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                           IDXGISwapChain1** chain) override
    {
        Record(device);
        return real_->CreateSwapChainForCoreWindow(device, window, desc, output, chain);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override
    {
        return real_->GetSharedResourceAdapterLuid(resource, luid);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND window, UINT message, DWORD* cookie) override
    {
        return real_->RegisterStereoStatusWindow(window, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override
    {
        return real_->RegisterStereoStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override { real_->UnregisterStereoStatus(cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND window, UINT message, DWORD* cookie) override
    {
        return real_->RegisterOcclusionStatusWindow(window, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override
    {
        return real_->RegisterOcclusionStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override { real_->UnregisterOcclusionStatus(cookie); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* output, IDXGISwapChain1** chain) override
    {
        Record(device);
        return real_->CreateSwapChainForComposition(device, desc, output, chain);
    }

    // IDXGIFactory3 .. 7: the newer interfaces, through the real factory's own.
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return With<IDXGIFactory3, UINT>(0, [](auto* f) { return f->GetCreationFlags(); }); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) override
    {
        return With<IDXGIFactory4, HRESULT>(E_NOINTERFACE, [&](auto* f) { return f->EnumAdapterByLuid(luid, riid, adapter); });
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) override
    {
        return With<IDXGIFactory4, HRESULT>(E_NOINTERFACE, [&](auto* f) { return f->EnumWarpAdapter(riid, adapter); });
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override
    {
        return With<IDXGIFactory5, HRESULT>(E_NOINTERFACE, [&](auto* f) { return f->CheckFeatureSupport(feature, data, size); });
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE preference, REFIID riid,
                                                         void** adapter) override
    {
        return With<IDXGIFactory6, HRESULT>(E_NOINTERFACE, [&](auto* f) {
            return f->EnumAdapterByGpuPreference(index, preference, riid, adapter);
        });
    }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD* cookie) override
    {
        return With<IDXGIFactory7, HRESULT>(E_NOINTERFACE, [&](auto* f) { return f->RegisterAdaptersChangedEvent(event, cookie); });
    }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override
    {
        return With<IDXGIFactory7, HRESULT>(E_NOINTERFACE, [&](auto* f) { return f->UnregisterAdaptersChangedEvent(cookie); });
    }

  private:
    template <class Interface, class Result, class Call> Result With(Result fallback, Call call)
    {
        Interface* newer = nullptr;
        if (FAILED(real_->QueryInterface(IID_PPV_ARGS(&newer))) || newer == nullptr)
            return fallback;
        const Result result = call(newer);
        newer->Release();
        return result;
    }

    void Record(IUnknown* device)
    {
        ++creates_;
        Forget();
        if (device != nullptr)
            device->QueryInterface(IID_PPV_ARGS(&queue_)); // a D3D11 device answers no: nothing is recorded
    }

    IDXGIFactory2* real_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    LONG refs_ = 1;
    int creates_ = 0;
};

// ---- the device, the queue and the hidden window -----------------------------------------------------------------

struct Device
{
    IDXGIFactory2* factory = nullptr;
    IDXGIAdapter3* adapter3 = nullptr; // local video memory, for the leak a refused destroy leaves behind
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    HWND window = nullptr;
    // The window and chain size, and whether the window is shown (it never takes the focus).
    uint32_t size = kWindowSize;
    bool shown = false;
    bool debugLayer = false; // the debug layer was enabled before the device was created
    // The factory the libraries are handed in the presenting modes: it records the queue each library creates its real
    // chain on. The release-model rows keep the plain factory, which is what their config=direct names.
    RecordingFactory* recorder = nullptr;

    IDXGIFactory2* LibraryFactory() const { return recorder != nullptr ? recorder : factory; }

    DXGI_SWAP_CHAIN_DESC baseDesc {};

    bool Create()
    {
        if (g_debugLayer)
        {
            // Both before the device exists: the debug layer and DRED only describe a device created after them. A machine
            // without the Graphics Tools feature has no debug layer; the line says so and the run goes on without it.
            ID3D12Debug* debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug != nullptr)
            {
                debug->EnableDebugLayer();
                debug->Release();
                debugLayer = true;
            }
            ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
            const bool dredOn = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))) && dred != nullptr;
            if (dredOn)
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->Release();
            }
            Emitf("fg-probe debug layer=%s dred=%s\n", debugLayer ? "on" : "unavailable", dredOn ? "on" : "unavailable");
        }
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        {
            Emit("fg-probe error: no DXGI factory\n");
            return false;
        }
        if (shown)
            recorder = new RecordingFactory(factory);
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(0, &adapter) == DXGI_ERROR_NOT_FOUND)
        {
            Emit("fg-probe error: no adapter\n");
            return false;
        }
        if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
            adapter3 = nullptr; // not every adapter exposes the memory budget: the field just stays 0
        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        adapter->Release();
        if (FAILED(hr))
        {
            Emitf("fg-probe error: D3D12CreateDevice 0x%08X (needs a DX12 GPU)\n", static_cast<unsigned>(hr));
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
        {
            Emit("fg-probe error: no command queue\n");
            return false;
        }

        const wchar_t* className = L"OptiScalerFgProbeHostWindow";
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Emitf("fg-probe error: no window class error=%lu\n", GetLastError());
            return false;
        }
        // A shown window never takes the focus or any input: no activation, a tool window (no taskbar button), shown with
        // SW_SHOWNA. Nothing is ever sent to it or to any other window.
        const DWORD exStyle = shown ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0;
        window = CreateWindowExW(exStyle, className, L"", WS_POPUP, 0, 0, static_cast<int>(size), static_cast<int>(size),
                                 nullptr, nullptr, windowClass.hInstance, nullptr);
        if (window == nullptr)
        {
            Emitf("fg-probe error: no window error=%lu\n", GetLastError());
            return false;
        }
        if (shown)
            ShowWindow(window, SW_SHOWNA);

        baseDesc.OutputWindow = window;
        baseDesc.Windowed = TRUE;
        baseDesc.BufferDesc.Width = size;
        baseDesc.BufferDesc.Height = size;
        baseDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        baseDesc.BufferDesc.RefreshRate.Numerator = 0;
        baseDesc.BufferDesc.RefreshRate.Denominator = 1;
        baseDesc.SampleDesc.Count = 1;
        baseDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        baseDesc.BufferCount = 2;
        baseDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        return true;
    }

    // Local video memory in use, in MiB. A context that a refused destroy left behind shows up as a growth across a
    // run whose chain was freed.
    int64_t LocalVideoMemoryMb() const
    {
        if (adapter3 == nullptr)
            return 0;
        DXGI_QUERY_VIDEO_MEMORY_INFO info {};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;
        return static_cast<int64_t>(info.CurrentUsage / (1024ull * 1024ull));
    }

    // Can a plain flip chain be created on the window? True means nothing is bound to it (yet).
    bool WindowFree() const
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = size;
        desc.Height = size;
        desc.Format = baseDesc.BufferDesc.Format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Scaling = DXGI_SCALING_STRETCH;

        IDXGISwapChain1* chain = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &chain)))
            return false;
        chain->Release();
        return true;
    }

    struct WindowRead
    {
        bool free = false;
        bool stable = false; // the same answer held for kQuietMs without a change
        int heldMs = 0;
    };

    // A single read is what makes an asynchronous teardown look like M2; so read repeatedly, spaced, and only accept an
    // answer that held for kQuietMs.
    WindowRead PollWindow() const
    {
        WindowRead read;
        bool previous = WindowFree();
        int held = 0;
        int elapsed = 0;
        do
        {
            Sleep(kReadStepMs);
            elapsed += kReadStepMs;
            held += kReadStepMs;
            const bool now = WindowFree();
            if (now != previous)
            {
                previous = now;
                held = 0;
            }
        } while (held < kQuietMs && elapsed < kReadBudgetMs);

        read.free = previous;
        read.heldMs = held;
        read.stable = held >= kQuietMs;
        return read;
    }
};

// ---- the XeLL context the product always attaches to its XeFG context ---------------------------------------------

// xellD3D12CreateContext, xellSetSleepMode and xellDestroyContext from libxell.dll, in the same low-latency mode the
// product sets: this is the configuration a XeFG row has to be measured in.
class XeLl
{
  public:
    Module module;
    xell_context_handle_t context = nullptr;

    bool Load(const std::wstring& folder, char (&error)[256])
    {
        if (!module.Load(folder, L"libxell.dll", error))
            return false;
        _create = module.Get<decltype(&xellD3D12CreateContext)>("xellD3D12CreateContext");
        _setSleepMode = module.Get<decltype(&xellSetSleepMode)>("xellSetSleepMode");
        _destroy = module.Get<decltype(&xellDestroyContext)>("xellDestroyContext");
        if (_create == nullptr || _setSleepMode == nullptr || _destroy == nullptr)
        {
            std::snprintf(error, sizeof(error), "libxell.dll has no XeLL entry points");
            return false;
        }
        return true;
    }

    bool Create(ID3D12Device* device)
    {
        if (_create == nullptr || _create(device, &context) != XELL_RESULT_SUCCESS || context == nullptr)
            return false;
        xell_sleep_params_t params {};
        params.minimumIntervalUs = 0;
        params.bLowLatencyMode = 1;
        params.bLowLatencyBoost = 0;
        if (_setSleepMode(context, &params) != XELL_RESULT_SUCCESS)
        {
            Destroy();
            return false;
        }
        return true;
    }

    void Destroy()
    {
        if (context != nullptr && _destroy != nullptr)
            _destroy(context);
        context = nullptr;
    }

  private:
    decltype(&xellD3D12CreateContext) _create = nullptr;
    decltype(&xellSetSleepMode) _setSleepMode = nullptr;
    decltype(&xellDestroyContext) _destroy = nullptr;
};

// ---- one class: three runs of the same measurement ---------------------------------------------------------------

struct Fsr
{
    Module module;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;

    static const char* Config() { return "direct"; }
    // The teardown order this class is measured in: the library's destroy first, then the caller's reference.
    static bool PinFirst() { return false; }

    bool Load(const std::wstring& folder, char (&error)[256])
    {
        if (!module.Load(folder, L"amd_fidelityfx_framegeneration_dx12.dll", error))
            return false;
        create = module.Get<PfnFfxCreateContext>("ffxCreateContext");
        destroy = module.Get<PfnFfxDestroyContext>("ffxDestroyContext");
        if (create == nullptr || destroy == nullptr)
        {
            std::snprintf(error, sizeof(error), "amd_fidelityfx_framegeneration_dx12.dll has no ffxCreateContext/ffxDestroyContext");
            return false;
        }
        return true;
    }

    struct Chain
    {
        ffxContext context = nullptr;
        IDXGISwapChain* chain = nullptr;
        int createMs = 0;
        int32_t destroyCode = 0;
    };

    // The FSR FG leg needs no extra context: the library is called directly, which is what the row's config says.
    bool Prepare(Device&) { return true; }
    void Finish() {}

    // FSR FG has no companion DLL and no context that survives its own destroy.
    bool IdentifyCompanion(LibraryIdentity&) const { return false; }
    bool HasSecondDestroy() const { return false; }
    uint32_t DestroyAgain(Chain&) const { return 0; }

    // `queue` is the game queue the chain is created with: the host's own queue unless a replay of the switch says
    // otherwise.
    Chain CreateChain(Device& device, ID3D12CommandQueue* queue = nullptr) const
    {
        Chain out;
        ffxCreateContextDescFrameGenerationSwapChainNewDX12 desc {};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12;
        desc.dxgiFactory = device.LibraryFactory();
        desc.gameQueue = queue != nullptr ? queue : device.queue;
        desc.desc = &device.baseDesc;
        IDXGISwapChain4* swapchain = nullptr;
        desc.swapchain = &swapchain;

        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version {};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        desc.header.pNext = &version.header;

        const ULONGLONG start = GetTickCount64();
        const ffxReturnCode_t code = create(&out.context, &desc.header, nullptr);
        out.createMs = static_cast<int>(GetTickCount64() - start);
        if (code != FFX_API_RETURN_OK || swapchain == nullptr)
        {
            Emitf("fg-probe class=fsrfg create failed code=0x%08X\n", static_cast<unsigned>(code));
            out.context = nullptr;
            if (swapchain != nullptr)
                swapchain->Release();
            return out;
        }
        out.chain = swapchain;
        return out;
    }

    void DestroyChain(Chain& chain) const
    {
        if (chain.context != nullptr)
        {
            const ffxReturnCode_t code = destroy(&chain.context, nullptr);
            Emitf("fg-probe class=fsrfg destroy code=0x%08X\n", static_cast<unsigned>(code));
            chain.context = nullptr;
        }
    }
};

struct XeFg
{
    Module module;
    XeLl xell;
    xefg_swapchain_result_t (*create)(ID3D12Device*, xefg_swapchain_handle_t*) = nullptr;
    xefg_swapchain_result_t (*init)(xefg_swapchain_handle_t, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*,
                                    const xefg_swapchain_d3d12_init_params_t*) = nullptr;
    xefg_swapchain_result_t (*chainPtr)(xefg_swapchain_handle_t, REFIID, void**) = nullptr;
    xefg_swapchain_result_t (*destroy)(xefg_swapchain_handle_t) = nullptr;
    xefg_swapchain_result_t (*setLatencyReduction)(xefg_swapchain_handle_t, void*) = nullptr;

    static const char* Config() { return "xell"; }
    // The teardown order this class is measured in, and the one the product uses for it: the caller's reference is
    // released first, then the library's destroy runs exactly once. An XeFG context refuses to be destroyed while the
    // caller still holds the chain, so the other order leaves it allocated with nobody to own it.
    static bool PinFirst() { return true; }

    bool Load(const std::wstring& folder, char (&error)[256])
    {
        if (!module.Load(folder, L"libxess_fg.dll", error))
            return false;
        create = module.Get<decltype(create)>("xefgSwapChainD3D12CreateContext");
        init = module.Get<decltype(init)>("xefgSwapChainD3D12InitFromSwapChainDesc");
        chainPtr = module.Get<decltype(chainPtr)>("xefgSwapChainD3D12GetSwapChainPtr");
        destroy = module.Get<decltype(destroy)>("xefgSwapChainDestroy");
        setLatencyReduction = module.Get<decltype(setLatencyReduction)>("xefgSwapChainSetLatencyReduction");
        if (create == nullptr || init == nullptr || chainPtr == nullptr || destroy == nullptr ||
            setLatencyReduction == nullptr)
        {
            std::snprintf(error, sizeof(error), "libxess_fg.dll has no XeFG swapchain entry points");
            return false;
        }
        if (!xell.Load(folder, error))
            return false; // without XeLL the measurement is not the product's configuration
        return true;
    }

    // The XeLL context lives across the three runs, as it does across the product's XeFG context, and is released after
    // the last one (the product releases it after its own XeFG context is destroyed).
    bool Prepare(Device& device) { return xell.Create(device.device); }
    void Finish() { xell.Destroy(); }

    struct Chain
    {
        xefg_swapchain_handle_t handle = nullptr;
        IDXGISwapChain* chain = nullptr;
        int createMs = 0;
        int32_t destroyCode = 0;
        bool destroyCalled = false;
    };

    // libxell.dll is the companion the row's config=xell names: its build is part of the measured pair.
    bool IdentifyCompanion(LibraryIdentity& identity) const { return xell.module.Identify(identity); }
    bool HasSecondDestroy() const { return true; }
    // The second destroy, after the pin release. XeFG answers POINTER_STILL_IN_USE while the bridge still holds the
    // chain, so the first destroy can leave the context alive; with the chain gone, the same call is the only way to
    // find out whether the context was leaked or just deferred. A success means destroyed; anything else is recorded as
    // leaked, and the handle is dropped either way (it is not a handle the caller may try a third time).
    uint32_t DestroyAgain(Chain& chain) const
    {
        if (chain.handle == nullptr || !chain.destroyCalled)
            return 0;
        const xefg_swapchain_result_t code = destroy(chain.handle);
        chain.handle = nullptr;
        return static_cast<uint32_t>(code);
    }

    Chain CreateChain(Device& device, ID3D12CommandQueue* queue = nullptr) const
    {
        Chain out;
        if (create(device.device, &out.handle) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            Emit("fg-probe class=xefg create-context failed\n");
            out.handle = nullptr;
            return out;
        }

        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = device.size;
        desc.Height = device.size;
        desc.Format = device.baseDesc.BufferDesc.Format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Scaling = DXGI_SCALING_STRETCH;

        xefg_swapchain_d3d12_init_params_t params {};
        params.maxInterpolatedFrames = 1;
        params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
        params.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;

        const ULONGLONG start = GetTickCount64();

        // Exactly what the product does, in the product's order: create the context, hand it the XeLL context, and only
        // then initialize it (XeFG_Dx12.cpp sets the latency reduction before D3D12InitFromSwapChainDesc). The order is
        // not cosmetic: without a XeLL context the initialize call answers
        // ERROR_LATENCY_REDUCTION_UNSUPPORTED, which is what a first run of this host measured.
        const xefg_swapchain_result_t latency = setLatencyReduction(out.handle, xell.context);
        if (latency != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            Emitf("fg-probe class=xefg set-latency-reduction failed code=%u\n", static_cast<unsigned>(latency));
            destroy(out.handle);
            out.handle = nullptr;
            return out;
        }
        const xefg_swapchain_result_t code =
            init(out.handle, device.window, &desc, nullptr, queue != nullptr ? queue : device.queue, device.LibraryFactory(),
                 &params);
        out.createMs = static_cast<int>(GetTickCount64() - start);
        if (code != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            Emitf("fg-probe class=xefg init failed code=%u\n", static_cast<unsigned>(code));
            destroy(out.handle);
            out.handle = nullptr;
            return out;
        }
        if (chainPtr(out.handle, IID_PPV_ARGS(&out.chain)) != XEFG_SWAPCHAIN_RESULT_SUCCESS || out.chain == nullptr)
        {
            Emit("fg-probe class=xefg swapchain-ptr failed\n");
            destroy(out.handle);
            out.handle = nullptr;
            return out;
        }
        return out;
    }

    void DestroyChain(Chain& chain) const
    {
        if (chain.handle != nullptr)
        {
            const xefg_swapchain_result_t code = destroy(chain.handle);
            chain.destroyCode = static_cast<int32_t>(code);
            chain.destroyCalled = true;
            Emitf("fg-probe class=xefg destroy code=%u\n", static_cast<unsigned>(code));
            // The handle stays: a second destroy after the pin release is what tells a deferred context from a leaked
            // one. It is dropped once that call has been made, or when the object was destroyed.
        }
    }
};

// ---- the measurement of one class --------------------------------------------------------------------------------

template <class Library>
bool MeasureClass(Device& device, Library& library, Class cls, int runs, std::vector<ProbeRun>& out,
                  bool& witnessAttempted, bool& witnessAccepted, bool& witnessReleased)
{
    witnessAttempted = false;
    witnessAccepted = true;
    witnessReleased = true;
    out.clear();

    for (int run = 0; run < runs; ++run)
    {
        const int64_t vramBefore = device.LocalVideoMemoryMb();

        typename Library::Chain chain = library.CreateChain(device);
        if (chain.chain == nullptr)
            return false;
        const int createMs = chain.createMs;

        // The standstill count the row carries: the references the chain has as created (the creation reference plus
        // whatever the provider's context holds). AddRef/Release around one read, nothing is destroyed.
        const ULONG before = chain.chain->AddRef();
        const ULONG standstill = chain.chain->Release();

        // The witness goes on before the destroy and is released with the object that holds it. The state is allocated
        // on purpose and never freed: if the library does not drop the interface, freeing it here would hide exactly the
        // leak this measurement exists to find.
        auto* state = new WitnessState();
        auto* witness = new Witness(*state);
        const bool accepted = SUCCEEDED(chain.chain->SetPrivateDataInterface(kWitnessGuid, witness));
        witness->Release(); // the object holds its own reference
        witnessAttempted = true;
        witnessAccepted = witnessAccepted && accepted;

        ProbeRun measured;
        measured.standstillRefs = static_cast<uint32_t>(standstill);
        measured.pinFirst = Library::PinFirst();

        if (Library::PinFirst())
        {
            // The order this class is measured with, and the one the product uses for it: the caller's reference goes
            // first, then exactly one destroy. A leaked context (a destroy that refuses while somebody else holds the
            // chain) can only be seen this way, and a second destroy is never attempted here: on a chain the first call
            // already freed, it is an access violation.
            chain.chain->Release();
            chain.chain = nullptr;
            const bool aliveAfterPin = accepted && state->alive != 0 && state->released == 0;
            library.DestroyChain(chain);
            const Device::WindowRead freed = device.PollWindow();
            measured.windowFreeAfterDestroy = freed.free;
            measured.windowStableAfterDestroy = freed.stable;
            measured.windowFreeAfterPin = freed.free;
            measured.windowStableAfterPin = freed.stable;
            measured.witnessAliveBeforePin = aliveAfterPin;
            measured.witnessAliveAfterDestroy = accepted && state->released == 0;
            measured.firstDestroyCode = chain.destroyCode;
            int64_t vramAfter = 0;
            if (vramBefore != 0)
                vramAfter = device.LocalVideoMemoryMb() - vramBefore;
            measured.vramDeltaMb = vramAfter;
            out.push_back(measured);
            witnessReleased = witnessReleased && (state->released != 0);
            Emitf("fg-probe class=%s run=%d order=pinfirst createMs=%d afterDestroy=%s stable=%d heldMs=%d "
                  "witnessAtPin=%s witnessAfterDestroy=%s destroyCode=0x%08X refs=%u vramDeltaMb=%lld\n",
                  ClassText(cls), run + 1, createMs, freed.free ? "free" : "bound", freed.stable ? 1 : 0, freed.heldMs,
                  accepted ? (aliveAfterPin ? "alive" : "died-with-object") : "refused",
                  measured.witnessAliveAfterDestroy ? "alive" : "died-with-object",
                  static_cast<unsigned>(chain.destroyCode), standstill, static_cast<long long>(vramAfter));
            continue;
        }

        library.DestroyChain(chain);
        const Device::WindowRead afterDestroy = device.PollWindow();

        // Only when the window is still bound AND the witness agrees the object is still there may the pin be released.
        // The witness is read HERE, at the end of the quiet window and before the release: under M2 the release is what
        // frees the object, so a sample taken after it always reads "gone".
        const bool witnessAliveAtPin = accepted && state->alive != 0 && state->released == 0;
        bool pinReleased = false;
        bool releaseSkipped = false;
        Device::WindowRead afterPin {};
        if (!afterDestroy.free && afterDestroy.stable)
        {
            if (accepted && !witnessAliveAtPin)
            {
                releaseSkipped = true;
            }
            else
            {
                pinReleased = true;
                chain.chain->Release();
                chain.chain = nullptr;
                afterPin = device.PollWindow();
            }
        }
        else if (!afterDestroy.free)
        {
            // The reads never settled: nothing may be touched, and the run cannot be used.
            afterPin = afterDestroy;
        }

        // 3. The second destroy, after the pin release, only when the first one refused (a successful first destroy
        // already ended the context, and a second call would be a double destroy). It is off by default: on XeFG this
        // call stops the process once the chain's release has freed the object the context still points at, which is
        // itself the answer (the context cannot be destroyed after the chain's release), so it is a deliberate
        // experiment and never part of a row.
        uint32_t destroyAfterPin = 0;
        const bool destroyAfterPinAttempted =
            g_secondDestroy && pinReleased && library.HasSecondDestroy() && chain.destroyCode != 0;
        if (destroyAfterPinAttempted)
        {
            destroyAfterPin = library.DestroyAgain(chain);
            Emitf("fg-probe class=%s run=%d destroyAfterPin code=0x%08X (%s)\n", ClassText(cls), run + 1,
                  static_cast<unsigned>(destroyAfterPin), destroyAfterPin == 0 ? "destroyed" : "refused");
        }

        // 4. The other order on a fresh chain: release the pin first, then destroy. Measured only when this run said M2,
        // because that is the only case in which releasing an unreleased reference is safe by definition.
        bool orderBMeasured = false;
        bool orderBFree = false;
        if (pinReleased)
        {
            typename Library::Chain againB = library.CreateChain(device);
            if (againB.chain != nullptr)
            {
                againB.chain->Release();
                againB.chain = nullptr;
                library.DestroyChain(againB); // the first destroy, with no creation reference left
                if (library.HasSecondDestroy() && againB.destroyCode != 0)
                    library.DestroyAgain(againB); // a refused destroy left the context: do not leak it across the runs
                const Device::WindowRead freed = device.PollWindow();
                orderBMeasured = true;
                orderBFree = freed.free && freed.stable;
                Emitf("fg-probe class=%s run=%d orderB windowAfterDestroy=%s stable=%d heldMs=%d\n", ClassText(cls),
                      run + 1, freed.free ? "free" : "bound", freed.stable ? 1 : 0, freed.heldMs);
            }
        }

        // 5. Local video memory across the run: a context that a refused destroy leaked shows up here and nowhere else.
        int64_t vramDelta = 0;
        if (vramBefore != 0)
        {
            const int64_t vramAfter = device.LocalVideoMemoryMb();
            vramDelta = vramAfter - vramBefore;
        }

        measured.windowFreeAfterDestroy = afterDestroy.free;
        measured.windowStableAfterDestroy = afterDestroy.stable;
        measured.windowFreeAfterPin = afterPin.free;
        measured.windowStableAfterPin = afterPin.stable;
        measured.witnessAliveBeforePin = witnessAliveAtPin;
        measured.releaseSkipped = releaseSkipped;
        measured.destroyAfterPin = destroyAfterPin;
        measured.destroyAfterPinAttempted = destroyAfterPinAttempted;
        measured.orderBMeasured = orderBMeasured;
        measured.orderBFree = orderBFree;
        measured.vramDeltaMb = vramDelta;
        measured.standstillRefs = static_cast<uint32_t>(standstill);
        measured.firstDestroyCode = chain.destroyCode;
        out.push_back(measured);

        // Run A, the page-heap corroboration: only when run B said M2, so each release here is safe by definition.
        // A clean return corroborates M2; under a page heap an M1 library would stop at the kept release instead. The
        // final line carries a verdict token, not only numbers.
        if (pinReleased)
        {
            typename Library::Chain again = library.CreateChain(device);
            if (again.chain != nullptr)
            {
                again.chain->AddRef(); // the kept reference
                library.DestroyChain(again);
                Emitf("fg-probe class=%s run=%d page-heap-step=release-the-kept-reference\n", ClassText(cls), run + 1);
                again.chain->Release(); // the kept reference: a stop after the line above means M1
                again.chain->Release(); // the bridge's reference, which frees the object under M2
                again.chain = nullptr;
                const Device::WindowRead freed = device.PollWindow();
                Emitf("fg-probe class=%s run=%d runA windowFree=%s stable=%d heldMs=%d verdict=%s\n", ClassText(cls),
                      run + 1, freed.free ? "free" : "bound", freed.stable ? 1 : 0, freed.heldMs,
                      freed.free && freed.stable ? "corroborates-M2" : "not-M2");
            }
        }
        witnessReleased = witnessReleased && (state->released != 0);

        Emitf("fg-probe class=%s run=%d createMs=%d windowAfterDestroy=%s stable=%d heldMs=%d windowAfterPin=%s "
              "stable=%d witnessAtPin=%s witnessAfterPin=%s pinRelease=%d skipped=%d refs=%u destroyAfterPin=%s "
              "orderB=%s vramDeltaMb=%lld heldBudget=%d\n",
              ClassText(cls), run + 1, createMs, afterDestroy.free ? "free" : "bound", afterDestroy.stable ? 1 : 0,
              afterDestroy.heldMs, afterPin.free ? "free" : "bound", afterPin.stable ? 1 : 0,
              accepted ? (witnessAliveAtPin ? "alive" : "died-with-object") : "refused",
              state->released != 0 ? "died-with-object" : "alive", pinReleased ? 1 : 0, releaseSkipped ? 1 : 0,
              standstill,
              !destroyAfterPinAttempted ? "not-measured" : (destroyAfterPin == 0 ? "destroyed" : "refused"),
              !orderBMeasured ? "not-measured" : (orderBFree ? "clean" : "leaked"), static_cast<long long>(vramDelta),
              afterDestroy.heldMs >= kQuietMs ? 1 : 0);
    }
    return true;
}

// ---- presenting a chain: the cycle and the replay of the switch's queue rules ------------------------------------

// The rows above never present a chain, and the live switch does: the game presents through every chain it creates,
// and the crashes of the live switch happened only after a create or a teardown that was then presented through. The
// cycle answers whether a library survives create -> present -> destroy -> create -> present in one process (a crash
// here is the library's; clean here moves the defect into the switch's own sequence), and which queue the library put
// its inner chain on. The replay runs the switch's own queue rules (ext/fg/outswitch/QueueRules.h) against the real
// libraries, in the order a benchmark's six directions take.

constexpr int kWriterSlots = 3;
constexpr int kFrameSleepMs = 4; // about the frame time of a game near 200 fps

struct Frames
{
    int ok = 0;
    int occluded = 0;
    int failed = 0;
    uint32_t lastFailure = 0;
};

bool DeviceLost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

// The window belongs to this process: its messages are dispatched here so it never reads as hung. Nothing is sent to it.
void Pump()
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::string NameOf(const char* narrow, const wchar_t* wide)
{
    if (narrow != nullptr)
        return narrow;
    if (wide != nullptr)
        return Narrow(wide);
    return "unnamed";
}

// What DRED recorded when the device was removed: the command lists that did not finish, and the page fault.
void DumpDred(ID3D12Device* device)
{
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || dred == nullptr)
    {
        Emit("fg-probe dred unavailable\n");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
    {
        int shown = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node != nullptr && shown < 16;
             node = node->pNext)
        {
            const UINT done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount)
                continue; // a list that finished is not the one the GPU stopped in
            ++shown;
            Emitf("fg-probe dred list=%s queue=%s done=%u of=%u next=%d\n",
                  NameOf(node->pCommandListDebugNameA, node->pCommandListDebugNameW).c_str(),
                  NameOf(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW).c_str(), done,
                  node->BreadcrumbCount,
                  node->pCommandHistory != nullptr ? static_cast<int>(node->pCommandHistory[done]) : -1);
        }
        if (shown == 0)
            Emit("fg-probe dred breadcrumbs: every recorded list finished\n");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)))
    {
        Emitf("fg-probe dred pagefault va=0x%llx\n", static_cast<unsigned long long>(fault.PageFaultVA));
        int count = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* node = fault.pHeadExistingAllocationNode; node != nullptr && count < 8;
             node = node->pNext, ++count)
            Emitf("fg-probe dred existing name=%s type=%d\n", NameOf(node->ObjectNameA, node->ObjectNameW).c_str(),
                  static_cast<int>(node->AllocationType));
        count = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* node = fault.pHeadRecentFreedAllocationNode; node != nullptr && count < 8;
             node = node->pNext, ++count)
            Emitf("fg-probe dred freed name=%s type=%d\n", NameOf(node->ObjectNameA, node->ObjectNameW).c_str(),
                  static_cast<int>(node->AllocationType));
    }
    dred->Release();
}

// The debug layer's messages since the last call: errors and corruption are printed (the first few), warnings counted.
// Vendor chatter is information; a device removal, an access violation or a hang is the verdict.
void DrainDebug(Device& device, const char* phase)
{
    if (!device.debugLayer)
        return;
    ID3D12InfoQueue* queue = nullptr;
    if (FAILED(device.device->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
        return;
    const UINT64 count = queue->GetNumStoredMessages();
    int errors = 0;
    int warnings = 0;
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)) || size == 0)
            continue;
        std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &size)))
            continue;
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            if (++errors <= 4)
                Emitf("fg-probe debug phase=%s severity=%d id=%d text=%.300s\n", phase, static_cast<int>(message->Severity),
                      static_cast<int>(message->ID), message->pDescription != nullptr ? message->pDescription : "");
        }
        else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
        {
            ++warnings;
        }
    }
    queue->ClearStoredMessages();
    queue->Release();
    if (errors != 0 || warnings != 0)
        Emitf("fg-probe debug phase=%s errors=%d warnings=%d\n", phase, errors, warnings);
}

// Writes a back buffer and presents it, frame after frame: a clear the GPU has to finish before the chain may show the
// buffer, recorded on the queue the bridge records its interop copy on.
class Writer
{
  public:
    ~Writer()
    {
        for (auto*& allocator : allocators_)
            if (allocator != nullptr)
                allocator->Release();
        if (list_ != nullptr)
            list_->Release();
        if (fence_ != nullptr)
            fence_->Release();
        if (heap_ != nullptr)
            heap_->Release();
        if (event_ != nullptr)
            CloseHandle(event_);
    }

    bool Init(ID3D12Device* device)
    {
        device_ = device;
        for (auto*& allocator : allocators_)
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
                return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0], nullptr,
                                             IID_PPV_ARGS(&list_))))
            return false;
        list_->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = kWriterSlots;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_))))
            return false;
        rtvSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event_ != nullptr;
    }

    // Every write this writer submitted, and everything before it on `queue`, has retired. False when the wait expired.
    bool Idle(ID3D12CommandQueue* queue)
    {
        const uint64_t value = ++value_;
        if (FAILED(queue->Signal(fence_, value)))
            return false;
        return WaitFor(value);
    }

    // `frames` frames on `chain`: the clear goes to `writeQueue`; a chain that presents on another queue waits for it on
    // `presentQueue` first, which is the bridge's own order (the interop copy, then the present queue waits on its fence).
    // `sideQueue` gets one signal per frame: the interop's own submissions (the upscaler's evaluate thread in the game).
    Frames Present(IDXGISwapChain* chain, ID3D12CommandQueue* writeQueue, ID3D12CommandQueue* presentQueue, int frames,
                   int seed, ID3D12CommandQueue* sideQueue = nullptr)
    {
        Frames out;
        IDXGISwapChain3* chain3 = nullptr;
        if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) || chain3 == nullptr)
        {
            out.failed = 1;
            out.lastFailure = static_cast<uint32_t>(E_NOINTERFACE);
            return out;
        }
        for (int i = 0; i < frames; ++i)
        {
            Pump();
            const int slot = i % kWriterSlots;
            if (!WaitFor(slotValues_[slot]))
            {
                ++out.failed;
                out.lastFailure = WAIT_TIMEOUT;
                break;
            }
            ID3D12Resource* buffer = nullptr;
            HRESULT hr = chain->GetBuffer(chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer));
            if (FAILED(hr) || buffer == nullptr)
            {
                ++out.failed;
                out.lastFailure = static_cast<uint32_t>(hr);
                break;
            }
            allocators_[slot]->Reset();
            list_->Reset(allocators_[slot], nullptr);
            Transition(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(slot) * rtvSize_;
            device_->CreateRenderTargetView(buffer, nullptr, rtv);
            // Dim shades: the window is on the desktop and must not flash.
            const float shade = 0.04f + 0.08f * static_cast<float>((i + seed * 3) % 8) / 8.0f;
            const float colour[4] = { shade, shade, shade * 1.25f, 1.0f };
            list_->ClearRenderTargetView(rtv, colour, 0, nullptr);
            Transition(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            list_->Close();
            ID3D12CommandList* lists[] = { list_ };
            writeQueue->ExecuteCommandLists(1, lists);
            const uint64_t value = ++value_;
            writeQueue->Signal(fence_, value);
            slotValues_[slot] = value;
            if (presentQueue != nullptr && presentQueue != writeQueue)
                presentQueue->Wait(fence_, value);
            buffer->Release();
            if (sideQueue != nullptr && sideQueue != writeQueue)
                sideQueue->Wait(fence_, value);

            hr = chain->Present(0, 0);
            if (hr == S_OK)
                ++out.ok;
            else if (hr == DXGI_STATUS_OCCLUDED)
                ++out.occluded;
            else
            {
                ++out.failed;
                out.lastFailure = static_cast<uint32_t>(hr);
                if (DeviceLost(hr))
                    break;
            }
            Sleep(kFrameSleepMs);
        }
        chain3->Release();
        return out;
    }

  private:
    bool WaitFor(uint64_t value)
    {
        if (value == 0 || fence_->GetCompletedValue() >= value)
            return true;
        if (FAILED(fence_->SetEventOnCompletion(value, event_)))
            return false;
        return WaitForSingleObject(event_, 5000) == WAIT_OBJECT_0;
    }

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list_->ResourceBarrier(1, &barrier);
    }

    ID3D12Device* device_ = nullptr;
    ID3D12CommandAllocator* allocators_[kWriterSlots] {};
    uint64_t slotValues_[kWriterSlots] {};
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    uint64_t value_ = 0;
    HANDLE event_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    UINT rtvSize_ = 0;
};

// The queue the library created its real chain on, as the factory recorded it, with one reference that is the caller's.
// Null when the factory saw no D3D12 queue (the identity is then unknown, never guessed).
ID3D12CommandQueue* LibraryQueue(Device& device)
{
    return device.recorder != nullptr ? device.recorder->TakeQueue() : nullptr;
}

ULONG CountOf(IUnknown* object)
{
    if (object == nullptr)
        return 0;
    object->AddRef();
    return object->Release();
}

// A class's own teardown order, which is the product's and the measured row's: XeFG releases the caller's reference
// first and destroys once; FSR FG destroys first and releases after.
template <class Library> void TearDownChain(Library& library, typename Library::Chain& chain)
{
    if (Library::PinFirst())
    {
        chain.chain->Release();
        chain.chain = nullptr;
        library.DestroyChain(chain);
    }
    else
    {
        library.DestroyChain(chain);
        chain.chain->Release();
        chain.chain = nullptr;
    }
}

struct CycleTally
{
    int legs = 0;
    int clean = 0;
    int ownQueue = 0;             // legs whose chain presents on a queue that is not the host's
    int unknownQueue = 0;         // legs where the factory recorded no queue
    int queueDiedWithLibrary = 0; // legs whose library queue was held only by this host after the destroy
    bool lost = false;
};

// create -> present -> destroy, twice per run, on the host's own game queue.
template <class Library>
void CycleClass(Device& device, Library& library, Class cls, int runs, int frames, Writer& writer, CycleTally& tally)
{
    for (int run = 0; run < runs && !tally.lost; ++run)
    {
        for (int leg = 0; leg < 2 && !tally.lost; ++leg)
        {
            ++tally.legs;
            typename Library::Chain chain = library.CreateChain(device);
            if (chain.chain == nullptr)
            {
                Emitf("fg-probe cycle class=%s run=%d leg=%d create=failed\n", ClassText(cls), run + 1, leg + 1);
                continue;
            }
            const int createMs = chain.createMs;
            ID3D12CommandQueue* own = LibraryQueue(device); // one reference of ours, held across the destroy
            const bool separate = own != nullptr && own != device.queue;
            if (separate)
                ++tally.ownQueue;
            if (own == nullptr)
                ++tally.unknownQueue;
            const ULONG refsLive = own != nullptr ? CountOf(own) - 1 : 0;

            const Frames f = writer.Present(chain.chain, device.queue, device.queue, frames, run * 2 + leg);
            const bool idle = writer.Idle(device.queue);
            const HRESULT afterPresent = device.device->GetDeviceRemovedReason();
            DrainDebug(device, "present");

            TearDownChain(library, chain);
            ULONG refsLeft = 0;
            if (own != nullptr)
            {
                // The queue is alive because this reference holds it; it is made idle before the reference goes.
                if (separate)
                    writer.Idle(own);
                refsLeft = own->Release();
            }
            if (separate && refsLeft == 0)
                ++tally.queueDiedWithLibrary;
            const HRESULT afterTeardown = device.device->GetDeviceRemovedReason();
            DrainDebug(device, "teardown");

            const bool clean = f.failed == 0 && f.ok + f.occluded == frames && idle && afterPresent == S_OK &&
                               afterTeardown == S_OK;
            if (clean)
                ++tally.clean;
            Emitf("fg-probe cycle class=%s run=%d leg=%d createMs=%d presents=%d ok=%d occluded=%d failed=%d "
                  "lastHr=0x%08X idle=%d libQueue=%s refsLive=%lu refsAfterDestroy=%lu destroyCode=0x%08X "
                  "removed=0x%08X result=%s\n",
                  ClassText(cls), run + 1, leg + 1, createMs, frames, f.ok, f.occluded, f.failed, f.lastFailure,
                  idle ? 1 : 0, own == nullptr ? "unknown" : (separate ? "own" : "host"), refsLive, refsLeft,
                  static_cast<unsigned>(chain.destroyCode),
                  static_cast<unsigned>(afterTeardown != S_OK ? afterTeardown : afterPresent), clean ? "clean" : "FAILED");
            if (afterPresent != S_OK || afterTeardown != S_OK)
            {
                tally.lost = true;
                DumpDred(device.device);
            }
        }
    }
}

// ---- the replay: the switch's queue rules, against the real libraries ----------------------------------------------

// What the product's process state looks like around the switch, as far as queues go (QueueRules.h has the roles).
struct QueueWorld
{
    ID3D12CommandQueue* interop = nullptr; // the D3D11 interop's queue
    ID3D12CommandQueue* state = nullptr;   // the global command queue, which follows the interop
    ID3D12CommandQueue* bridge = nullptr;  // the bridge's copy queue; the bridge holds no reference on it
    ID3D12CommandQueue* pinned = nullptr;  // the switch's own reference, when the rules take one
};

enum class Kind
{
    Plain,
    Fsr,
    Xe
};

const char* KindText(Kind kind) { return kind == Kind::Fsr ? "fsrfg" : kind == Kind::Xe ? "xefg" : "none"; }

struct LiveReplay
{
    Kind kind = Kind::Plain;
    Fsr::Chain fsr;
    XeFg::Chain xe;
    IDXGISwapChain* plain = nullptr;
    ID3D12CommandQueue* presentQueue = nullptr; // the queue the chain was created with (borrowed)

    IDXGISwapChain* Chain() const { return kind == Kind::Fsr ? fsr.chain : kind == Kind::Xe ? xe.chain : plain; }
};

IDXGISwapChain* CreatePlain(Device& device, ID3D12CommandQueue* queue)
{
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = device.size;
    desc.Height = device.size;
    desc.Format = device.baseDesc.BufferDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Scaling = DXGI_SCALING_STRETCH;
    IDXGISwapChain1* chain = nullptr;
    const HRESULT hr = device.factory->CreateSwapChainForHwnd(queue, device.window, &desc, nullptr, nullptr, &chain);
    if (FAILED(hr))
    {
        Emitf("fg-probe replay plain create failed hr=0x%08X\n", static_cast<unsigned>(hr));
        return nullptr;
    }
    return chain;
}

// One create of `kind` on `creation`, then the library's factory path as the product sees it: the interop and the global
// queue are pointed at the queue the library put its inner chain on, and, for a create the switch makes, the switch's
// restore rule runs. The boot create is upstream's own and restores nothing.
bool CreateReplay(Device& device, Fsr& fsr, XeFg& xe, Kind kind, ID3D12CommandQueue* creation, QueueWorld& q,
                  LiveReplay& live, bool bySwitch)
{
    live = LiveReplay {};
    live.kind = kind;
    live.presentQueue = creation;
    if (kind == Kind::Plain)
    {
        live.plain = CreatePlain(device, creation);
        return live.plain != nullptr;
    }
    if (kind == Kind::Fsr)
        live.fsr = fsr.CreateChain(device, creation);
    else
        live.xe = xe.CreateChain(device, creation);
    if (live.Chain() == nullptr)
        return false;

    ID3D12CommandQueue* own = LibraryQueue(device);
    ID3D12CommandQueue* clobbered = own != nullptr ? own : creation;
    if (own != nullptr)
        own->Release(); // only the identity is needed: the library holds its queue, the host does not
    ID3D12CommandQueue* before = q.interop;
    q.interop = bySwitch ? static_cast<ID3D12CommandQueue*>(ext::fg::outswitch::queues::InteropAfterCreate(before, clobbered))
                         : clobbered;
    q.state = q.interop;
    Emitf("fg-probe replay create to=%s creation=%p libQueue=%p interop=%s\n", KindText(kind),
          static_cast<void*>(creation), static_cast<void*>(clobbered),
          q.interop == before ? "kept" : "moved-to-library");
    return true;
}

void TearDownReplay(Fsr& fsr, XeFg& xe, LiveReplay& live)
{
    if (live.kind == Kind::Fsr)
        TearDownChain(fsr, live.fsr);
    else if (live.kind == Kind::Xe)
        TearDownChain(xe, live.xe);
    else if (live.plain != nullptr)
    {
        live.plain->Release();
        live.plain = nullptr;
    }
}

// Boot FSR FG, then the six directions a benchmark's two boots take: FSR->XeFG, XeFG->plain, plain->FSR, FSR->plain,
// plain->XeFG, XeFG->FSR. At every teardown the host holds one probe reference on each queue the switch goes on using
// afterwards (the creation queue of the next chain, the bridge's queue, the interop queue); a probe that turns out to be
// the last reference means the switch would have used a queue its owner just freed, and the replay stops there without
// using it.
bool Replay(Device& device, Fsr& fsr, XeFg& xe, int frames, Writer& writer)
{
    namespace qr = ext::fg::outswitch::queues;
    Emitf("fg-probe replay begin pinBridgeQueue=%d\n", qr::kPinBridgeQueue ? 1 : 0);
    const Kind order[] = { Kind::Fsr, Kind::Xe, Kind::Plain, Kind::Fsr, Kind::Plain, Kind::Xe, Kind::Fsr };

    // Boot: the interop owns the host's queue; the boot library's create moves the interop to the library's own queue;
    // the bridge is constructed afterwards and takes the interop's queue as its copy queue.
    QueueWorld q;
    q.interop = device.queue;
    q.state = device.queue;
    LiveReplay live;
    if (!CreateReplay(device, fsr, xe, order[0], device.queue, q, live, false))
        return false;
    q.bridge = q.interop;
    Emitf("fg-probe replay boot bridgeQueue=%s\n", q.bridge == device.queue ? "host" : "boot-library");
    Frames f = writer.Present(live.Chain(), q.bridge, live.presentQueue, frames, 0, q.interop);
    bool ok = f.failed == 0 && device.device->GetDeviceRemovedReason() == S_OK;

    for (size_t step = 1; step < sizeof(order) / sizeof(order[0]) && ok; ++step)
    {
        // The capture: the pin is taken before anything is torn down, and the creation queue is chosen.
        if (qr::kPinBridgeQueue && qr::RepinNeeded(q.pinned, q.bridge))
        {
            q.bridge->AddRef();
            if (q.pinned != nullptr)
                q.pinned->Release();
            q.pinned = q.bridge;
        }
        auto* creation = static_cast<ID3D12CommandQueue*>(qr::CreationQueue(q.pinned, q.state));

        // The drain before the teardown, then the probes.
        writer.Idle(q.bridge);
        if (live.presentQueue != q.bridge)
            writer.Idle(live.presentQueue);
        ID3D12CommandQueue* probes[3] = { creation, q.bridge, q.interop };
        const char* roles[3] = { "creation", "bridge", "interop" };
        for (int i = 0; i < 3; ++i)
        {
            bool seen = false;
            for (int j = 0; j < i; ++j)
                seen = seen || probes[j] == probes[i];
            if (seen)
                probes[i] = nullptr; // one probe per queue
            else if (probes[i] != nullptr)
                probes[i]->AddRef();
        }

        const Kind from = live.kind;
        TearDownReplay(fsr, xe, live);

        bool died = false;
        for (int i = 0; i < 3; ++i)
        {
            if (probes[i] == nullptr)
                continue;
            const ULONG left = probes[i]->Release();
            if (left == 0)
            {
                died = true;
                Emitf("fg-probe replay step=%zu from=%s to=%s FAILED the %s queue died with the %s teardown (only the "
                      "probe held it)\n",
                      step, KindText(from), KindText(order[step]), roles[i], KindText(from));
            }
        }
        if (died)
        {
            ok = false;
            break; // the queue is gone now: nothing may touch it, so the replay stops here
        }

        if (!CreateReplay(device, fsr, xe, order[step], creation, q, live, true))
        {
            ok = false;
            break;
        }
        f = writer.Present(live.Chain(), q.bridge, live.presentQueue, frames, static_cast<int>(step), q.interop);
        const HRESULT removed = device.device->GetDeviceRemovedReason();
        DrainDebug(device, "replay");
        Emitf("fg-probe replay step=%zu from=%s to=%s creation=%s ok=%d occluded=%d failed=%d lastHr=0x%08X "
              "removed=0x%08X\n",
              step, KindText(from), KindText(order[step]), creation == q.bridge ? "bridge" : "other", f.ok, f.occluded,
              f.failed, f.lastFailure, static_cast<unsigned>(removed));
        if (f.failed != 0 || removed != S_OK)
        {
            ok = false;
            if (removed != S_OK)
                DumpDred(device.device);
        }
    }

    if (live.Chain() != nullptr && device.device->GetDeviceRemovedReason() == S_OK)
    {
        writer.Idle(live.presentQueue);
        TearDownReplay(fsr, xe, live);
    }
    if (q.pinned != nullptr)
    {
        q.pinned->Release();
        q.pinned = nullptr;
    }
    Emitf("fg-probe replay verdict=%s\n", ok ? "pass" : "FAILED");
    return ok;
}

const char* QueueVerdict(const CycleTally& tally)
{
    if (tally.unknownQueue != 0)
        return "unknown";
    if (tally.ownQueue == tally.legs)
        return "own";
    return tally.ownQueue == 0 ? "host" : "mixed";
}

// The two presenting modes, with the writer's lifetime inside them: it is gone before the device is released.
int RunPresenting(Device& device, Fsr& fsr, XeFg& xe, const std::wstring& folder, bool cycle, bool wantFsr, bool wantXe,
                  int runs, int frames)
{
    Emitf("fg-probe %s begin runs=%d frames=%d window=%u shown=1\n", cycle ? "cycle" : "replay", runs, frames,
          device.size);
    Writer writer;
    if (!writer.Init(device.device))
    {
        Emit("fg-probe error: the writer's D3D12 objects could not be created\n");
        return 1;
    }

    char error[256] {};
    if (!cycle)
    {
        // The replay needs both classes: its order takes every direction between them and plain.
        if (!fsr.Load(folder, error) || !xe.Load(folder, error) || !xe.Prepare(device))
        {
            Emitf("fg-probe replay error: %s\n", error[0] != '\0' ? error : "the XeLL context could not be created");
            return 1;
        }
        const bool ok = Replay(device, fsr, xe, frames, writer);
        if (device.device->GetDeviceRemovedReason() == S_OK)
            writer.Idle(device.queue);
        xe.Finish();
        return ok ? 0 : 1;
    }

    bool allClean = true;
    if (wantFsr)
    {
        CycleTally tally;
        if (fsr.Load(folder, error) && fsr.Prepare(device))
        {
            CycleClass(device, fsr, Class::FsrFg, runs, frames, writer, tally);
            fsr.Finish();
        }
        else
            Emitf("fg-probe cycle class=fsrfg error: %s\n", error);
        const bool clean = tally.legs > 0 && tally.clean == tally.legs;
        allClean = allClean && clean;
        Emitf("fg-probe cycle verdict class=fsrfg legs=%d clean=%d libQueue=%s queueDiedWithLibrary=%d result=%s\n",
              tally.legs, tally.clean, QueueVerdict(tally), tally.queueDiedWithLibrary, clean ? "clean" : "FAILED");
    }
    if (wantXe && device.device->GetDeviceRemovedReason() == S_OK)
    {
        CycleTally tally;
        if (xe.Load(folder, error) && xe.Prepare(device))
        {
            CycleClass(device, xe, Class::XeFg, runs, frames, writer, tally);
            xe.Finish();
        }
        else
            Emitf("fg-probe cycle class=xefg error: %s\n", error);
        const bool clean = tally.legs > 0 && tally.clean == tally.legs;
        allClean = allClean && clean;
        Emitf("fg-probe cycle verdict class=xefg legs=%d clean=%d libQueue=%s queueDiedWithLibrary=%d result=%s\n",
              tally.legs, tally.clean, QueueVerdict(tally), tally.queueDiedWithLibrary, clean ? "clean" : "FAILED");
    }
    if (device.device->GetDeviceRemovedReason() == S_OK)
        writer.Idle(device.queue);
    return allClean ? 0 : 1;
}

void ReleaseDevice(Device& device)
{
    if (device.recorder != nullptr)
        device.recorder->Release();
    if (device.adapter3 != nullptr)
        device.adapter3->Release();
    if (device.queue != nullptr)
        device.queue->Release();
    if (device.device != nullptr)
        device.device->Release();
    if (device.factory != nullptr)
        device.factory->Release();
    if (device.window != nullptr)
        DestroyWindow(device.window);
    device = Device {};
}

// A library call that never returns must not keep the process (and the DLLs) alive: print what has been measured and
// stop. TerminateProcess is the only way out of a blocked library call.
DWORD WINAPI Watchdog(void*)
{
    for (;;)
    {
        Sleep(kWatchdogStepMs);
        const uint64_t last = g_lastOutputMs.load(std::memory_order_relaxed);
        if (last != 0 && GetTickCount64() - last < static_cast<uint64_t>(kWatchdogMs))
            continue;
        Emitf("fg-probe error: a library call did not return within %d s, stopping\n", kWatchdogMs / 1000);
        if (g_out != nullptr)
            std::fclose(g_out);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    // Unbuffered: a crash or a kill must not lose the runs that already happened.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string folderArg;
    std::string outPath;
    int runs = 3;
    bool fsr = true;
    bool xefg = true;
    bool cycle = false;
    bool replay = false;
    int frames = 180;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--folder" && i + 1 < argc)
            folderArg = argv[++i];
        else if (arg == "--out" && i + 1 < argc)
            outPath = argv[++i];
        else if (arg == "--runs" && i + 1 < argc)
            runs = std::atoi(argv[++i]);
        else if (arg == "--class" && i + 1 < argc)
        {
            const std::string cls = argv[++i];
            fsr = cls == "all" || cls == "fsrfg";
            xefg = cls == "all" || cls == "xefg";
        }
        else if (arg == "--second-destroy")
        {
            g_secondDestroy = true;
        }
        else if (arg == "--cycle")
        {
            cycle = true;
        }
        else if (arg == "--replay")
        {
            replay = true;
        }
        else if (arg == "--frames" && i + 1 < argc)
        {
            frames = std::atoi(argv[++i]);
        }
        else if (arg == "--debug")
        {
            g_debugLayer = true;
        }
        else
        {
            Emit("usage: fg-probe --folder <dir with the FG libraries> [--out <file>] [--runs 3] "
                 "[--class fsrfg|xefg|all] [--cycle | --replay] [--frames 180] [--debug]\n");
            return 2;
        }
    }
    if (folderArg.empty())
    {
        Emit("fg-probe: --folder is required (the directory holding the frame-generation libraries)\n");
        return 2;
    }
    const bool presenting = cycle || replay;
    if (presenting)
    {
        // The presenting modes measure no row: any run count from one to the row's count, and always with the debug
        // layer and DRED, on a window the desktop composes.
        g_debugLayer = true;
        if (runs < 1)
            runs = 1;
        if (runs > static_cast<int>(kProbeRuns))
            runs = static_cast<int>(kProbeRuns);
        if (frames < 1)
            frames = 1;
    }
    else if (runs < static_cast<int>(kProbeRuns))
    {
        Emitf("fg-probe: at least %zu runs are needed for one row\n", kProbeRuns);
        return 2;
    }
    if (!presenting && runs > static_cast<int>(kProbeRuns))
    {
        // Clamping is fine, but not silently: a saved log has to say how many runs the row really rests on.
        Emitf("fg-probe: --runs %d capped at %zu (that is what a row needs)\n", runs, kProbeRuns);
        runs = static_cast<int>(kProbeRuns);
    }

    // A relative folder gives LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR error 87 (it needs a fully qualified path), and the row
    // would then say "the library could not create a chain", which is misleading.
    wchar_t full[MAX_PATH * 2] {};
    const std::wstring folderWide = Widen(folderArg);
    if (folderWide.empty() || GetFullPathNameW(folderWide.c_str(), static_cast<DWORD>(MAX_PATH * 2), full, nullptr) == 0)
    {
        Emitf("fg-probe: --folder could not be resolved: %s\n", folderArg.c_str());
        return 2;
    }
    const std::wstring folder = full;
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Emitf("fg-probe: --folder does not exist: %s\n", Narrow(folder.c_str()).c_str());
        return 2;
    }

    if (!outPath.empty())
    {
        if (fopen_s(&g_out, outPath.c_str(), "wb") != 0)
            g_out = nullptr;
    }

    const bool pageHeap = PageHeapActive();
    Emitf("fg-probe host begin runs=%d folder=%s pageheap=%s\n", runs, Narrow(folder.c_str()).c_str(),
          pageHeap ? "on" : "off");

    // The library modules are declared before the device so that they are unloaded AFTER the device, the queue and the
    // window are released (declaration order, reverse destruction): a library that registered a callback or private data
    // on the device must not be unloaded first.
    Fsr fsrLibrary;
    XeFg xefgLibrary;

    Device device;
    if (presenting)
    {
        device.shown = true;
        device.size = kCycleWindowSize;
    }
    if (!device.Create())
        return 1;

    g_lastOutputMs.store(GetTickCount64(), std::memory_order_relaxed);
    HANDLE watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);

    if (presenting)
    {
        const int code = RunPresenting(device, fsrLibrary, xefgLibrary, folder, cycle, fsr, xefg, runs, frames);
        if (g_out != nullptr)
        {
            std::fclose(g_out);
            g_out = nullptr;
        }
        ReleaseDevice(device);
        if (watchdog != nullptr)
            CloseHandle(watchdog);
        return code;
    }

    struct Request
    {
        Class cls;
        bool wanted;
    };
    const Request requests[] = { { Class::FsrFg, fsr }, { Class::XeFg, xefg } };

    int measuredRows = 0;
    for (const Request& request : requests)
    {
        if (!request.wanted)
            continue;

        LibraryIdentity identity;
        std::vector<ProbeRun> measured;
        bool attempted = false;
        bool accepted = false;
        bool released = false;
        bool measuredOk = false;
        char error[256] {};
        LibraryIdentity companion;

        if (request.cls == Class::FsrFg)
        {
            if (fsrLibrary.Load(folder, error) && fsrLibrary.module.Identify(identity) && fsrLibrary.Prepare(device))
            {
                measuredOk = MeasureClass(device, fsrLibrary, request.cls, runs, measured, attempted, accepted, released);
                fsrLibrary.Finish();
            }
        }
        else
        {
            if (xefgLibrary.Load(folder, error) && xefgLibrary.module.Identify(identity) && xefgLibrary.Prepare(device))
            {
                measuredOk =
                    MeasureClass(device, xefgLibrary, request.cls, runs, measured, attempted, accepted, released);
                xefgLibrary.Finish();
                if (!xefgLibrary.IdentifyCompanion(companion))
                    std::snprintf(error, sizeof(error), "the companion DLL could not be identified");
            }
        }

        const char* config = request.cls == Class::XeFg ? XeFg::Config() : Fsr::Config();
        if (!measuredOk)
        {
            Emitf("fg-probe model class=%s file=%s size=%llu stamp=%08X config=%s verdict=unknown witness=no "
                  "runs=0 evidence=%s\n",
                  ClassText(request.cls), identity.file, static_cast<unsigned long long>(identity.size), identity.stamp,
                  config, error[0] != '\0' ? error : "the library could not create a chain");
            continue;
        }

        ProbeRuns probeRuns;
        probeRuns.ran = true;
        probeRuns.runCount = measured.size() <= kProbeRuns ? measured.size() : kProbeRuns;
        for (size_t i = 0; i < probeRuns.runCount; ++i)
            probeRuns.runs[i] = measured[i];
        probeRuns.witnessAttempted = attempted;
        probeRuns.witnessAccepted = accepted;
        probeRuns.witnessReleasedWithOuter = released;

        // The standstill count is part of the row, so every run has to agree on it: a count that moved means the chain
        // held something different each time and the preflight's comparison would be against a guess.
        uint32_t refs = measured.empty() ? 0 : measured[0].standstillRefs;
        bool refsAgree = refs != 0;
        for (const ProbeRun& run : measured)
            if (run.standstillRefs != refs)
                refsAgree = false;

        // The three informational answers: a second destroy that succeeded means the first one only deferred the
        // context; the release-then-destroy order either leaves the window free or not; and the largest local
        // video-memory growth of the runs is what a leaked context costs.
        bool anyDestroyAfterPin = false;
        bool allDestroyedAfterPin = true;
        bool anyOrderB = false;
        bool allOrderBFree = true;
        bool allFirstDestroyRefused = true;
        bool noneFirstDestroyRefused = true;
        int64_t vramDelta = 0;
        for (const ProbeRun& run : measured)
        {
            if (run.destroyAfterPinAttempted)
            {
                anyDestroyAfterPin = true;
                if (run.destroyAfterPin != 0)
                    allDestroyedAfterPin = false;
            }
            if (run.firstDestroyCode != 0)
                noneFirstDestroyRefused = false;
            else
                allFirstDestroyRefused = false;
            if (run.orderBMeasured)
            {
                anyOrderB = true;
                if (!run.orderBFree)
                    allOrderBFree = false;
            }
            if (run.vramDeltaMb > vramDelta)
                vramDelta = run.vramDeltaMb;
        }

        const Classified classified = ClassifyProbeRuns(probeRuns);
        ModelRow row;
        row.cls = request.cls;
        row.library = identity;
        row.companion = companion;
        row.model = classified.model;
        row.pinFirst = classified.pinFirst;
        row.standstillRefs = refs;
        std::snprintf(row.config, sizeof(row.config), "%s", config);
        // A context that refused its own destroy while the bridge still held the chain has nobody left to destroy it
        // after the pin release its handle was dropped without the library destroying it. The label says what was
        // measured (the destroy refused), not what it costs: nothing here measures the memory a dropped context keeps. A
        // destroy that succeeded ended it. The optional second destroy (off by
        // default) answers the same question from the other side; on XeFG it stops the process once the chain is gone.
        const bool leakedByRefusal = allFirstDestroyRefused && !measured.empty();
        std::snprintf(row.context, sizeof(row.context), "%s",
                      !anyDestroyAfterPin ? (leakedByRefusal ? "refused" : (noneFirstDestroyRefused ? "destroyed" : "unknown"))
                                          : (allDestroyedAfterPin ? "destroyed" : "refused"));
        std::snprintf(row.orderB, sizeof(row.orderB), "%s",
                      !anyOrderB ? "unknown" : (allOrderBFree ? "clean" : "leaked"));
        row.vramDeltaMb = static_cast<int32_t>(vramDelta);

        const char* evidence = classified.evidence;
        const bool usable = row.model.model != Model::Unknown && refsAgree &&
                            (request.cls != Class::XeFg || !companion.Empty());
        if (!usable)
        {
            // A line with verdict=unknown is skipped by the table script: a rejected measurement never becomes a row.
            if (row.model.model != Model::Unknown)
                row.model.model = Model::Unknown;
            if (!refsAgree)
                evidence = "the standstill reference count differed between the runs";
            if (request.cls == Class::XeFg && companion.Empty())
                evidence = "the companion DLL (libxell.dll) could not be identified";
        }

        char formatted[512] {};
        FormatRow(row, "fg-probe", formatted);
        Emitf("%s ctx=%s orderB=%s vramDeltaMb=%d pageheap=%s runs=%zu evidence=%s\n", formatted, row.context,
              row.orderB, static_cast<int>(row.vramDeltaMb), pageHeap ? "on" : "off", probeRuns.runCount, evidence);
        if (usable)
            ++measuredRows;
    }

    Emitf("fg-probe host done rows=%d\n", measuredRows);

    if (g_out != nullptr)
    {
        std::fclose(g_out);
        g_out = nullptr;
    }

    // The device goes first, then the library modules (their destructors run at the end of main).
    ReleaseDevice(device);

    if (watchdog != nullptr)
    {
        // The watchdog sleeps for kWatchdogMs; nothing waits for it, and the process exit ends it.
        CloseHandle(watchdog);
    }
    return 0;
}
