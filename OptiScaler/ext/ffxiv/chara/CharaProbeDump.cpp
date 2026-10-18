#include "pch.h"

#include "ext/ffxiv/chara/CharaProbeDump.h"

#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/ui/UiDiagFormat.h"

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace ext::ffxiv::chara::probe::dump
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint64_t kGiveUpMs = 10000; // a dump whose GPU work did not finish by then is dropped

struct Item
{
    DumpImage image;
    ComPtr<ID3D11Texture2D> staging; // null when the image is not staged
};

struct Job
{
    std::vector<Item> items;
    ComPtr<ID3D11Query> query;
    std::string stateText;
    uint64_t startMs = 0;
};

bool g_hasJob = false;
Job g_job;
OutputBudget g_budget;
fs::path g_rootOverride;
std::atomic<bool> g_inFlight { false };
std::atomic<uint32_t> g_dumps { 0 };
std::atomic<uint32_t> g_refusals { 0 };
std::atomic<uint32_t> g_files { 0 };
std::atomic<uint64_t> g_bytes { 0 };
std::mutex g_textMutex;
std::string g_lastFolder;
std::string g_lastMessage;

std::string FormatText(uint32_t format) { return std::format("{}({})", ext::ffxiv::ui::dfmt::FormatName(format), format); }

// The three calls that talk to the GPU, replaceable by a test (SetHooksForTest): a map that is still busy, a pitch that is not tight, a query
// that never finishes cannot be produced on WARP, which finishes at once.
HRESULT RealMap(ID3D11DeviceContext* context, ID3D11Resource* resource, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    return context->Map(resource, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, mapped);
}
void RealUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource) { context->Unmap(resource, 0); }
HRESULT RealQuery(ID3D11DeviceContext* context, ID3D11Query* query) { return context->GetData(query, nullptr, 0, 0); }
TestHooks g_hooks { &RealMap, &RealUnmap, &RealQuery };

// The result of a dump that gave up.
DumpOutcome GaveUp()
{
    DumpOutcome outcome;
    outcome.message = "failed, the GPU did not finish the copies within 10 seconds";
    return outcome;
}

void Record(const DumpOutcome& outcome)
{
    if (outcome.result == DumpOutcome::Result::Written)
    {
        ++g_dumps;
        g_files += outcome.files;
        g_bytes += outcome.bytes;
    }
    else if (outcome.result == DumpOutcome::Result::Refused)
        ++g_refusals;
    std::lock_guard lock(g_textMutex);
    if (outcome.result == DumpOutcome::Result::Written)
        g_lastFolder = outcome.folder;
    g_lastMessage = outcome.message;
}

void Drop()
{
    g_job = Job {};
    g_hasJob = false;
    g_inFlight.store(false, std::memory_order_relaxed);
}

DumpOutcome Finish(std::vector<DumpImage> images, const std::string& stateText)
{
    DumpOutcome outcome = WriteDump(OutputRoot(), StampNow(), images, stateText, g_budget);
    Record(outcome);
    return outcome;
}
} // namespace

fs::path OptiScalerDirectory()
{
    wchar_t path[32768] {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OptiScalerDirectory), &module) ||
        !GetModuleFileNameW(module, path, 32768))
        return {};
    return fs::path(path).parent_path() / "OptiScaler";
}

fs::path OutputRoot()
{
    if (!g_rootOverride.empty())
        return g_rootOverride;
    const fs::path folder = OptiScalerDirectory();
    return folder.empty() ? fs::path() : folder / "PreviewDebug";
}

bool InFlight() { return g_inFlight.load(std::memory_order_relaxed); }

bool PredicationActive(ID3D11DeviceContext* context)
{
    if (context == nullptr)
        return false;
    ComPtr<ID3D11Predicate> predicate;
    BOOL value = FALSE;
    context->GetPredication(&predicate, &value);
    return predicate != nullptr;
}

bool Begin(ID3D11DeviceContext* context, const std::vector<Source>& sources, std::string stateText, uint64_t nowMs, std::string& why)
{
    if (context == nullptr)
    {
        why = "no device context";
        return false;
    }
    if (g_hasJob)
    {
        why = "a dump is already in progress";
        return false;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
    {
        why = "no device";
        return false;
    }

    Job job;
    job.stateText = std::move(stateText);
    job.startMs = nowMs;
    for (const Source& source : sources)
    {
        Item item;
        DumpImage& img = item.image;
        img.name = source.name;
        img.age = source.age;
        if (source.texture == nullptr)
        {
            img.note = source.missing.empty() ? "no texture" : source.missing;
            job.items.push_back(std::move(item));
            continue;
        }
        D3D11_TEXTURE2D_DESC desc {};
        source.texture->GetDesc(&desc);
        img.width = desc.Width;
        img.height = desc.Height;
        img.format = static_cast<uint32_t>(desc.Format);
        img.formatText = FormatText(img.format);
        if (desc.SampleDesc.Count != 1)
            img.note = "the texture is multisampled (" + std::to_string(desc.SampleDesc.Count) + " samples)";
        else if (LayoutOfFormat(img.format) == PixelLayout::Unsupported)
            img.note = "format " + img.formatText + " is not supported (only B8G8R8A8, R8G8B8A8 and R16G16B16A16_FLOAT formats are converted)";
        else if (desc.Width > kMaxPngDimension || desc.Height > kMaxPngDimension)
            img.note = "the texture is larger than " + std::to_string(kMaxPngDimension) + " pixels";
        else
        {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &item.staging);
            if (FAILED(hr) || item.staging == nullptr)
            {
                item.staging.Reset();
                img.note = std::format("the staging texture could not be created (0x{:08X})", static_cast<uint32_t>(hr));
            }
        }
        job.items.push_back(std::move(item));
    }

    D3D11_QUERY_DESC qd {};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateQuery(&qd, &job.query)) || job.query == nullptr)
    {
        why = "the GPU query could not be created";
        return false;
    }

    {
        ReplayScope scope; // our own copies and the query go through the hooked context
        for (size_t i = 0; i < job.items.size(); ++i)
        {
            if (job.items[i].staging == nullptr)
                continue;
            // Source texture i is the i-th source with a texture; sources and items are in the same order.
            context->CopySubresourceRegion(job.items[i].staging.Get(), 0, 0, 0, 0, sources[i].texture, 0, nullptr);
        }
        context->End(job.query.Get());
    }

    g_job = std::move(job);
    g_hasJob = true;
    g_inFlight.store(true, std::memory_order_relaxed);
    return true;
}

