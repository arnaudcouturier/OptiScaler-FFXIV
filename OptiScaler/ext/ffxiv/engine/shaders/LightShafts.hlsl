// LightShafts.hlsl: the sun's light shafts, added to a copy of the lit scene before the engine's tone map reads it.
//
// Five pixel shaders and one vertex shader, all driven by one constant buffer and no sampler (every read is a Load on an exact texel, so the
// result does not depend on filter state and a test can predict it):
//   VS            (vs_5_0) one oversized triangle from SV_VertexID
//   PS_Mask       (ps_5_0) half the render window: 1 where the depth is the far plane (sky), 0 on every surface
//   PS_Blur       (ps_5_0) half the render window: the sky mask blurred toward the sun's screen position
//   PS_SunColour  (ps_5_0) one texel: the sky's colour around the sun, which is the colour the shafts take when the engine reports none
//   PS_Composite  (ps_5_0) the render window: the scene plus the blurred mask, tinted and scaled
//
// Inputs of PS_Mask:
//   t0 Depth      Texture2D<float>   the light draw's g_SamplerDepth (the engine's depth, reversed-Z: 0 is the far plane)
// Inputs of PS_Blur:
//   t1 Mask       Texture2D<float>   PS_Mask's output
// Inputs of PS_SunColour:
//   t0 Depth      Texture2D<float>   the same depth, for the "only sky texels are the sun's colour" test
//   t2 Scene      Texture2D<float4>  the engine's lit scene
// Inputs of PS_Composite:
//   t0 Depth      Texture2D<float>   the same depth, for the "never glow on the sky itself" test
//   t2 Scene      Texture2D<float4>  the engine's lit scene (the tone map's t0), read only
//   t3 Term       Texture2D<float>   PS_Blur's output
//   t4 SunTint    Texture2D<float4>  PS_SunColour's one texel: rgb = the sky's colour around the sun, a = 1 when there was sky to sample
//
// Rules:
//  1. The engine allocates the frame's targets at the display size, draws a smaller window inside them and starts that window at a half-pixel
//     origin (a captured frame's viewport is `1920x1080+0.5+0.5`), so:
//       - SV_Position.xy is already the render target's pixel (k + 0.5) whatever the viewport's origin: a full-resolution read is int2(position.xy)
//         and the viewport's origin is NOT added (adding it read the neighbour, one pixel down and right of every pixel);
//       - a half-resolution texel k of the window stands for the window's own scene pixels 2k and 2k+1, so the half-resolution coordinate is
//         (position.xy - Window.xy) * 0.5: the origin comes out, and both the mask and the composite agree on which texel covers which pixel.
//  2. Nothing is written to the engine's textures. PS_Composite reads the engine's scene and writes this pass's own copy of it.
//  3. A pixel that is sky keeps the scene's own value: the shafts land on the surfaces the sun's light crosses, never on the sky it comes
//     from, which would only smear the sky's own brightness.
//  4. The term is a fixed function of the frame's buffers: no frame number, no random offset, no history. The blur's march is offset per pixel by
//     a hash of the pixel's own coordinate, which is still a function of the frame alone.
//  5. The added light is measured against the frame's own white point (White.x), and the composite never adds more than the pixel's remaining room
//     to that white: no intensity key can push a channel past it, so no key can burn the frame.
//  6. The term is read bilinearly between the half-resolution texels whose centres surround the pixel, so one term texel is not painted over a
//     2x2 block of the window.
//
// The register slots and the constant-buffer layout are mirrored by ShaftsPass.cpp; change them together.

cbuffer Params : register(b0)
{
    float4 Window; // xy = the render window's origin in scene pixels, zw = its extent in scene pixels
    float4 Sun;    // xy = the sun's position in scene pixels, z = intensity, w = decay
    float4 Tuning; // x = samples, y = density, z = 0 the light's colour is used / 1 the sky's colour at the sun, w = the gain against the white
    float4 Tint;   // rgb = the light's colour (a colour of peak 1)
    float4 White;  // x = the frame's white point in scene units, y = the held sun's fade (1 = this frame's own read), zw = 0
};

Texture2D<float> Depth : register(t0);
Texture2D<float> Mask : register(t1);
Texture2D<float4> Scene : register(t2);
Texture2D<float> Term : register(t3);
Texture2D<float4> SunTint : register(t4);

