#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDiag.h"
#include "ext/ffxiv/chara/CharaDiagRules.h"
#include "ext/ffxiv/chara/CharaDispatch.h"

#include <array>
#include <atomic>
#include <fstream>
#include <vector>

namespace ext::ffxiv::chara::diag
{
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
struct Job
{
    int phase = 0;
    int group = 0; // the number in the file names
    uint32_t width = 0;
    uint32_t height = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    std::array<ComPtr<ID3D11Texture2D>, kFilesPerDump> staging;
    DumpFormats formats {}; // DXGI format values, in kRoles order
    uint64_t bytes = 0;     // the budget cost of the whole dump, worked out once from those formats
    std::array<uint32_t, kFilesPerDump> widths {}, heights {};
    ComPtr<ID3D11Buffer> cameraStaging;
    uint32_t cameraBytes = 0;
    ComPtr<ID3D11Query> query;
};

bool g_hasJob = false;
Job g_job;
Budget g_budget;
PhaseGate g_phases;
int g_groups = 0;
TraceLimiter g_limiter;
fs::path g_directoryOverride;
std::atomic<uint32_t> g_filesWritten { 0 };
std::atomic<uint64_t> g_bytesWritten { 0 };

fs::path DefaultDirectory()
{
    wchar_t path[32768] {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&DefaultDirectory), &module) ||
        !GetModuleFileNameW(module, path, 32768))
        return {};
    return fs::path(path).parent_path() / "OptiScaler" / "PreviewDLSSDiagnostics";
}

ID3D11Texture2D* SourceOf(const Request& r, Role role)
{
    switch (role)
    {
    case Role::Native: return r.native;
    case Role::Input: return r.input;
    case Role::Dlss: return r.dlss;
    case Role::Composed: return r.composed;
    case Role::Depth: return r.depth;
    case Role::Motion: return r.motion;
    }
    return nullptr;
}

// The image of a role must have the size the format of the file says.
bool Plausible(const D3D11_TEXTURE2D_DESC& d, Role role, const Request& r)
{
    return d.Width == r.width * RoleScale(role) && d.Height == r.height * RoleScale(role) && d.SampleDesc.Count == 1 &&
           d.ArraySize == 1 && d.MipLevels == 1;
}

void LogMotionAndCamera(const Job& job, const std::vector<std::vector<uint8_t>>& files, const std::vector<uint8_t>& camera)
{
    MotionStats stats;
    const std::vector<uint8_t>& motion = files[static_cast<size_t>(Role::Motion)];
    for (size_t i = 0; i + 4 <= motion.size(); i += 4)
    {
        uint16_t x, y;
        std::memcpy(&x, motion.data() + i, 2);
        std::memcpy(&y, motion.data() + i + 2, 2);
        stats.Add(HalfToFloat(x), HalfToFloat(y));
    }
    float zw[2] {};
    const size_t offset = 58 * 16 + 8;
    if (camera.size() >= offset + 8)
        std::memcpy(zw, camera.data() + offset, 8);
    LOG_INFO("CharaView diagnostics {}: Camera[58].zw ({}, {}), motion x [{}, {}] y [{}, {}], nonzero {}/{}, invalid {}, jitter {}/{}",
             job.group, zw[0], zw[1], stats.minX, stats.maxX, stats.minY, stats.maxY, stats.nonZero, stats.total, stats.invalid,
             job.jitterX, job.jitterY);
}
} // namespace

bool CaptureEnabled() { return ext::Cfg().ffxiv.chara.previewGuideCapture.value_or_default(); }
bool TraceEnabled() { return ext::Cfg().ffxiv.chara.previewNrTrace.value_or_default(); }

bool Capture(ID3D11DeviceContext* context, const Request& request, uint64_t evaluations, uint64_t nowMs)
{
    if (!CaptureEnabled() || g_hasJob || context == nullptr || !SizeAllowed(request.width, request.height))
        return false;

    // The real format of every image decides its row size and the size budget (FormatBytesPerPixel), so all six are looked
    // at before the budget is checked.
    Job job;
    std::array<D3D11_TEXTURE2D_DESC, kFilesPerDump> descs {};
    for (size_t i = 0; i < kRoles.size(); ++i)
    {
        ID3D11Texture2D* source = SourceOf(request, kRoles[i]);
        if (source == nullptr)
            return false;
        source->GetDesc(&descs[i]);
        if (!Plausible(descs[i], kRoles[i], request))
            return false;
        job.formats[i] = static_cast<uint32_t>(descs[i].Format);
        job.widths[i] = descs[i].Width;
        job.heights[i] = descs[i].Height;
    }
    job.bytes = DumpBytes(request.width, request.height, job.formats);
    if (!g_budget.Fits(kFilesPerDump, job.bytes))
        return false;
    const int phase = g_phases.Next(evaluations, nowMs);
    if (phase < 0)
        return false;

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr)
        return false;

    job.phase = phase;
    job.group = g_groups + 1;
    job.width = request.width;
    job.height = request.height;
    job.jitterX = request.jitterX;
    job.jitterY = request.jitterY;

    for (size_t i = 0; i < kRoles.size(); ++i)
    {
        D3D11_TEXTURE2D_DESC desc = descs[i];
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &job.staging[i])))
            return false;
    }
    if (request.camera != nullptr)
    {
        D3D11_BUFFER_DESC bd {};
        request.camera->GetDesc(&bd);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(device->CreateBuffer(&bd, nullptr, &job.cameraStaging)))
            job.cameraBytes = bd.ByteWidth;
    }
    D3D11_QUERY_DESC qd {};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateQuery(&qd, &job.query)))
        return false;

    {
        ReplayScope scope; // our own copies go through the hooked CopyResource
        for (size_t i = 0; i < kRoles.size(); ++i)
            context->CopyResource(job.staging[i].Get(), SourceOf(request, kRoles[i]));
        if (job.cameraStaging != nullptr)
            context->CopyResource(job.cameraStaging.Get(), request.camera);
        context->End(job.query.Get());
    }

    g_phases.Started(phase, nowMs);
    ++g_groups;
    g_job = std::move(job);
    g_hasJob = true;
    return true;
}

