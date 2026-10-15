#include "pch.h"
#include "Companion.h"
#include "CompanionRuntime.h"
#include "CompanionMarkers.h"
#include "CompanionNative.h"
#include "CompanionGpu.h"
#include "CompanionLayer.h"
#include <atomic>
#include <imgui/imgui.h>
#include <Config.h>
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/game/Game.h"

namespace ext::ffxiv::companion
{
bool DiagnosticsEnabled()
{
    const auto* c=Config::Instance();
    return c->LogLevel.value_or_default()<6 &&
        (c->LogToFile.value_or_default() || c->LogToConsole.value_or_default() ||
         c->LogToDebug.value_or_default() || c->LogToNGX.value_or_default());
}

bool Install(ID3D11Device*) noexcept
{
    try
    {
        Native::Install();
        return Native::GetStatistics().installed;
    }
    catch (...)
    {
        LOG_WARN("FFXIV Companion: native hook installation failed; native drawing remains active");
        return false;
    }
}

void Unhook() noexcept
{
    try { Native::Detach(); }
    catch (...) { LOG_WARN("FFXIV Companion: teardown failed"); }
}

void AfterSharedCopy(ID3D11DeviceContext* context, ID3D11Texture2D* target) noexcept
{
    try { DrawSourceMarkers(context, target); }
    catch (...) { LOG_WARN("FFXIV Companion: source-copy diagnostics failed"); }
}

bool IsInstalled() noexcept { return Native::GetStatistics().installed; }

namespace
{
void TagShader(ID3D11PixelShader* shader, const void* code, SIZE_T size) { Gpu::TagShader(shader, code, size); }
void TagPixel(ID3D11PixelShader* shader, const void* code, SIZE_T size) { Layer::TagPixel(shader, code, size); }
void TagLayout(ID3D11InputLayout* layout, const D3D11_INPUT_ELEMENT_DESC* desc, UINT count, const void* code, SIZE_T size)
{
    Layer::TagLayout(layout, desc, count, code, size);
}
void ObserveDraw(ID3D11DeviceContext* context) { Layer::Observe(context); }
bool InterceptVisibilityIndexed(ID3D11DeviceContext* context, UINT count, UINT start, INT baseVertex, IndexedOriginal original)
{
    return Layer::InterceptVisibilityIndexed(context, count, start, baseVertex, original);
}
struct DrawState { DrawCall draw; void* state; };
void InvokeDraw(void* state)
{
    auto& call = *static_cast<DrawState*>(state);
    call.draw(call.state);
}
void AroundIndexed(ID3D11DeviceContext* context, UINT count, UINT start, DrawCall draw, void* state)
{
    DrawState call { draw, state };
    Gpu::AroundIndexed(context, count, start, [&] { InvokeDraw(&call); });
}
void DrawCompanionCard(ext::menu::Ctx&) { DrawSettings(); }
bool CompanionVisible(const ext::menu::Ctx&) { return ext::game::Current().hdr != nullptr; }
const ext::menu::AutoRegister companionCard {
    { "ffxiv_companion", "Compatibility", "FFXIV Companion", DrawCompanionCard, nullptr, CompanionVisible }
};
} // namespace

CaptureCallbacks GetCaptureCallbacks() noexcept
{
    return { TagShader, TagPixel, TagLayout, ObserveDraw, InterceptVisibilityIndexed, AroundIndexed };
}

static std::atomic<bool> markersAllowed {false};
static std::atomic<uint64_t> sourceFrames {0}, skippedFrames {0};
void DrawSourceMarkers(ID3D11DeviceContext* context, ID3D11Texture2D* target)
{
    if (!DiagnosticsEnabled())
    {
        Gpu::requested=false;
        if (!ext::Cfg().ffxiv.companionHudReplacement.value_or_default() && Native::GetStatistics().requested)
            Native::SetRequested(false);
    }
    Layer::Tick(context);
    // Report independently of marker visibility. Aggregate only; no per-draw I/O.
    static ULONGLONG reportAt = 0;
    if (Gpu::requested.load() && GetTickCount64() - reportAt >= 5000)
    {
        reportAt = GetTickCount64();
        LOG_INFO("Companion GPU geometry: publications {}, matched {}, copied {}, MiB {:.2f}, unsupported {}, budget {}, allocation {}, identity-read {}, busy {}, stale {}, material-age {} ms, sampled-CPU-total {} us, peak {} us, layouts {}/{}/{}/{}, shaders {}/{}/{}/{}",
            Gpu::publications.load(), Gpu::matched.load(), Gpu::copied.load(), Gpu::copiedBytes.load() / 1048576.0,
            Gpu::unsupported.load(), Gpu::budgetSkips.load(), Gpu::allocationFailures.load(), Gpu::readFailures.load(),
            Gpu::busy.load(), Gpu::stale.load(), Gpu::lastAgeMs.load(), Gpu::cpuUs.load(), Gpu::cpuPeakUs.load(),
            Gpu::layouts[0].load(), Gpu::layouts[1].load(), Gpu::layouts[2].load(), Gpu::layouts[3].load(),
            Gpu::tagged[0].load(), Gpu::tagged[1].load(), Gpu::tagged[2].load(), Gpu::tagged[3].load());
        LOG_INFO("Companion GPU detail: copied-layouts {}/{}/{}/{}, IA-strides {}/{}/{}/{}, vertex-bytes {}/{}/{}/{}, index-bytes {}/{}/{}/{}, slot-masks {:X}/{:X}/{:X}/{:X}, rejects context {} missing {} format {} flags {} range {} oversize {} byte-budget {} allocation {}",
            Gpu::copiedLayouts[0].load(), Gpu::copiedLayouts[1].load(), Gpu::copiedLayouts[2].load(), Gpu::copiedLayouts[3].load(),
            Gpu::observedStrides[0].load(), Gpu::observedStrides[1].load(), Gpu::observedStrides[2].load(), Gpu::observedStrides[3].load(),
            Gpu::observedVertexBytes[0].load(), Gpu::observedVertexBytes[1].load(), Gpu::observedVertexBytes[2].load(), Gpu::observedVertexBytes[3].load(),
            Gpu::observedIndexBytes[0].load(), Gpu::observedIndexBytes[1].load(), Gpu::observedIndexBytes[2].load(), Gpu::observedIndexBytes[3].load(),
            Gpu::observedSlotMasks[0].load(), Gpu::observedSlotMasks[1].load(), Gpu::observedSlotMasks[2].load(), Gpu::observedSlotMasks[3].load(),
            Gpu::results[1].load(), Gpu::results[2].load(), Gpu::results[3].load(), Gpu::results[4].load(),
            Gpu::results[5].load(), Gpu::results[6].load(), Gpu::results[7].load(), Gpu::results[8].load());
    }
    if (!DiagnosticsEnabled() || !markersAllowed.load()) return;
    Snapshot snapshot;
    Status status;
    // Take ONE owned snapshot for this source image; later plugin updates cannot
    // reposition its marks during the multiple generated presentations of that image.
    if (!mailbox.Read(snapshot, status, Now()) || !(snapshot.frame.flags & Preview)) return;
    if (PaintMarkers(context, target, snapshot))
    {
        const auto painted = ++sourceFrames;
        if (painted == 1 || painted % 600 == 0)
            LOG_INFO("Companion source-frame markers: drawn {}, snapshot {}, plates {}, age {:.2f} ms, skipped {}",
                     painted, snapshot.frame.sequence, snapshot.frame.count,
                     1000.0 * status.ageQpc / Frequency(), skippedFrames.load());
    }
    else ++skippedFrames;
}
void DrawSettings()
{
    if (!ImGui::CollapsingHeader("FFXIV OptiScaler Companion")) return;
    Snapshot snapshot;
    Status status;
    const bool fresh = mailbox.Read(snapshot, status, Now());
    ImGui::TextUnformatted(fresh ? "Receiving native nameplate data" : "Waiting for fresh visible nameplates");
    ImGui::Text("Nameplates: %u", status.count);
    Layer::DrawSettings();
    if (!DiagnosticsEnabled() || !ImGui::TreeNode("Companion diagnostics")) return;
    ImGui::Text("Accepted: %llu | Rejected: %llu", status.accepted, status.rejected);
    if (status.sequence) ImGui::Text("Snapshot age: %.1f ms", 1000.0 * status.ageQpc / Frequency());
    bool allowed = markersAllowed.load();
    if (ImGui::Checkbox("Allow Companion alignment markers", &allowed)) markersAllowed.store(allowed);
    ImGui::Text("DX11 source frames marked: %llu | Skipped: %llu", sourceFrames.load(), skippedFrames.load());
    ImGui::TextWrapped("Markers are attached to the DX11 HUD copy before HDR and frame generation.");
    auto native = Native::GetStatistics();
    ImGui::BeginDisabled(ext::Cfg().ffxiv.companionHudReplacement.value_or_default());
    bool copied = native.requested;
    ImGui::BeginDisabled(!native.installed);
    if (ImGui::Checkbox("Use copied nameplate submissions (test)", &copied)) Native::SetRequested(copied);
    ImGui::EndDisabled();
    ImGui::Text("Captured: %llu | Copied batches: %llu", native.captures, native.replacements);
    ImGui::Text("Copied packets: %llu | Fallbacks: %llu", native.packets, native.fallbacks);
    ImGui::TextWrapped("Uses copied draw commands with the native vertex buffers, fonts, materials and draw order. This test does not increase refresh rate.");
    if (!native.installed) ImGui::TextWrapped("Native submission test unavailable: executable guards or hooks did not match.");
    bool geometry = Gpu::requested.load();
    ImGui::BeginDisabled(!native.installed);
    if (ImGui::Checkbox("Test owned GPU geometry (sampled)", &geometry))
    {
        Gpu::Clear();
        if (geometry) Native::SetRequested(true);
        Gpu::requested.store(geometry);
    }
    ImGui::EndDisabled();
    ImGui::Text("Matching draws: %llu | Owned geometry draws: %llu", Gpu::matched.load(), Gpu::copied.load());
    ImGui::Text("Copied by layout (0/4/5/6): %llu / %llu / %llu / %llu", Gpu::copiedLayouts[0].load(), Gpu::copiedLayouts[1].load(), Gpu::copiedLayouts[2].load(), Gpu::copiedLayouts[3].load());
    ImGui::Text("Copied: %.1f MiB | Budget skips: %llu", Gpu::copiedBytes.load() / 1048576.0, Gpu::budgetSkips.load());
    ImGui::Text("Unsupported: %llu | Allocation failures: %llu", Gpu::unsupported.load(), Gpu::allocationFailures.load());
    ImGui::Text("Last material age: %llu ms | Peak sampled CPU: %.3f ms", Gpu::lastAgeMs.load(), Gpu::cpuPeakUs.load() / 1000.0);
    ImGui::TextWrapped("Tests owned vertex/index buffers in place of native buffers for up to four matching draws per 100 ms. Shared UI materials can also match. No extra draws or higher refresh; all shaders, textures, ordering and input stay native. Logs include layout coverage and fallback reasons. CPU timing includes the sampled native draw and is not GPU latency.");
    ImGui::TextWrapped("Use /opticompanion in Dalamud to enable markers. Alignment markers are optional for this test.");
    ImGui::EndDisabled();
    Layer::DrawDiagnostics();
    ImGui::TreePop();
}
}

#include "CompanionExports.inl"
