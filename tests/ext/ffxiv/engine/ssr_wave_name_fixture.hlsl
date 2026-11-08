// Fixture for the screen-space reflections test: a water material whose only water anchor is named `g_SamplerWaveMap1`, which is the rename that
// would silently kill the feature - the parse asks for the exact name `g_SamplerWaveMap` (SsrRules.h's kWaveName), and a shader that declares a
// numbered sibling instead is a sampler of the map this pass does not answer for, so every water draw of such a build would be passed on with
// the map still the engine's.
//
//   t0 g_SamplerVPosition      the view-space position of every pixel
//   t4 g_SamplerWaveMap1       a water anchor whose name has a suffix the module does not know
//   t8 g_SamplerReflectionMap  the engine's planar reflection map
//   b2 g_CameraParameter       the camera table

Texture2D<float4> g_SamplerVPosition : register(t0);
Texture2D<float4> g_SamplerWaveMap1 : register(t4);
Texture2D<float4> g_SamplerReflectionMap : register(t8);

cbuffer g_CameraParameter : register(b2)
{
    float4 g_CameraRows[16];
};

float4 PS_WaveName1(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy);
    return g_SamplerReflectionMap.Load(int3(texel, 0)) + g_SamplerVPosition.Load(int3(texel, 0)) + g_SamplerWaveMap1.Load(int3(texel, 0)) +
           g_CameraRows[0];
}