void Pump(ID3D11DeviceContext* context, uint64_t nowMs)
{
    (void) nowMs;
    if (!g_hasJob || context == nullptr)
        return;
    if (g_job.query != nullptr && context->GetData(g_job.query.Get(), nullptr, 0, 0) != S_OK)
        return; // the GPU is not done with the copies

    // Map every image without waiting. A busy image is retried at the next call; any other failure drops the dump.
    std::array<D3D11_MAPPED_SUBRESOURCE, kFilesPerDump> mapped {};
    size_t mappedCount = 0;
    HRESULT failure = S_OK;
    for (; mappedCount < kFilesPerDump; ++mappedCount)
    {
        failure = context->Map(g_job.staging[mappedCount].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[mappedCount]);
        if (FAILED(failure))
            break;
    }
    if (FAILED(failure))
    {
        for (size_t i = 0; i < mappedCount; ++i)
            context->Unmap(g_job.staging[i].Get(), 0);
        if (failure == DXGI_ERROR_WAS_STILL_DRAWING)
            return;
        LOG_WARN("CharaView diagnostics: a staging image could not be read; dump {} dropped", g_job.group);
        g_phases.Finished(g_job.phase);
        g_job = Job {};
        g_hasJob = false;
        return;
    }

    // Copy the rows out (packed), then unmap before touching the disk.
    std::vector<std::vector<uint8_t>> files(kFilesPerDump);
    for (size_t i = 0; i < kFilesPerDump; ++i)
    {
        const uint32_t bpp = RoleBytesPerPixel(kRoles[i], g_job.formats[i]);
        const uint32_t w = g_job.widths[i], h = g_job.heights[i];
        std::vector<uint8_t>& out = files[i];
        out.resize(static_cast<size_t>(w) * h * bpp);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y) * w * bpp, static_cast<const uint8_t*>(mapped[i].pData) + static_cast<size_t>(y) * mapped[i].RowPitch,
                        static_cast<size_t>(w) * bpp);
        context->Unmap(g_job.staging[i].Get(), 0);
    }
    std::vector<uint8_t> camera;
    if (g_job.cameraStaging != nullptr)
    {
        D3D11_MAPPED_SUBRESOURCE m {};
        if (SUCCEEDED(context->Map(g_job.cameraStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)))
        {
            camera.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + g_job.cameraBytes);
            context->Unmap(g_job.cameraStaging.Get(), 0);
        }
    }

    const uint64_t dumpBytes = g_job.bytes;
    uint32_t saved = 0;
    if (g_budget.Fits(kFilesPerDump, dumpBytes))
    {
        const fs::path dir = g_directoryOverride.empty() ? DefaultDirectory() : g_directoryOverride;
        std::error_code ec;
        if (!dir.empty())
            fs::create_directories(dir, ec);
        if (!ec && !dir.empty())
        {
            for (size_t i = 0; i < kFilesPerDump; ++i)
            {
                const fs::path path = dir / (std::to_string(g_job.group) + "-" + RoleName(kRoles[i]) + ".bin");
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                const uint32_t header[3] { g_job.widths[i], g_job.heights[i], g_job.formats[i] };
                file.write(reinterpret_cast<const char*>(header), sizeof(header));
                file.write(reinterpret_cast<const char*>(files[i].data()), static_cast<std::streamsize>(files[i].size()));
                if (file.good())
                    ++saved;
            }
        }
    }
    if (saved != 0)
    {
        g_budget.Add(saved, dumpBytes);
        g_filesWritten.fetch_add(saved, std::memory_order_relaxed);
        g_bytesWritten.fetch_add(dumpBytes, std::memory_order_relaxed);
    }
    LOG_INFO("CharaView diagnostics: diagnostic {}: six synchronized textures saved={}, jitter {}/{}", g_job.group, saved == kFilesPerDump,
             g_job.jitterX, g_job.jitterY);
    LogMotionAndCamera(g_job, files, camera);

    g_phases.Finished(g_job.phase);
    g_job = Job {};
    g_hasJob = false;
}

void Trace(TraceCategory category, uint64_t nowMs, const std::string& text)
{
    if (!TraceEnabled() || !g_limiter.Allow(static_cast<size_t>(category), nowMs))
        return;
    LOG_INFO("CharaView trace: {}", text);
}

uint32_t FilesWritten() { return g_filesWritten.load(std::memory_order_relaxed); }
uint64_t BytesWritten() { return g_bytesWritten.load(std::memory_order_relaxed); }

void SetDirectoryForTest(const std::filesystem::path& directory) { g_directoryOverride = directory; }

void Release()
{
    g_job = Job {};
    g_hasJob = false;
}
} // namespace ext::ffxiv::chara::diag
