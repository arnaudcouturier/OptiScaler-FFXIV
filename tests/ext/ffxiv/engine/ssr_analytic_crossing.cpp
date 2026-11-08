// Analytic floor-to-wall reflection geometry sweep using the existing WARP harness.
#define main OriginalSsrWarpMain
#include "ssr_warp.cpp"
#undef main

namespace
{
Frame AnalyticScene(float floorY, float wallZ, float samplePhase)
{
    Frame f = BuildScene(false, false);
    std::fill(f.position.begin(), f.position.end(), 0.0f);
    std::fill(f.colour.begin(), f.colour.end(), 0.0f);
    const float tanY = std::tan(kFovDegrees * 0.5f * kPi / 180.0f);
    constexpr float bright[3] { 0.95f, 0.85f, 0.10f };
    for (uint32_t y=0; y<kWindow; ++y) for (uint32_t x=0; x<kWindow; ++x)
    {
        const float nx = (float(x)+samplePhase)/kWindow*2.0f-1.0f;
        const float ny = 1.0f-(float(y)+samplePhase)/kWindow*2.0f;
        const float3 ray=Make3(nx*tanY,ny*tanY,-1.0f);
        float t=0.0f;
        if (ray.y < -1e-6f) t=floorY/ray.y;
        const float tw=wallZ/ray.z;
        if (tw>0 && (t<=0 || tw<t) && ray.y*tw>=floorY) t=tw;
        if (t<=0) continue;
        const float3 p=Mul(ray,t);
        const size_t i=(size_t(y)*kScene+x)*3;
        f.position[i]=HalfRound(p.x); f.position[i+1]=HalfRound(p.y); f.position[i+2]=HalfRound(p.z);
        const bool isWall=std::fabs(p.z-wallZ)<0.01f;
        for(int c=0;c<3;++c) f.colour[i+c]=HalfRound(isWall?bright[c]:kFloorColour[c]);
    }
    return f;
}

bool HasVisiblePhysicalHit(const Frame& frame, const float3& p, float floorY, float wallZ)
{
    const float3 n=Make3(0,1,0), v=Normalized(p);
    const float3 r=Normalized(Sub(v,Mul(n,2.0f*Dot(v,n))));
    if (std::fabs(r.z)<1e-7f) return false;
    const float t=(wallZ-p.z)/r.z;
    if (!(t>2.0f && t<100.0f)) return false;
    const float3 h=Add(p,Mul(r,t));
    if (h.y<floorY || h.z>=p.z) return false;
    const float tanY=std::tan(kFovDegrees*0.5f*kPi/180.0f);
    const float nx=h.x/(-h.z*tanY), ny=h.y/(-h.z*tanY);
    if (!(nx>=-0.95f && nx<=0.95f && ny>=-0.95f && ny<=0.95f)) return false;
    // Certify primary visibility: from the camera, the wall must precede the floor.
    const float3 primary = Mul(h, 1.0f / -h.z);
    const float floorT = primary.y < -1e-6f ? floorY / primary.y : 1e30f;
    if (-wallZ > floorT + 1e-4f) return false;
    // An analytic subpixel intersection is not sufficient for screen-space reflection:
    // the captured position/colour samples must actually contain that wall target.
    const int px = int(std::floor((nx*0.5f+0.5f)*kWindow));
    const int py = int(std::floor((0.5f-ny*0.5f)*kWindow));
    for (int oy=-1;oy<=1;++oy) for (int ox=-1;ox<=1;++ox)
    {
        const int sx=px+ox, sy=py+oy;
        if (sx<0 || sy<0 || sx>=int(kWindow) || sy>=int(kWindow)) return false;
        const size_t sample=(size_t(sy)*kScene+sx)*3;
        if (std::fabs(frame.position[sample+2]-wallZ)>0.01f) return false;
    }
    return true;
}
}

int main(int, char**)
{
    Harness harness;
    if(!harness.Create()) { std::puts("no D3D11 WARP device"); return 2; }
    Pass pass;
    std::vector<uint8_t> pixels;
    int physical=0, unchanged=0;
    uint64_t codes[256] {};
    for(float fy : {-0.5f,-1.0f,-2.0f,-5.0f,-10.0f})
      for(float wz : {-8.0f,-16.0f,-32.0f,-64.0f})
       for(float phase : {0.5f,0.0f})
       {
        Frame f=AnalyticScene(fy,wz,phase);
        if(!harness.Run(pass,f,pixels)) { std::printf("WARP run failed fy=%g wz=%g phase=%g\n",fy,wz,phase); return 1; }
        // The shader writes the half-size active quadrant and loads scene position at 2*own.
        // Comparing an unscaled source texel or the untouched outer map would count false failures.
        for(uint32_t y=0;y<kWindow/2;++y) for(uint32_t x=0;x<kWindow/2;++x)
        {
            const size_t i=(size_t(2*y)*kScene+2*x)*3;
            const float3 p=Make3(f.position[i],f.position[i+1],f.position[i+2]);
            if(p.y>=-1e-5f || std::fabs(p.z+0.0f)<1e-6f || std::fabs(p.z-wz)<0.01f) continue;
            if(!HasVisiblePhysicalHit(f,p,fy,wz)) continue;
            ++physical;
            float got[4]{}; ReadTexel(pixels,x,y,got);
#ifdef SSR_ANALYTIC_DIAG
            const auto code = static_cast<uint32_t>(std::lround(got[0]*255.0f));
            if (code < 256) ++codes[code];
            if (code != 10) ++unchanged;
#else
            uint8_t original[4]{}; EngineBytes(x,y,f.engineValue,original);
            if(std::abs(got[0]-float(original[2])/255.0f)<1e-4f && std::abs(got[1]-float(original[1])/255.0f)<1e-4f) {
                ++unchanged;
                if (unchanged <= 20)
                    std::printf("physical-hit unchanged fy=%g wz=%g phase=%g own=(%u,%u) P=(%.4f,%.4f,%.4f)\n",fy,wz,phase,x,y,p.x,p.y,p.z);
            }
#endif
        }
       }
    std::printf("sweep physical=%d unchanged=%d\n",physical,unchanged);
    for (uint32_t code=0;code<256;++code)
        if (codes[code]) std::printf("analytic eligible GPU reason %u: %llu\n",code,static_cast<unsigned long long>(codes[code]));
    CHECK(physical > 0);
    CHECK_EQ(unchanged, 0);
    return Finish("ssr_analytic_crossing");
}
