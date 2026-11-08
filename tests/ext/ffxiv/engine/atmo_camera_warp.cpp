// Atmosphere's camera sources (CAMFIX) on the D3D11 WARP software adapter: the real godrays::FrameReader (ext/ffxiv/engine/GodraysSun.cpp) as the atmosphere instantiates it
// twice - once for the copy taken at the sun-shaped light draw, once for the copy taken at the frame's first light draw of any shape - on camera tables this test writes into
// real constant buffers: the user's three live captures of 2026-11-07 (day, dawn, night) and the BENCHMARK capture (20261107-071150, whose lighting layout has no sun-shaped
// light draw at all).
//
// Why this test exists: the atmosphere took its camera only from the copy that lands with the sun-shaped light draw. The benchmark has none (so the stage would have skipped
// every benchmark frame for "camera", and the real-engine A/B would have said the haze does nothing), and the live session skipped 868 frames in zone transitions. Depth of field
// and the light bounce get the same table elsewhere. What is checked:
//   the light draws' copy reads each table back verified (the atmosphere's own gate: ProjectionOk and ViewProjectionOk, inside FrameReader) - including the benchmark's, which
//     is the case the fix exists for - with the inverse projection, the three view rows and the inverse view's second row bit-equal to the fixture's words;
//   the two copies of one buffer are bit-equal in every field the stage folds into its hold (so a frame that has both is the same camera whichever copy is used);
//   a frame whose only copy is the light draws' one: the fold picks it (the atmosphere runs); with both: the sun draw's copy; with neither: no camera (the stage skips "camera");
//   a table that is not this build's camera (zeros, NaN, a damaged view-projection, a projection with the wrong w row) lands and is refused, and the fold takes nothing from it
//     (the stage skips "camera");
//   the light draws' ring follows the frames like the sun draw's: the newest copy it holds is the frame before, with the camera that frame wrote.
#include "check.h"
#include "camera_fixtures.h"
#include "live_fog_fixtures.h"

#include "ext/ffxiv/engine/AtmoRules.h"
#include "ext/ffxiv/engine/GodraysSun.h"

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::godrays;
namespace atmo = ext::ffxiv::engine::atmo;

