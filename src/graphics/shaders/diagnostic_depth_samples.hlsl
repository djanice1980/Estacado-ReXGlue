// Diagnostic only: preserve every host depth/stencil sample, no MSAA resolve,
// float24 conversion, EDRAM packing, filtering, or guest-memory write.
cbuffer Extent : register(b0) { uint width; uint height; uint samples; };
#if SINGLE_SAMPLE
Texture2D<float> depth_source : register(t0);
Texture2D<uint2> stencil_source : register(t1);
#else
Texture2DMS<float> depth_source : register(t0);
Texture2DMS<uint2> stencil_source : register(t1);
#endif
RWByteAddressBuffer result : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height || id.z >= samples) return;
#if SINGLE_SAMPLE
  float d = depth_source.Load(int3(id.xy, 0));
  uint s = stencil_source.Load(int3(id.xy, 0)).y;
#else
  float d = depth_source.Load(id.xy, id.z);
  uint s = stencil_source.Load(id.xy, id.z).y;
#endif
  result.Store2(((id.y * width + id.x) * samples + id.z) * 8,
                uint2(asuint(d), s));
}
