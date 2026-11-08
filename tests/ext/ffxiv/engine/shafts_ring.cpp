// The sun's light and camera read-back (ext/ffxiv/engine/ShaftsSun.cpp) on the D3D11 WARP software adapter, on buffers this test writes itself.
//
// Why this test exists: the pass reads the light's direction and the camera's projection from the game's own buffers, and reading them at the frame
// that copied them asks the GPU for bytes it has not written yet - at 4K the GPU is a frame or two behind, so the map that must not wait returns
// "still drawing" on every frame and the pass never gets a sun. What is checked here is that the values arrive, that they belong to the newest
// copy the ring still holds (the frame's own copy is never read back as if it were finished), that an older copy is used when the newest one has
// not landed, and that a read which has nothing to say is not a not-ready one:
//   a frame whose ring holds no older copy does not read anything (the first frame of a session),
//   every read that lands holds the values of the newest copy it may take - the frame before this one - not of the frame that just copied them
//   (each frame writes its own direction and its own projection scale, so a same-frame read, or a read of one fixed older age, is a wrong answer),
//   a copy this frame made is never read, even when it has already landed,
//   a newer copy is never replaced by an older one when the same frame is read twice,
//   an older copy that has landed is taken when the newest is still in the GPU's queue,
//   a light at a world position and a camera buffer that is not a projection are reported, not silently accepted,
//   the counters keep a not-ready map and a successful read apart, and no map ever waits for the GPU,
//   Reset drops the facts and the staging pairs.
#include "check.h"

#include "ext/ffxiv/engine/ShaftsSun.h"

#include <d3d11.h>

#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::shafts;

