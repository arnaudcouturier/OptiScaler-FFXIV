Texture2D<float4> ui : register(t0);
Texture2D<float4> scene : register(t1);

float3 Linear(float3 x)
{
    return float3(x.r <= .04045 ? x.r / 12.92 : pow((x.r + .055) / 1.055, 2.4),
                  x.g <= .04045 ? x.g / 12.92 : pow((x.g + .055) / 1.055, 2.4),
                  x.b <= .04045 ? x.b / 12.92 : pow((x.b + .055) / 1.055, 2.4));
}

float3 Decode(float3 x)
{
    float3 p = pow(saturate(x), 32.0 / 2523.0);
    return 10000 * pow(max(p - 3424.0 / 4096.0, 0) / max(2413.0 / 128.0 - 2392.0 / 128.0 * p, 1e-6),
                       16384.0 / 2610.0);
}

float3 Encode(float3 nits)
{
    float3 p = pow(max(nits, 0) / 10000, 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) / (1 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
}

float4 VS(uint id : SV_VertexID) : SV_POSITION
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PS(float4 pos : SV_POSITION) : SV_Target
{
    int3 at = int3(pos.xy, 0);
    float4 layer = ui.Load(at), background = scene.Load(at);
    if (layer.a <= 0)
        return background; // No colour roundtrip outside the UI.

    float3 rgb = Linear(saturate(layer.rgb / layer.a));
    float3 nits = 203 * float3(dot(rgb, float3(.627404, .329282, .043314)),
                               dot(rgb, float3(.069097, .919540, .011362)),
                               dot(rgb, float3(.016391, .088013, .895595)));
    return float4(Encode(nits * layer.a + Decode(background.rgb) * (1 - layer.a)), background.a);
}
