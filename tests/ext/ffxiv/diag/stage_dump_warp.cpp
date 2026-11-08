// The per-stage image dump over a real D3D11 device (WARP): what a capture frame writes for the stages of this fork, and what a frame that is
// not the capture frame costs.
//
// The stage hooks are the ones the stages call (diag/StageDump.h); the runtime under test is FfxivDrawDiag.cpp's, included whole as the other
// draw-diagnostic WARP test does. The pictures are small textures with a pattern that tells every byte's origin, so a file proves which texture
// it holds, which crop of it, and in which order the stages' copies were made.
#include "check.h"

#include "ext/ffxiv/diag/FfxivDrawDiag.cpp" // the runtime under test

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>

// The runtime reads the engine pass module's counters for its capture folder; this test has no modules, so every read answers "off".
namespace ext::ffxiv::engine
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
}

namespace ext::ffxiv::engine::ssr
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
FrameObservation CurrentFrameObservation() { return {}; }
void OnRenderCallback(ID3D11DeviceContext*) {}
void OnDiagnosticCaptureStart() {}
}

namespace ext::ffxiv::engine::gloss
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
ToneDiagnostics CurrentToneDiagnostics() { return ToneDiagnostics {}; }
FrameObservation CurrentFrameObservation() { return {}; }
ToneFrameObservation CurrentToneFrameObservation() { return {}; }
}

namespace ext::present { uint64_t FrameCount() noexcept { return 1; } }

// A counting allocator: the claim "a frame that is not the capture frame allocates nothing" is measured on this test's own heap.
namespace
{
std::atomic<size_t> g_allocations { 0 };
}
void* operator new(size_t bytes)
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using namespace ext::ffxiv::diag;
using Microsoft::WRL::ComPtr;

namespace
{
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

std::string Slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Picture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t width = 0, height = 0, bytesPerPixel = 0;
    std::vector<unsigned char> bytes; // the texture's whole content, row-major and packed
};

// A texture whose every byte tells its picture (`seed`), its column and its row.
Picture MakePicture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t bytesPerPixel, unsigned seed)
{
    Picture p;
    p.width = width;
    p.height = height;
    p.bytesPerPixel = bytesPerPixel;
    p.bytes.resize(size_t(width) * height * bytesPerPixel);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (uint32_t b = 0; b < bytesPerPixel; ++b)
                p.bytes[(size_t(y) * width + x) * bytesPerPixel + b] = static_cast<unsigned char>((seed * 41u + x * 7u + y * 13u + b * 3u) & 0xFFu);
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial { p.bytes.data(), width * bytesPerPixel, 0 };
    if (FAILED(device->CreateTexture2D(&desc, &initial, &p.texture)))
        return p;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
    viewDesc.Format = format;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(p.texture.Get(), &viewDesc, &p.view);
    return p;
}

// The crop of a picture the file must hold, packed.
std::vector<unsigned char> CropOf(const Picture& p, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
    std::vector<unsigned char> out;
    for (uint32_t y = top; y < top + height; ++y)
        out.insert(out.end(), p.bytes.begin() + (size_t(y) * p.width + left) * p.bytesPerPixel,
                   p.bytes.begin() + (size_t(y) * p.width + left + width) * p.bytesPerPixel);
    return out;
}

// A capture in the state the draw path leaves it in at the stages: the dump armed, the window opened by a tone map, nothing identified yet.
void ArmCaptureFrame(State& s, uint32_t capture)
{
    s.phase.store(kCapturing, std::memory_order_relaxed);
    s.dumpActive = true;
    s.dumpWritten = false;
    s.dumpItems.clear();
    s.dumpHeldBytes = 0;
    s.dumpBudget = kMaxDumpBytes;
    s.split = FrameSplitter {};
    s.split.fresh = false;
    s.dumpToneFrame = 0;
    s.dumpToneAtPresent = false;
    s.frames = 0;
    s.drawsInFrame = 17;
    s.drawSequence = 900;
    s.runDir.clear();
    s.captureArm.captures = capture; // a folder of its own for every case
    ResetStageDump(s);
    RefreshStageOpen(s);
}

