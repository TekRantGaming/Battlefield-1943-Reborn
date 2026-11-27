// NVIDIA Image Scaling (NIS) for guest output presentation, based on
// NIS_Main.hlsl of the NVIDIA Image Scaling SDK v1.0.3 (MIT license, see
// thirdparty/nis/LICENSE.txt).
//
// NIS_SCALER 1: NVScaler, upscaling by 1x to 2x along each axis with
// sharpening. NIS_SCALER 0: NVSharpen, sharpening only (1:1).
// Compiled with (from src/ui/shaders):
//   fxc /T cs_5_1 /O3 /E main /D NIS_SCALER=1 /D NIS_BLOCK_HEIGHT=24
//       /Fh bytecode/d3d12_5_1/guest_output_nis_scale_cs.h
//       /Vn guest_output_nis_scale_cs guest_output_nis.cs.hlsl
//   fxc /T cs_5_1 /O3 /E main /D NIS_SCALER=0 /D NIS_BLOCK_HEIGHT=32
//       /Fh bytecode/d3d12_5_1/guest_output_nis_sharpen_cs.h
//       /Vn guest_output_nis_sharpen_cs guest_output_nis.cs.hlsl
// The dispatch is (output width / 32) x (output height / NIS_BLOCK_HEIGHT)
// groups, rounded up.

#define NIS_HLSL 1
#define NIS_BLOCK_WIDTH 32
#define NIS_THREAD_GROUP_SIZE 256

// NISConfig from NIS_Config.h, as root constants.
cbuffer cb : register(b0) {
  float kDetectRatio;
  float kDetectThres;
  float kMinContrastRatio;
  float kRatioNorm;

  float kContrastBoost;
  float kEps;
  float kSharpStartY;
  float kSharpScaleY;

  float kSharpStrengthMin;
  float kSharpStrengthScale;
  float kSharpLimitMin;
  float kSharpLimitScale;

  float kScaleX;
  float kScaleY;

  float kDstNormX;
  float kDstNormY;
  float kSrcNormX;
  float kSrcNormY;

  uint kInputViewportOriginX;
  uint kInputViewportOriginY;
  uint kInputViewportWidth;
  uint kInputViewportHeight;

  uint kOutputViewportOriginX;
  uint kOutputViewportOriginY;
  uint kOutputViewportWidth;
  uint kOutputViewportHeight;
};

SamplerState samplerLinearClamp : register(s0);
Texture2D in_texture : register(t0);
RWTexture2D<float4> out_texture : register(u0);

#include "nis/nis_coefficients.hlsli"
#include "nis/nis_scaler.hlsli"

[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID) {
#if NIS_SCALER
  NVScaler(blockIdx.xy, threadIdx.x);
#else
  NVSharpen(blockIdx.xy, threadIdx.x);
#endif
}