// The engine's depth is reversed-Z: a pixel at the far plane is sky, and it is where the light comes from. A cannot-see value (a NaN, a read
// the depth view refuses) is a surface, so a broken depth adds no light instead of filling the frame with glow.
static const float kSkyDepth = 1.0e-6;

float SkyOf(float depth)
{
    return (depth == depth && depth >= 0.0 && depth <= kSkyDepth) ? 1.0 : 0.0;
}

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput VS(uint id : SV_VertexID)
{
    VSOutput o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float PS_Mask(float4 position : SV_Position) : SV_Target0
{
    // The scene pixel this half-resolution texel stands for: the window's pixels 2k and 2k+1, whose centre is 2k + 1 (see rule 1). An odd
    // window extent makes the last half texel ask for the column after the last one, and a load outside the depth texture returns 0 - which the
    // reversed-Z sky test reads as sky, so the composite would add a one-pixel glow at that edge. The read is clamped onto the depth's own extent.
    uint width = 0;
    uint height = 0;
    Depth.GetDimensions(width, height);
    const int2 last = max(int2(width, height) - int2(1, 1), int2(0, 0));
    const int2 at = min(int2(Window.xy + position.xy * 2.0), last);
    return SkyOf(Depth.Load(int3(at, 0)));
}

// A per-pixel fraction of a step, from the pixel's own coordinate: an integer hash, so a still frame stays still (no frame number, no history, no
// sampler state) and two runs of the same frame are one picture.
static const float kInverse24 = 1.0 / 16777216.0;
float Hash01(float2 pixel)
{
    uint h = uint(pixel.x) * 1664525u + uint(pixel.y) * 1013904223u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) * kInverse24;
}

float PS_Blur(float4 position : SV_Position) : SV_Target0
{
    const float count = max(Tuning.x, 1.0);
    // The sun in the half-resolution window, and the step from this pixel toward it. A sun outside the window is a valid position: the
    // samples then run off the mask, whose out-of-range reads are zero (the sky is only where the frame has one).
    const float2 sun = (Sun.xy - Window.xy) * 0.5;
    const float2 stepToward = (sun - position.xy) * Tuning.y / count;
    // Every pixel starts its march at a different fraction of its own first step. With one lattice shared by the whole frame, a texel's ray and
    // its neighbour's took the same taps a pixel apart and the term came out piecewise constant along the ray and stepped across it - the hard
    // edges and the bands a session saw. The offset is a hash of the pixel's own coordinate, so it costs no state and does not change over time.
    const float jitter = Hash01(position.xy);
    float weight = 1.0;
    float sum = 0.0;
    float total = 0.0;
    for (int i = 0; i < int(count); ++i)
    {
        const float2 at = position.xy + stepToward * (float(i) + jitter);
        total += Mask.Load(int3(int2(at), 0)) * weight;
        sum += weight;
        weight *= Sun.w;
    }
    return sum > 0.0 ? total / sum : 0.0;
}

// Rule 7. The beams are the term's variation, not its level.
//
// The blurred mask is the share of sky along each pixel's ray toward the sun. Near a low sun that share is large and smooth over most of the frame,
// and adding it everywhere is a veil (a session measured +27 % on a castle and +14 % on the sea). What a viewer calls a shaft is where an occluder
// cuts the ray: the term changes faster than its own neighbourhood. So the composite adds the part of the term that stands above a wide low-pass of
// itself, boosted, plus a small kept share of the level, and fades the sum with the distance from the sun:
//   shafts = min((max(term - low, 0) * boost + keep * term) * falloff, 1),   falloff = exp(-(d / radius)^2),   d in WINDOW HEIGHTS from the sun.
// The low-pass is PS_Low's block mean of the term (one Low texel per kBeamBlock x kBeamBlock term texels, only the texels that exist at the right
// and bottom edge), read bilinearly like the term: block j is centred at term coordinate (j + 0.5) * kBeamBlock, so the read is
// halfCoordinate / kBeamBlock - 0.5. The boost is applied to the term, BEFORE the white bound (ShaftAdded), never after, and the sum is clamped to 1
// so a black pixel never gets more than PeakWhiteShare of the white. The constants are ShaftsRules.h's; a test pins them equal.
//   t5 Low   Texture2D<float>   PS_Low's output (PS_Low reads t3, the term)
Texture2D<float> Low : register(t5);
static const int kBeamBlock = 24;
static const float kBeamBoost = 4.0;
static const float kBeamKeep = 0.05;
static const float kBeamFalloffRadius = 0.8;
static const float kBeamTermMax = 1.0;

