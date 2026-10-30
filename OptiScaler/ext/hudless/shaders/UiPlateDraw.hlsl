// Native nameplate output pass: draws the plate-only image over the presented frame, one instanced quad per mover
// (or one fullscreen quad for the endpoint stage), each shifted by its already output-scaled offset. The plate image
// is premultiplied, so the blend is ONE / INV_SRC_ALPHA with no alpha write.
cbuffer Params : register(b0)
{
    uint MoverCount;
    float ImageW;
    float ImageH;
    uint Pad;
};

cbuffer Movers : register(b1)
{
    float4 SrcRects[64]; // image pixels: x0, y0, x1, y1 (original positions)
    float4 Offsets[64];  // image pixels: ox, oy (already scaled by this output's share)
};

Texture2D<float4> PlateTexture : register(t0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    uint inst : TEXCOORD1;
};

VsOut VSMain(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    // Quad corners from two triangles: 0:(x0,y0) 1:(x1,y0) 2:(x0,y1) 3:(x1,y0) 4:(x1,y1) 5:(x0,y1).
    float4 r = SrcRects[inst];
    float2 off = Offsets[inst].xy;
    float2 xy = (vid == 0) ? r.xy : (vid == 1) ? float2(r.z, r.y) : (vid == 2) ? float2(r.x, r.w)
        : (vid == 3)           ? float2(r.z, r.y)
        : (vid == 4)           ? r.zw
                               : float2(r.x, r.w);
    float2 shifted = xy + off;
    VsOut o;
    o.pos = float4(shifted.x / ImageW * 2.0 - 1.0, 1.0 - shifted.y / ImageH * 2.0, 0.0, 1.0);
    o.uv = xy; // the image holds the unshifted frame: sample where the plate was
    o.inst = inst;
    return o;
}

float4 PSMain(VsOut i) : SV_Target
{
    // An output pixel covered by a lower-index mover's shifted rectangle belongs to it: it draws its own copy.
    float2 p = i.uv + Offsets[i.inst].xy;
    for (uint j = 0; j < i.inst && j < MoverCount; ++j)
    {
        // The drawn quad carries a 2 px margin for glyph edges; the hole it cuts in a mover above it is the exact
        // rect, without that margin (otherwise covered edges would eat slivers of the mover below).
        float4 r = SrcRects[j] + Offsets[j].xyxy + float4(2.0, 2.0, -2.0, -2.0);
        if (r.x < r.z && r.y < r.w && p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w)
            discard;
    }
    float4 c = PlateTexture.Load(int3(int2(i.uv), 0));
    if (c.a <= 0.0)
        discard;
    return c;
}
