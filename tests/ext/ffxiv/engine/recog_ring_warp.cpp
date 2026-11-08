// God rays' cascade and fog read-backs (ext/ffxiv/engine/GodraysSun.cpp) on the D3D11 WARP software adapter, on buffers this test writes itself.
//
// Why this test exists: the god rays' march needs four rings to land in one frame (light and camera, cascades, fog, and the fog's height profile).
// Two bugs kept the last two from ever delivering:
//   the cascade ring kept the count of buffers it had copied into a slot when the slot was reused by a later frame, so after the first pass over
//   its four slots a frame's copies were either skipped as "already copied" (the game's cascade buffers keep their addresses) or refused as a
//   fifth buffer, and the slot kept the frame number of the first pass - which ages out of the ring's read window for good;
//   the fog ring picked the height profile by the shape of its bytes, and neither of the two 64-byte buffers the fog post binds has a rigid
//   shape; the profile is now the buffer the caller names, and its declared 48 bytes are checked before it is decoded.
// What is checked here:
//   a slot reused by a later frame holds that frame's copies (each frame writes its own matrix value), the read returns the newest copy the
//   ring may take with the frame it was written at, and it does so again many frames into a session;
//   the fog read decodes the real rows of a live capture through the profile buffer it was given, refuses the clip-to-world matrix when that is
//   what it was given, reports a frame without a profile as a frame without one (and clears an older frame's profile flag when the slot is
//   reused), and carries the viewport buffer's second row raw.
#include "check.h"
#include "recog_fixtures.h"

#include "ext/ffxiv/engine/GodraysSun.h"

#include <d3d11.h>