float PS_Low(float4 position : SV_Position) : SV_Target0
{
    uint termWidth = 1;
    uint termHeight = 1;
    Term.GetDimensions(termWidth, termHeight);
    // position.xy is (j + 0.5, k + 0.5) for Low texel (j, k): the first Term texel of its block.
    const int2 first = int2(position.xy) * kBeamBlock;
    const int2 end = min(first + int2(kBeamBlock, kBeamBlock), int2(termWidth, termHeight));
    float sum = 0.0;
    float count = 0.0;
    for (int y = first.y; y < end.y; ++y)
        for (int x = first.x; x < end.x; ++x)
        {
            sum += Term.Load(int3(x, y, 0));
            count += 1.0;
        }
    return count > 0.0 ? sum / count : 0.0;
}

// The colour of the shafts: the light's own colour when the engine reports one, otherwise the sky's own colour around the sun.
//
// Only texels whose depth is the far plane count. That is the point of a separate one-texel pass: with the sun low behind trees the sun's own
// pixel is a lit leaf, and a leaf's colour is not the sun's - a shaft tinted by it is green or brown, or nothing when the leaf is dark. When the
// search finds no sky at all the frame has no visible sun, so the shafts add nothing: the same rule as the brightness floor below, and the
// reason a night sky or a sun behind a wall never glows over every surface next to it.
//
// The sampled colour is then normalised to its own peak. A sun that is a quarter of a window above the frame clamps to the top border row, and that
// row of a daylight sky reads about (0.18, 0.28, 0.38): at its own level it would dim every shaft by three times, and at the top border of a frame
// behind trees it is the only sky the search can find. Dividing by the peak keeps the sampled sky's hue - a warm sun sample stays warm, a blue sky
// sample stays blue - and puts its brightest channel at 1, so the shaft's brightness is the key's and the mask's alone. A sample whose peak is below
// kMinTintPeak is scaled up to it instead: a quarter-lit sky still tints the shafts rather than turning them black, and the floor is what keeps a
// nearly black sample (a night sky at the border) from being stretched into a bright colour.
static const float kMinTintLuminance = 0.02;
static const float kMinTintPeak = 0.25;
static const float3 kLumaWeights = float3(0.2126390059, 0.7151686788, 0.0721923154);
static const int kSunColourRadius = 12; // scene pixels around the sun's position that the search looks at

float4 PS_SunColour(float4 position : SV_Position) : SV_Target0
{
    uint width = 0;
    uint height = 0;
    Scene.GetDimensions(width, height);
    const float2 last = float2(max(float(width) - 1.0, 0.0), max(float(height) - 1.0, 0.0));
    const int2 sunPixel = int2(clamp(Sun.xy, float2(0.0, 0.0), last));
    float3 sum = float3(0.0, 0.0, 0.0);
    float sky = 0.0;
    for (int dy = -kSunColourRadius; dy <= kSunColourRadius; ++dy)
        for (int dx = -kSunColourRadius; dx <= kSunColourRadius; ++dx)
        {
            const int2 at = sunPixel + int2(dx, dy);
            if (at.x < 0 || at.y < 0 || at.x >= int(width) || at.y >= int(height))
                continue;
            if (SkyOf(Depth.Load(int3(at, 0))) < 0.5)
                continue;
            sum += saturate(Scene.Load(int3(at, 0)).rgb);
            sky += 1.0;
        }
    if (!(sky > 0.0))
        return float4(0.0, 0.0, 0.0, 0.0);
    const float3 colour = sum / sky;
    if (!(dot(colour, kLumaWeights) >= kMinTintLuminance))
        return float4(0.0, 0.0, 0.0, 0.0);
    // The sample's own peak, so the tint is a colour of peak 1 whatever level the sky around the sun is at (see the comment above).
    const float peak = max(max(colour.r, colour.g), colour.b);
    return float4(saturate(colour / max(peak, kMinTintPeak)), 1.0);
}

