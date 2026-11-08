// Ambient occlusion: the pure rules (ext/ffxiv/engine/AoRules.h). No device, no shader, no file.
//
// The rules are what keeps the pass fail-open: which draw is a reader of the engine's occlusion texture and which is the engine's 4x4
// placeholder, which reader has the inputs the pass needs, what a texel of ours stands for in the depth, when the key is today's picture, when
// it pauses and looks again, and what the frame remembers about the engine's texture.
#include "check.h"

#include "ext/ffxiv/engine/AoRules.h"

#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace ext::ffxiv::engine::ao;

namespace
{
// A reader that passes every rule: the shape measured in a session (a 1920x1080 occlusion texture inside a 3840x2160 allocation, the
// render window at the +0.5 origin the engine uses, the camera table at its full size).
ReaderFacts Good()
{
    ReaderFacts facts;
    facts.hasOcclusion = true;
    facts.hasDepth = true;
    facts.hasNormal = true;
    facts.hasCamera = true;
    facts.occlusionFormat = kOcclusionFormat;
    facts.occlusionWidth = 1920;
    facts.occlusionHeight = 1080;
    facts.occlusionSamples = 1;
    facts.depthWidth = 3840;
    facts.depthHeight = 2160;
    facts.depthSamples = 1;
    facts.normalWidth = 3840;
    facts.normalHeight = 2160;
    facts.normalSamples = 1;
    facts.cameraBytes = kCameraBytes;
    facts.window[0] = 0.5f;
    facts.window[1] = 0.5f;
    facts.window[2] = 1920.0f;
    facts.window[3] = 1080.0f;
    return facts;
}

Params Tuned()
{
    Params params;
    params.on = true;
    return params;
}

bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }
} // namespace

