// SMAA 1x (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro,
// Diego Gutierrez; MIT license, see smaa/LICENSE.txt; reference commit
// 71c806a8 of github.com/iryoku/smaa) as three compute passes on the
// gamma-corrected guest output, in place of the FXAA pass:
//   SMAA_PASS 0: luma edge detection      (color -> edges, R8G8)
//   SMAA_PASS 1: blending weights         (edges, AreaTex, SearchTex -> weights, R8G8B8A8)
//   SMAA_PASS 2: neighborhood blending    (color, weights -> guest output)
// One thread per pixel and every pixel is written (an edge-free pixel stores
// zero edges instead of the pixel shader's discard), so the reference's
// stencil masking and render target clears are not needed.

cbuffer SmaaConstants : register(b0) {
  uint2 smaa_size;
  float2 smaa_size_inv;
};

#define SMAA_RT_METRICS float4(smaa_size_inv, float2(smaa_size))
#define SMAA_PRESET_HIGH 1
#define SMAA_CUSTOM_SL 1

SamplerState smaa_linear_sampler : register(s0);
SamplerState smaa_point_sampler : register(s1);

// Compute shaders have no implicit derivatives: every sample is level zero.
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(smaa_linear_sampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(smaa_point_sampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(smaa_linear_sampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(smaa_linear_sampler, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(smaa_point_sampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(smaa_linear_sampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(smaa_linear_sampler, coord, 0)

// The edge detection functions discard edge-free pixels; here they return
// zero edges (every pixel of the edges texture is written each frame).
#define discard return float2(0.0, 0.0)
#include "smaa/SMAA.hlsl"
#undef discard

Texture2D smaa_source_0 : register(t0);
Texture2D smaa_source_1 : register(t1);
Texture2D smaa_source_2 : register(t2);
#if SMAA_PASS == 0
RWTexture2D<float2> smaa_dest : register(u0);
#else
RWTexture2D<float4> smaa_dest : register(u0);
#endif

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id.xy >= smaa_size)) {
    return;
  }
  float2 texcoord = (float2(xe_thread_id.xy) + 0.5) * smaa_size_inv;
#if SMAA_PASS == 0
  float4 offset[3];
  SMAAEdgeDetectionVS(texcoord, offset);
  smaa_dest[xe_thread_id.xy] = SMAALumaEdgeDetectionPS(texcoord, offset, smaa_source_0);
#elif SMAA_PASS == 1
  float2 pixcoord;
  float4 offset[3];
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
  smaa_dest[xe_thread_id.xy] = SMAABlendingWeightCalculationPS(
      texcoord, pixcoord, offset, smaa_source_0, smaa_source_1, smaa_source_2,
      float4(0.0, 0.0, 0.0, 0.0));
#else
  float4 offset;
  SMAANeighborhoodBlendingVS(texcoord, offset);
  smaa_dest[xe_thread_id.xy] =
      SMAANeighborhoodBlendingPS(texcoord, offset, smaa_source_0, smaa_source_1);
#endif
}