float4 PS_Composite(float4 position : SV_Position) : SV_Target0
{
    // The scene's own pixel and this pass's copy's own pixel are the same one: SV_Position already holds the target pixel, so the window's
    // half-pixel origin is not added here (see rule 1).
    const int2 at = int2(position.xy);
    const float4 scene = Scene.Load(int3(at, 0));
    if (!(Sun.z > 0.0))
        return scene;
    // The term is half the window's resolution: read it bilinearly between the four texels whose centres surround this pixel's half-resolution
    // coordinate. A nearest read paints one texel over a 2x2 block of the window (a 4x4 block of the upscaled output), which is the mosaic a
    // session saw on the rock; the four loads are all this needs - no sampler, no filter state, and a test can predict every value.
    uint termWidth = 1;
    uint termHeight = 1;
    Term.GetDimensions(termWidth, termHeight);
    const int2 lastHalf = max(int2(termWidth, termHeight) - int2(1, 1), int2(0, 0));
    const float2 coordinate = (position.xy - Window.xy) * 0.5 - 0.5;
    const int2 base = int2(floor(coordinate));
    const float2 fraction = saturate(coordinate - float2(base));
    const float top = lerp(Term.Load(int3(clamp(base, int2(0, 0), lastHalf), 0)),
                           Term.Load(int3(clamp(base + int2(1, 0), int2(0, 0), lastHalf), 0)), fraction.x);
    const float bottom = lerp(Term.Load(int3(clamp(base + int2(0, 1), int2(0, 0), lastHalf), 0)),
                              Term.Load(int3(clamp(base + int2(1, 1), int2(0, 0), lastHalf), 0)), fraction.x);
    const float level = max(lerp(top, bottom, fraction.y), 0.0);
    // The term's low-pass at this pixel: PS_Low's block means, read bilinearly at the pixel's own coordinate (rule 7).
    uint lowWidth = 1;
    uint lowHeight = 1;
    Low.GetDimensions(lowWidth, lowHeight);
    const int2 lastLow = max(int2(lowWidth, lowHeight) - int2(1, 1), int2(0, 0));
    const float2 lowCoordinate = (coordinate + 0.5) / float(kBeamBlock) - 0.5;
    const int2 lowBase = int2(floor(lowCoordinate));
    const float2 lowFraction = saturate(lowCoordinate - float2(lowBase));
    const float lowTop = lerp(Low.Load(int3(clamp(lowBase, int2(0, 0), lastLow), 0)),
                              Low.Load(int3(clamp(lowBase + int2(1, 0), int2(0, 0), lastLow), 0)), lowFraction.x);
    const float lowBottom = lerp(Low.Load(int3(clamp(lowBase + int2(0, 1), int2(0, 0), lastLow), 0)),
                                 Low.Load(int3(clamp(lowBase + int2(1, 1), int2(0, 0), lastLow), 0)), lowFraction.x);
    const float low = lerp(lowTop, lowBottom, lowFraction.y);
    // The falloff around the sun, by the pixel's distance from it in window heights.
    const float reach = length(position.xy - Sun.xy) / max(Window.w, 1.0) / kBeamFalloffRadius;
    const float falloff = exp(-reach * reach);
    const float shafts = min((max(level - low, 0.0) * kBeamBoost + kBeamKeep * level) * falloff, kBeamTermMax);
    if (!(shafts > 0.0))
        return scene;
    // Never on the sky itself: the shafts are the light crossing the frame, not a second glow on the sky it starts from.
    const float sky = SkyOf(Depth.Load(int3(at, 0)));
    if (sky > 0.5)
        return scene;
    float3 tint = Tint.rgb;
    if (Tuning.z > 0.5)
    {
        const float4 sampled = SunTint.Load(int3(0, 0, 0));
        if (!(sampled.a > 0.5))
            return scene; // no sky around the sun: the frame has no sun to take a colour from
        tint = sampled.rgb;
    }
    // The light this pixel gets, in fractions of the frame's own white: the blurred mask times the key, the tint's own channel, the gain and the
    // held sun's fade. The addition is bounded by how much room the pixel has left to the white - head = white - scene, add = head*raw/(raw+head) -
    // so the sum is always strictly below the white point: no key, no mask and no fade can push a channel past it, and a pixel already at white gets
    // nothing instead of a clipped white smear. The same key adds the same share of the white in a bright frame and a dusk one, because the white is
    // the frame's own. (ShaftsRules.h ShaftAdded is this expression, and its tests pin the bound.)
    const float3 light = shafts * Sun.z * tint * Tuning.w * White.y;
    const float3 head = max(White.x - scene.rgb, 0.0);
    const float3 raw = light * White.x;
    const float3 add = head * raw / max(raw + head, 1.0e-6);
    float4 outColor = scene;
    outColor.rgb += add;
    return outColor;
}