int main()
{
    // ---- the reader's inputs: the packed word the shader table carries
    {
        Inputs inputs;
        CHECK(!inputs.HasDepth() && !inputs.HasNormal() && !inputs.HasCamera());
        CHECK(!inputs.Complete());
        CHECK_EQ(PackInputs(inputs), 0u); // 0 is "this module read nothing here", so it can never collide with a real packing

        inputs.depth = 0;
        inputs.normal = 7;
        inputs.camera = 1;
        CHECK(inputs.Complete());
        const uint32_t packed = PackInputs(inputs);
        CHECK(packed != 0u);
        const Inputs back = UnpackInputs(packed);
        CHECK_EQ(uint32_t(back.depth), 0u); // slot 0 packs to 1, so a zero field really is "missing"
        CHECK_EQ(uint32_t(back.normal), 7u);
        CHECK_EQ(uint32_t(back.camera), 1u);
        CHECK(back.Complete());

        // A slot that is not a texture slot is not an input, however the reflection wrote it.
        Inputs tooHigh;
        tooHigh.depth = uint8_t(kTextureSlots);
        tooHigh.camera = uint8_t(kConstantBufferSlots);
        CHECK(!tooHigh.HasDepth() && !tooHigh.HasCamera());
        CHECK_EQ(PackInputs(tooHigh), 0u);

        // The names are the engine's own, and the occlusion one is the shared table's (one row per knob there).
        CHECK(std::strcmp(kDepthName, "g_SamplerDepth") == 0);
        CHECK(std::strcmp(kNormalName, "g_SamplerGBuffer") == 0);
        CHECK(std::strcmp(kCameraName, "g_CameraParameter") == 0);
    }

    // ---- the reader's slots: the TEXTURE binding's register, never its sampler's
    //
    // The effect compiler declares a texture and its sampler under one name and writes the pair into the reflection as `<name>.T` and `<name>.S`,
    // and it writes every sampler before every texture. A scan that accepts any binding type therefore stamps the sampler's register, and the
    // pass reads whatever texture another shader bound there.
    {
        auto row = [](const char* name, uint32_t type, uint32_t bind) {
            return ext::ffxiv::diag::ShaderBinding { name, uint32_t(std::strlen(name)), type, bind, 1 };
        };
        // The capture's own RDEF order: samplers first, then textures, then the constant buffers. This is the reader whose refusal line named
        // the normal at slot 8 - its sampler's register - while its normal's texture is at t9.
        const ext::ffxiv::diag::ShaderBinding ambient[] = {
            row("g_SamplerGBuffer.S", ext::ffxiv::diag::kBindSampler, 8),
            row("g_SamplerGBuffer.T", ext::ffxiv::diag::kBindTexture, 9),
            row("g_SamplerDepth.S", ext::ffxiv::diag::kBindSampler, 0),
            row("g_SamplerDepth.T", ext::ffxiv::diag::kBindTexture, 0),
            row("g_CameraParameter", ext::ffxiv::diag::kBindConstantBuffer, 1),
        };
        const Inputs read = InputsOfBindings(ambient, std::size(ambient));
        CHECK_EQ(uint32_t(read.normal), 9u); // today it is 8: the sampler's slot
        CHECK_EQ(uint32_t(read.depth), 0u);
        CHECK_EQ(uint32_t(read.camera), 1u);
        CHECK(read.Complete());

        // A depth whose sampler sits at s3 and whose texture is at t7: the texture's slot is the one the draw binds.
        const ext::ffxiv::diag::ShaderBinding shifted[] = {
            row("g_SamplerDepth.S", ext::ffxiv::diag::kBindSampler, 3),
            row("g_SamplerDepth.T", ext::ffxiv::diag::kBindTexture, 7),
        };
        CHECK_EQ(uint32_t(InputsOfBindings(shifted, std::size(shifted)).depth), 7u); // today it is 3

        // A sampler on its own declares no slot this module can read, a slot past D3D11's ceiling is not an input, and an empty table or a
        // row with no name is "nothing read" rather than a guessed slot.
        const ext::ffxiv::diag::ShaderBinding samplerOnly[] = { row("g_SamplerGBuffer.S", ext::ffxiv::diag::kBindSampler, 8) };
        CHECK(!InputsOfBindings(samplerOnly, std::size(samplerOnly)).HasNormal());
        const ext::ffxiv::diag::ShaderBinding tooHigh[] = { row("g_SamplerDepth.T", ext::ffxiv::diag::kBindTexture, kTextureSlots) };
        CHECK(!InputsOfBindings(tooHigh, std::size(tooHigh)).HasDepth());
        CHECK(!InputsOfBindings(nullptr, 0).Complete());
        CHECK_EQ(PackInputs(InputsOfBindings(nullptr, 0)), 0u);
        const ext::ffxiv::diag::ShaderBinding nameless[] = { { nullptr, 0, ext::ffxiv::diag::kBindTexture, 9, 1 } };
        CHECK(!InputsOfBindings(nameless, std::size(nameless)).HasNormal());

        // The first texture carrying a name wins, exactly as the reflection lists it.
        const ext::ffxiv::diag::ShaderBinding twice[] = {
            row("g_SamplerGBuffer.T", ext::ffxiv::diag::kBindTexture, 9),
            row("g_SamplerGBuffer.T", ext::ffxiv::diag::kBindTexture, 12),
        };
        CHECK_EQ(uint32_t(InputsOfBindings(twice, std::size(twice)).normal), 9u);

        // A texture that carries the camera's name is not the camera table: only a constant buffer can be, and only below D3D11's 14.
        const ext::ffxiv::diag::ShaderBinding cameraTexture[] = { row("g_CameraParameter", ext::ffxiv::diag::kBindTexture, 3) };
        CHECK(!InputsOfBindings(cameraTexture, std::size(cameraTexture)).HasCamera());
        const ext::ffxiv::diag::ShaderBinding cameraTooHigh[] = {
            row("g_CameraParameter.T", ext::ffxiv::diag::kBindConstantBuffer, kConstantBufferSlots)
        };
        CHECK(!InputsOfBindings(cameraTooHigh, std::size(cameraTooHigh)).HasCamera());
    }

    // ---- the tuning: ranges, the identity, and the quality's steps
    {
        CHECK(Near(ClampRadius(std::numeric_limits<float>::quiet_NaN()), kRadiusDefault));
        CHECK(Near(ClampRadius(0.0f), kMinRadius));
        CHECK(Near(ClampRadius(9.0f), kMaxRadius));
        CHECK(Near(ClampRadius(2.5f), 2.5f));
        CHECK(Near(ClampStrength(std::numeric_limits<float>::quiet_NaN()), kStrengthDefault));
        CHECK(Near(ClampStrength(-1.0f), kMinStrength));
        CHECK(Near(ClampStrength(3.0f), kMaxStrength));
        CHECK(Near(ClampBlend(std::numeric_limits<float>::quiet_NaN()), kBlendDefault));
        CHECK(Near(ClampBlend(-0.5f), kMinBlend));
        CHECK(Near(ClampBlend(2.0f), kMaxBlend));

        CHECK_EQ(Steps(Quality::Low), 4u);
        CHECK_EQ(Steps(Quality::Medium), 8u);
        CHECK_EQ(Steps(Quality::High), 16u);
        CHECK(ParseQuality("low") == Quality::Low);
        CHECK(ParseQuality("high") == Quality::High);
        CHECK(ParseQuality("medium") == Quality::Medium);
        CHECK(ParseQuality("HIGH") == Quality::Medium); // a value the ini does not name reads as the default, never as a guess
        CHECK(ParseQuality("") == Quality::Medium);
        CHECK(std::strcmp(QualityName(ParseQuality("low")), "low") == 0);
        CHECK(std::strcmp(QualityName(ParseQuality("high")), "high") == 0);
        CHECK(std::strcmp(QualityName(ParseQuality("medium")), "medium") == 0);
    }

    // ---- the feature bit: off, and both "no occlusion is applied" values, are today's picture
    {
        Params params;
        params.on = false;
        CHECK(IsIdentity(params));
        CHECK(!Wants(params));

        params.on = true;
        CHECK(!IsIdentity(params));
        CHECK(Wants(params));

        params.strength = 0.0f;
        CHECK(IsIdentity(params)); // strength 0 applies nothing at all, so nothing runs
        CHECK(!Wants(params));

        params.strength = 1.0f;
        params.blend = 0.0f;
        CHECK(IsIdentity(params)); // the engine's own texture, untouched
        CHECK(!Wants(params));

        params.blend = std::numeric_limits<float>::quiet_NaN();
        CHECK(IsIdentity(params)); // a broken value never turns the pass on
        params.blend = 0.5f;
        params.strength = std::numeric_limits<float>::quiet_NaN();
        CHECK(IsIdentity(params));
    }

    // ---- what one texel comes to
    {
        // The strength scales the darkening, and both ends are clipped: 1 is unoccluded, 0 is fully occluded.
        CHECK(Near(Factor(1.0f, 1.0f), 1.0f));
        CHECK(Near(Factor(0.0f, 1.0f), 0.0f));
        CHECK(Near(Factor(0.5f, 1.0f), 0.5f));
        CHECK(Near(Factor(0.5f, 0.0f), 1.0f)); // the test's "strength 0 = pure 1.0"
        CHECK(Near(Factor(0.5f, 2.0f), 0.0f)); // twice the algorithm's own answer, clipped at fully occluded
        CHECK(Near(Factor(0.0f, 3.0f), 0.0f));
        CHECK(Near(Factor(0.25f, 0.5f), 0.625f));

        // Two visibility factors in the same 1 = unoccluded convention compose by multiplication, and the blend takes that product part of
        // the way. A term of 1.0 is the identity at every blend, which is what stops an empty search from deleting the engine's occlusion; a
        // blend of 0 is the engine's own value; and the result never leaves 0..1.
        CHECK(Near(Mix(0.3f, 1.0f, 1.0f), 0.3f));  // a no-op term is the identity (a linear mix made this 1.0)
        CHECK(Near(Mix(0.6f, 0.6f, 1.0f), 0.36f)); // two visibilities compose (a linear mix made this 0.6)
        CHECK(Near(Mix(0.3f, 0.9f, 1.0f), 0.27f));
        CHECK(Near(Mix(0.3f, 0.9f, 0.0f), 0.3f));
        CHECK(Near(Mix(0.3f, 0.9f, 0.5f), 0.285f));
        CHECK(Near(Mix(0.0f, 0.0f, 1.0f), 0.0f));
        CHECK(Near(Mix(1.0f, 2.0f, 1.0f), 1.0f));   // clipped at unoccluded
        CHECK(Near(Mix(0.5f, -3.0f, 1.0f), 0.0f));  // and at fully occluded
    }

    // ---- which draw is a reader, and which is the engine's own placeholder
    {
        CHECK(IsReaderTarget(kOcclusionFormat, 1920, 1080));
        CHECK(!IsReaderTarget(kOcclusionFormat, 4, 4));         // the 4x4 placeholder the engine binds on a few draws
        CHECK(!IsReaderTarget(kOcclusionFormat, 8, 1080));      // too narrow
        CHECK(!IsReaderTarget(28 /* R8G8B8A8_UNORM */, 1920, 1080)); // another format is another texture
        CHECK(!IsReaderTarget(0, 0, 0));
    }

    // ---- the validation, one reason at a time
    {
        CHECK(Classify(Good()) == Decline::None);

        ReaderFacts facts = Good();
        facts.replay = true;
        CHECK(Classify(facts) == Decline::Replay);

        facts = Good();
        facts.hasOcclusion = false;
        CHECK(Classify(facts) == Decline::NoOcclusion);

        facts = Good();
        facts.occlusionFormat = 28;
        CHECK(Classify(facts) == Decline::OcclusionShape);

        facts = Good();
        facts.occlusionWidth = 4;
        facts.occlusionHeight = 4;
        CHECK(Classify(facts) == Decline::OcclusionShape);

        facts = Good();
        facts.occlusionSamples = 2; // the pass copies it into a single-sample target, and that copy has no return code
        CHECK(Classify(facts) == Decline::OcclusionShape);

        facts = Good();
        facts.hasDepth = false;
        CHECK(Classify(facts) == Decline::NoDepth);

        facts = Good();
        facts.depthSamples = 4;
        CHECK(Classify(facts) == Decline::NoDepth); // a multisampled depth cannot be read at a texel

        facts = Good();
        facts.depthWidth = 0; // an empty binding: nothing was measured
        CHECK(Classify(facts) == Decline::NoDepth);

        facts = Good();
        facts.hasNormal = false;
        CHECK(Classify(facts) == Decline::NoNormal);

        facts = Good();
        facts.normalSamples = 2;
        CHECK(Classify(facts) == Decline::NoNormal);

        facts = Good();
        facts.hasCamera = false;
        CHECK(Classify(facts) == Decline::NoCamera);

        facts = Good();
        facts.cameraBytes = kCameraBytes - 16;
        CHECK(Classify(facts) == Decline::NoCamera); // the matrices are read past the end of a smaller table

        facts = Good();
        facts.normalWidth = 1920;
        CHECK(Classify(facts) == Decline::SizeMismatch); // the normal is not the depth's allocation

        facts = Good();
        facts.depthWidth = 320;
        facts.depthHeight = 180;
        facts.normalWidth = 320;
        facts.normalHeight = 180;
        facts.window[2] = 320.0f;
        facts.window[3] = 180.0f;
        CHECK(Classify(facts) == Decline::None); // the smallest scene still counts (a preview-sized allocation)

        facts = Good();
        facts.depthWidth = 256;
        facts.depthHeight = 144;
        facts.normalWidth = 256;
        facts.normalHeight = 144;
        CHECK(Classify(facts) == Decline::SizeMismatch);

        facts = Good();
        facts.window[2] = 0.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch); // an empty render window

        facts = Good();
        facts.window[2] = 8.0f; // a window a few texels wide is a preview's scratch target, not the scene
        facts.window[3] = 8.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);

        // Narrow but full height: 8 texels wide over 1,080 tall. Its region is not empty (3 x 539 texels at this mapping), so neither the
        // "no whole texel of ours" rule nor the height bound can stand in for the width bound - the 8x8 case above is refused by its height.
        facts = Good();
        facts.window[2] = 8.0f;
        facts.window[3] = 1080.0f;
        CHECK(!RegionOf(facts).Empty());
        CHECK(Classify(facts) == Decline::SizeMismatch);
        CHECK_EQ(kMinWindowWidth, 16u);

        // A small but real window is served: the pass is not tied to a display size.
        facts = Good();
        facts.depthWidth = 64;
        facts.depthHeight = 64;
        facts.normalWidth = 64;
        facts.normalHeight = 64;
        facts.occlusionWidth = 64;
        facts.occlusionHeight = 64;
        facts.window[0] = 0.0f;
        facts.window[1] = 0.0f;
        facts.window[2] = 64.0f;
        facts.window[3] = 64.0f;
        CHECK(Classify(facts) == Decline::None);

        facts = Good();
        facts.window[0] = -1.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);

        facts = Good();
        facts.window[0] = 1921.0f; // the window starts past the depth's width, once its own width is added
        CHECK(Classify(facts) == Decline::SizeMismatch);

        facts = Good();
        facts.window[1] = 2160.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);

        // Half a pixel of slack: a window that ends half a texel past the allocation is the engine's +0.5 origin, not a broken binding.
        facts = Good();
        facts.window[0] = float(facts.depthWidth) - facts.window[2] + 0.5f;
        CHECK(Classify(facts) == Decline::None);
        facts.window[0] += 1.0f;
        CHECK(Classify(facts) == Decline::SizeMismatch);

        // Every reason has a line, and the table covers the enum.
        CHECK_EQ(kDeclineReasonCount, 13u);
        for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        {
            const char* name = DeclineName(Decline(i));
            CHECK(name != nullptr && name[0] != '\0');
        }
        CHECK(std::strcmp(DeclineName(Decline::None), "run") == 0);
        CHECK(DeclineName(Decline(kDeclineReasonCount)) != nullptr); // out of range answers, it does not read past the table
    }

    // ---- the shader stamp: the whole of what this module remembers about a shader, packed into one word
    //
    // The word is stored on the shader object itself (AoSwap.cpp's detail::StampShader / detail::ReadStamp), which is what a device test drives:
    // a stamp written on one shader survives five thousand later creations and an unstamped shader reads 0. What is pinned here is the packing
    // those two calls put on the object and read back.
    {
        const Inputs reader { 0, 7, 13 };
        const uint32_t packed = PackInputs(reader);
        CHECK(packed != 0);
        CHECK(UnpackInputs(packed).Complete());
        CHECK_EQ(uint32_t(UnpackInputs(packed).depth), 0u);
        CHECK_EQ(uint32_t(UnpackInputs(packed).normal), 7u);
        CHECK_EQ(uint32_t(UnpackInputs(packed).camera), 13u);

        // A shader this module never read carries no word at all, which is a refusal and not a pass with guessed slots.
        CHECK_EQ(PackInputs(Inputs {}), 0u);
        CHECK(!UnpackInputs(0).Complete());
        CHECK(!UnpackInputs(0).HasDepth());

        // Every field round-trips, including the "this shader does not declare it" state, and no field can reach another. The width of a field
        // holds every slot D3D11 can bind, so a reader that declares its depth past 16 is the same reader as one that declares it at 0.
        for (const uint32_t depth : { 0u, 7u, 15u, 16u, 31u, 40u, 100u, 127u })
        {
            const Inputs one { uint8_t(depth), kSlotMissing, kSlotMissing };
            const Inputs back = UnpackInputs(PackInputs(one));
            CHECK_EQ(uint32_t(back.depth), depth);
            CHECK_EQ(uint32_t(back.normal), uint32_t(kSlotMissing));
            CHECK_EQ(uint32_t(back.camera), uint32_t(kSlotMissing));
        }
        for (uint32_t slot = 0; slot < 14; ++slot)
        {
            const Inputs all { 127, 100, uint8_t(slot) };
            const Inputs back = UnpackInputs(PackInputs(all));
            CHECK_EQ(uint32_t(back.depth), 127u);
            CHECK_EQ(uint32_t(back.normal), 100u);
            CHECK_EQ(uint32_t(back.camera), slot);
        }
    }

    // ---- the mapping from an occlusion texel to the depth texel behind it
    {
        // The session's own shape: a 1920x1080 occlusion texture over a 3840x2160 depth allocation, so one texel of ours covers a 2x2 block of
        // depth texels - and the mapping has no origin, because the readers sample the texture by the allocation's fraction, not by the window's.
        const Mapping mapping = MappingOf(Good());
        CHECK(Near(mapping.scaleX, 2.0f));
        CHECK(Near(mapping.scaleY, 2.0f));
        CHECK(Near(mapping.DepthX(0.0f), 0.0f));
        CHECK(Near(mapping.DepthY(0.0f), 0.0f));
        CHECK(Near(mapping.DepthX(20.0f), 40.0f));
        CHECK(Near(mapping.DepthY(540.0f), 1080.0f));

        // The occlusion texture at the depth's own size: one texel of ours is one depth texel, and the window's +0.5 origin does not move it.
        ReaderFacts equal = Good();
        equal.occlusionWidth = 3840;
        equal.occlusionHeight = 2160;
        const Mapping identity = MappingOf(equal);
        CHECK(Near(identity.scaleX, 1.0f));
        CHECK(Near(identity.scaleY, 1.0f));
        CHECK(Near(identity.DepthX(0.0f), 0.0f));
        CHECK(Near(identity.DepthX(1919.0f), 1919.0f));

        // The two ratios are different numbers whenever the depth allocation is not the occlusion texture's aspect: a 2560x2160 depth
        // allocation behind a 1920x1080 occlusion texture is 4:3 across and 2:1 down. Every fixture above is square in ratio, which is why this
        // one is written out: a mapping that swapped its two ratios would answer correctly there and wrongly here.
        ReaderFacts tall = Good();
        tall.depthWidth = 2560;
        tall.depthHeight = 2160;
        tall.normalWidth = 2560;
        tall.normalHeight = 2160;
        const Mapping uneven = MappingOf(tall);
        CHECK(Near(uneven.scaleX, 4.0f / 3.0f));
        CHECK(Near(uneven.scaleY, 2.0f));
        CHECK(Near(uneven.DepthX(20.0f), 26.6667f));
        CHECK(Near(uneven.DepthY(540.0f), 1080.0f));

        // An empty target is the identity, not a division by zero.
        ReaderFacts empty;
        const Mapping none = MappingOf(empty);
        CHECK(Near(none.scaleX, 1.0f));
    }

    // ---- the region the pass computes: the texels of ours whose whole depth block is inside the render window, and no others
    {
        // The session: a 1920x1080 window at the allocation's origin over a 1920x1080 occlusion texture is its top-left 960x540 quadrant. The
        // engine's own producer fills that quadrant and every reader samples it through the allocation-relative coordinate, so this is the part
        // that lines up with the engine's answer.
        const Region quadrant = RegionOf(Good());
        CHECK_EQ(quadrant.x, 1u); // the window's +0.5 origin leaves the first column half outside it, so it is not ours
        CHECK_EQ(quadrant.y, 1u);
        CHECK_EQ(quadrant.width, 959u);
        CHECK_EQ(quadrant.height, 539u);
        CHECK_EQ(quadrant.Right(), 960u); // the quadrant ends where the engine's own producer's viewport does
        CHECK_EQ(quadrant.Bottom(), 540u);

        // The window floored to the texel grid (what the draw path measures): the whole quadrant.
        ReaderFacts floored = Good();
        floored.window[0] = 0.0f;
        floored.window[1] = 0.0f;
        const Region whole = RegionOf(floored);
        CHECK_EQ(whole.width, 960u);
        CHECK_EQ(whole.height, 540u);

        // A window at the allocation's size over an occlusion texture of the same size: the whole target.
        ReaderFacts same = floored;
        same.occlusionWidth = 3840;
        same.occlusionHeight = 2160;
        same.window[2] = 3840.0f;
        same.window[3] = 2160.0f;
        const Region full = RegionOf(same);
        CHECK_EQ(full.width, 3840u);
        CHECK_EQ(full.height, 2160u);

        // A window that is not at the origin: the region starts where the window does, in occlusion texels, and stops at its far edge.
        ReaderFacts offset = floored;
        offset.window[0] = 1920.0f; // the bottom-right quadrant of the depth allocation
        offset.window[1] = 1080.0f;
        const Region bottomRight = RegionOf(offset);
        CHECK_EQ(bottomRight.x, 960u);
        CHECK_EQ(bottomRight.y, 540u);
        CHECK_EQ(bottomRight.width, 960u);
        CHECK_EQ(bottomRight.height, 540u);

        // The occlusion target larger than the depth allocation (a render scale below 1): the window's own half-pixel origin puts its right
        // edge at occlusion texel 3841, one past the target. The region is what the pass writes, so it stops at the target's last texel; a
        // region that answered 3841 would address a texel the target does not have. No session's shape is this one today, so this is the
        // guard's own case rather than a picture anyone sees.
        ReaderFacts larger = Good();
        larger.occlusionWidth = 3840;
        larger.occlusionHeight = 2160;
        larger.depthWidth = 1920;
        larger.depthHeight = 1080;
        larger.normalWidth = 1920;
        larger.normalHeight = 1080;
        larger.window[0] = 0.0f;
        larger.window[1] = 0.0f;
        larger.window[2] = 1920.5f; // the half-pixel slack the engine's viewport origin leaves
        larger.window[3] = 1080.0f;
        const Region capped = RegionOf(larger);
        CHECK_EQ(capped.width, 3840u);
        CHECK_EQ(capped.Right(), 3840u);
        CHECK_EQ(capped.height, 2160u);

        // A window of a few texels inside an occlusion texture so small that one texel of ours covers more than the window: nothing to compute,
        // so the validation refuses it rather than running a pass that would write no texel of its own target.
        ReaderFacts thin = floored;
        thin.occlusionWidth = 16;
        thin.occlusionHeight = 16;
        thin.window[2] = 16.0f;
        thin.window[3] = 16.0f;
        CHECK(RegionOf(thin).Empty());
        CHECK(Classify(thin) == Decline::SizeMismatch);

        // An empty target has no region at all.
        ReaderFacts empty;
        CHECK(RegionOf(empty).Empty());
    }

    // ---- the shader's constant block
    {
        Params params = Tuned();
        params.radius = 1.5f;
        params.strength = 0.75f;
        params.blend = 0.5f;
        params.quality = Quality::High;
        const Constants constants = ConstantsOf(Good(), params);
        CHECK_EQ(sizeof(Constants), size_t(80)); // five float4s, the shader's Params
        CHECK(Near(constants.window[0], 0.5f));
        CHECK(Near(constants.window[2], 1920.0f));
        CHECK(Near(constants.mapping[0], 2.0f)); // depth texels per occlusion texel
        CHECK(Near(constants.mapping[2], 1920.0f));
        CHECK(Near(constants.mapping[3], 1080.0f));
        CHECK(Near(constants.sizes[0], 3840.0f));
        CHECK(Near(constants.sizes[1], 2160.0f));
        CHECK(Near(constants.sizes[2], 16.0f)); // the steps of the quality
        CHECK(Near(constants.sizes[3], 1.5f));  // the radius
        CHECK(Near(constants.tuning[0], 0.75f));
        CHECK(Near(constants.tuning[1], 0.5f));
        CHECK(Near(constants.tuning[2], kHorizonBias));
        CHECK(Near(kHorizonBias, 0.02f)); // the tangent plane's bias, in cosine units: a tap this close to the plane is the pixel's own surface
        CHECK(Near(constants.tuning[3], 0.0f));
        CHECK(Near(constants.region[0], 1.0f));
        CHECK(Near(constants.region[2], 959.0f)); // the quadrant the pass writes, from the same rule
        CHECK(Near(constants.region[3], 539.0f));
    }

    // ---- the lines a session reads: the refused first reader carries that reader's own shape, and the served count per frame
    {
        // A forward draw that declares the normal but no depth and no camera: the line has to make that visible, because it is the one refusal a
        // session cannot otherwise tell from a shader whose slots were never stamped.
        const std::string line = FormatRefusedReader(Decline::NoDepth, Good(), Inputs { kSlotMissing, 7, kSlotMissing });
        CHECK(line.rfind("Ambient occlusion:", 0) == 0);
        CHECK(line.find(DeclineName(Decline::NoDepth)) != std::string::npos);
        CHECK(line.find("occlusion 1920x1080 in format 87") != std::string::npos);
        CHECK(line.find("depth 3840x2160 at slot none") != std::string::npos);
        CHECK(line.find("normal 3840x2160 at slot 7") != std::string::npos);
        CHECK(line.find("camera 352 bytes at slot none") != std::string::npos);
        CHECK(line.find("window 1920x1080 at ") != std::string::npos);

        // The slot text of a declared name, and the served-per-frame line the card and the log carry.
        CHECK_EQ(FormatSlot(0), std::string("0"));
        CHECK_EQ(FormatSlot(13), std::string("13"));
        CHECK_EQ(FormatSlot(kSlotMissing), std::string("none"));
        CHECK(FormatServed(299).find("299 reader draws") != std::string::npos);
        CHECK(FormatServed(0).find("0 reader draws") != std::string::npos);
        std::printf("  %s\n  %s\n", line.c_str(), FormatServed(299).c_str()); // what a session's log shows
    }

    // ---- the frame's memory of the engine's texture
    {
        FrameCache cache;
        cache.Begin(7);
        CHECK(cache.MayRun()); // the first reader of a frame gets the attempt
        cache.TakeAttempt();
        CHECK(!cache.MayRun()); // and only one reader per frame does

        int engineTexture = 0, otherTexture = 0;
        int engineView = 0, ourTexture = 0;
        cache.Record(&engineTexture, &engineView, &ourTexture);
        CHECK(cache.Has(&engineTexture));
        CHECK(!cache.Has(&otherTexture)); // the engine's 4x4 placeholder and a second texture are not served
        CHECK(!cache.Has(nullptr));
        CHECK(cache.Ours() == &ourTexture);

        // The same frame keeps its entry, and the two things the engine keeps constant across a frame are still there.
        cache.Begin(7);
        CHECK(cache.Has(&engineTexture));
        cache.Serve();
        CHECK(cache.Has(&engineTexture));

        // The next frame forgets everything: the engine alternates between two occlusion textures, so nothing is carried over.
        const int nextFrameTexture = 1;
        cache.Begin(8);
        CHECK(!cache.Has(&engineTexture));
        CHECK(!cache.Has(&nextFrameTexture));
        CHECK(cache.Ours() == nullptr);
        CHECK(cache.MayRun());

        // A frame token that does not advance expires the entry too, so a session whose present count stands still costs one pass every few
        // thousand readers instead of a texture that stays at the first frame's picture.
        FrameCache stale;
        stale.Begin(3);
        stale.Record(&engineTexture, &engineView, &ourTexture);
        for (uint32_t i = 0; i < kMaxServedDraws - 1; ++i)
            stale.Serve();
        CHECK(stale.Has(&engineTexture));
        stale.Serve();
        CHECK(!stale.Has(&engineTexture));
        CHECK(stale.Ours() == nullptr);
    }

    // ---- the frame's three inputs: the first reader of a frame does not always declare them, and the frame's first one used to decide
    {
        // The three shapes one session measured, at the slots its reflections carried: a material draw (the camera table at cb3, neither
        // the depth nor the G-buffer normal), the deferred composite (the normal at t9, the camera at cb1, no depth) and the light draw (the
        // depth at t0, the camera at cb1, no normal). They alternate as a frame's first occlusion binding. `mainView` is the camera table's
        // device object, which is the identity of the view its readers draw (measured: all three shapes bind the same one).
        const int mainViewTag = 1, otherViewTag = 2; // the identities are only compared, never read
        const void* const mainView = &mainViewTag;
        const void* const otherView = &otherViewTag;
        const BoundInput materialDepth {}, materialNormal {}, materialCamera { true, false, 3, 0, 0, 0, 1024, mainView };
        const BoundInput lightDepth { true, true, 0, 3840, 2160, 1, 0, nullptr }, lightNormal {};
        const BoundInput lightCamera { true, false, 1, 0, 0, 0, 1024, mainView };
        const BoundInput compositeDepth {}, compositeNormal { true, true, 9, 3840, 2160, 1, 0, nullptr };
        const BoundInput compositeCamera { true, false, 1, 0, 0, 0, 1024, mainView };

        FrameInputs frame;
        CHECK(frame.Begin(11));     // a new token starts a frame
        CHECK(!frame.Begin(11));    // the same token is the same frame
        CHECK(!frame.Complete());
        // A material draw binds the camera table and none of the pass's textures: it contributes nothing, because a view is adopted with the
        // textures that belong to it.
        FrameInputs::Contribution taken = frame.Merge(materialCamera, materialDepth, materialNormal);
        CHECK(!taken.Kept() && !taken.otherView);
        CHECK(!frame.Complete() && !frame.hasView);
        // The composite supplies the normal at its own slot and is the reader the view comes from: the frame's camera is its table.
        taken = frame.Merge(compositeCamera, compositeDepth, compositeNormal);
        CHECK(taken.normal && taken.camera && !taken.depth && !taken.otherView);
        CHECK(frame.hasView && frame.view == mainView);
        CHECK(!frame.Complete());
        // The frame's facts are the reader's occlusion and window with the frame's inputs in place, and the frame is still waiting for the
        // depth: this is what a frame whose first reader declares no depth looks like.
        const ReaderFacts waiting = FactsOf(Good(), frame);
        CHECK(waiting.hasCamera && waiting.cameraBytes == 1024u);
        CHECK(waiting.hasNormal && !waiting.hasDepth);
        CHECK(Classify(waiting) == Decline::NoDepth);
        CHECK(IsWaitingDecline(Decline::NoDepth) && IsWaitingDecline(Decline::NoNormal) && IsWaitingDecline(Decline::NoCamera));
        CHECK(!IsWaitingDecline(Decline::SizeMismatch) && !IsWaitingDecline(Decline::OcclusionShape) &&
              !IsWaitingDecline(Decline::NoOcclusion));
        const ReaderFacts waitingNormal = waiting;
        const Inputs waitingSlots = frame.Slots();
        // Another usable normal cannot move the frame's binding.
        taken = frame.Merge(compositeCamera, compositeDepth, BoundInput { true, true, 7, 3840, 2160, 1, 0, nullptr });
        CHECK(!taken.Kept());

        // The light draw supplies the depth with the same camera table, and the frame is complete: the pass runs for a reader that declares
        // no normal at all.
        taken = frame.Merge(lightCamera, lightDepth, lightNormal);
        CHECK(taken.depth && !taken.camera && !taken.otherView);
        CHECK(frame.Complete());
        taken = frame.Merge(lightCamera, BoundInput { true, true, 4, 1920, 1080, 1, 0, nullptr }, lightNormal);
        CHECK(!taken.Kept()); // the first usable binding of an input is kept
        const ReaderFacts facts = FactsOf(Good(), frame);
        CHECK_EQ(facts.depthWidth, 3840u);
        CHECK_EQ(facts.normalWidth, 3840u);
        CHECK_EQ(facts.normalHeight, 2160u);
        CHECK_EQ(facts.cameraBytes, 1024u);
        CHECK(Classify(facts) == Decline::None);
        // The run line names each input at the slot the reader that supplied it declared.
        CHECK_EQ(uint32_t(frame.Slots().depth), 0u);
        CHECK_EQ(uint32_t(frame.Slots().normal), 9u);
        CHECK_EQ(uint32_t(frame.Slots().camera), 1u);

        // A frame whose readers span two views: the second view's depth and its camera table are not this frame's, and the frame is refused
        // instead of served with a search computed from two views' buffers (the depth has the same allocation's size, so only the camera's
        // identity tells them apart).
        FrameInputs mixed;
        mixed.Begin(15);
        CHECK(mixed.Merge(compositeCamera, compositeDepth, compositeNormal).normal); // the main view's normal, and its camera table
        const BoundInput otherCamera { true, false, 1, 0, 0, 0, 1024, otherView };
        const FrameInputs::Contribution crossed = mixed.Merge(otherCamera, lightDepth, lightNormal);
        CHECK(crossed.otherView);
        CHECK(!crossed.Kept()); // nothing of the other view's was taken
        CHECK(!mixed.depth.present);
        CHECK(Classify(FactsOf(Good(), mixed)) == Decline::NoDepth); // the frame itself still has one input missing
        CHECK(std::string(DeclineName(Decline::ViewMismatch)).find("another view") != std::string::npos);
        const std::string viewLine = FormatRefusedReader(Decline::ViewMismatch, FactsOf(Good(), mixed), mixed.Slots());
        CHECK(viewLine.find("the frame could not be served") != std::string::npos);
        CHECK(viewLine.find(DeclineName(Decline::ViewMismatch)) != std::string::npos);
        std::printf("  %s\n", viewLine.c_str()); // what a session's log shows for a frame whose readers span two views

        // A texture that arrives without a usable camera table cannot be attributed to a view, so it is not taken either.
        FrameInputs unattributed;
        unattributed.Begin(16);
        CHECK(!unattributed.Merge(BoundInput {}, compositeDepth, compositeNormal).Kept());
        CHECK(!unattributed.Complete() && !unattributed.hasView);

        // A binding the pass cannot read is not remembered, so it cannot block the frame from a later usable one, and the frame's facts do
        // not carry it.
        FrameInputs picky;
        picky.Begin(12);
        const BoundInput goodCamera { true, false, 1, 0, 0, 0, kCameraBytes, mainView };
        CHECK(!picky.Merge(goodCamera, BoundInput { true, false, 0, 3840, 2160, 1, 0, nullptr }, BoundInput {}).Kept()); // not a plain Texture2D
        CHECK(!picky.Merge(goodCamera, BoundInput { true, true, 0, 3840, 2160, 2, 0, nullptr }, BoundInput {}).Kept());  // multisampled
        CHECK(!picky.Merge(goodCamera, BoundInput { true, true, 0, 0, 0, 1, 0, nullptr }, BoundInput {}).Kept());        // empty
        CHECK(!picky.Complete());
        CHECK(!picky.Merge(BoundInput { true, false, 1, 0, 0, 0, kCameraBytes - 1, mainView },
                           BoundInput { true, true, 0, 3840, 2160, 1, 0, nullptr }, BoundInput {})
                   .Kept()); // a camera table too small for the matrices attributes nothing
        CHECK(!picky.Complete());
        CHECK(picky.Merge(goodCamera, BoundInput { true, true, 0, 3840, 2160, 1, 0, nullptr }, BoundInput {}).depth);
        CHECK(!picky.Complete()); // the depth is in hand with its view, the normal is not: the frame is not ready
        CHECK(picky.Merge(goodCamera, BoundInput {}, BoundInput { true, true, 9, 3840, 2160, 1, 0, nullptr }).normal);
        CHECK(picky.Complete());

        // Two inputs of different allocations are a frame this pass cannot serve, and that is a refusal of the frame, not a wait.
        FrameInputs mismatched;
        mismatched.Begin(13);
        CHECK(mismatched.Merge(goodCamera, BoundInput { true, true, 0, 3840, 2160, 1, 0, nullptr },
                               BoundInput { true, true, 9, 1920, 1080, 1, 0, nullptr })
                  .depth);
        CHECK(Classify(FactsOf(Good(), mismatched)) == Decline::SizeMismatch);

        // The waiting is bounded, so a frame no reader completes costs a fixed number of state queries: the bound is a literal a test can see.
        CHECK_EQ(kMaxFrameInputReaders, 64u);
        // A new frame forgets the last one's inputs, its view, its waiting flag and its reader count with them.
        CHECK(frame.Begin(14));
        CHECK(!frame.Complete());
        CHECK(frame.hasFrame);
        CHECK(!frame.hasView && frame.view == nullptr);
        CHECK(!frame.depth.present && !frame.normal.present && !frame.camera.present);
        CHECK(!frame.noted && frame.readers == 0u);

        const std::string line = FormatWaiting(Decline::NoDepth, waitingNormal, waitingSlots);
        CHECK(line.rfind("Ambient occlusion:", 0) == 0);
        CHECK(line.find("waits for another reader") != std::string::npos);
        CHECK(line.find(DeclineName(Decline::NoDepth)) != std::string::npos);
        CHECK(line.find("occlusion 1920x1080 in format 87") != std::string::npos);
        CHECK(line.find("depth 0x0 at slot none") != std::string::npos); // the input the frame is still missing
        CHECK(line.find("normal 3840x2160 at slot 9") != std::string::npos);
        CHECK(line.find("camera 1024 bytes at slot 1") != std::string::npos);
        std::printf("  %s\n", line.c_str()); // what a session's log shows while a frame waits
    }

    // ---- the pause's look: the frame the pause inspects is the frame that must be able to bring the module back, so one of its readers
    // waiting must not end the inspection or defer the next look. Without this the pause is a stand-down in disguise: every frame of the
    // measured session had a first reader that only declared part of the pass's inputs, and a look that stopped there never reached the reader
    // that would have run the pass.
    {
        Session session;
        uint64_t frame = 900;
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
        {
            bool counted = false;
            CHECK(!NoteFrame(session, counted, ++frame, true)); // a frame no reader completed
            CHECK(counted);
        }
        bool counted = false;
        CHECK(NoteFrame(session, counted, ++frame, true)); // the frame that pauses the module, and the pause's first look
        CHECK(session.Paused());
        const uint64_t probe = frame;

        // The pause's look frame is inspected as a whole: its first draw starts the look and every later draw of that same frame is part of
        // it (the look was granted to the frame, not to a count of draws).
        CHECK(session.Look(probe) == LookStep::Probe);

        const int mainViewTag = 1;
        const void* const mainView = &mainViewTag;
        const BoundInput camera { true, false, 1, 0, 0, 0, kCameraBytes, mainView };
        const BoundInput lightDepth { true, true, 0, 3840, 2160, 1, 0, nullptr };
        const BoundInput compositeNormal { true, true, 9, 3840, 2160, 1, 0, nullptr };
        FrameInputs inputs;
        inputs.Begin(probe);
        // A material draw declares the camera table at slot 3 and neither of the pass's textures, and a texture is only taken with the camera
        // table it is attributed to: it contributes nothing.
        CHECK(!inputs.Merge(BoundInput { true, false, 3, 0, 0, 0, kCameraBytes, mainView }, BoundInput {}, BoundInput {}).Kept());
        CHECK(!inputs.Complete());
        // The composite supplies the G-buffer normal and the camera table at slot 1: the frame is one input short and this reader waits.
        CHECK(inputs.Merge(camera, BoundInput {}, compositeNormal).normal);
        CHECK(!inputs.Complete());
        // That reader waits. While the module is paused this is the look doing its job, not a refused frame: nothing is counted and the look
        // is not deferred, so the frame's next readers are still inspected.
        CHECK(!NoteFrame(session, inputs.noted, probe, true));
        CHECK(!inputs.noted);
        CHECK(session.Look(probe) == LookStep::Probe);
        // A light draw of the same frame supplies the depth at slot 0: the pass can run at this reader, which is the reader the pause exists
        // to reach.
        CHECK(inputs.Merge(camera, lightDepth, BoundInput {}).depth);
        CHECK(inputs.Complete());
        CHECK(Classify(FactsOf(Good(), inputs)) == Decline::None); // the state RunFrame runs the pass in

        // The pass ran: the scene is back, so the pause and the streak are over and the next frame is inspected like any other.
        session.NoteRun();
        CHECK(!session.Paused());
        CHECK(session.Look(probe + 1) == LookStep::Frame);
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
        {
            bool again = false;
            CHECK(!NoteFrame(session, again, probe + 1 + i, false));
        }
        CHECK(!session.Paused()); // the streak starts over after a served frame

        // A look granted to a frame that is not the pause's own starts that frame from scratch (`ProbeBegin`), which is what lets the draw
        // path clear an attempt the frame already spent; the rest of that frame is `Probe` again.
        Session later;
        for (uint32_t i = 0; i < kDeclineLimit; ++i)
            later.NoteDecline(7);
        CHECK(later.Paused());
        CHECK(later.Look(7) == LookStep::Skip); // the frame that was refused has nothing left to inspect
        CHECK(later.Look(7 + kPauseFrames) == LookStep::ProbeBegin);
        CHECK(later.Look(7 + kPauseFrames) == LookStep::Probe);
        // A look that ends without serving its frame (a frame-level refusal, or simply the next frame arriving) sets the next look a whole
        // interval after the frame it looked at, so a build that cannot be served pays one bounded frame inspection per interval.
        later.NoteDecline(7 + kPauseFrames);
        CHECK(later.Look(7 + kPauseFrames + 1) == LookStep::Skip);
        CHECK(later.Look(7 + 2 * kPauseFrames) == LookStep::ProbeBegin);

        // A wait inside the look's frame defers nothing: when that frame ends without serving it, the next look is one interval after the frame
        // the look was on, not one interval after some later refusal.
        Session waited;
        bool waitedCount = false;
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
        {
            waitedCount = false;
            CHECK(!NoteFrame(waited, waitedCount, i, true));
        }
        waitedCount = false;
        CHECK(NoteFrame(waited, waitedCount, kDeclineLimit, true)); // the sixty-fourth frame in a row pauses the module, on a wait
        CHECK(waited.Look(kDeclineLimit) == LookStep::Probe);      // and the frame that paused it is the pause's first look
        bool waitCounted = false;
        CHECK(!NoteFrame(waited, waitCounted, kDeclineLimit, true)); // a reader of that frame waits
        CHECK(!waitCounted);
        CHECK(waited.Look(kDeclineLimit) == LookStep::Probe);                  // its next readers are still inspected
        CHECK(waited.Look(kDeclineLimit + 1) == LookStep::Skip);               // the frame is over and did not serve it
        CHECK(waited.Look(kDeclineLimit + kPauseFrames - 1) == LookStep::Skip);
        CHECK(waited.Look(kDeclineLimit + kPauseFrames) == LookStep::ProbeBegin);

        // A look granted long after the frame it was due (a menu held the pause for a while): when that frame ends without serving it, the next
        // look is one interval after the frame that was looked at - not the very next frame, which is what a lookAt left in the past would give.
        Session late;
        for (uint32_t i = 0; i < kDeclineLimit; ++i)
            late.NoteDecline(7);
        CHECK(late.Look(400) == LookStep::ProbeBegin); // granted far past the frame it was due on
        CHECK(late.Look(400) == LookStep::Probe);
        CHECK(late.Look(401) == LookStep::Skip);
        CHECK(late.Look(400 + kPauseFrames - 1) == LookStep::Skip);
        CHECK(late.Look(400 + kPauseFrames) == LookStep::ProbeBegin);

        // A present counter that stands still must not pause the module for good: one draw in kMaxPausedDraws grants the next look, and it is
        // a fresh start (a `ProbeBegin`, never nothing to inspect), because the look clears the frame's spent attempt.
        Session still;
        for (uint32_t i = 0; i < kDeclineLimit; ++i)
            still.NoteDecline(7);
        CHECK(still.Paused());
        uint32_t passedOn = 0;
        while (still.Look(7) == LookStep::Skip)
            ++passedOn;
        CHECK_EQ(passedOn, kMaxPausedDraws - 1);
    }

    // ---- one frame counts once: a frame whose first reader waits and whose last reader refuses is one frame, not two
    {
        FrameInputs frame;
        frame.Begin(77);
        CHECK(frame.TakeCount());   // the wait at the frame's first reader counts the frame
        CHECK(!frame.TakeCount());  // the frame-level refusal of the same frame counts nothing
        CHECK(!frame.TakeCount());
        CHECK(frame.Begin(78));
        CHECK(frame.TakeCount());   // a new frame counts again

        Session session;
        bool counted = false;
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
        {
            counted = false;
            CHECK(!NoteFrame(session, counted, i, true));
        }
        CHECK(!session.Paused());
        // A frame that waits and is then refused passes both declines through one count: the refusal's `NoteFrame` sees the flag and counts
        // nothing, so sixty-four such frames are still sixty-four frames in a row and not thirty-two.
        counted = false;
        CHECK(NoteFrame(session, counted, kDeclineLimit, true));
        CHECK(session.Paused());

        // The same rule, one frame short of the limit: sixty-two counted frames, then a frame whose first reader waits and whose last reader
        // is refused. That frame is the sixty-third, so the next frame's refusal is what pauses the module - a second count for it would pause
        // it a frame early, which is the double count this exists to prevent.
        Session once;
        bool taken = false;
        for (uint32_t i = 1; i < kDeclineLimit - 1; ++i)
        {
            taken = false;
            CHECK(!NoteFrame(once, taken, i, false));
            CHECK(taken);
        }
        CHECK(!once.Paused());
        taken = false;
        CHECK(!NoteFrame(once, taken, kDeclineLimit - 1, true)); // the sixty-third frame, at its first wait
        CHECK(taken && !once.Paused());
        CHECK(!NoteFrame(once, taken, kDeclineLimit - 1, false)); // its own frame-level refusal: the same frame
        CHECK(!once.Paused());
        taken = false;
        CHECK(NoteFrame(once, taken, kDeclineLimit, false)); // the next frame is the sixty-fourth, and it pauses
        CHECK(once.Paused());

        // A frame-level refusal of the pause's look ends the look, even when that frame's flag was already taken for a wait's line: a present
        // counter that stands still has to be able to reach the next look with nothing left to inspect.
        Session ended;
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
        {
            bool flag = false;
            NoteFrame(ended, flag, i, true);
        }
        bool endedFlag = false;
        CHECK(NoteFrame(ended, endedFlag, kDeclineLimit, true)); // paused on a wait: the frame is the pause's first look
        CHECK(ended.Look(kDeclineLimit) == LookStep::Probe);
        endedFlag = true; // the draw path's wait branch sets the frame's flag for the log's once-a-frame line
        CHECK(!NoteFrame(ended, endedFlag, kDeclineLimit, false));
        CHECK(ended.Look(kDeclineLimit) == LookStep::Skip); // the look is over: the same token is passed on
    }


    // ---- the refusal streak and the log gate
    {
        Session session;
        uint64_t frame = 500;
        CHECK(!session.Paused());
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
            CHECK(!session.NoteDecline(++frame));
        CHECK(!session.Paused());
        CHECK(session.NoteDecline(++frame)); // the sixty-fourth in a row is the one that pauses the module
        CHECK(session.Paused());
        CHECK(!session.NoteDecline(frame)); // a frame after the pause is not counted again

        // The limit itself, in literal frames: sixty-three leave the module running and the sixty-fourth pauses it. The loops above are written
        // against the constant, so they move with it; these numbers do not.
        Session literal;
        for (uint32_t i = 1; i < 64; ++i)
            CHECK(!literal.NoteDecline(1));
        CHECK(!literal.Paused());
        CHECK(literal.NoteDecline(1));
        CHECK(literal.Paused());
        CHECK_EQ(kDeclineLimit, 64u);

        Session recovered;
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
            recovered.NoteDecline(1);
        recovered.NoteRun(); // one pass that runs starts the streak over
        CHECK(!recovered.Paused());
        for (uint32_t i = 1; i < kDeclineLimit; ++i)
            CHECK(!recovered.NoteDecline(1));
        CHECK(!recovered.Paused());

        LogGate gate;
        CHECK(gate.Take(1));
        CHECK(!gate.Take(1)); // the same reason twice in a row is one line
        CHECK(gate.Take(2));
        unsigned written = 2;
        for (uint32_t tag = 3; written < kMaxLogLines; ++tag, ++written)
            CHECK(gate.Take(tag));
        CHECK(!gate.Take(999u)); // the budget is a session's, not a frame's
    }

    // ---- the lines a reader of the log sees
    {
        Params params = Tuned();
        params.quality = Quality::Low;
        params.radius = 2.0f;
        params.strength = 0.5f;
        params.blend = 0.25f;
        // The target's size and the two input allocations are three different numbers on purpose: the line has to name each one where it belongs.
        ReaderFacts facts = Good();
        facts.normalWidth = 1920;
        facts.normalHeight = 1080;
        const std::string run = FormatRun(params, facts, Inputs { 0, 6, 1 }, false, 0.0);
        CHECK(run.find("Ambient occlusion: 1920x1080") != std::string::npos);
        CHECK(run.find("4 steps") != std::string::npos); // the quality, in the number it stands for
        CHECK(run.find("depth 3840x2160 at 0") != std::string::npos);
        CHECK(run.find("normal 1920x1080 at 6") != std::string::npos);
        CHECK(run.find("camera 352 at 1") != std::string::npos);
        CHECK(run.find("not measured yet") != std::string::npos);
        CHECK(run.find("blend 0.25") != std::string::npos);
        const std::string measured = FormatRun(params, facts, Inputs { 0, 6, 1 }, true, 0.8125);
        CHECK(measured.find("0.8125 ms") != std::string::npos);
        std::printf("  %s\n", run.c_str()); // what a session's log shows for a run
        CHECK(FormatDecline(Decline::NoDepth).find("no usable depth") != std::string::npos);
        CHECK(FormatPaused().find(std::to_string(kDeclineLimit)) != std::string::npos);
        CHECK(FormatPaused().find("pauses") != std::string::npos); // a pause, not the end of the session
        CHECK(FormatPaused().find("off for this session") == std::string::npos);
        CHECK(FormatPaused().find(std::to_string(kPauseFrames)) != std::string::npos); // the wait is stated in frames ...
        CHECK(FormatPaused().find("2.7 seconds") != std::string::npos);                // ... and in the seconds it comes to in the measured session
    }

    return Finish("ao_rules");
}
