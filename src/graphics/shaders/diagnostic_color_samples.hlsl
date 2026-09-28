// Diagnostic only. Read each FP16 sample independently, without resolving,
// filtering, guest-format conversion or a write to the source render target.
cbuffer Region : register(b0) {
  uint left; uint top; uint width; uint height; uint samples;
};
#if SINGLE_SAMPLE
Texture2D<float4> color_source : register(t0);
#else
Texture2DMS<float4> color_source : register(t0);
#endif
RWByteAddressBuffer result : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height || id.z >= samples) return;
  uint2 position = uint2(left, top) + id.xy;
#if SINGLE_SAMPLE
  float4 value = color_source.Load(int3(position, 0));
#else
  float4 value = color_source.Load(position, id.z);
#endif
  // Finite FP16 values round-trip exactly through float32. NaN payload/sign
  // preservation is not claimed and must be rejected by the evidence analyzer.
  uint4 bits = f32tof16(value);
  result.Store2(((id.y * width + id.x) * samples + id.z) * 8,
                uint2(bits.x | (bits.y << 16), bits.z | (bits.w << 16)));
}
