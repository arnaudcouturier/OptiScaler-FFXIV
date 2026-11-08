// The draw diagnostic over a capture the game itself forces: its flare family (the halo ring, the sun sprite and the ghost discs) is created while
// the game loads, before a capture can collect anything, and the session's reflection table is bounded and fills up - so the values those passes are
// drawn with can reach cbdump.csv only if the shaders kept from creation carry their own slots. This test creates one such shader while the
// diagnostic waits for its capture with the session's table already full, then drives one draw on a D3D11 WARP device and reads what the capture
// collected: the two flare buffers sampled, and the draw row naming them.
//
// It also holds the two lines of engine.txt that must not depend on another card being on: the sun's projection, which the diagnostic computes
// from the light draw's own parameters (there is no light shafts module in this test at all), and the reflections' last water draw, which is this
// capture's only. The sun sample's age is part of that: a capture that arms inside the second a waiting read spent still reads its own frame.
#include "check.h"

#include "ext/ffxiv/diag/FfxivDrawDiag.cpp" // the runtime under test: the creation fixtures, the cbdump and engine.txt's shaping

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <filesystem>
#include <fstream>
#include <cstring>
#include <iterator>

// The runtime reads the engine pass module's counters, the reflections' and the puddles' two stages for its capture folder; this test has no
// modules, so every read answers from state the test sets (or "off").
namespace ext::ffxiv::engine
{
Diagnostics CurrentDiagnostics() { return Diagnostics {}; }
}

namespace ext::ffxiv::engine::ssr
{
Diagnostics g_ssrDiagnostics; // what the reflection block of engine.txt is shaped from
Diagnostics CurrentDiagnostics() { return g_ssrDiagnostics; }
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

using namespace ext::ffxiv::diag;
using Microsoft::WRL::ComPtr;

namespace
{
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// A pixel shader whose reflection declares the halo ring's and the sun sprite's parameter buffers, the pair the flare family is drawn with. Its
// two buffers are bound at their real sizes (cHaloParam 64 B, cSunParam 128 B as the engine binds them).
constexpr const char* kFlareFixture = R"(
Texture2D<float4> g_SamplerReflectionMapTexture : register(t5);
SamplerState g_SamplerReflectionMapState : register(s2);
cbuffer cHaloParam : register(b1) { float4 haloParam[4]; };
cbuffer cSunParam : register(b2) { float4 sunParam[8]; };

float4 PS(float4 position : SV_Position) : SV_Target0
{
    const float4 sampled = g_SamplerReflectionMapTexture.SampleLevel(g_SamplerReflectionMapState, position.xy * 0.001, 0);
    return sampled + haloParam[0] + sunParam[0];
}
)";

// A pixel shader that declares the view-position buffer the reflections' passes sample. The dump identifies that buffer's file by the declared
// name, so the reflection here is the whole point of the fixture.
constexpr const char* kViewPositionFixture = R"(
Texture2D<float4> g_SamplerViewPosition : register(t3);
SamplerState g_SamplerViewPositionSampler : register(s1);

float4 PS(float4 position : SV_Position) : SV_Target0
{
    return g_SamplerViewPosition.SampleLevel(g_SamplerViewPositionSampler, position.xy * 0.001, 0);
}
)";

std::string Compile(const char* source)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "draw_diag_capture_fixture", nullptr, nullptr, "PS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || code == nullptr)
    {
        if (errors)
            std::printf("    compile failed: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    return std::string(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
}
} // namespace

