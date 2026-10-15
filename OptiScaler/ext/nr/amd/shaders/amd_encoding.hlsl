
Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);
cbuffer Params:register(b0){uint w,h,mode,inverse;}
float transform(float v){float a=abs(v);float y=a;
 if(mode==2)y=inverse?(a<=0.0031308?12.92*a:1.055*pow(a,1.0/2.4)-0.055):(a<=0.04045?a/12.92:pow((a+0.055)/1.055,2.4));
 if(mode==3)y=pow(a,inverse?1.0/2.2:2.2);
 return sign(v)*y;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x>=w||p.y>=h)return;float4 c=src.Load(int3(p.xy,0));dst[p.xy]=float4(transform(c.r),transform(c.g),transform(c.b),c.a);}
