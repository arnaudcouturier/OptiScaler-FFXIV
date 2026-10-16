#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/hdr/screenshot/HdrScreenshot.h"
#include "ext/hdr/screenshot/Image.h"
#include "ext/hdr/screenshot/MenuCapture.h"
#include "ext/hdr/screenshot/Readback.h"

#include <Config.h>
#include <Logger.h>
#include <Util.h>
#include <menu/menu_common.h>

#include <atomic>
#include <mutex>
#include <new>

namespace ext::hdr::screenshot
{
using Microsoft::WRL::ComPtr;

struct Job;
namespace
{
struct State;
State& S();

struct State
{
    std::atomic<bool> busy { false };
    std::atomic<ULONGLONG> requested { 0 };
    std::mutex mutex;
    std::string message = "Screenshots save to game/OptiScaler/Screenshots";
    std::shared_ptr<Job> prepared;
};

State& S()
{
    static auto* state = new State;
    return *state;
}

void Message(std::string value)
{
    std::lock_guard lock(S().mutex);
    S().message = std::move(value);
}

void Submit(const std::shared_ptr<Job>& job, ID3D12CommandQueue* queue);
} // namespace

struct Job : Readback
{
    std::filesystem::path path;
    ID3D12CommandList* commands = nullptr;
    ULONGLONG preparedAt = 0;
    bool hdr = false;
    bool iccOnly = false;
    // Keep submitted readback resources and this DLL alive through the worker.
    std::shared_ptr<Job> pendingOwnership;
    ~Job();
};

namespace
{
void ReleaseBusy() { S().busy = false; }
}

Job::~Job() { ReleaseBusy(); }

std::string Status()
{
    std::lock_guard lock(S().mutex);
    return S().message;
}

void Request()
{
    if (!Active())
    {
        Message("OptiHDR must be active to take a screenshot");
        return;
    }

    bool expired = false;
    {
        std::lock_guard lock(S().mutex);
        if (S().prepared && GetTickCount64() - S().prepared->preparedAt > 5000)
        {
            S().prepared.reset();
            expired = true;
        }
    }
    if (expired)
        Message("Previous screenshot was not submitted");

    if (S().busy.load())
    {
        Message("Screenshot encoding is still in progress");
        return;
    }
    S().requested = GetTickCount64();
    Message("Screenshot requested");
}

bool Pending()
{
    const auto requested = S().requested.load();
    return requested && GetTickCount64() - requested <= 1000 && !S().busy.load();
}

bool PendingHDR()
{
    return Pending() && ext::Cfg().hdr.screenshotFormat.value_or_default() == 1;
}

void Prepare(HWND window, ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* hdrSource,
             D3D12_RESOURCE_STATES hdrState, ID3D12Resource* sdrSource, D3D12_RESOURCE_STATES sdrState)
{
    const auto requested = S().requested.exchange(0);
    if (!requested || GetTickCount64() - requested > 1000)
        return;
    if (!Active() || !window || !device || !commands || !hdrSource || !sdrSource)
    {
        Message("Screenshot source unavailable");
        return;
    }

    const int key = ext::Cfg().hdr.screenshotKey.value_or_default();
    if (key <= 0 || key >= 256)
    {
        Message("Bind an OptiHDR screenshot key in Keybinds");
        return;
    }
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(window, GA_ROOT))
    {
        Message("Screenshot skipped because FFXIV is not the foreground window");
        return;
    }
    if (S().busy.exchange(true))
        return;

    std::shared_ptr<Job> job;
    try
    {
        const auto& config = ext::Cfg().hdr;
        job = std::make_shared<Job>();
        job->hdr = config.screenshotFormat.value_or_default() == 1;
        job->iccOnly = job->hdr && config.screenshotIccOnly.value_or_default();
        const auto sourceState = job->hdr ? hdrState : sdrState;
        image::Check(job->Allocate(device, job->hdr ? hdrSource : sdrSource, job->hdr));

        SYSTEMTIME time {};
        GetLocalTime(&time);
        job->path = Util::DllPath().parent_path() / L"OptiScaler" / L"Screenshots" /
                    std::format(L"OptiHDR-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}-{}.png", time.wYear, time.wMonth,
                                time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetTickCount64(),
                                job->hdr ? L"HDR" : L"SDR");
        job->commands = commands;
        job->preparedAt = GetTickCount64();
        job->RecordPreservingState(commands, sourceState);
        {
            std::lock_guard lock(S().mutex);
            S().prepared = job;
        }
        Message(job->hdr ? "HDR screenshot queued" : "SDR screenshot queued");
    }
    catch (...)
    {
        if (!job)
            S().busy = false;
        Message("Screenshot allocation failed (see OptiScaler.log)");
        LOG_ERROR("OptiHDR screenshot allocation or preparation failed");
    }
}

bool MenuCaptureWanted()
{
    const auto& config = ext::Cfg().hdr;
    return WantsMenuCapture({ Active(), config.screenshotFormat.value_or_default(),
                              config.screenshotMenu.value_or_default(),
                              Config::Instance()->OverlayMenu.value_or_default(), MenuCommon::IsVisible() });
}