#include <cmath>
#include <cstring>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::godrays;

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    if (!context)
        return Finish("recog_ring_warp");

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
        context->GetData(done.Get(), nullptr, 0, 0); // blocking: the test's own wait, never the pass's
    };

    // ---- the cascade ring: a slot reused by a later frame holds that frame's copies -------------------------------------------------------------
    {
        // The game's four cascade buffers keep their addresses frame to frame; each frame rewrites them. The first float of a buffer says which
        // frame and which cascade wrote it.
        ComPtr<ID3D11Buffer> cascades[4];
        for (auto& cascade : cascades)
            cascade = makeBuffer(kCascadeBytes, nullptr);
        CHECK(cascades[0] && cascades[1] && cascades[2] && cascades[3]);
        if (!cascades[0] || !cascades[1] || !cascades[2] || !cascades[3])
            return Finish("recog_ring_warp");
        CascadeReader reader;
        const auto value = [](uint64_t frame, uint32_t index) { return float(frame * 10u + index); };
        for (uint64_t frame = 100; frame < 110; ++frame) // ten frames: the four slots are reused twice over
        {
            for (uint32_t i = 0; i < 4; ++i)
            {
                float bytes[kCascadeBytes / 4] {};
                bytes[0] = value(frame, i);
                context->UpdateSubresource(cascades[i].Get(), 0, nullptr, bytes, 0, 0);
                CHECK(reader.Copy(context.Get(), cascades[i].Get(), frame));
                CHECK(reader.Copy(context.Get(), cascades[i].Get(), frame)); // the assembly draws bind each buffer several times a frame
            }
            waitForCopies();
        }
        const CascadeRead read = reader.Read(context.Get(), 110, true);
        if (!read.landed)
            std::printf("    the cascade ring landed nothing after ten frames\n");
        CHECK(read.landed);
        CHECK_EQ(read.count, 4u);
        CHECK_EQ(read.writtenFrame, uint64_t(109));
        for (uint32_t i = 0; i < read.count && i < 4; ++i)
            CHECK(std::fabs(read.cascades[i].matrix[0] - value(109, i)) < 0.5f);

        // And again further into the session, with a frame the ring has not seen for a while in between.
        for (uint64_t frame = 400; frame < 403; ++frame)
        {
            for (uint32_t i = 0; i < 4; ++i)
            {
                float bytes[kCascadeBytes / 4] {};
                bytes[0] = value(frame, i);
                context->UpdateSubresource(cascades[i].Get(), 0, nullptr, bytes, 0, 0);
                CHECK(reader.Copy(context.Get(), cascades[i].Get(), frame));
            }
            waitForCopies();
        }
        const CascadeRead later = reader.Read(context.Get(), 403, true);
        CHECK(later.landed);
        CHECK_EQ(later.writtenFrame, uint64_t(402));
        if (later.landed)
            CHECK(std::fabs(later.cascades[0].matrix[0] - value(402, 0)) < 0.5f);

        // A fifth distinct buffer in one frame is still refused (the march needs no more), and the frame's four stay.
        ComPtr<ID3D11Buffer> fifth = makeBuffer(kCascadeBytes, nullptr);
        CHECK(!reader.Copy(context.Get(), fifth.Get(), 402));
        reader.Reset();
        CHECK(!reader.Read(context.Get(), 403, true).landed);
    }

    // ---- the fog ring: the profile is the buffer the caller names -------------------------------------------------------------------------------
    {
        using namespace recog_fixture;
        // c1's bytes: cFogParam (128 B) and cExpHeightFogParam (64 B), and the clip-to-world stand-in (the camera buffer's +0x0A0..0x0DF).
        ComPtr<ID3D11Buffer> fog = makeBuffer(kFogParamBytes, kC1FogParam);
        ComPtr<ID3D11Buffer> profile = makeBuffer(kExpHeightFogBytes, kC1ExpHeightFog);
        ComPtr<ID3D11Buffer> matrix = makeBuffer(kExpHeightFogBytes, kC1Camera + 40);
        const float viewportRow[8] { 0.5f, 0.5f, 1.0f, 1.0f, 0.00104f, -0.00185f, -1.0f, 1.0f };
        ComPtr<ID3D11Buffer> viewport = makeBuffer(kViewportParamBytes, viewportRow);
        CHECK(fog && profile && matrix && viewport);
        if (!fog || !profile || !matrix || !viewport)
            return Finish("recog_ring_warp");

        FogReader reader;
        // Tag 20: the real pair. The read at 21 decodes the state the capture's fog post bound.
        CHECK(reader.Copy(context.Get(), fog.Get(), profile.Get(), viewport.Get(), 20));
        waitForCopies();
        const FogRead good = reader.Read(context.Get(), 21, true);
        CHECK(good.landed && good.profile && !good.profileRefused);
        CHECK(good.usable);
        CHECK(std::fabs(good.fog.distB - 10.0f) < 1.0e-6f);
        CHECK(std::fabs(good.fog.density1 - 0.0017325f) < 1.0e-7f);
        CHECK_EQ(good.writtenFrame, uint64_t(20));
        CHECK(good.viewportLanded);
        CHECK(std::fabs(good.ndcScale[0] - 0.00104f) < 1.0e-7f && std::fabs(good.ndcScale[1] + 0.00185f) < 1.0e-7f);
        CHECK(std::fabs(good.ndcOffset[0] + 1.0f) < 1.0e-7f && std::fabs(good.ndcOffset[1] - 1.0f) < 1.0e-7f);

        // Tag 22: the clip-to-world matrix where the profile belongs (the wrong slot): the copy lands and is refused, never decoded.
        CHECK(reader.Copy(context.Get(), fog.Get(), matrix.Get(), viewport.Get(), 22));
        waitForCopies();
        const FogRead wrong = reader.Read(context.Get(), 23, true);
        CHECK(wrong.landed && wrong.profile);
        CHECK(wrong.profileRefused);
        CHECK(!wrong.usable);

        // Tag 24 reuses tag 20's slot with no profile at all: the frame reports none, and the older frame's profile is not read as this one's.
        CHECK(reader.Copy(context.Get(), fog.Get(), nullptr, nullptr, 24));
        waitForCopies();
        const FogRead none = reader.Read(context.Get(), 25, true);
        CHECK(none.landed);
        CHECK(!none.profile && !none.usable && !none.profileRefused);

        // A profile whose declared bytes are unusable (a negative density) is refused too.
        float damaged[16];
        std::memcpy(damaged, kC1ExpHeightFog, sizeof damaged);
        damaged[2] = -0.5f;
        ComPtr<ID3D11Buffer> damagedBuffer = makeBuffer(kExpHeightFogBytes, damaged);
        CHECK(reader.Copy(context.Get(), fog.Get(), damagedBuffer.Get(), nullptr, 26));
        waitForCopies();
        const FogRead refused = reader.Read(context.Get(), 27, true);
        CHECK(refused.landed && refused.profile && refused.profileRefused && !refused.usable);

        reader.Reset();
        CHECK(!reader.Read(context.Get(), 27, true).landed);
    }

    return Finish("recog_ring_warp");
}
