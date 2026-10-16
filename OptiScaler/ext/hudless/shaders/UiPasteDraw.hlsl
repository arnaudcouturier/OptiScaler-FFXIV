// S10 paste draw (docs/HUDLESS.md section 7): full-screen triangle over the presented backbuffer. `discard` where the
// paste image has alpha <= 0, otherwise the pixel is replaced (output alpha 1, no blend state in the PSO).
// DebugTint (menu "Show detected UI"): magenta where the alpha is 1, cyan for the cleanup level.
cbuffer Params : register(b0)
{
    uint DebugTint;
};

Texture2D<float4> PasteTexture : register(t0);

float4 VSMain(uint vid : SV_VertexID) : SV_Position
{
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float4 c = PasteTexture.Load(int3(int2(pos.xy), 0));

    if (c.a <= 0.0)
        discard;

    if (DebugTint != 0)
        c.rgb = lerp(c.rgb, c.a > 0.75 ? float3(1.0, 0.0, 1.0) : float3(0.0, 1.0, 1.0), 0.35);

    return float4(c.rgb, 1.0);
}