namespace
{
// A perspective projection as g_CameraParameter stores it (row-major, a column-vector multiply): the shape ProjectionOk accepts.
void Perspective(float rows[kProjectionRows], float scale)
{
    std::memset(rows, 0, sizeof(float) * kProjectionRows);
    rows[0] = scale;
    rows[5] = scale * 1.5f;
    rows[10] = 1.0001f;
    rows[11] = -0.1f;
    rows[14] = -1.0f;
}

// The camera's view-projection, which g_CameraParameter holds beside the projection: the same perspective through an unrotated view, with three
// times the projection's own x scale. The two differ, so a read one offset off answers another matrix' scale, and the sun's world direction is
// projected through this one.
void ViewProjection(float rows[kProjectionRows], float scale)
{
    Perspective(rows, scale);
    rows[0] = scale * 3.0f;
}

// One frame's g_LightParam: a direction that says which frame it belongs to, and an optional world position (a lamp).
struct LightBytes
{
    float position[3] { 0.0f, 0.0f, 0.0f };
    float padding = 0.0f;
    float direction[3] { 0.0f, 0.0f, 0.0f };
    float padding2 = 0.0f;
    float diffuse[3] { 0.0f, 0.0f, 0.0f };
    float padding3 = 0.0f;
};
static_assert(sizeof(LightBytes) == 0x30, "g_LightParam's first 0x30 bytes are the three ranges the pass reads");

// The camera's first 0x160 bytes: the view-projection sits at +0x060 and the projection at +0x120, as the pass reads them.
struct CameraBytes
{
    unsigned char bytes[kCameraBytes] {};
};

// The direction a given frame writes: a unit vector whose z says which frame it is and whose y is half way up, so it is the directional light
// above the horizon IsSunLight requires. Two frames are 0.05 apart in z - far more than the float precision of the copy.
float FrameDirectionZ(uint64_t frame) { return -0.5f + 0.05f * float(frame); }
float FrameDirectionX(uint64_t frame)
{
    const float z = FrameDirectionZ(frame);
    return std::sqrt(1.0f - 0.25f - z * z);
}
void SetFrameLight(uint64_t frame, LightBytes& bytes)
{
    bytes.direction[0] = FrameDirectionX(frame);
    bytes.direction[1] = 0.5f;
    bytes.direction[2] = FrameDirectionZ(frame);
}
float FrameProjectionScale(uint64_t frame) { return 0.5f + 0.25f * float(frame); }
} // namespace

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    if (!context)
        return Finish("shafts_ring");

    // The game's own two buffers: small constant buffers the pass copies a range out of.
    const auto makeBuffer = [&device](uint32_t bytes, const void* data) {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = bytes;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        device->CreateBuffer(&desc, &initial, &buffer);
        return buffer;
    };

    LightBytes light {};
    light.direction[1] = 1.0f;
    CameraBytes camera {};
    // The camera's view: the identity, so a view-space direction's world elevation is its own y and the two readings of the horizon test agree.
    // The copy the pass takes carries it beside the two matrices, which is what a frame that turned between the read and its use is projected with.
    // Row-major, four floats a row as the buffer stores it: the identity's three rows.
    const float view[kViewRows] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    std::memcpy(camera.bytes + kViewAt, view, sizeof view);
    float projection[kProjectionRows] {};
    Perspective(projection, 0.75f);
    std::memcpy(camera.bytes + kProjectionOffset, projection, sizeof projection);
    float cameraViewProjection[kProjectionRows] {};
    ViewProjection(cameraViewProjection, 0.75f);
    std::memcpy(camera.bytes + kViewProjectionAt, cameraViewProjection, sizeof cameraViewProjection);
    ComPtr<ID3D11Buffer> lightBuffer = makeBuffer(sizeof light, &light);
    ComPtr<ID3D11Buffer> cameraBuffer = makeBuffer(uint32_t(sizeof camera), &camera);
    CHECK(lightBuffer != nullptr && cameraBuffer != nullptr);
    if (!lightBuffer || !cameraBuffer)
        return Finish("shafts_ring");

    // A software adapter completes the copies asynchronously: the test waits for its own work (an event query the code under test never uses), so
    // that a read which lands really does land, and the assertion is about which frame's bytes came back rather than about the driver's timing.
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

    // ---- the product's no-wait read on a software adapter: the copy has not landed, so nothing is read, no value changes, and the counter says the
    //      map was not ready (a refusal-free frame). This is also the live case at a render scale where the GPU is a frame behind.
    {
        SunReader noWait;
        CHECK(noWait.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 0));
        waitForCopies();
        const SunRead read = noWait.Read(context.Get(), kNewestReadAge);
        if (!read.light)
        {
            CHECK(read.notReady);
            CHECK(!noWait.HaveFacts());
            CHECK(!noWait.HaveProjection());
            CHECK_EQ(noWait.Reads(), 0u);
            CHECK_EQ(noWait.NotReady(), 1u);
        }
        else
        {
            // A driver that does satisfy the no-wait map: then the same values must be there, and the counters must say one read.
            CHECK_EQ(noWait.Reads(), 1u);
            CHECK_EQ(noWait.NotReady(), 0u);
        }
    }

    SunReader reader;

    // ---- before the ring holds a copy, nothing is read: no facts, no counters, and no not-ready
    {
        CHECK(!reader.HaveFacts() && !reader.HaveProjection());
        const SunRead read = reader.Read(context.Get(), 0);
        CHECK(!read.light && !read.notReady && !read.projectionRefused);
        CHECK(!reader.HaveRead() && reader.Reads() == 0 && reader.NotReady() == 0);
    }

    // ---- eight frames, each with its own direction and its own projection scale: every read that lands must hold the values of the newest copy
    //      the ring may take, which is the frame before this one. The shape this replaced read one fixed age (two frames back), so its values are
    //      a different frame's; a same-frame read would hand this frame's own values back. Either is a visible wrong answer in the picture.
    {
        uint32_t landed = 0;
        uint32_t empty = 0;
        for (uint64_t frame = 0; frame < 8; ++frame)
        {
            // The frame's own values, written into the game's buffers the way the engine writes them.
            LightBytes frameLight {};
            SetFrameLight(frame, frameLight);
            context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &frameLight, 0, 0);
            float frameProjection[kProjectionRows] {};
            Perspective(frameProjection, FrameProjectionScale(frame));
            std::memcpy(camera.bytes + kProjectionOffset, frameProjection, sizeof frameProjection);
            float frameViewProjection[kProjectionRows] {};
            ViewProjection(frameViewProjection, FrameProjectionScale(frame));
            std::memcpy(camera.bytes + kViewProjectionAt, frameViewProjection, sizeof frameViewProjection);
            context->UpdateSubresource(cameraBuffer.Get(), 0, nullptr, &camera, 0, 0);

            CHECK(reader.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), frame));
            waitForCopies();
            const SunRead read = reader.Read(context.Get(), frame, true);
            if (frame < kNewestReadAge)
            {
                // No copy older than this frame exists: a read that has nothing to say, and not a not-ready one.
                CHECK(!read.light && !read.notReady);
                ++empty;
                continue;
            }
            CHECK(read.light); // a blocking read of a copy the test waited for always lands
            ++landed;
            const uint64_t readFrame = frame - kNewestReadAge; // the newest copy a read may take
            // The value that landed is that frame's own: the ring's newest allowed age, not one fixed age and not this frame's.
            CHECK(std::fabs(reader.Facts().direction[2] - FrameDirectionZ(readFrame)) < 0.01f);
            CHECK(std::fabs(reader.Projection()[0] - FrameProjectionScale(readFrame)) < 1.0e-4f);
            CHECK(reader.HaveProjection());
            // The view comes from the same copy, so the light's view-space direction has a world elevation this read can state: with the
            // identity view it is the direction's own y, which is the frame's horizon answer.
            CHECK(reader.HaveView());
            CHECK(std::fabs(WorldElevationOf(reader.View(), reader.Facts().direction) - reader.Facts().direction[1]) < 1.0e-5f);
            CHECK(IsSunLight(reader.Facts())); // no world position, a unit direction
        }
        CHECK_EQ(empty, kNewestReadAge); // only the first frame has no older copy
        CHECK_EQ(landed, 7u);
        CHECK_EQ(reader.NotReady(), 0u);
        CHECK_EQ(reader.Reads(), landed);
        CHECK(!ReadStale(true, reader.LastReadFrame(), reader.LastReadFrame() + kMaxReadbackAge));
        CHECK(ReadStale(true, reader.LastReadFrame(), reader.LastReadFrame() + kMaxReadbackAge + 1));
    }

    // ---- the literal same-frame case: a copy this frame made is never read, even when it has already landed. With an age of 0 accepted the read
    //      would hand back the values the draw of this very frame wrote, which is the defect the age rule exists for.
    {
        SunReader same;
        LightBytes own {};
        own.direction[2] = -1.0f;
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &own, 0, 0);
        CHECK(same.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 40));
        waitForCopies();
        const SunRead read = same.Read(context.Get(), 40, true);
        CHECK(!read.light && !read.notReady && !read.projectionRefused);
        CHECK(!same.HaveFacts() && !same.HaveRead());
        CHECK_EQ(same.Reads(), 0u);
        CHECK_EQ(same.NotReady(), 0u);
    }

    // ---- a newer copy is never replaced by an older one: after a read has taken frame 11's copy, the same frame read again finds nothing newer
    //      (the slots it still holds are older), so the values only ever move forward. The next frame reads again.
    {
        SunReader ordered;
        for (uint64_t frame = 10; frame <= 13; ++frame)
        {
            LightBytes frameLight {};
            SetFrameLight(frame, frameLight);
            context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &frameLight, 0, 0);
            CHECK(ordered.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), frame));
        }
        waitForCopies();
        const SunRead first = ordered.Read(context.Get(), 12, true); // frame 11's copy
        CHECK(first.light);
        CHECK(std::fabs(ordered.Facts().direction[2] - FrameDirectionZ(11)) < 0.01f);
        const SunRead again = ordered.Read(context.Get(), 12, true); // nothing newer than frame 11 in the ring
        CHECK(!again.light && !again.notReady);
        CHECK_EQ(ordered.Reads(), 1u);
        CHECK(std::fabs(ordered.Facts().direction[2] - FrameDirectionZ(11)) < 0.01f);
        const SunRead next = ordered.Read(context.Get(), 13, true); // frame 12's copy, which is newer
        CHECK(next.light);
        CHECK(std::fabs(ordered.Facts().direction[2] - FrameDirectionZ(12)) < 0.01f);
        CHECK_EQ(ordered.Reads(), 2u);
    }

    // ---- the newest copy not landed, an older one taken: frame 20's copy was waited for and frame 21's is still in the command buffer (never
    //      submitted), so the read at 23 can only take frame 20's - the oldest age the ring holds. One fixed age would read one of them once and
    //      give up, which is the "armed but never drawing" session this round is for.
    {
        SunReader behind;
        LightBytes frameLight {};
        SetFrameLight(20, frameLight);
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &frameLight, 0, 0);
        float behindProjection[kProjectionRows] {};
        Perspective(behindProjection, FrameProjectionScale(20));
        std::memcpy(camera.bytes + kProjectionOffset, behindProjection, sizeof behindProjection);
        context->UpdateSubresource(cameraBuffer.Get(), 0, nullptr, &camera, 0, 0);
        CHECK(behind.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 20));
        waitForCopies();
        // Frame 21's copy: issued, never flushed, so its map is either "still drawing" (the live case) or a driver that answered it anyway.
        SetFrameLight(21, frameLight);
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &frameLight, 0, 0);
        CHECK(behind.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 21));

        const SunRead read = behind.Read(context.Get(), 23, false); // age 1 = frame 22 (not written), age 2 = 21, age 3 = 20
        CHECK(read.light); // an older copy carried the frame instead of the read coming back empty
        CHECK_EQ(behind.Reads(), 1u);
        const float z = behind.Facts().direction[2];
        const bool newest = std::fabs(z - FrameDirectionZ(21)) < 0.01f;
        const bool older = std::fabs(z - FrameDirectionZ(20)) < 0.01f;
        CHECK(newest || older); // never this frame's own, and never a frame the ring does not hold
        CHECK(!(newest && older));
        // The two maps of the copy that was taken are independent, and this read never waits: the light's bytes can have landed while the
        // camera's are still in the queue, and that is the same GPU-behind answer the counter exists for (a read that landed is not a refusal).
        // So the counter may count this read once - what it must never do is count a map twice or count a read that did not land.
        CHECK(behind.NotReady() <= 1u);
    }

    // ---- a lamp: the light carries a world position, so the rules refuse it, and the read still happens (the caller names the reason)
    {
        LightBytes lamp {};
        lamp.position[0] = 43.2f;
        lamp.position[1] = 15.2f;
        lamp.position[2] = -66.3f;
        lamp.direction[2] = -1.0f;
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &lamp, 0, 0);
        CHECK(reader.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 8));
        waitForCopies();
        const SunRead read = reader.Read(context.Get(), 8 + kNewestReadAge, true);
        if (read.light)
        {
            CHECK(!IsSunLight(reader.Facts()));
            CHECK(!IsZeroPosition(reader.Facts().position));
            CHECK(reader.LastLight() == static_cast<const void*>(lightBuffer.Get()));
        }
        // Forget drops what was read, keeps the counters, and the next frame reads again.
        const uint32_t reads = reader.Reads();
        reader.Forget();
        CHECK(!reader.HaveFacts() && !reader.HaveProjection() && !reader.HaveRead());
        CHECK(!reader.ProjectionRefused());
        CHECK_EQ(reader.Reads(), reads);
    }

    // ---- a refusal does not pin the ring's age. The copy a refused read took is not an accepted one, so the frame after the refusal may take a
    //      copy that is now two frames old - which is what stops a light the rules refuse from costing every frame until an older copy lands (a
    //      session alternated "refused" and "no sun" for 85 frames on exactly that).
    {
        SunReader refusal;
        LightBytes frameLight {};
        SetFrameLight(200, frameLight);
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &frameLight, 0, 0);
        CHECK(refusal.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 200));
        waitForCopies();
        const SunRead first = refusal.Read(context.Get(), 201, true); // the copy the frame before this one made: age 1
        CHECK(first.light);
        if (first.light)
            CHECK(std::fabs(refusal.Facts().direction[2] - FrameDirectionZ(200)) < 0.01f);
        refusal.Forget(); // the caller refused that light: it is not an accepted read
        // No new copy since: the only copy the ring still holds is the one at frame 200, two frames back. The next frame's read may take it, and on
        // the build this replaces it could not (the refused read's own frame was the floor).
        const SunRead again = refusal.Read(context.Get(), 202, true);
        CHECK(again.light);
        if (again.light)
        {
            CHECK(std::fabs(refusal.Facts().direction[2] - FrameDirectionZ(200)) < 0.01f);
            CHECK_EQ(refusal.Reads(), 2u);
        }
    }

    // ---- the tone map's own parameters: the exposure multiplier is read from a copy one frame old, never from the one the frame just made, and a ring
    //      that has been reset answers nothing.
    {
        struct ToneBytes
        {
            float values[4] { 0.0f, 1.21f, 0.0f, 0.0f }; // the second float is the exposure multiplier
        };
        ToneBytes tone {};
        ComPtr<ID3D11Buffer> toneBuffer = makeBuffer(uint32_t(sizeof tone), &tone);
        CHECK(toneBuffer != nullptr);
        ToneReader toneReader;
        CHECK(toneReader.Copy(context.Get(), toneBuffer.Get(), 40));
        waitForCopies();
        const ToneRead own = toneReader.Read(context.Get(), 40); // age 0: this frame's own copy is never read
        CHECK(!own.landed);
        const ToneRead read = toneReader.Read(context.Get(), 41, true);
        CHECK(read.landed);
        if (read.landed)
        {
            CHECK(std::fabs(read.exposure - 1.21f) < 1.0e-5f);
            CHECK(std::fabs(WhitePointFromExposure(read.exposure) - 1.0f / 1.21f) < 1.0e-5f);
        }
        // The copy a read took is not handed out twice, and a newer copy replaces it: the ring is not a queue that empties, and a white point
        // that has landed is not re-read for every frame that asks.
        const ToneRead again = toneReader.Read(context.Get(), 41, true);
        CHECK(!again.landed);
        struct ToneBytesB
        {
            float values[4] { 0.0f, 1.5f, 0.0f, 0.0f };
        };
        ToneBytesB toneB {};
        ComPtr<ID3D11Buffer> toneBufferB = makeBuffer(uint32_t(sizeof toneB), &toneB);
        CHECK(toneBufferB != nullptr);
        CHECK(toneReader.Copy(context.Get(), toneBufferB.Get(), 42));
        waitForCopies();
        const ToneRead newer = toneReader.Read(context.Get(), 43, true);
        CHECK(newer.landed && std::fabs(newer.exposure - 1.5f) < 1.0e-5f);
        toneReader.Reset();
        CHECK(!toneReader.Read(context.Get(), 41, true).landed);
        // A light buffer bound in its place, or no buffer at all, is not a copy: nothing to read later.
        CHECK(!toneReader.Copy(context.Get(), nullptr, 42));
        // The ring is as deep as the light's: a copy two frames back still answers when the newest is missing (a GPU that is two behind, and the
        // fixed one-frame age that would never land on such a session).
        CHECK(toneReader.Copy(context.Get(), toneBuffer.Get(), 50));
        waitForCopies();
        const ToneRead older = toneReader.Read(context.Get(), 52, true); // age 2: the copy at 50, since 51 has none
        CHECK(older.landed);
        if (older.landed)
            CHECK(std::fabs(older.exposure - 1.21f) < 1.0e-5f);
    }

    // ---- the sky's own sun parameters: the first row is copied and read a frame late, the row the rules judge comes back whole, and a ring that
    //      has been reset answers nothing.
    {
        struct SunBytes
        {
            float values[4] { 1.7778f, 1.0f, 0.6995f, 0.1751f }; // the session's own cSunParam[0]
        };
        SunBytes row {};
        ComPtr<ID3D11Buffer> rowBuffer = makeBuffer(uint32_t(sizeof row), &row);
        CHECK(rowBuffer != nullptr);
        SunUvReader reader;
        CHECK(reader.Copy(context.Get(), rowBuffer.Get(), 60));
        waitForCopies();
        const SunUvRead own = reader.Read(context.Get(), 60); // age 0: this frame's own copy is never read
        CHECK(!own.landed);
        const SunUvRead read = reader.Read(context.Get(), 61, true);
        CHECK(read.landed);
        if (read.landed)
        {
            CHECK_EQ(read.writtenFrame, uint64_t(60));
            CHECK(SunParamUvUsable(read.uv));
            CHECK(std::fabs(read.uv[0] - 1.7778f) < 1.0e-5f && std::fabs(read.uv[2] - 0.6995f) < 1.0e-5f &&
                  std::fabs(read.uv[3] - 0.1751f) < 1.0e-5f);
            const SunProjection projected = ProjectSunFromUv(read.uv[2], read.uv[3], Window { 0.0f, 0.0f, 1920.0f, 1080.0f });
            CHECK(projected.unit && SunVisible(projected));
        }
        // The copy a read took is not handed out twice; a newer copy replaces it, and its own frame travels with the row so the age rule can be
        // applied without guessing.
        CHECK(!reader.Read(context.Get(), 61, true).landed);
        struct SunBytesB
        {
            float values[4] { 1.0f, 1.0f, 0.5f, 0.25f };
        };
        SunBytesB rowB {};
        ComPtr<ID3D11Buffer> rowBufferB = makeBuffer(uint32_t(sizeof rowB), &rowB);
        CHECK(rowBufferB != nullptr);
        CHECK(reader.Copy(context.Get(), rowBufferB.Get(), 62));
        waitForCopies();
        const SunUvRead newer = reader.Read(context.Get(), 63, true);
        CHECK(newer.landed && newer.writtenFrame == 62 && std::fabs(newer.uv[3] - 0.25f) < 1.0e-5f);
        reader.Reset();
        CHECK(!reader.Read(context.Get(), 63, true).landed);
        // A draw that bound no buffer is not a copy: the frame keeps the sun it placed the shafts with.
        CHECK(!reader.Copy(context.Get(), nullptr, 64));
    }

    // ---- the frame's directional shadow, copied and read with the light: a 128-byte buffer of the shadow matrix's shape carries the light's own
    //      axis at +0x20, a neighbour of the same size without that shape is kept but never judged, and a draw with no such buffer is Missing -
    //      the light's direction alone then decides, never a refusal.
    {
        // The captured matrix's shape, built from its own numbers: the third row is minus the light times the cascade's scale, the first two rows
        // are an orthogonal basis of the plane beside it, and the fourth row is (0, 0, 0, 1). A frame's own shadow therefore always agrees with
        // the direction that frame's light reported - if the two came from different copies, they do not.
        const auto shadowFor = [](const float direction[3], float values[kShadowFloats]) {
            std::memset(values, 0, sizeof(float) * kShadowFloats);
            const float up[3] { 0.0f, 0.0f, 1.0f };
            const float x[3] { up[1] * direction[2] - up[2] * direction[1], up[2] * direction[0] - up[0] * direction[2],
                               up[0] * direction[1] - up[1] * direction[0] };
            const float xLength = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
            const float y[3] { direction[1] * x[2] - direction[2] * x[1], direction[2] * x[0] - direction[0] * x[2],
                               direction[0] * x[1] - direction[1] * x[0] };
            for (uint32_t i = 0; i < 3; ++i)
            {
                values[i] = x[i] / xLength * 0.5f;      // row 0: one axis, its own scale
                values[4 + i] = y[i] / xLength * 0.25f; // row 1: the second axis, another scale
                values[8 + i] = -direction[i] * 0.05f;  // row 2: minus the light, scaled by the cascade
            }
            values[15] = 1.0f; // row 3: a light-space transform has no projective row
        };
        // A direction that turns with the frame: 0.12 rad of yaw per frame is 6.9 degrees, so a row from another frame's copy is refused by the
        // rule's three-degree tolerance.
        const auto frameDirection = [](uint64_t frame, float direction[3]) {
            const float angle = 0.12f * float(frame);
            direction[0] = std::cos(angle) * 0.8660254f;
            direction[1] = 0.5f;
            direction[2] = -std::sin(angle) * 0.8660254f;
        };
        float shadowBytes[kShadowFloats] {};
        float frameDirectional[3] {};
        frameDirection(70, frameDirectional);
        shadowFor(frameDirectional, shadowBytes);
        CHECK(ShadowMatrixShape(shadowBytes));
        CHECK(std::fabs(ShadowDegrees(shadowBytes + kShadowRowAt / 4, frameDirectional)) < 0.1f); // a tenth of a degree: the fixture's floats
        // A 128-byte neighbour that is not a light-space matrix (g_FakeSpecularParam's shape is unknown; its numbers are not one).
        float neighbour[kShadowFloats] {};
        neighbour[0] = 0.3f;
        neighbour[5] = 0.7f;
        neighbour[10] = 0.9f;
        neighbour[15] = 2.0f; // a fourth row that is not (0, 0, 0, 1)
        CHECK(!ShadowMatrixShape(neighbour));
        ComPtr<ID3D11Buffer> shadowBuffer = makeBuffer(uint32_t(sizeof shadowBytes), shadowBytes);
        ComPtr<ID3D11Buffer> neighbourBuffer = makeBuffer(uint32_t(sizeof neighbour), neighbour);
        CHECK(shadowBuffer != nullptr && neighbourBuffer != nullptr);
        if (shadowBuffer && neighbourBuffer)
        {
            SunReader reader;
            LightBytes sun {};
            sun.direction[0] = frameDirectional[0];
            sun.direction[1] = frameDirectional[1];
            sun.direction[2] = frameDirectional[2];
            context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &sun, 0, 0);
            ID3D11Buffer* candidates[2] { shadowBuffer.Get(), neighbourBuffer.Get() };
            CHECK(reader.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 70, candidates, 2));
            waitForCopies();
            const SunRead read = reader.Read(context.Get(), 71, true);
            CHECK(read.light);
            // The candidates come back with the same copy as the light: the shaped one has the shadow matrix's shape, the neighbour does not, and
            // the judgement over them agrees with the light this copy reported.
            CHECK_EQ(reader.ShadowCount(), 2u);
            if (reader.ShadowCount() == 2)
            {
                CHECK(reader.Shadows()[0].shaped);
                CHECK(!reader.Shadows()[1].shaped);
                const ShadowJudgement judgement = JudgeShadow(reader.Shadows(), reader.ShadowCount(), reader.Facts().direction);
                CHECK(judgement.result == ShadowResult::Agreed);
                CHECK(judgement.degrees >= 0.0f && judgement.degrees < 0.1f);
            }
            // A frame whose light draw bound no 128-byte buffer (or whose copy has not arrived): Missing, and the direction alone decides.
            SunReader plain;
            CHECK(plain.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 72));
            waitForCopies();
            const SunRead plainRead = plain.Read(context.Get(), 73, true);
            CHECK(plainRead.light);
            CHECK_EQ(plain.ShadowCount(), 0u);
            CHECK(JudgeShadow(plain.Shadows(), plain.ShadowCount(), plain.Facts().direction).result == ShadowResult::Missing);
            // The rows belong to the copy the read took, not to the frame that asked: frame 74's copy carries another direction and another
            // shadow, and the read at 75 answers with those.
            float later[3] {};
            frameDirection(74, later);
            shadowFor(later, shadowBytes);
            context->UpdateSubresource(shadowBuffer.Get(), 0, nullptr, &shadowBytes[0], 0, 0);
            sun.direction[0] = later[0];
            sun.direction[1] = later[1];
            sun.direction[2] = later[2];
            context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &sun, 0, 0);
            CHECK(reader.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 74, candidates, 2));
            waitForCopies();
            const SunRead next = reader.Read(context.Get(), 75, true);
            CHECK(next.light);
            if (next.light && reader.ShadowCount() == 2)
            {
                CHECK(std::fabs(reader.Facts().direction[0] - later[0]) < 1.0e-5f);
                CHECK(JudgeShadow(reader.Shadows(), reader.ShadowCount(), reader.Facts().direction).result == ShadowResult::Agreed);
            }
        }
    }

    // ---- a camera buffer that is not a projection: reported as a refusal of the projection, and the light's facts still arrive
    {
        LightBytes sun {};
        sun.direction[1] = 0.5f;
        sun.direction[2] = -0.8660254f;
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &sun, 0, 0);
        float notAProjection[kProjectionRows] {};
        notAProjection[0] = 1.0f; // no w row at all
        std::memcpy(camera.bytes + kProjectionOffset, notAProjection, sizeof notAProjection);
        context->UpdateSubresource(cameraBuffer.Get(), 0, nullptr, &camera, 0, 0);
        CHECK(reader.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 11));
        waitForCopies();
        const SunRead read = reader.Read(context.Get(), 11 + kNewestReadAge, true);
        if (read.light)
        {
            CHECK(!read.projection);
            CHECK(read.projectionRefused);
            CHECK(reader.ProjectionRefused()); // the counted reason, which a camera that has not landed is not
            CHECK(!reader.HaveProjection());
            CHECK(IsSunLight(reader.Facts()));
        }
        else
        {
            CHECK(read.notReady);
        }
    }

    // ---- a projection with a slightly different w row is still a projection (the tolerance the live buffer needs), and a wrong one is not
    {
        float almost[kProjectionRows] {};
        Perspective(almost, 1.0f);
        almost[12] = 2.0e-4f;  // a stored row that is not exactly zero
        almost[14] = -0.99996f; // and a w that is not exactly -1
        CHECK(ProjectionOk(almost));
        float wrong[kProjectionRows] {};
        Perspective(wrong, 1.0f);
        wrong[12] = 0.02f;
        CHECK(!ProjectionOk(wrong));
        float nanRow[kProjectionRows] {};
        Perspective(nanRow, 1.0f);
        nanRow[9] = std::nanf("");
        CHECK(!ProjectionOk(nanRow));
        CHECK(!ProjectionOk(nullptr));
    }

    // ---- the projection is read at its own offset: +0x120 (the projection), not +0x60 (the view-projection beside it on every frame). The
    //      fixture writes the two matrices at literal offsets and gives each its own scale, so a read at the wrong one either lands on a matrix
    //      the shape check refuses or answers with another matrix' scale - and a read that moved one offset early would do so on every frame.
    {
        LightBytes sun {};
        sun.direction[1] = 0.5f;
        sun.direction[2] = -0.8660254f;
        context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &sun, 0, 0);
        std::vector<unsigned char> table(0x160); // the engine's table ends with the projection's four rows, at +0x120..+0x160
        float projection[kProjectionRows] {};
        Perspective(projection, 2.25f);
        std::memcpy(table.data() + 0x120, projection, sizeof projection);
        float viewProjection[kProjectionRows] {};
        Perspective(viewProjection, 3.75f);
        viewProjection[15] = 1.0f; // a view-projection's w row carries the camera's own position: not a projection's (0, 0, -1, 0)
        std::memcpy(table.data() + 0x60, viewProjection, sizeof viewProjection);
        ComPtr<ID3D11Buffer> tableBuffer = makeBuffer(uint32_t(table.size()), table.data());
        CHECK(tableBuffer != nullptr);
        if (tableBuffer)
        {
            SunReader camera;
            CHECK(camera.Copy(context.Get(), lightBuffer.Get(), tableBuffer.Get(), 5));
            waitForCopies();
            const SunRead read = camera.Read(context.Get(), 5 + kNewestReadAge, true);
            CHECK(read.light);
            CHECK(read.projection);
            CHECK(!read.projectionRefused);
            CHECK(camera.HaveProjection());
            CHECK(std::fabs(camera.Projection()[0] - 2.25f) < 1.0e-4f); // the projection's scale, never the view-projection's 3.75
        }
    }

    // ---- Reset: no facts, no staging pairs, and a frame after it reads nothing (the ring starts over)
    {
        reader.Reset();
        CHECK(!reader.HaveFacts() && !reader.HaveProjection() && !reader.HaveRead());
        CHECK(!reader.ProjectionRefused());
        CHECK_EQ(reader.Reads(), 0u);
        CHECK_EQ(reader.NotReady(), 0u);
        const SunRead read = reader.Read(context.Get(), 100);
        CHECK(!read.light && !read.notReady);
    }

    // ---- a device change: the ring's staging pairs belong to the device that made them. A copy on a new context into one is refused silently
    //      (CopySubresourceRegion returns void, the slot still says "written", and the read then maps the bytes the old device wrote - for the
    //      rest of the session). The pair is rebuilt on the new device, so the values that land are the new device's own.
    {
        ComPtr<ID3D11Device> other;
        ComPtr<ID3D11DeviceContext> otherContext;
        CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &other, nullptr, &otherContext)));
        if (otherContext)
        {
            const auto makeOtherBuffer = [&other](uint32_t bytes, const void* data) {
                D3D11_BUFFER_DESC desc {};
                desc.ByteWidth = bytes;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
                ComPtr<ID3D11Buffer> buffer;
                other->CreateBuffer(&desc, &initial, &buffer);
                return buffer;
            };
            LightBytes otherLight {};
            otherLight.direction[2] = -1.0f; // a unit direction no frame of the first device's ring wrote
            CameraBytes otherCamera {};
            float otherProjection[kProjectionRows] {};
            Perspective(otherProjection, 3.5f);
            std::memcpy(otherCamera.bytes + kProjectionOffset, otherProjection, sizeof otherProjection);
            float otherViewProjection[kProjectionRows] {};
            ViewProjection(otherViewProjection, 3.5f);
            std::memcpy(otherCamera.bytes + kViewProjectionAt, otherViewProjection, sizeof otherViewProjection);
            ComPtr<ID3D11Buffer> otherLightBuffer = makeOtherBuffer(sizeof otherLight, &otherLight);
            ComPtr<ID3D11Buffer> otherCameraBuffer = makeOtherBuffer(uint32_t(sizeof otherCamera), &otherCamera);
            CHECK(otherLightBuffer != nullptr && otherCameraBuffer != nullptr);
            ComPtr<ID3D11Query> otherDone;
            D3D11_QUERY_DESC otherEventDesc { D3D11_QUERY_EVENT, 0 };
            CHECK(SUCCEEDED(other->CreateQuery(&otherEventDesc, &otherDone)) && otherDone != nullptr);
            const auto waitOther = [&otherContext, &otherDone]() {
                if (!otherDone)
                    return;
                otherContext->End(otherDone.Get());
                otherContext->Flush();
                otherContext->GetData(otherDone.Get(), nullptr, 0, 0);
            };
            SunReader moved;
            LightBytes firstLight {};
            SetFrameLight(4, firstLight);
            context->UpdateSubresource(lightBuffer.Get(), 0, nullptr, &firstLight, 0, 0);
            CHECK(moved.Copy(context.Get(), lightBuffer.Get(), cameraBuffer.Get(), 4));
            waitForCopies();
            const SunRead before = moved.Read(context.Get(), 4 + kNewestReadAge, true);
            CHECK(before.light);
            CHECK(std::fabs(moved.Facts().direction[2] - FrameDirectionZ(4)) < 0.01f);

            CHECK(moved.Copy(otherContext.Get(), otherLightBuffer.Get(), otherCameraBuffer.Get(), 10));
            waitOther();
            const SunRead after = moved.Read(otherContext.Get(), 10 + kNewestReadAge, true);
            CHECK(after.light); // the pair was rebuilt on the new device, so its copy landed
            CHECK(std::fabs(moved.Facts().direction[2] - (-1.0f)) < 0.01f);
            CHECK(std::fabs(moved.Projection()[0] - 3.5f) < 1.0e-4f);
        }
    }

    // ---- the copies are bounds-checked: a null buffer is not a copy, and the frame's slot is not written by it
    {
        CHECK(!reader.Copy(context.Get(), nullptr, cameraBuffer.Get(), 20));
        CHECK(!reader.Copy(context.Get(), lightBuffer.Get(), nullptr, 20));
        CHECK(!reader.Copy(nullptr, lightBuffer.Get(), cameraBuffer.Get(), 20));
        const SunRead read = reader.Read(context.Get(), 20 + kNewestReadAge);
        CHECK(!read.light && !read.notReady);
    }
    return Finish("shafts_ring");
}