namespace
{
constexpr uint32_t kTableBytes = 1024; // g_CameraParameter as the engine binds it
constexpr uint32_t kLightBufferBytes = 64;
constexpr uint32_t kUsedBytes = 0x160;                       // the window the readers copy
constexpr uint32_t kViewWords = 0, kViewProjectionWords = 0x60 / 4, kInverseViewRow1Words = 0x40 / 4;
constexpr uint32_t kInverseProjectionWords = 0xE0 / 4, kProjectionWords = 0x120 / 4;

struct Fixture
{
    const char* name;
    const uint32_t* words; // the table's first 0x160 bytes
};

// A 1024-byte table: the capture's first 0x160 bytes, zeros after (the readers never look past 0x160).
std::vector<unsigned char> TableOf(const uint32_t* words)
{
    std::vector<unsigned char> bytes(kTableBytes, 0);
    std::memcpy(bytes.data(), words, kUsedBytes);
    return bytes;
}

// The fields the stage folds into its hold must be the fixture's words, bit for bit.
bool FieldsEqualWords(const FrameRead& read, const uint32_t* words)
{
    return std::memcmp(read.inverseProjection, words + kInverseProjectionWords, sizeof read.inverseProjection) == 0 &&
           std::memcmp(read.inverseViewRow1, words + kInverseViewRow1Words, sizeof read.inverseViewRow1) == 0 &&
           std::memcmp(read.view, words + kViewWords, sizeof read.view) == 0 &&
           std::memcmp(read.projection, words + kProjectionWords, sizeof read.projection) == 0;
}

bool FieldsEqual(const FrameRead& a, const FrameRead& b)
{
    return std::memcmp(a.inverseProjection, b.inverseProjection, sizeof a.inverseProjection) == 0 &&
           std::memcmp(a.inverseViewRow1, b.inverseViewRow1, sizeof a.inverseViewRow1) == 0 && std::memcmp(a.view, b.view, sizeof a.view) == 0 &&
           std::memcmp(a.projection, b.projection, sizeof a.projection) == 0;
}

atmo::CameraCopy CopyOf(const FrameRead& read) { return atmo::CameraCopy { read.landed, read.landed && read.camera }; }
} // namespace

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    if (!context)
        return Finish("atmo_camera_warp");

    const auto makeBuffer = [&device](uint32_t bytes, const void* data) {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        device->CreateBuffer(&desc, data != nullptr ? &initial : nullptr, &buffer);
        return buffer;
    };
    ComPtr<ID3D11Query> done;
    D3D11_QUERY_DESC eventDesc { D3D11_QUERY_EVENT, 0 };
    CHECK(SUCCEEDED(device->CreateQuery(&eventDesc, &done)) && done != nullptr);
    const auto waitForCopies = [&context, &done]() {
        if (!done)
            return;
        context->End(done.Get());
        context->Flush();
        context->GetData(done.Get(), nullptr, 0, 0); // blocking: the test's own wait, never the stage's
    };

    // The two light buffers a frame can offer: a lamp (a point light at a world position, never the sun) and a sun-class directional light. The camera fallback ignores both.
    float lampBytes[kLightBufferBytes / 4] {};
    lampBytes[0] = 12.0f;  // position
    lampBytes[1] = 3.0f;
    lampBytes[2] = -40.0f;
    lampBytes[4] = 0.0f;   // direction
    lampBytes[5] = -1.0f;
    lampBytes[6] = 0.0f;
    lampBytes[8] = 0.2f;   // diffuse
    lampBytes[9] = 0.2f;
    lampBytes[10] = 0.2f;
    float sunBytes[kLightBufferBytes / 4] {};
    sunBytes[4] = 0.25f;
    sunBytes[5] = 0.8f;
    sunBytes[6] = 0.54f;
    sunBytes[8] = 1.4f;
    sunBytes[9] = 1.0f;
    sunBytes[10] = 0.7f;
    const ComPtr<ID3D11Buffer> lamp = makeBuffer(kLightBufferBytes, lampBytes);
    const ComPtr<ID3D11Buffer> sunLight = makeBuffer(kLightBufferBytes, sunBytes);
    CHECK(lamp && sunLight);
    if (!lamp || !sunLight)
        return Finish("atmo_camera_warp");

    const Fixture fixtures[] = {
        { "live day (164414-3)", live_fog::kLiveDayCamera },
        { "live dawn (164337-2)", live_fog::kLiveDawnCamera },
        { "live night (164135)", live_fog::kLiveNightCamera },
        { "benchmark (071150)", camera_fixtures::kBenchmarkCamera },
    };

    for (const Fixture& fixture : fixtures)
    {
        const std::vector<unsigned char> table = TableOf(fixture.words);
        const ComPtr<ID3D11Buffer> camera = makeBuffer(kTableBytes, table.data());
        CHECK(camera != nullptr);
        if (!camera)
            continue;

        // The light draws' copy (the lamp rides along, the camera is what is read).
        FrameReader lightDrawCopy;
        CHECK(lightDrawCopy.Copy(context.Get(), lamp.Get(), camera.Get(), 100));
        // The sun draw's copy of the same buffer, on its own ring.
        FrameReader sunDrawCopy;
        CHECK(sunDrawCopy.Copy(context.Get(), sunLight.Get(), camera.Get(), 100));
        waitForCopies();
        const FrameRead any = lightDrawCopy.Read(context.Get(), 101, true);
        const FrameRead sun = sunDrawCopy.Read(context.Get(), 101, true);
        if (!any.landed || !any.camera)
            std::printf("    %s: the light draws' copy did not read back verified (landed %d, camera %d)\n", fixture.name, int(any.landed), int(any.camera));
        CHECK(any.landed && any.camera && any.haveView);
        CHECK(sun.landed && sun.camera && sun.haveView);
        CHECK(FieldsEqualWords(any, fixture.words));
        CHECK(FieldsEqualWords(sun, fixture.words));
        CHECK(FieldsEqual(any, sun)); // the same buffer through either ring: the same camera, bit for bit

        // What the stage does with the two reads.
        const atmo::HoldStamps empty {};
        const atmo::CameraCopy lightOnly = CopyOf(any);
        const atmo::CameraCopy sunCopy = CopyOf(sun);
        const atmo::CameraFold fromLight = atmo::FoldCameraSources(empty, atmo::CameraCopy {}, lightOnly, 101);
        CHECK(fromLight.source == atmo::CameraSource::LightDraw && atmo::CameraStands(fromLight.stamps, 101) && !atmo::SunStands(fromLight.stamps, 101));
        const atmo::CameraFold fromBoth = atmo::FoldCameraSources(empty, sunCopy, lightOnly, 101);
        CHECK(fromBoth.source == atmo::CameraSource::SunDraw && atmo::CameraStands(fromBoth.stamps, 101) && atmo::SunStands(fromBoth.stamps, 101));
        const atmo::CameraFold fromSun = atmo::FoldCameraSources(empty, sunCopy, atmo::CameraCopy {}, 101);
        CHECK(fromSun.source == atmo::CameraSource::SunDraw);

        // Neither ring copied anything: nothing lands, nothing stands (the stage skips "camera").
        FrameReader never;
        const FrameRead nothing = never.Read(context.Get(), 101, true);
        CHECK(!nothing.landed && !nothing.camera);
        const atmo::CameraFold fromNeither = atmo::FoldCameraSources(empty, CopyOf(nothing), CopyOf(nothing), 101);
        CHECK(fromNeither.source == atmo::CameraSource::None && !atmo::CameraStands(fromNeither.stamps, 101));
    }

    // ---- tables that are not this build's camera ---------------------------------------------------------------------------------
    {
        std::vector<unsigned char> good = TableOf(live_fog::kLiveDayCamera);
        const auto floatAt = [](std::vector<unsigned char>& bytes, uint32_t wordIndex) -> float& { return *reinterpret_cast<float*>(bytes.data() + wordIndex * 4u); };
        std::vector<unsigned char> zeros(kTableBytes, 0);
        std::vector<unsigned char> nan = good;
        floatAt(nan, kProjectionWords) = std::numeric_limits<float>::quiet_NaN();
        std::vector<unsigned char> noViewProjection = good;
        for (uint32_t i = 12; i < 16; ++i)
            floatAt(noViewProjection, kViewProjectionWords + i) = 0.0f; // the view-projection's last row (a rotation's row, unit length) damaged
        std::vector<unsigned char> wrongW = good;
        floatAt(wrongW, kProjectionWords + 14) = 1.0f; // a projection whose w row is not (0, 0, -1, 0)
        const struct
        {
            const char* name;
            const std::vector<unsigned char>* bytes;
        } refused[] = { { "zeros", &zeros }, { "NaN in the projection", &nan }, { "damaged view-projection", &noViewProjection }, { "wrong w row", &wrongW } };
        for (const auto& table : refused)
        {
            const ComPtr<ID3D11Buffer> camera = makeBuffer(kTableBytes, table.bytes->data());
            CHECK(camera != nullptr);
            if (!camera)
                continue;
            FrameReader lightDrawCopy;
            CHECK(lightDrawCopy.Copy(context.Get(), lamp.Get(), camera.Get(), 300));
            waitForCopies();
            const FrameRead any = lightDrawCopy.Read(context.Get(), 301, true);
            if (!any.landed || any.camera)
                std::printf("    refused table '%s': landed %d, camera %d (wanted landed, refused)\n", table.name, int(any.landed), int(any.camera));
            CHECK(any.landed && !any.camera);
            const atmo::CameraFold fold = atmo::FoldCameraSources(atmo::HoldStamps {}, atmo::CameraCopy {}, CopyOf(any), 301);
            CHECK(fold.source == atmo::CameraSource::None && !atmo::CameraStands(fold.stamps, 301));
            // ... and a refused sun draw's table does not hide a good light draws' one.
            FrameReader sunDrawCopy;
            CHECK(sunDrawCopy.Copy(context.Get(), sunLight.Get(), camera.Get(), 300));
            waitForCopies();
            const FrameRead refusedSun = sunDrawCopy.Read(context.Get(), 301, true);
            CHECK(refusedSun.landed && !refusedSun.camera);
            const atmo::CameraFold rescued = atmo::FoldCameraSources(atmo::HoldStamps {}, CopyOf(refusedSun), atmo::CameraCopy { true, true }, 301);
            CHECK(rescued.source == atmo::CameraSource::LightDraw);
        }
    }

    // ---- the light draws' ring follows the frames -----------------------------------------------------------------------------------
    {
        std::vector<unsigned char> table = TableOf(live_fog::kLiveDayCamera);
        const ComPtr<ID3D11Buffer> camera = makeBuffer(kTableBytes, table.data());
        CHECK(camera != nullptr);
        if (camera)
        {
            FrameReader lightDrawCopy;
            for (uint64_t frame = 200; frame < 206; ++frame)
            {
                float marker = float(frame);
                std::memcpy(table.data() + 11 * 4, &marker, sizeof marker); // the view's third row translation: a marker the frame wrote (not read by the camera's check)
                context->UpdateSubresource(camera.Get(), 0, nullptr, table.data(), 0, 0);
                CHECK(lightDrawCopy.Copy(context.Get(), lamp.Get(), camera.Get(), frame));
                waitForCopies();
            }
            const FrameRead read = lightDrawCopy.Read(context.Get(), 206, true);
            CHECK(read.landed && read.camera);
            CHECK_EQ(read.writtenFrame, uint64_t(205));
            CHECK(std::fabs(read.view[11] - 205.0f) < 0.5f);
            // A second read at the same frame takes nothing older than what it took (the ring never goes back).
            const FrameRead again = lightDrawCopy.Read(context.Get(), 206, true);
            CHECK(!again.landed);
        }
    }

    return Finish("atmo_camera_warp");
}
