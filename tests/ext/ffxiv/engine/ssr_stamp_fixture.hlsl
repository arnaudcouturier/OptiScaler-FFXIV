// Fixture for the screen-space reflections test: the names a water shader declares, so the module's bytecode parse can be checked against a real
// compiled reflection instead of a table the test wrote by hand. Not a shader of the game and not a copy of one: four entry points over the same
// declarations, one per shape the recognition has to tell apart.
//
//   PS_Capture  the capture's own shape: the reflection map at t8, the view position at t0, the camera table at b2, the wave map at t4 and the
//               caustics map at t6 - the slots the water draws of a captured frame declare: a reader this pass serves
//   PS_Water    the reflection map, the view position, the camera table and one water anchor (the wave map)
//   PS_NoWave   the same without the water anchors: a sampler of the map that is not a water surface
//   PS_NoMap    neither the map nor a water anchor: nothing this module has anything to read
//
// Every resource is read in the body, so no compiler pass can drop a declaration out of the reflection.
//
//   t0 g_SamplerVPosition     Texture2D<float4>  the view-space position of every pixel
//   t4 g_SamplerWaveMap       Texture2D<float4>  a water anchor
//   t6 g_SamplerCaustics      Texture2D<float4>  the other water anchor
//   t8 g_SamplerReflectionMap Texture2D<float4>  the engine's planar reflection map
//   t1 g_SamplerColour        Texture2D<float4>  a texture that is neither of those

Texture2D<float4> g_SamplerVPosition : register(t0);
Texture2D<float4> g_SamplerWaveMap : register(t4);
Texture2D<float4> g_SamplerCaustics : register(t6);
Texture2D<float4> g_SamplerReflectionMap : register(t8);
Texture2D<float4> g_SamplerColour : register(t1);

cbuffer g_CameraParameter : register(b2)
{
    float4 g_CameraRows[16];
};

float4 PS_Capture(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy);
    return g_SamplerReflectionMap.Load(int3(texel, 0)) + g_SamplerVPosition.Load(int3(texel, 0)) + g_SamplerWaveMap.Load(int3(texel, 0)) +
           g_SamplerCaustics.Load(int3(texel, 0)) + g_CameraRows[0];
}

float4 PS_Water(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy);
    return g_SamplerReflectionMap.Load(int3(texel, 0)) + g_SamplerVPosition.Load(int3(texel, 0)) + g_SamplerWaveMap.Load(int3(texel, 0)) +
           g_CameraRows[0];
}

float4 PS_NoWave(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy);
    return g_SamplerReflectionMap.Load(int3(texel, 0)) + g_SamplerVPosition.Load(int3(texel, 0)) + g_CameraRows[1];
}

float4 PS_NoMap(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy);
    return g_SamplerColour.Load(int3(texel, 0)) + g_SamplerVPosition.Load(int3(texel, 0));
}