void PrepareMenu(ID3D12GraphicsCommandList* commands, ID3D12Resource* output) noexcept
{
    try
    {
        if (!commands || !output || !Pending() || !MenuCaptureWanted())
            return;

        ComPtr<ID3D12Device> device;
        if (FAILED(commands->GetDevice(IID_PPV_ARGS(&device))))
        {
            Message("Screenshot source unavailable");
            return;
        }

        // Same job as the bridge's capture, HDR only: the source is the finished PQ image in its render target state.
        Prepare(MenuCommon::Handle(), device.Get(), commands, output, D3D12_RESOURCE_STATE_RENDER_TARGET, output,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    catch (...)
    {
        LOG_ERROR("OptiHDR menu screenshot preparation failed");
    }
}

void Reset(ID3D12CommandList* commands) noexcept
{
    try
    {
        std::shared_ptr<Job> discarded;
        {
            std::lock_guard lock(S().mutex);
            if (S().prepared && S().prepared->commands == commands)
                discarded = std::move(S().prepared);
        }
        if (discarded)
            Message("Screenshot recording was reset before submission");
    }
    catch (...)
    {
        LOG_ERROR("OptiHDR screenshot reset callback failed; pending resources retained");
    }
}

namespace
{
struct Worker
{
    std::shared_ptr<Job> job;
    HMODULE module;
};

DWORD WINAPI Encode(void* parameter)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto* work = static_cast<Worker*>(parameter);
    const HMODULE module = work->module;
    auto& job = *work->job;
    bool com = false, mapped = false;
    try
    {
        // Never block the present path or release a readback before its queue fence completes.
        while (job.fence->GetCompletedValue() == 0)
            Sleep(10);
        if (job.fence->GetCompletedValue() == UINT64_MAX)
            throw DXGI_ERROR_DEVICE_REMOVED;
        image::Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        com = true;
        void* data = nullptr;
        D3D12_RANGE read { 0, SIZE_T(job.bytes) };
        image::Check(job.readback->Map(0, &read, &data));
        mapped = true;
        std::filesystem::create_directories(job.path.parent_path());
        const auto* pixels = static_cast<BYTE*>(data) + job.footprint.Offset;
        const auto& size = job.footprint.Footprint;
        image::SavePng(job.path, pixels, size.Width, size.Height, size.RowPitch, job.hdr, size.Format, job.iccOnly);
        Message("Saved " + job.path.filename().string());
        LOG_INFO("OptiHDR screenshot saved: {} (colour metadata: {})", job.path.string(),
                 job.hdr ? (job.iccOnly ? "BT.2100 PQ ICC only" : "BT.2100 PQ cICP") : "sRGB");
    }
    catch (HRESULT error)
    {
        Message("Screenshot failed (see OptiScaler.log)");
        LOG_ERROR("OptiHDR screenshot failed: {:08X}, path {}", static_cast<unsigned>(error), job.path.string());
        std::error_code ignored;
        std::filesystem::remove(job.path, ignored);
    }
    catch (...)
    {
        Message("Screenshot failed (see OptiScaler.log)");
        LOG_ERROR("OptiHDR screenshot encoding/readback failed");
        std::error_code error;
        std::filesystem::remove(job.path, error);
    }
    if (mapped)
    {
        D3D12_RANGE none { 0, 0 };
        job.readback->Unmap(0, &none);
    }
    if (com)
        CoUninitialize();
    job.pendingOwnership.reset();
    delete work;
    FreeLibraryAndExitThread(module, 0);
}
} // namespace

namespace
{
void Submit(const std::shared_ptr<Job>& job, ID3D12CommandQueue* queue)
{
    if (!job)
        return;
    job->pendingOwnership = job;
    if (!queue || FAILED(queue->Signal(job->fence.Get(), 1)))
    {
        Message("Screenshot disabled: GPU completion signal failed");
        LOG_ERROR("OptiHDR screenshot fence signal failed; pending resources retained");
        return;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Encode), &module))
    {
        Message("Screenshot worker unavailable");
        return;
    }
    auto* work = new (std::nothrow) Worker { job, module };
    if (!work)
    {
        Message("Screenshot worker allocation failed; capture disabled");
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, Encode, work, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
    {
        Message("Screenshot worker unavailable");
        LOG_ERROR("OptiHDR screenshot worker creation failed; pending resources retained");
    }
}
} // namespace

void AfterExecute(void*, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept
{
    try
    {
        std::shared_ptr<Job> job;
        {
            std::lock_guard lock(S().mutex);
            if (S().prepared)
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] == S().prepared->commands)
                    {
                        job = std::move(S().prepared);
                        break;
                    }
        }
        if (job)
            Submit(job, queue);
    }
    catch (...)
    {
        LOG_ERROR("OptiHDR screenshot submission callback failed; pending resources retained");
    }
}
} // namespace ext::hdr::screenshot