// The tone map of the capture frame has been seen (the draw diagnostic sets this at the tone-map draw, then closes the frame): the chain runs.
void OpenChain(State& s)
{
    s.dumpToneFrame = 1;
    s.dumpToneSequence = s.drawSequence;
    s.frames = 1;
    s.dumpToneAtPresent = false;
    RefreshStageOpen(s);
}

// The reference count of a texture, read without changing it.
ULONG RefsOf(ID3D11Texture2D* texture)
{
    texture->AddRef();
    return texture->Release();
}

std::string FactsOf(State& s)
{
    return Slurp(RunFolder() / kDumpFolder / kDumpFactsFile);
}

bool Armed() { return true; }
bool NotArmed() { return false; }
std::string DofValues() { return "strength=0.50 taps=12"; }
// How a probed one-texel state reads as words (the depth-of-field stage's own formatter is in DofDraw.cpp; this one has the same shape).
std::string ProbeText(const float* values, size_t count)
{
    char text[96];
    std::snprintf(text, sizeof text, "focusM=%.2f centreM=%.2f n=%zu", double(values[0]), double(values[1]), count);
    return text;
}
} // namespace

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    if (device == nullptr || context == nullptr)
        return Finish("stage_dump_warp");
    State& s = S();

    // ---- the stage hooks are installed by the runtime, and a stage that registered is asked at the capture frame ------------------------------
    CHECK(detail::g_stageImage != nullptr && detail::g_stageStatus != nullptr);

    // ---- a frame that is not the capture frame writes nothing and allocates nothing ------------------------------------------------------------
    {
        s.phase.store(kIdle, std::memory_order_relaxed);
        s.dumpActive = false;
        s.dumpWritten = false;
        s.dumpItems.clear();
        s.dumpHeldBytes = 0;
        ResetStageDump(s);
        RefreshStageOpen(s);
        CHECK_EQ(detail::g_stageOpen.load(), 0u);
        const Picture a = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);
        const Picture b = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 2);
        CHECK(a.view != nullptr && b.view != nullptr);
        const size_t before = g_allocations.load();
        const StageWindow window { 2, 1, 4, 3 };
        for (int frame = 0; frame < 100; ++frame)
        {
            StageRan(StageId::Shafts, context.Get(), a.view.Get(), b.view.Get(), window);
            StageImage(StageId::Gi, StagePort::Out, context.Get(), b.view.Get(), window);
            StageReport(StageId::Dof, StageOutcome::Declined, "scene format not served");
        }
        CHECK_EQ(g_allocations.load() - before, size_t(0)); // not one allocation: a relaxed load and a branch per call
        CHECK(s.dumpItems.empty() && s.dumpHeldBytes == 0);
        for (size_t i = 0; i < kStageCount; ++i)
            CHECK(s.stageRecords[i].outcome == StageOutcome::None);

        // The same while a capture runs but the window is not open yet (the splitter still waits for the first tone map): the partial frame
        // the capture started in is not the capture frame.
        s.phase.store(kCapturing, std::memory_order_relaxed);
        s.dumpActive = true;
        s.split = FrameSplitter {};
        RefreshStageOpen(s);
        CHECK_EQ(detail::g_stageOpen.load(), 0u);
        const size_t before2 = g_allocations.load();
        StageRan(StageId::Shafts, context.Get(), a.view.Get(), b.view.Get(), window);
        CHECK_EQ(g_allocations.load() - before2, size_t(0));
        CHECK(s.dumpItems.empty());
    }

    // ---- the open mask follows the capture frame through the runtime ---------------------------------------------------------------------------
    {
        ArmCaptureFrame(s, 2);
        CHECK_EQ(detail::g_stageOpen.load(), kStagePreBit);
        CHECK(StageOpen(StageId::Ao) && !StageOpen(StageId::Gi)); // the pre-light stages report, the chain does not yet
        OpenChain(s);
        CHECK_EQ(detail::g_stageOpen.load(), kStageChainBit);
        CHECK(!StageOpen(StageId::Ao) && StageOpen(StageId::Gi)); // the chain reports, the next frame's pre-light stages do not
        s.frames = 2; // a later frame was closed: the capture frame is over
        RefreshStageOpen(s);
        CHECK_EQ(detail::g_stageOpen.load(), 0u);
        s.frames = 1;
        s.dumpToneAtPresent = false;
        RefreshStageOpen(s);
        CHECK_EQ(detail::g_stageOpen.load(), kStageChainBit);
        s.dumpToneAtPresent = true; // the bridge Present closed the frame
        RefreshStageOpen(s);
        CHECK_EQ(detail::g_stageOpen.load(), 0u);
    }

    // ---- the capture frame: a synthetic two-stage chain, the chain input and both outputs ---------------------------------------------------------
    {
        // The stages' armed probes are registered by the stages themselves at start-up; the capture reads them where its window opens.
        StageRegister(StageId::Godrays, &NotArmed, nullptr);
        StageRegister(StageId::Gi, &Armed, nullptr);
        StageRegister(StageId::Dof, &Armed, &DofValues);
        StageRegister(StageId::Ao, &Armed, nullptr);
        StageRegister(StageId::ContactShadows, &NotArmed, nullptr);
        ArmCaptureFrame(s, 3);
        // A pre-light stage reports at its own draw, before the tone map: the shadow-mapping lift rode on the contact pass's combine.
        StageReport(StageId::ShadowLift, StageOutcome::Combined, "lift 0.30 ridden on the contact-shadow mask copy");
        StageReport(StageId::Gi, StageOutcome::Declined, "reported too early"); // the chain is not open yet: ignored
        OpenChain(s);

        // Three pictures of one size, FP16 RGBA like the scene: the game's scene, the first stage's output, the second stage's output.
        const Picture scene = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 1);
        const Picture shafts = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 2);
        const Picture tone = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 3);
        CHECK(scene.view != nullptr && shafts.view != nullptr && tone.view != nullptr);
        const StageWindow window { 2.5f, 1.0f, 4, 3 }; // a half-pixel origin: floors to 2, so the crop holds columns 2..5 of rows 1..3
        const ULONG refsBefore[3] { RefsOf(scene.texture.Get()), RefsOf(shafts.texture.Get()), RefsOf(tone.texture.Get()) };

        // The stages run in chain order; the copies are made after each stage returned (here: the call right after the "pass").
        StageRan(StageId::Shafts, context.Get(), scene.view.Get(), shafts.view.Get(), window);
        StageRan(StageId::PuddleTone, context.Get(), shafts.view.Get(), tone.view.Get(), window); // its input is the first stage's output
        // The first stage answered again later in the frame (a replay of the tone-map draw): the first run's pictures stand, the count moves.
        StageRan(StageId::Shafts, context.Get(), scene.view.Get(), tone.view.Get(), window);
        // A later decline never overwrites "ran".
        StageReport(StageId::Shafts, StageOutcome::Declined, "already answered this frame");
        StageReport(StageId::Dof, StageOutcome::Declined, "scene format not served");
        StageReport(StageId::PuddleTone, StageOutcome::Held);
        StageReport(StageId::Flare, StageOutcome::Ran);

        CHECK_EQ(s.dumpItems.size(), size_t(3)); // chain input, stage 1 out, stage 2 out (stage 2's input is stage 1's output: a reference)
        CHECK_EQ(StageImagesHeld(s), size_t(3));  // all three are marked as stage images: they count against the stage places, not the dump's 24
        for (const State::DumpItem& item : s.dumpItems)
            CHECK(item.stage && item.rank == uint8_t(DumpRank::Stage));
        s.frames = 1;
        s.dumpToneAtPresent = true;
        DumpWrite(context.Get(), s);
        CHECK(s.dumpWritten);
        CHECK_EQ(detail::g_stageOpen.load(), 0u); // the dump is written: the mask is closed again
        // The dump holds no reference on the game's textures once it is written (a held target would outlive a resize): the stage sources are
        // gone and every texture is back at the count it had before the stages reported.
        CHECK(s.stageSources.empty());
        CHECK_EQ(RefsOf(scene.texture.Get()), refsBefore[0]);
        CHECK_EQ(RefsOf(shafts.texture.Get()), refsBefore[1]);
        CHECK_EQ(RefsOf(tone.texture.Get()), refsBefore[2]);

        const std::filesystem::path dir = RunFolder() / kDumpFolder;
        const std::string facts = FactsOf(s);
        const auto fileOf = [&dir](const char* name) {
            const std::string text = Slurp(dir / name);
            return std::vector<unsigned char>(text.begin(), text.end());
        };
        const std::vector<unsigned char> in1 = fileOf("stage-06-shafts-in.bin");
        const std::vector<unsigned char> out1 = fileOf("stage-06-shafts-out.bin");
        const std::vector<unsigned char> out2 = fileOf("stage-07-puddle-tone-out.bin");
        CHECK(in1 == CropOf(scene, 2, 1, 4, 3));  // the chain's input: the scene, cropped to the window
        CHECK(out1 == CropOf(shafts, 2, 1, 4, 3)); // the first stage's own output
        CHECK(out2 == CropOf(tone, 2, 1, 4, 3));  // the second stage's own output
        CHECK_EQ(in1.size(), size_t(4 * 3 * 8));
        CHECK(in1 != out1 && out1 != out2 && in1 != out2); // an out dumped before the stage ran would equal its input
        CHECK(!std::filesystem::exists(dir / "stage-07-puddle-tone-in.bin")); // the second input is the first output again: one file
        CHECK(Has(facts, "stage-06-shafts-in.bin,R16G16B16A16_FLOAT,4x3,96,"));
        CHECK(Has(facts, "stage-06-shafts-out.bin,R16G16B16A16_FLOAT,4x3,96,"));
        CHECK(Has(facts, "# stage-image stage-07-puddle-tone-in same_as=stage-06-shafts-out"));
        CHECK(Has(facts, "# stage-image stage-06-shafts-out stage=shafts port=out view_format=R16G16B16A16_FLOAT window=2.5,1,4,3 box=2,1,6,4 texture=8x6"));

        // Every stage has its line.
        CHECK(Has(facts, "# stage 06 shafts: ran (2 times) | frame position 17 | images in+out"));
        CHECK(Has(facts, "# stage 07 puddle-tone: ran | frame position 17 | images in+out")); // held was reported too: ran is stronger
        CHECK(Has(facts, "# stage 10 dof: declined: scene format not served | frame position 17 | images none | values strength=0.50 taps=12"));
        CHECK(Has(facts, "# stage 11 flare: ran | frame position 17 | images none (this stage makes no image)"));
        CHECK(Has(facts, "# stage 02 shadow-lift: combined: lift 0.30 ridden on the contact-shadow mask copy"));
        CHECK(Has(facts, "# stage 09 gi: not reached this frame")); // armed, nothing reached it
        CHECK(Has(facts, "# stage 08 godrays: not armed"));          // its probe says off
        CHECK(Has(facts, "# stage 01 contact-shadows: not armed"));
        CHECK(Has(facts, "# stage 03 ao: not reached this frame"));  // armed, but the chain frame has no occlusion reader
        CHECK(Has(facts, "# stage 04 gloss: not armed"));            // no probe registered: not armed
        CHECK(Has(facts, "# stage 05 ssr: not armed"));
        CHECK_EQ(Count(facts, "\n# stage "), size_t(11)); // one line each, the stage that did not run included

        // The existing CSV lines are unchanged: seven columns, name.bin first.
        CHECK(Has(facts, "# file,format,size,bytes,row_pitch,draw_ordinal,pointer"));
        CHECK(Has(facts, "# the stage images:")); // the legend
    }

    // ---- the budget: an image that does not fit is skipped with a line, one that fits is written -----------------------------------------------
    {
        ArmCaptureFrame(s, 4);
        OpenChain(s);
        s.dumpBudget = 4 * 3 * 4 * 2 + 10; // two 4x3 RGBA8 crops fit, a third does not
        const Picture a = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);
        const Picture b = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 2);
        const Picture c = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 3);
        const StageWindow window { 2, 1, 4, 3 };
        StageRan(StageId::Gi, context.Get(), a.view.Get(), b.view.Get(), window);      // fits (in + out = 96 B)
        StageRan(StageId::Dof, context.Get(), b.view.Get(), c.view.Get(), window);     // its input is Gi's output; its output does not fit
        CHECK_EQ(s.dumpHeldBytes, uint64_t(4 * 3 * 4 * 2));
        s.frames = 1;
        s.dumpToneAtPresent = true;
        DumpWrite(context.Get(), s);
        const std::filesystem::path dir = RunFolder() / kDumpFolder;
        const std::string facts = FactsOf(s);
        CHECK(std::filesystem::exists(dir / "stage-09-gi-in.bin") && std::filesystem::exists(dir / "stage-09-gi-out.bin"));
        CHECK(!std::filesystem::exists(dir / "stage-10-dof-out.bin"));
        CHECK(Has(facts, "# skipped stage-10-dof-out.bin: over the dump's bound"));
        CHECK(Has(facts, "# stage 10 dof: ran")); // the run is still reported
        CHECK(Has(facts, "images in")); // ... with the images it did get (the input was a reference)
    }

    // ---- a stage image displaces the atlas (a lower rank), never an engine target ----------------------------------------------------------------
    {
        ArmCaptureFrame(s, 5);
        OpenChain(s);
        State::DumpItem atlas;
        atlas.name = "atlas";
        atlas.rank = uint8_t(DumpRank::Atlas);
        atlas.bytes = 64;
        atlas.copied = true;
        s.dumpItems.push_back(atlas);
        State::DumpItem tone;
        tone.name = "tonemap";
        tone.rank = uint8_t(DumpRank::Rest);
        tone.bytes = 64;
        tone.copied = true;
        s.dumpItems.push_back(tone);
        s.dumpHeldBytes = 128;
        s.dumpBudget = 128 + 48; // room for 48 more: a 4x3 RGBA8 crop is 48 B
        const Picture a = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);
        const Picture b = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 2);
        const StageWindow window { 2, 1, 4, 3 };
        StageImage(StageId::Gi, StagePort::In, context.Get(), a.view.Get(), window);   // fits in the room that is left
        StageImage(StageId::Gi, StagePort::Out, context.Get(), b.view.Get(), window);  // needs the atlas's place
        bool atlasDisplaced = false, toneKept = true, outHeld = false;
        for (const State::DumpItem& item : s.dumpItems)
        {
            if (item.name == "atlas")
                atlasDisplaced = Has(item.note, "displaced by");
            if (item.name == "tonemap")
                toneKept = item.note.empty();
            if (item.name == "stage-09-gi-out")
                outHeld = item.note.empty() && item.copied;
        }
        CHECK(atlasDisplaced);
        CHECK(toneKept);
        CHECK(outHeld);
    }

    // ---- the pre-light stages: a cropped mask pair, a typeless texture and its typed view named in the note ------------------------------------------
    {
        ArmCaptureFrame(s, 6);
        const Picture mask = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8_UNORM, 1, 5);
        const Picture copy = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8_UNORM, 1, 6);
        const StageWindow window { 0, 0, 0, 0 }; // a stage without a window: the whole texture
        StageImage(StageId::ContactShadows, StagePort::In, context.Get(), mask.view.Get(), window);
        StageImage(StageId::ContactShadows, StagePort::Out, context.Get(), copy.view.Get(), window);
        StageReport(StageId::ContactShadows, StageOutcome::Ran);
        // The pre-light group is open before the tone map; the chain's stages are not.
        StageImage(StageId::Gi, StagePort::Out, context.Get(), copy.view.Get(), window);
        CHECK_EQ(s.dumpItems.size(), size_t(2));
        OpenChain(s);
        s.frames = 1;
        s.dumpToneAtPresent = true;
        DumpWrite(context.Get(), s);
        const std::filesystem::path dir = RunFolder() / kDumpFolder;
        const std::string facts = FactsOf(s);
        const std::string whole = Slurp(dir / "stage-01-contact-shadows-out.bin");
        CHECK(std::vector<unsigned char>(whole.begin(), whole.end()) == copy.bytes);
        CHECK(Has(facts, "stage-01-contact-shadows-in.bin,R8_UNORM,8x6,48,"));
        CHECK(Has(facts, "window_unknown=1"));
        CHECK(Has(facts, "# stage 01 contact-shadows: ran | frame position 17 | images in+out"));
    }

    // ---- the 2026-11-06 failure: stage images beside the engine textures must not use up the engine places; depth.bin is cropped; a full set says so -----------
    {
        ArmCaptureFrame(s, 8);
        std::vector<Picture> engine;
        const auto identify = [&](const std::string& name, Picture& picture, DumpRank rank, uint64_t sequence) {
            DumpIdentify(s, name, picture.texture.Get(), ReadTextureInfo(picture.texture.Get()), 7, sequence, rank);
        };
        engine.reserve(40);
        for (int i = 0; i < 18; ++i) // the engine textures identified before the fog post
        {
            engine.push_back(MakePicture(device.Get(), 4, 3, DXGI_FORMAT_R8_UNORM, 1, unsigned(10 + i)));
            identify("e" + std::to_string(i), engine.back(), DumpRank::Light, 901);
        }
        // The pre-light stages' images: ten files, pushed before the fog post and the tone map (the capture-1 shape: they used up the places).
        const StageId pre[] { StageId::ContactShadows, StageId::ShadowLift, StageId::Ao, StageId::Gloss, StageId::Ssr };
        std::vector<Picture> stageImages;
        stageImages.reserve(10);
        for (const StageId id : pre)
            for (const StagePort port : { StagePort::In, StagePort::Out })
            {
                stageImages.push_back(MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8_UNORM, 1, unsigned(60 + stageImages.size())));
                StageImage(id, port, context.Get(), stageImages.back().view.Get(), StageWindow { 0, 0, 0, 0 });
            }
        CHECK_EQ(StageImagesHeld(s), size_t(10));
        CHECK_EQ(DumpCount(s), size_t(18)); // the engine places in use: the stage images are not counted
        // The fog post's three files, the AO readers' own depth, then the scene and the tone map: all of them find a place (18 + 4 + 1 + 1 = 24 engine items, with ten
        // stage images beside).
        Picture sky = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 31);
        Picture lut = MakePicture(device.Get(), 4, 1, DXGI_FORMAT_R16G16_FLOAT, 4, 32);
        Picture depth = MakePicture(device.Get(), 16, 12, DXGI_FORMAT_R24G8_TYPELESS, 4, 33); // the engine's depth: a whole texture, 16 x 12
        Picture scene = MakePicture(device.Get(), 16, 12, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 34);
        Picture toneMap = MakePicture(device.Get(), 16, 12, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 35);
        CHECK(depth.texture != nullptr);
        identify("sky", sky, FogDumpRankOf(kFogSkyDumpFile), 902);
        identify("fog_lut", lut, FogDumpRankOf(kFogLutDumpFile), 902);
        identify("depth", depth, FogDumpRankOf(kDepthDumpFile), 902);
        s.dumpDrawWindow[0] = 4.5f; // the fog post's viewport: a half-pixel origin floors to 4, 2
        s.dumpDrawWindow[1] = 2.5f;
        s.dumpDrawWindow[2] = 8.0f;
        s.dumpDrawWindow[3] = 6.0f;
        DumpMarkDepth(s, kDepthDumpFile, depth.texture.Get(), 46); // R24_UNORM_X8_TYPELESS, the fog post's view of it
        Picture aoDepth = MakePicture(device.Get(), 16, 12, DXGI_FORMAT_R24G8_TYPELESS, 4, 36); // the AO readers' own copy: another texture, the same window
        identify("aodepth", aoDepth, FogDumpRankOf(kAoDepthDumpFile), 902);
        DumpMarkDepth(s, kAoDepthDumpFile, aoDepth.texture.Get(), 46);
        identify("scene", scene, DumpRank::Scene, 903);
        identify("tonemap", toneMap, DumpRank::Rest, 903);
        CHECK_EQ(DumpCount(s), size_t(24));
        Picture extra = MakePicture(device.Get(), 4, 3, DXGI_FORMAT_R8_UNORM, 1, 40);
        identify("copyps", extra, DumpRank::Rest, 904); // the 25th engine item: the places are full
        CHECK_EQ(DumpCount(s), size_t(24));             // it took no place ...
        bool skippedHeld = false;
        for (const State::DumpItem& item : s.dumpItems)
            skippedHeld = skippedHeld || (item.name == "copyps" && item.note == kDumpNoPlaceReason);
        CHECK(skippedHeld);                             // ... and it is held as a skipped file with its reason, once
        identify("copyps", extra, DumpRank::Rest, 905); // identified again by a later draw: still one line
        size_t copypsItems = 0;
        for (const State::DumpItem& item : s.dumpItems)
            copypsItems += item.name == "copyps" ? 1 : 0;
        CHECK_EQ(copypsItems, size_t(1));
        DumpCopyPending(context.Get(), s, 950);
        OpenChain(s);
        s.frames = 1;
        s.dumpToneAtPresent = true;
        DumpWrite(context.Get(), s);
        CHECK(s.dumpWritten);
        const std::filesystem::path dir = RunFolder() / kDumpFolder;
        const std::string facts = FactsOf(s);
        for (const char* file : { "sky.bin", "fog_lut.bin", "depth.bin", "aodepth.bin", "scene.bin", "tonemap.bin", "e0.bin", "e17.bin" })
            CHECK(std::filesystem::exists(dir / file));
        CHECK(!std::filesystem::exists(dir / "copyps.bin"));
        CHECK(Has(facts, "# skipped copyps.bin: over the dump's places"));
        CHECK_EQ(Count(facts, "over the dump's places"), size_t(1));
        size_t stageFiles = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            stageFiles += entry.path().filename().string().rfind("stage-", 0) == 0 ? 1 : 0;
        CHECK_EQ(stageFiles, size_t(10));
        // The depth: the draw's window of it, packed, with the facts line of the cropped size and its note.
        const std::string depthFile = Slurp(dir / "depth.bin");
        CHECK(std::vector<unsigned char>(depthFile.begin(), depthFile.end()) == CropOf(depth, 4, 2, 8, 6));
        CHECK_EQ(depthFile.size(), size_t(8 * 6 * 4));
        CHECK(Has(facts, "depth.bin,R24G8_TYPELESS,8x6,192,"));
        CHECK(Has(facts, "# depth-image depth view_format=R24_UNORM_X8_TYPELESS window=4.5,2.5,8,6 box=4,2,12,8 texture=16x12"));
        const std::string aoDepthFile = Slurp(dir / "aodepth.bin");
        CHECK(std::vector<unsigned char>(aoDepthFile.begin(), aoDepthFile.end()) == CropOf(aoDepth, 4, 2, 8, 6)); // the same crop, from its own texture
        CHECK(Has(facts, "aodepth.bin,R24G8_TYPELESS,8x6,192,"));
        CHECK(Has(facts, "# depth-image aodepth view_format=R24_UNORM_X8_TYPELESS window=4.5,2.5,8,6 box=4,2,12,8 texture=16x12"));
        const std::string skyFile = Slurp(dir / "sky.bin");
        CHECK(std::vector<unsigned char>(skyFile.begin(), skyFile.end()) == sky.bytes); // the other fog files are whole
    }

    // ---- the late numbers and the probe: appended to the stage's values text at the write, the format of the line unchanged ----------------------------------
    {
        ArmCaptureFrame(s, 9);
        OpenChain(s);
        const Picture a = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1);
        const Picture b = MakePicture(device.Get(), 8, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 2);
        const StageWindow window { 2, 1, 4, 3 };
        StageRan(StageId::Gi, context.Get(), a.view.Get(), b.view.Get(), window);
        StageFrameValues gi;
        gi.haveWhite = true;
        gi.white = 0.838f;
        gi.fade = 1.0f;
        StageNoteFrame(StageId::Gi, gi);
        CHECK(std::fabs(StageFrameWhite() - 0.838f) < 1e-6f); // a later stage of the chain can name it
        StageRan(StageId::Dof, context.Get(), b.view.Get(), a.view.Get(), window);
        // The one-texel focus state the depth-of-field stage keeps on the GPU: R = the smoothed focus, G = the raw sample.
        const float focusState[2] { 8.43f, 8.40f };
        D3D11_TEXTURE2D_DESC focusDesc {};
        focusDesc.Width = 1;
        focusDesc.Height = 1;
        focusDesc.MipLevels = 1;
        focusDesc.ArraySize = 1;
        focusDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        focusDesc.SampleDesc.Count = 1;
        focusDesc.Usage = D3D11_USAGE_DEFAULT;
        focusDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA focusInitial { focusState, sizeof focusState, 0 };
        ComPtr<ID3D11Texture2D> focusTexture;
        ComPtr<ID3D11ShaderResourceView> focusView;
        CHECK(SUCCEEDED(device->CreateTexture2D(&focusDesc, &focusInitial, &focusTexture)));
        CHECK(SUCCEEDED(device->CreateShaderResourceView(focusTexture.Get(), nullptr, &focusView)));
        StageProbe(StageId::Dof, context.Get(), focusView.Get(), &ProbeText);
        StageProbe(StageId::Dof, context.Get(), focusView.Get(), &ProbeText); // the first run of the capture frame is the one that is shown
        StageNoteText(StageId::Dof, "white=0.838 fade=1.00");
        CHECK_EQ(s.stageProbes.size(), size_t(1));
        s.frames = 1;
        s.dumpToneAtPresent = true;
        DumpWrite(context.Get(), s);
        CHECK(s.stageProbes.empty()); // the staging copy is mapped and released at the write
        const std::string facts = FactsOf(s);
        CHECK(Has(facts, "# stage 09 gi: ran | frame position 17 | images in+out | values white=0.838 fade=1.00\n"));
        // The text a stage noted at its run comes first, the probed state's words (mapped at the write) after it.
        CHECK(Has(facts, "# stage 10 dof: ran | frame position 17 | images in+out | values strength=0.50 taps=12 white=0.838 fade=1.00 focusM=8.43 centreM=8.40 n=2\n"));
        CHECK_EQ(Count(facts, "\n# stage "), size_t(11)); // still one line per stage

        // A frame that is not the capture frame: the hooks cost one test and allocate nothing, and store nothing.
        const size_t before = g_allocations.load();
        StageNoteFrame(StageId::Gi, gi);
        StageProbe(StageId::Dof, context.Get(), focusView.Get(), &ProbeText);
        CHECK_EQ(g_allocations.load(), before);
        CHECK(s.stageProbes.empty());
    }

    // ---- the next capture starts clean --------------------------------------------------------------------------------------------------------------
    {
        ArmCaptureFrame(s, 7);
        for (size_t i = 0; i < kStageCount; ++i)
            CHECK(s.stageRecords[i].outcome == StageOutcome::None && !s.stageRecords[i].images[0] && !s.stageRecords[i].images[1]);
        CHECK(s.stageSources.empty() && s.stageNotes.empty());
    }

    return Finish("stage_dump_warp");
}