int main()
{
    // ---- a shader created while the capture waits, with the session's reflection table full
    //
    // That is the state of a real session: the game creates its shaders while it loads, and the table of the shaders the diagnostic reflects is
    // bounded, so the passes created first are exactly the ones it misses - the flare family among them. The creation fixtures already keep their
    // bytecode; a capture needs their slots too, or the rows name nothing and cbdump.csv samples nothing.
    {
        ext::Cfg().ffxiv.drawDiagnosticsDelaySeconds.value = 60; // the capture waits, so the shader is created before it can collect
        ext::Cfg().ffxiv.drawDiagnosticsFrames.value = 1;
        OnConfigLoaded(true);
        CHECK_EQ(S().phase.load(std::memory_order_relaxed), uint8_t(kWaiting));

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                                  &context);
        CHECK(SUCCEEDED(created));
        CHECK(device != nullptr && context != nullptr);

        const std::string blob = Compile(kFlareFixture);
        CHECK(!blob.empty());
        ComPtr<ID3D11PixelShader> shader;
        if (blob.empty() || device == nullptr)
            return Finish("draw_diag_capture");
        CHECK(SUCCEEDED(device->CreatePixelShader(blob.data(), blob.size(), nullptr, &shader)));

        // The session's table is at its bound: nothing more is reflected, which is what a long session looks like when the game creates tens of
        // thousands of pixel shaders. The creation fixtures are outside that bound on purpose.
        {
            std::lock_guard lock(S().mutex);
            S().bindings = kMaxBindingsTotal;
        }
        const uint32_t crc = 0x1234ABCDu;
        OnPixelShaderCreated(shader.Get(), blob.data(), blob.size(), crc);

        {
            std::lock_guard lock(S().mutex);
            CHECK_EQ(S().byCrc.count(crc), size_t(0));               // the session's table kept nothing: it was full
            CHECK_EQ(S().creationReflections.count(crc), size_t(1)); // the shader kept from creation carries its slots
        }

        // The capture arms now, at a draw, with nothing else changed: this is the state after the game has finished loading.
        StartCollecting();
        CHECK_EQ(S().phase.load(std::memory_order_relaxed), uint8_t(kCapturing));

        // The draw binds the two flare buffers at their real registers and sizes.
        context->PSSetShader(shader.Get(), nullptr, 0);
        const unsigned char haloBytes[64] {};
        const unsigned char sunBytes[128] {};
        D3D11_BUFFER_DESC desc {};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.ByteWidth = sizeof haloBytes;
        D3D11_SUBRESOURCE_DATA data { haloBytes, 0, 0 };
        ComPtr<ID3D11Buffer> haloBuffer;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc, &data, &haloBuffer)));
        desc.ByteWidth = sizeof sunBytes;
        data.pSysMem = sunBytes;
        ComPtr<ID3D11Buffer> sunBuffer;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc, &data, &sunBuffer)));
        ID3D11Buffer* bound[2] { haloBuffer.Get(), sunBuffer.Get() };
        context->PSSetConstantBuffers(1, 2, bound);
        D3D11_VIEWPORT viewport { 0.5f, 0.5f, 1920.0f, 1080.0f, 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);

        OnDraw(context.Get(), "Draw");

        // What the capture collected: the draw row names both buffers instead of `-`, and the byte dump samples them.
        CHECK_EQ(S().drawsInFrame, uint32_t(1));
        CHECK(Has(S().frameRows, "cHaloParam"));
        CHECK(Has(S().frameRows, "cSunParam"));
        CHECK_EQ(S().shadersWithoutReflection, uint64_t(0)); // the row was named, so the draw is described
        CHECK(Has(S().cbdump, "cHaloParam"));
        CHECK(Has(S().cbdump, "cSunParam"));
        CHECK_EQ(S().cbDumpRows, uint64_t(2));

        // A second draw in the same second is not sampled again: the dump costs one copy and one map a second per name. The clock is set here
        // rather than left to the wall clock, so the check cannot straddle a second boundary.
        S().cbDumpLast[CbDumpNameIndex("cHaloParam")] = Qpc();
        S().cbDumpLast[CbDumpNameIndex("cSunParam")] = Qpc();
        OnDraw(context.Get(), "Draw");
        CHECK_EQ(S().cbDumpRows, uint64_t(2));

        // A shader the session never saw is still not named: the creation table is an answer, not a guess.
        {
            std::lock_guard lock(S().mutex);
            CHECK(ReflectionFor(S(), 0x0BADF00Du) == nullptr);
            CHECK(ReflectionFor(S(), crc) != nullptr);
            const Reflection* reflection = ReflectionFor(S(), crc);
            CHECK(reflection != nullptr && NameAt(S(), reflection, BindKind::ConstantBuffer, 1) != nullptr);
            CHECK(reflection != nullptr && std::strcmp(NameAt(S(), reflection, BindKind::ConstantBuffer, 1), "cHaloParam") == 0);
            CHECK(reflection != nullptr && std::strcmp(NameAt(S(), reflection, BindKind::ConstantBuffer, 2), "cSunParam") == 0);
            // The kind is part of the question: the texture at t5 is a texture, never a constant buffer.
            CHECK(reflection != nullptr && NameAt(S(), reflection, BindKind::ConstantBuffer, 5) == nullptr);
            CHECK(reflection != nullptr && NameAt(S(), reflection, BindKind::Texture, 5) != nullptr);
        }
    }

    // ---- the one-frame dump's view-position file: the draw that binds a texture at the slot its shader names g_SamplerViewPosition has that
    // texture identified, copied at the next draw, and written to dump\vposition.bin
    //
    // The dump walks the draw's texture slots in one else-if chain, and the branch for the hierarchical-Z map is a loop of its own. The braces
    // around that loop are what keep the view-position branch attached to the chain: without them the branch belongs to the hierarchical-Z
    // condition instead, so the view-position buffer is identified only by a hierarchical-Z draw that renders into no target at all, and its
    // file is never written - which is the state a live capture was in.
    {
        State& s = S();
        // A capture in its identifying phase, with the window already opened by a tone map: the state a draw of the frame sees. The clock and the
        // frame counters are this block's own, so a row is collected and the dump's "a later draw" rule counts from here.
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
        s.byCrc.clear();
        s.bindings = 0;
        s.fixtures.clear();
        s.fixtureBytes = 0;
        s.commonFixtures = 0;
        s.lightFixtures = 0;
        s.frames = 0;
        s.drawsInFrame = 0;
        s.rowsWritten = 0;
        s.omittedInFrame = 0;
        s.drawSequence = 500;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
        CHECK(device != nullptr && context != nullptr);
        if (device == nullptr || context == nullptr)
            return Finish("draw_diag_capture");

        const std::string blob = Compile(kViewPositionFixture);
        CHECK(!blob.empty());
        ComPtr<ID3D11PixelShader> shader;
        if (blob.empty())
            return Finish("draw_diag_capture");
        CHECK(SUCCEEDED(device->CreatePixelShader(blob.data(), blob.size(), nullptr, &shader)));
        const uint32_t crc = 0x5EA17F00u;
        OnPixelShaderCreated(shader.Get(), blob.data(), blob.size(), crc);

        // The texture the shader's g_SamplerViewPosition names: every word a different value, so the file proves which resource it holds.
        constexpr uint32_t width = 4, height = 4;
        constexpr uint32_t bytesPerTexel = 16; // R32G32B32A32_FLOAT: four words
        std::vector<uint32_t> texels(size_t(width) * height * (bytesPerTexel / sizeof(uint32_t)));
        for (size_t i = 0; i < texels.size(); ++i)
            texels[i] = 0x3F800000u + uint32_t(i);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial { texels.data(), uint32_t(width * bytesPerTexel), 0 };
        ComPtr<ID3D11Texture2D> texture;
        CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)));
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &srvDesc, &view)));

        context->PSSetShader(shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[16] {};
        views[3] = view.Get(); // the register the fixture declares g_SamplerViewPosition at
        context->PSSetShaderResources(0, 16, views);
        D3D11_VIEWPORT viewport { 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);

        // The draw that binds it: identified here (the hook runs before the game's draw), copied at the next one.
        OnDraw(context.Get(), "Draw");
        const auto itemOf = [&s]() -> State::DumpItem* {
            for (State::DumpItem& item : s.dumpItems)
                if (item.name == "vposition")
                    return &item;
            return nullptr;
        };
        CHECK_EQ(s.drawsInFrame, uint32_t(1));
        CHECK(itemOf() != nullptr); // the fix: the view-position branch is reached at a draw that binds it
        if (itemOf() == nullptr)
        {
            std::printf("    the view-position binding was not identified; the dump holds %zu item(s)\n", s.dumpItems.size());
        }
        else
        {
            const State::DumpItem& item = *itemOf();
            CHECK(item.note.empty()); // identified and writable, not held as a skip
            CHECK(item.resource.Get() == texture.Get());
            CHECK_EQ(item.width, width);
            CHECK_EQ(item.height, height);
            CHECK_EQ(item.rank, uint8_t(DumpRank::ViewPosition));
        }

        OnDraw(context.Get(), "Draw"); // the later draw the copy waits for
        CHECK(itemOf() != nullptr && itemOf()->copied);

        // The frame's tone map was identified and the frame is over: the product's own gate is what writes the folder.
        s.frames = 1;
        s.dumpToneFrame = 1;
        s.dumpToneAtPresent = true;
        MaybeWriteDump(context.Get(), s);
        CHECK(s.dumpWritten);

        const std::filesystem::path dir = RunFolder() / kDumpFolder;
        const std::filesystem::path file = dir / "vposition.bin";
        CHECK(std::filesystem::exists(file));
        std::vector<unsigned char> written;
        if (std::ifstream in(file, std::ios::binary); in)
            written.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        CHECK_EQ(written.size(), size_t(width) * height * bytesPerTexel);
        const auto* source = reinterpret_cast<const unsigned char*>(texels.data());
        CHECK(written.size() == size_t(width) * height * bytesPerTexel &&
              std::memcmp(written.data(), source, written.size()) == 0); // the file holds the texture's own texels
        std::string facts;
        if (std::ifstream in(dir / kDumpFactsFile, std::ios::binary); in)
            facts.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        CHECK(Has(facts, "vposition.bin,R32G32B32A32_FLOAT,4x4"));
    }

    // ---- engine.txt's sun line: the light draw's own direction, projected here, with no light shafts module in this build
    //
    // The line is the answer to whether an F9 could have the engine's flare in frame, and the next capture runs with the light shafts card off.
    // So it is computed from the diagnostic's own light read - the unit direction g_LightParam carries through the same draw's camera - and it
    // must be there whether or not another module's card is on.
    {
        State::LightSample light;
        light.have = true;
        light.direction[0] = 0.6f;
        light.direction[1] = 0.8f;
        light.direction[2] = 0.0f;
        // The projection's row 0 scales x by 0.5 and row 1 scales y by 0.5; row 3 is the direction's own dot product, so clip w is 1 for this
        // direction.
        light.projection[0] = 0.5f;
        light.projection[5] = 0.5f;
        light.projection[12] = 0.6f;
        light.projection[13] = 0.8f;
        light.window[0] = 0.5f;
        light.window[1] = 0.5f;
        light.window[2] = 1920.0f;
        light.window[3] = 1080.0f;
        S().lastLight = light;
        const std::string text = EngineStatusText(EngineStatusNow());
        CHECK(Has(text, "module: off")); // the engine pass module is off in this test: the sun line does not depend on it
        CHECK(Has(text, "sun: 1248.5x324.5, in the window"));
        CHECK(!Has(text, "sun: -"));

        // A sun behind the camera has no screen position, and the line says so rather than printing one.
        S().lastLight.projection[12] = -0.6f;
        S().lastLight.projection[13] = -0.8f;
        CHECK(Has(EngineStatusText(EngineStatusNow()), "sun: behind the camera"));

        // A direction that is not a unit vector is a light this build cannot project: no line rather than a wrong one.
        S().lastLight.direction[0] = 0.3f;
        S().lastLight.direction[1] = 0.4f;
        CHECK(Has(EngineStatusText(EngineStatusNow()), "sun: -"));

        // Nothing read yet: the line is written, and empty, from the first capture on.
        S().lastLight = {};
        CHECK(Has(EngineStatusText(EngineStatusNow()), "sun: -"));
    }

    // ---- the line's read itself: the light draw's direction, its camera and the window it rendered
    //
    // The block above hands the line a sample, so it holds the projection but not the read that fills it: a read that kept no sample, or took no
    // window from the draw, would still print that line. This drives the read itself - a light buffer carrying a unit direction, a camera buffer
    // carrying the projection at +0x120, and a context whose viewport is the light draw's own window - and checks what the line makes of it.
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
        CHECK(device != nullptr && context != nullptr);
        if (device == nullptr || context == nullptr)
            return Finish("draw_diag_capture");

        const float direction[3] { 0.6f, 0.8f, 0.0f }; // a unit direction, as the light pass carries one
        unsigned char lightBytes[kLightParamBytes] {};
        std::memcpy(lightBytes + kLightParamDirectionAt, direction, sizeof direction);
        // The projection at +0x120: row 0 scales x by 0.5, row 1 scales y by 0.5, row 3 is the direction's own dot product, so clip w is 1 for this
        // direction. The view-projection at +0x060 is a different matrix here on purpose: the light's direction is in view space, so the position
        // must come from the projection, and a read that kept the wrong matrix would print another place (the identity would say "behind the camera").
        const float projection[16] { 0.5f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.5f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 1.0f, 0.0f,
                                     0.6f, 0.8f, 0.0f, 1.0f };
        const float viewProjection[16] { 1.0f, 0.0f, 0.0f, 0.0f,
                                         0.0f, 1.0f, 0.0f, 0.0f,
                                         0.0f, 0.0f, 1.0f, 0.0f,
                                         0.0f, 0.0f, 0.0f, 1.0f };
        unsigned char cameraBytes[kCameraParamProjectionAt + kCameraParamMatrixBytes] {};
        std::memcpy(cameraBytes + kCameraParamViewProjectionAt, viewProjection, sizeof viewProjection);
        std::memcpy(cameraBytes + kCameraParamProjectionAt, projection, sizeof projection);
        D3D11_BUFFER_DESC desc {};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.ByteWidth = sizeof lightBytes;
        const D3D11_SUBRESOURCE_DATA lightData { lightBytes, 0, 0 };
        ComPtr<ID3D11Buffer> lightBuffer;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc, &lightData, &lightBuffer)));
        desc.ByteWidth = sizeof cameraBytes;
        const D3D11_SUBRESOURCE_DATA cameraData { cameraBytes, 0, 0 };
        ComPtr<ID3D11Buffer> cameraBuffer;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc, &cameraData, &cameraBuffer)));

        // The window the light draw rendered: the sun's position is measured inside it, so its origin is part of the answer.
        const D3D11_VIEWPORT window { 64.0f, 32.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
        context->RSSetViewports(1, &window);
        S().lastLight = {};
        ReadConstants(context.Get(), 1, lightBuffer.Get(), 1, cameraBuffer.Get(), 0, false);

        // clip = (0.3, 0.4, 0, 1): x = 64 + (0.15 + 0.5) * 1920 = 1312, y = 32 + (0.5 - 0.2) * 1080 = 356.
        const EngineStatus read = EngineStatusNow();
        CHECK(read.haveSun);
        CHECK(!read.sunBehind);
        CHECK(read.sunX > 1311.9f && read.sunX < 1312.1f);
        CHECK(read.sunY > 355.9f && read.sunY < 356.1f);
        CHECK(Has(EngineStatusText(read), "sun: 1312x356, in the window"));

        // A second read answers with its own draw's window, not the first one's: x = 0.65 * 960 = 624, y = 0.3 * 540 = 162.
        const D3D11_VIEWPORT otherWindow { 0.0f, 0.0f, 960.0f, 540.0f, 0.0f, 1.0f };
        context->RSSetViewports(1, &otherWindow);
        ReadConstants(context.Get(), 2, lightBuffer.Get(), 1, cameraBuffer.Get(), 0, false);
        const EngineStatus reread = EngineStatusNow();
        CHECK(reread.haveSun);
        CHECK(reread.sunX > 623.9f && reread.sunX < 624.1f);
        CHECK(reread.sunY > 161.9f && reread.sunY < 162.1f);

        // ---- a capture that arms right after a waiting read reads its own frame
        //
        // The waiting diagnostic reads the light pass once a second, so a key capture can arm inside the second that read spent. The clock is
        // cleared at arm for exactly that case: without it the capture's first light pass is skipped and the sun line keeps the waiting frame's
        // camera pose - the frame's own direction through an earlier camera, a screen position this capture's own frame may not have.
        context->RSSetViewports(1, &window); // the waiting draw's window, the first pose
        NoteSampled(S());                     // the waiting read spent this second
        CHECK(S().lastReadQpc != 0);
        ReadConstants(context.Get(), 0, lightBuffer.Get(), 1, cameraBuffer.Get(), 0, false);
        CHECK(Has(EngineStatusText(EngineStatusNow()), "sun: 1312x356, in the window")); // what the line holds before the capture arms
        CHECK(!SampleDue(S())); // a collecting light pass inside this same second would be skipped
        StartCollecting();
        CHECK(SampleDue(S())); // arming cleared the clock, so the capture's first light pass reads
        context->RSSetViewports(1, &otherWindow);
        ReadConstants(context.Get(), 1, lightBuffer.Get(), 1, cameraBuffer.Get(), 0, false);
        const EngineStatus own = EngineStatusNow();
        CHECK(own.haveSun);
        CHECK(own.sunX > 623.9f && own.sunX < 624.1f); // the capture's own window, not the waiting read's pose
        CHECK(own.sunY > 161.9f && own.sunY < 162.1f);
        S().lastLight = {};
    }

    // ---- the reflections' last water draw is this capture's, not the session's
    //
    // The module keeps one read for the session, so a second capture of one session would otherwise print the first one's slot. The line answers
    // for the capture's own window: it is written when that window passed a water draw on, and reads `-` when it did not.
    {
        using namespace ext::ffxiv::engine::ssr;
        g_ssrDiagnostics = Diagnostics {};
        g_ssrDiagnostics.key = true;
        g_ssrDiagnostics.armed = true;
        g_ssrDiagnostics.read.have = true;
        g_ssrDiagnostics.read.stamp = 0x01030709u;
        g_ssrDiagnostics.read.slot = 8;
        g_ssrDiagnostics.read.view = 4; // TEXTURE2D
        g_ssrDiagnostics.read.array = 1;
        g_ssrDiagnostics.read.format = 10; // R16G16B16A16_FLOAT
        g_ssrDiagnostics.read.width = 3840;
        g_ssrDiagnostics.read.height = 2160;
        g_ssrDiagnostics.read.texture = 0x1e6b8cdeee0ULL;

        // The baseline of this capture's window: one more draw was passed on in the session before it armed.
        S().ssrBase = {};
        S().ssrBase.taken = true;
        g_ssrDiagnostics.passedOn[uint32_t(PassedOn::OtherMap)] = 21;
        S().ssrBase.passedOn[uint32_t(PassedOn::OtherMap)] = 20;
        std::string text = SsrStatusText(SsrStatusNow());
        CHECK(Has(text, "ssr last water draw: stamp 0x01030709, map slot 8, read slot 8, view TEXTURE2D, array 1, format R16G16B16A16_FLOAT, "
                        "3840x2160, texture 1e6b8cdeee0"));

        // No water draw in this window: the session's read is not this capture's answer.
        S().ssrBase.passedOn[uint32_t(PassedOn::OtherMap)] = 21;
        text = SsrStatusText(SsrStatusNow());
        CHECK(Has(text, "ssr last water draw: -\n"));
        CHECK(!Has(text, "ssr window passed on:")); // the window's own count agrees with the line: nothing was passed on in it
    }

    return Finish("draw_diag_capture");
}