std::optional<DumpOutcome> Pump(ID3D11DeviceContext* context, uint64_t nowMs)
{
    if (!g_hasJob || context == nullptr)
        return std::nullopt;
    try
    {
        // A dump whose GPU work did not finish in time is dropped, but only when the GPU really is not done: a dump that finished long ago and
        // is pumped late (no draws during a loading screen, a minimised window) is written, not reported as a GPU failure.
        const bool late = nowMs >= g_job.startMs && nowMs - g_job.startMs > kGiveUpMs;
        const auto giveUp = [&]
        {
            DumpOutcome outcome = GaveUp();
            Record(outcome);
            Drop();
            return outcome;
        };
        if (g_job.query != nullptr && g_hooks.queryDone(context, g_job.query.Get()) != S_OK)
            return late ? std::optional<DumpOutcome>(giveUp()) : std::nullopt; // the GPU is not done with the copies

        // Map every staged image without waiting. One that is still busy gives up this call (all mappings are undone); any other
        // failure only drops that image.
        std::vector<D3D11_MAPPED_SUBRESOURCE> mapped(g_job.items.size());
        std::vector<bool> isMapped(g_job.items.size(), false);
        bool busy = false;
        for (size_t i = 0; i < g_job.items.size() && !busy; ++i)
        {
            Item& item = g_job.items[i];
            if (item.staging == nullptr)
                continue;
            const HRESULT hr = g_hooks.map(context, item.staging.Get(), &mapped[i]);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
                busy = true;
            else if (FAILED(hr))
            {
                item.staging.Reset();
                item.image.note = std::format("the staging texture could not be read (0x{:08X})", static_cast<uint32_t>(hr));
            }
            else
                isMapped[i] = true;
        }
        if (busy)
        {
            for (size_t i = 0; i < g_job.items.size(); ++i)
                if (isMapped[i])
                    g_hooks.unmap(context, g_job.items[i].staging.Get());
            return late ? std::optional<DumpOutcome>(giveUp()) : std::nullopt;
        }

        // Copy the rows out packed and unmap before touching the disk.
        std::vector<DumpImage> images;
        images.reserve(g_job.items.size());
        for (size_t i = 0; i < g_job.items.size(); ++i)
        {
            Item& item = g_job.items[i];
            DumpImage& img = item.image;
            if (isMapped[i])
            {
                const size_t rowBytes = static_cast<size_t>(img.width) * BytesPerPixel(LayoutOfFormat(img.format));
                img.rowPitch = rowBytes;
                img.data.resize(rowBytes * img.height);
                for (uint32_t y = 0; y < img.height; ++y)
                    std::memcpy(img.data.data() + static_cast<size_t>(y) * rowBytes,
                                static_cast<const uint8_t*>(mapped[i].pData) + static_cast<size_t>(y) * mapped[i].RowPitch, rowBytes);
                g_hooks.unmap(context, item.staging.Get());
                img.present = true;
            }
            images.push_back(std::move(img));
        }
        const std::string stateText = std::move(g_job.stateText);
        Drop();
        return Finish(std::move(images), stateText);
    }
    catch (...)
    {
        Drop();
        DumpOutcome outcome;
        outcome.message = "failed, an error stopped the dump";
        Record(outcome);
        return outcome;
    }
}

DumpOutcome WriteStateOnly(const std::string& stateText) { return Finish({}, stateText); }

Status GetStatus()
{
    Status s;
    s.inFlight = g_inFlight.load(std::memory_order_relaxed);
    s.dumps = g_dumps.load(std::memory_order_relaxed);
    s.refusals = g_refusals.load(std::memory_order_relaxed);
    s.files = g_files.load(std::memory_order_relaxed);
    s.bytes = g_bytes.load(std::memory_order_relaxed);
    std::lock_guard lock(g_textMutex);
    s.lastFolder = g_lastFolder;
    s.lastMessage = g_lastMessage;
    return s;
}

void SetMessage(const std::string& message)
{
    std::lock_guard lock(g_textMutex);
    g_lastMessage = message;
}

void Release()
{
    if (g_hasJob)
        Drop();
}


void SetHooksForTest(const TestHooks& hooks)
{
    g_hooks.map = hooks.map != nullptr ? hooks.map : &RealMap;
    g_hooks.unmap = hooks.unmap != nullptr ? hooks.unmap : &RealUnmap;
    g_hooks.queryDone = hooks.queryDone != nullptr ? hooks.queryDone : &RealQuery;
}
void SetOutputRootForTest(const fs::path& root) { g_rootOverride = root; }

void ResetSessionForTest()
{
    g_budget = OutputBudget {};
    g_dumps = 0;
    g_refusals = 0;
    g_files = 0;
    g_bytes = 0;
    std::lock_guard lock(g_textMutex);
    g_lastFolder.clear();
    g_lastMessage.clear();
}
} // namespace ext::ffxiv::chara::probe::dump
