#include "render/D3D11Compositor.h"

#include "core/model/RenderVersion.h"
#include "render/ColorManagement.h"
#include "render/ColorOps.h"
#include "render/CubeLut.h"
#include "render/Transitions.h"
#include "render/CompositorParams.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sstream>
#include <stdexcept>

namespace cutline::render::gpu {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

// ------------------------------------------------------------------ shaders ----
//
// One source, compiled in two variants: picture textures held in memory (RGBA) and video planes left by a hardware
// decoder (YUV). The arithmetic is the software compositor's, line for line (render/Compositor.cpp: DrawFrame,
// SampleBilinear, ApplyOpacity, ApplyGrade, Layer::CompositeOnto, Layer::Mix).

constexpr const char* kShaderSource = R"hlsl(
cbuffer P : register(b0) {
  int4 sizes;     // output width, height, source width, height
  float4 geo0;    // centre x, y; anchor x, y (source pixels)
  float4 geo1;    // scale x, y (fit included); cos, sin of the negated rotation
  float4 crop;    // visible source rectangle: left, top, right, bottom (pixels)
  float4 solid;   // premultiplied colour of a generator
  int4 modes;     // source mode (0 picture, 1 solid, 2 nothing), operation count, array slice, unused
  float4 weight;  // dissolve progress
  float4 yuv0;    // y scale, y offset, chroma scale, chroma midpoint
  float4 yuv1;    // r from cr, g from cb, g from cr, b from cb
  float4 yuv2;    // normalisation of the stored value (1 for 8 bit, 65535/64512 for 10 bit in 16)
  float4 ops[96]; // blocks of six float4, one per operation; the first value of a block is its kind (render/ColorOps.h lists them)
};

#if YUV
Texture2DArray<float> lumaTex : register(t0);
Texture2DArray<float2> chromaTex : register(t1);
#else
Texture2D<float4> srcTex : register(t0);
#endif
Texture2D<float4> canvasIn : register(t2);
Texture2D<float4> layerA : register(t3);
Texture2D<float4> layerB : register(t4);
Texture3D<float4> lutTex0 : register(t5);
Texture3D<float4> lutTex1 : register(t6);
Texture3D<float4> lutTex2 : register(t7);
Texture3D<float4> lutTex3 : register(t8);
Texture3D<float4> lutTex4 : register(t9);
Texture3D<float4> lutTex5 : register(t10);
Texture3D<float4> lutTex6 : register(t11);
Texture3D<float4> lutTex7 : register(t12);
Texture2D<float> curveTex : register(t13);  // curves: 256 entries a row, five rows for each curves operation
RWTexture2D<float4> dst : register(u0);
RWTexture2D<unorm float4> out8 : register(u1);

float4 Fetch(int2 p) {
#if YUV
  float y = (lumaTex.Load(int4(p, modes.z, 0)) * yuv2.x - yuv0.y) * yuv0.x;
  // 4:2:0: each chroma sample serves the 2x2 luma pixels it covers, which is what swscale does when it converts without scaling
  // (the software decode path), so a picture looks the same decoded either way.
  float2 uv = chromaTex.Load(int4(p >> 1, modes.z, 0)) * yuv2.x;
  float cb = (uv.x - yuv0.w) * yuv0.z;
  float cr = (uv.y - yuv0.w) * yuv0.z;
  float3 rgb = float3(y + yuv1.x * cr, y + yuv1.y * cb + yuv1.z * cr, y + yuv1.w * cb);
  return float4(saturate(rgb), 1.0);
#else
  float4 t = srcTex.Load(int3(p, 0));
  return float4(t.rgb * t.a, t.a);   // premultiply as we read, as the software sampler does
#endif
}

// SampleBilinear(frame, x, y): transparent outside the picture, otherwise four clamped taps.
float4 SampleSource(float x, float y) {
  float w = (float)sizes.z, h = (float)sizes.w;
  if (x < -0.5 || y < -0.5 || x > w - 0.5 || y > h - 0.5) return float4(0, 0, 0, 0);
  float fx = floor(x), fy = floor(y);
  float tx = x - fx, ty = y - fy;
  int x0 = clamp((int)fx, 0, sizes.z - 1), x1 = clamp((int)fx + 1, 0, sizes.z - 1);
  int y0 = clamp((int)fy, 0, sizes.w - 1), y1 = clamp((int)fy + 1, 0, sizes.w - 1);
  float4 p00 = Fetch(int2(x0, y0)), p10 = Fetch(int2(x1, y0)), p01 = Fetch(int2(x0, y1)), p11 = Fetch(int2(x1, y1));
  return lerp(lerp(p00, p10, tx), lerp(p01, p11, tx), ty);
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float4 LutLoad(int slot, int4 location) {
  if (slot == 0) return lutTex0.Load(location);
  if (slot == 1) return lutTex1.Load(location);
  if (slot == 2) return lutTex2.Load(location);
  if (slot == 3) return lutTex3.Load(location);
  if (slot == 4) return lutTex4.Load(location);
  if (slot == 5) return lutTex5.Load(location);
  if (slot == 6) return lutTex6.Load(location);
  return lutTex7.Load(location);
}

// Colour tools, each the software compositor's arithmetic (render/ColorEffects.cpp, Filters.cpp, Compositor.cpp).
float CurveAt(int row, float v) {
  float position = saturate(v) * 255.0;
  int index = (int)position;
  if (index >= 255) return curveTex.Load(int3(255, row, 0));
  float t = position - (float)index;
  float a = curveTex.Load(int3(index, row, 0));
  float b = curveTex.Load(int3(index + 1, row, 0));
  return a + (b - a) * t;
}

float3 Lut(float3 c, float4 o0, float4 o1, float4 o2) {
  int slot = (int)o2.w;
  int size = (int)o0.z;
  float3 n = o1.w > 0.5 ? c : (c - o1.xyz) / o2.xyz;
  n = saturate(n);   // held at the edges of the domain; a NaN lands on the first entry
  float3 coord = n * (float)(size - 1);
  int3 lo = (int3)floor(coord);
  int3 hi = min(lo + 1, size - 1);
  float3 f = coord - (float3)lo;
  if (o0.w > 0.5) {
    float3 a = float3(LutLoad(slot, int4(lo.x, 0, 0, 0)).r, LutLoad(slot, int4(lo.y, 0, 0, 0)).g, LutLoad(slot, int4(lo.z, 0, 0, 0)).b);
    float3 b = float3(LutLoad(slot, int4(hi.x, 0, 0, 0)).r, LutLoad(slot, int4(hi.y, 0, 0, 0)).g, LutLoad(slot, int4(hi.z, 0, 0, 0)).b);
    return a + (b - a) * f;
  }
  float3 c000 = LutLoad(slot, int4(lo.x, lo.y, lo.z, 0)).rgb;
  float3 c100 = LutLoad(slot, int4(hi.x, lo.y, lo.z, 0)).rgb;
  float3 c010 = LutLoad(slot, int4(lo.x, hi.y, lo.z, 0)).rgb;
  float3 c110 = LutLoad(slot, int4(hi.x, hi.y, lo.z, 0)).rgb;
  float3 c001 = LutLoad(slot, int4(lo.x, lo.y, hi.z, 0)).rgb;
  float3 c101 = LutLoad(slot, int4(hi.x, lo.y, hi.z, 0)).rgb;
  float3 c011 = LutLoad(slot, int4(lo.x, hi.y, hi.z, 0)).rgb;
  float3 c111 = LutLoad(slot, int4(hi.x, hi.y, hi.z, 0)).rgb;
  float3 c00 = c000 + (c100 - c000) * f.x;
  float3 c10 = c010 + (c110 - c010) * f.x;
  float3 c01 = c001 + (c101 - c001) * f.x;
  float3 c11 = c011 + (c111 - c011) * f.x;
  float3 d0 = c00 + (c10 - c00) * f.y;
  float3 d1 = c01 + (c11 - c01) * f.y;
  return d0 + (d1 - d0) * f.z;
}

float3 Wheels(float3 c, float4 o0, float4 o1, float4 o2, float4 o3, float4 o4, float4 o5) {
  float3 v = max(o2.xyz * (c + o0.yzw * (1.0 - c)), 0.0);
  v.r = o1.x == 1.0 ? v.r : pow(v.r, o1.x);
  v.g = o1.y == 1.0 ? v.g : pow(v.g, o1.y);
  v.b = o1.z == 1.0 ? v.b : pow(v.b, o1.z);
  float l = dot(v, kLuma);
  float ws = 1.0 - smoothstep(0.0, 0.5, l);
  float wh = smoothstep(0.5, 1.0, l);
  float wm = 1.0 - ws - wh;
  return v + o3.xyz * ws + o4.xyz * wm + o5.xyz * wh;
}

float3 Curves(float3 c, float4 o0) {
  int flags = (int)o0.y;
  int row = (int)o0.z;
  if (flags & 1) c = float3(CurveAt(row, c.r), CurveAt(row, c.g), CurveAt(row, c.b));
  if (flags & 2) c.r = CurveAt(row + 1, c.r);
  if (flags & 4) c.g = CurveAt(row + 2, c.g);
  if (flags & 8) c.b = CurveAt(row + 3, c.b);
  if (flags & 16) {
    float l = dot(c, kLuma);
    float target = CurveAt(row + 4, l);
    if (l > 1e-5) c *= target / l; else c = float3(target, target, target);
  }
  return c;
}

float3 Adjust(float3 c, float4 o0, float4 o1) {
  c *= o0.yzw;
  if (abs(o1.x) > 1e-6) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float sat = mx > 1e-6 ? (mx - mn) / mx : 0.0;
    float factor = max(0.0, 1.0 + o1.x * (1.0 - sat));
    float l = dot(c, kLuma);
    c = l + (c - l) * factor;
  }
  if (abs(o1.y) > 1e-6 || abs(o1.z) > 1e-6) {
    float l = saturate(dot(c, kLuma));
    c += 0.25 * (o1.y * (1.0 - smoothstep(0.0, 0.5, l)) + o1.z * smoothstep(0.5, 1.0, l));
  }
  return c;
}

// The twenty-four values of an operation's block, by position.
float BlockF(int base, int k) { return ops[base + (k >> 2)][k & 3]; }

float3 ToHsl(float3 c) {
  float mx = max(c.r, max(c.g, c.b));
  float mn = min(c.r, min(c.g, c.b));
  float l = (mx + mn) * 0.5;
  float d = mx - mn;
  float h = 0.0, s = 0.0;
  if (d > 1e-7) {
    s = clamp(d / (1.0 - abs(2.0 * l - 1.0) + 1e-12), 0.0, 1.0);
    float hue;
    if (mx == c.r) hue = fmod((c.g - c.b) / d, 6.0);
    else if (mx == c.g) hue = (c.b - c.r) / d + 2.0;
    else hue = (c.r - c.g) / d + 4.0;
    h = hue * 60.0;
    if (h < 0.0) h += 360.0;
  }
  return float3(h, s, l);
}

float3 FromHsl(float3 hsl) {
  float chroma = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
  float h = fmod(hsl.x, 360.0);
  if (h < 0.0) h += 360.0;
  float sector = h / 60.0;
  float x = chroma * (1.0 - abs(fmod(sector, 2.0) - 1.0));
  float r = 0.0, g = 0.0, b = 0.0;
  if (sector < 1.0) { r = chroma; g = x; }
  else if (sector < 2.0) { r = x; g = chroma; }
  else if (sector < 3.0) { g = chroma; b = x; }
  else if (sector < 4.0) { g = x; b = chroma; }
  else if (sector < 5.0) { r = x; b = chroma; }
  else { r = chroma; b = x; }
  float m = hsl.z - chroma * 0.5;
  return float3(r + m, g + m, b + m);
}

// Periodic Catmull-Rom through six knots 60 degrees apart, the knots at float `first` of the block.
float Periodic(int base, int first, float hue) {
  float position = fmod(hue, 360.0) / 60.0;
  int b = (int)floor(position);
  float t = position - (float)b;
  float p0 = BlockF(base, first + (((b - 1) % 6) + 6) % 6);
  float p1 = BlockF(base, first + ((b % 6) + 6) % 6);
  float p2 = BlockF(base, first + (((b + 1) % 6) + 6) % 6);
  float p3 = BlockF(base, first + (((b + 2) % 6) + 6) % 6);
  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

float3 HueCurves(float3 c, int base) {
  float3 hsl = ToHsl(c);
  float weight = smoothstep(0.0, 0.1, hsl.y);
  if (weight <= 0.0) return c;
  float shift = Periodic(base, 1, hsl.x) * weight;
  float sat = Periodic(base, 7, hsl.x) * weight;
  float lum = Periodic(base, 13, hsl.x) * weight;
  hsl.x += shift;
  hsl.y = clamp(hsl.y * (1.0 + sat), 0.0, 1.0);
  hsl.z = lum >= 0.0 ? hsl.z + (1.0 - hsl.z) * min(lum, 1.0) : hsl.z * (1.0 + max(lum, -1.0));
  return FromHsl(hsl);
}

float Band(float value, float low, float high, float softness) {
  if (softness <= 0.0) return value >= low && value <= high ? 1.0 : 0.0;
  return smoothstep(low - softness, low, value) * (1.0 - smoothstep(high, high + softness, value));
}

float3 HslSecondary(float3 c, int base) {
  float hue_centre = BlockF(base, 1), hue_width = BlockF(base, 2), hue_softness = BlockF(base, 3);
  float3 hsl = ToHsl(c);
  float hue_matte = 1.0;
  if (hue_width < 359.99) {
    if (hsl.y < 1e-4) {
      hue_matte = 0.0;
    } else {
      float angle = abs(fmod(hsl.x - hue_centre, 360.0));
      if (angle > 180.0) angle = 360.0 - angle;
      float half_width = hue_width * 0.5;
      hue_matte = hue_softness <= 0.0 ? (angle <= half_width ? 1.0 : 0.0) : 1.0 - smoothstep(half_width, half_width + hue_softness, angle);
    }
  }
  float matte = hue_matte * Band(hsl.y, BlockF(base, 4), BlockF(base, 5), BlockF(base, 6)) * Band(hsl.z, BlockF(base, 7), BlockF(base, 8), BlockF(base, 9));
  if (BlockF(base, 14) > 0.5) matte = 1.0 - matte;
  if (BlockF(base, 13) > 0.5) return float3(matte, matte, matte);
  float3 corrected = hsl;
  corrected.x += BlockF(base, 10);
  corrected.y = clamp(corrected.y * BlockF(base, 11), 0.0, 1.0);
  float lightness = BlockF(base, 12);
  corrected.z = lightness >= 0.0 ? corrected.z + (1.0 - corrected.z) * lightness : corrected.z * (1.0 + lightness);
  float3 result = FromHsl(corrected);
  return c + (result - c) * matte;
}

float4 ApplyOps(float4 p) {
  int count = modes.y;
  [loop] for (int i = 0; i < count; ++i) {
    float4 o0 = ops[6 * i];
    float4 o1 = ops[6 * i + 1];
    float4 o2 = ops[6 * i + 2];
    int kind = (int)o0.x;
    if (kind == 1) {
      p *= o0.y;
    } else if (p.a > 0.0) {
      float inv = p.a >= 1.0 ? 1.0 : 1.0 / p.a;
      float3 c = p.rgb * inv;
      if (kind == 2) {
        c *= o0.y;
        c = (c - 0.5) * o0.z + 0.5;
        float luma = dot(c, kLuma);
        c = luma + (c - luma) * o0.w;
        c.r += o1.x * 0.1;
        c.b -= o1.x * 0.1;
      } else if (kind == 3) {
        float3 mapped = Lut(c, o0, o1, o2);
        c = c + (mapped - c) * o0.y;
      } else if (kind == 4) {
        c = Wheels(c, o0, o1, o2, ops[6 * i + 3], ops[6 * i + 4], ops[6 * i + 5]);
      } else if (kind == 5) {
        c = Curves(c, o0);
      } else if (kind == 6) {
        c = float3(dot(c, o0.yzw), dot(c, o1.xyz), dot(c, o2.xyz));
      } else if (kind == 7) {
        float l = saturate(dot(c, kLuma));
        float3 mapped = o0.yzw + (o1.xyz - o0.yzw) * l;
        c = c + (mapped - c) * o1.w;
      } else if (kind == 8) {
        float grey = dot(c, o0.yzw);
        c = c + (grey - c) * o1.x;
      } else if (kind == 9) {
        c = Adjust(c, o0, o1);
      } else if (kind == 10) {
        c = HueCurves(c, 6 * i);
      } else if (kind == 11) {
        c = HslSecondary(c, 6 * i);
      }
      c = clamp(c, 0.0, 1.0);
      p.rgb = c * p.a;
    }
  }
  return p;
}

// Draws one layer: the source placed by the motion transform and crop, with the clip's effects, either over the
// canvas (dest mode 0, reading canvasIn) or alone into a layer (dest mode 1).
[numthreads(16, 16, 1)] void CSDrawOver(uint3 id : SV_DispatchThreadID) {
  if ((int)id.x >= sizes.x || (int)id.y >= sizes.y) return;
  float4 p = float4(0, 0, 0, 0);
  if (modes.x == 1) {
    p = solid;
  } else if (modes.x == 0) {
    float dx = (float)id.x + 0.5 - geo0.x;
    float dy = (float)id.y + 0.5 - geo0.y;
    float rx = dx * geo1.z - dy * geo1.w;
    float ry = dx * geo1.w + dy * geo1.z;
    float sx = rx / geo1.x + geo0.z;
    float sy = ry / geo1.y + geo0.w;
    if (!(sx < crop.x || sx >= crop.z || sy < crop.y || sy >= crop.w)) p = SampleSource(sx - 0.5, sy - 0.5);
  }
  p = ApplyOps(p);
  float4 below = canvasIn.Load(int3(id.xy, 0));
  dst[id.xy] = p + below * (1.0 - p.a);
}

[numthreads(16, 16, 1)] void CSDrawLayer(uint3 id : SV_DispatchThreadID) {
  if ((int)id.x >= sizes.x || (int)id.y >= sizes.y) return;
  float4 p = float4(0, 0, 0, 0);
  if (modes.x == 1) {
    p = solid;
  } else if (modes.x == 0) {
    float dx = (float)id.x + 0.5 - geo0.x;
    float dy = (float)id.y + 0.5 - geo0.y;
    float rx = dx * geo1.z - dy * geo1.w;
    float ry = dx * geo1.w + dy * geo1.z;
    float sx = rx / geo1.x + geo0.z;
    float sy = ry / geo1.y + geo0.w;
    if (!(sx < crop.x || sx >= crop.z || sy < crop.y || sy >= crop.w)) p = SampleSource(sx - 0.5, sy - 0.5);
  }
  dst[id.xy] = ApplyOps(p);
}

// An adjustment clip or a sequence-level effect: the operations applied to what is already composited.
[numthreads(16, 16, 1)] void CSOpsOnCanvas(uint3 id : SV_DispatchThreadID) {
  if ((int)id.x >= sizes.x || (int)id.y >= sizes.y) return;
  dst[id.xy] = ApplyOps(canvasIn.Load(int3(id.xy, 0)));
}

// A dissolve: the two sides mixed by the progress, then over the canvas.
[numthreads(16, 16, 1)] void CSMixOver(uint3 id : SV_DispatchThreadID) {
  if ((int)id.x >= sizes.x || (int)id.y >= sizes.y) return;
  float w = weight.x;
  float4 m = layerA.Load(int3(id.xy, 0)) * (1.0 - w) + layerB.Load(int3(id.xy, 0)) * w;
  float4 below = canvasIn.Load(int3(id.xy, 0));
  dst[id.xy] = m + below * (1.0 - m.a);
}

float4 Flatten(float4 c) {
  float3 rgb = c.a >= 1.0 ? c.rgb : (c.a > 0.0 ? c.rgb * (1.0 / c.a) : float3(0, 0, 0));
  return float4(rgb, c.a);
}

[numthreads(16, 16, 1)] void CSOut32(uint3 id : SV_DispatchThreadID) {
  if ((int)id.x >= sizes.x || (int)id.y >= sizes.y) return;
  dst[id.xy] = Flatten(canvasIn.Load(int3(id.xy, 0)));
}

[numthreads(16, 16, 1)] void CSOut8(uint3 id : SV_DispatchThreadID) {
  if ((int)id.x >= sizes.x || (int)id.y >= sizes.y) return;
  out8[id.xy] = saturate(Flatten(canvasIn.Load(int3(id.xy, 0))));
}
)hlsl";

// The constants the shaders read; the layout mirrors the cbuffer above.
struct Params final {
  std::int32_t sizes[4]{};
  float geo0[4]{};
  float geo1[4]{};
  float crop[4]{};
  float solid[4]{};
  std::int32_t modes[4]{};
  float weight[4]{};
  float yuv0[4]{};
  float yuv1[4]{};
  float yuv2[4]{1, 0, 0, 0};
  float ops[96][4]{};
};
static_assert(sizeof(Params) % 16 == 0);
constexpr int kMaxOps = 16;
constexpr int kOpStride = 6;   // float4 values in one operation's block
constexpr int kMaxBoundLuts = 8;
constexpr int kMaxLutSize = 129;   // a bigger cube is left to the software path (129 points a side is 34 MB on the card)
constexpr int kMaxCachedLuts = 8;

enum SourceMode { kPicture = 0, kSolid = 1, kNothing = 2 };

std::string HrText(HRESULT hr) {
  std::ostringstream text;
  text << "0x" << std::hex << static_cast<unsigned long>(hr);
  return text.str();
}

Vendor VendorOf(UINT id, bool software) {
  if (software) return Vendor::Software;
  switch (id) {
    case 0x1002: return Vendor::Amd;
    case 0x10DE: return Vendor::Nvidia;
    case 0x8086: return Vendor::Intel;
    case 0x1414: return Vendor::Software;
    default: return Vendor::Unknown;
  }
}

std::string NarrowName(const wchar_t* wide) {
  std::string out;
  for (; *wide != 0; ++wide) out.push_back(*wide < 128 ? static_cast<char>(*wide) : '?');
  return out;
}

struct AdapterInfo final {
  ComPtr<IDXGIAdapter1> adapter;
  DeviceDescriptor descriptor;
};

std::vector<AdapterInfo> ListAdapters() {
  std::vector<AdapterInfo> adapters;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return adapters;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414;
    DeviceDescriptor descriptor;
    std::ostringstream id;
    id << "d3d11:" << std::hex << desc.AdapterLuid.HighPart << ":" << desc.AdapterLuid.LowPart;
    descriptor.id = id.str();
    descriptor.name = NarrowName(desc.Description);
    descriptor.backend = Backend::D3D11;
    descriptor.vendor = VendorOf(desc.VendorId, software);
    descriptor.dedicated_memory_bytes = desc.DedicatedVideoMemory;
    descriptor.shared_memory_bytes = desc.SharedSystemMemory;
    descriptor.integrated = !software && desc.DedicatedVideoMemory < (512ull << 20);
    descriptor.capabilities = Flag(Capability::Compositor) | Flag(Capability::Float16) | Flag(Capability::TenBit);
    // A hardware adapter that can decode video into its own textures can feed this compositor without a copy.
    if (!software) descriptor.capabilities |= Flag(Capability::ZeroCopyDecode);
    descriptor.available = true;
    adapters.push_back({adapter, std::move(descriptor)});
  }
  return adapters;
}

// ------------------------------------------------------------------ texture pool ----

struct PooledTexture final {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> view;
  int width{0};
  int height{0};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
  std::uint64_t used_frame{0};
};

struct Target final {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
};

}  // namespace

struct D3D11Compositor::Impl final {
  DeviceDescriptor descriptor;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11ComputeShader> draw_over, draw_layer, ops_on_canvas, mix_over, out32, out8;
  ComPtr<ID3D11ComputeShader> draw_over_yuv, draw_layer_yuv;

  int width{0}, height{0};
  Target canvas[2];
  Target layer_a, layer_b;
  Target out_f32, out_u8;
  ComPtr<ID3D11Texture2D> staging_f32, staging_u8;
  std::vector<PooledTexture> pool;
  std::uint64_t frame_counter{0};
  std::size_t texture_bytes{0};

  ComPtr<ID3D11Query> disjoint, begin_stamp, end_stamp;
  std::mutex mutex;

  // LUTs the card holds, by the file they came from, and files that could not be read (so a bad one is not parsed
  // again every frame, but is tried again when the file changes or appears).
  struct GpuLut final {
    CubeLut lut;
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    std::uint64_t used_frame{0};
  };
  struct LutFailure final {
    bool existed{false};
    std::filesystem::file_time_type modified{};
    std::string message;
  };
  std::map<std::string, GpuLut> luts;
  std::map<std::string, LutFailure> lut_failures;
  std::string asset_root;
  ComPtr<ID3D11Texture2D> curve_texture;
  ComPtr<ID3D11ShaderResourceView> curve_view;
  std::array<ID3D11ShaderResourceView*, kMaxBoundLuts> current_luts{};
  ID3D11ShaderResourceView* current_curves{nullptr};

  // Per-frame draw state.
  int current_canvas{0};
  Params params;
  GpuStatistics* stats{nullptr};

  [[nodiscard]] ComPtr<ID3D11ComputeShader> Compile(const char* entry, bool yuv, std::string* why) {
    const D3D_SHADER_MACRO macros[] = {{"YUV", yuv ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "cutline.hlsl", macros, nullptr, entry, "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
      if (why != nullptr) *why = std::string("shader ") + entry + " did not compile: " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : HrText(hr));
      return nullptr;
    }
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) {
      if (why != nullptr) *why = std::string("the device refused shader ") + entry;
      return nullptr;
    }
    return shader;
  }

  [[nodiscard]] bool Initialise(std::string* why) {
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Params);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constants))) {
      if (why != nullptr) *why = "could not create the constant buffer";
      return false;
    }
    struct Entry { ComPtr<ID3D11ComputeShader>* slot; const char* name; bool yuv; };
    for (const auto& entry : {Entry{&draw_over, "CSDrawOver", false}, Entry{&draw_layer, "CSDrawLayer", false},
                              Entry{&ops_on_canvas, "CSOpsOnCanvas", false}, Entry{&mix_over, "CSMixOver", false},
                              Entry{&out32, "CSOut32", false}, Entry{&out8, "CSOut8", false},
                              Entry{&draw_over_yuv, "CSDrawOver", true}, Entry{&draw_layer_yuv, "CSDrawLayer", true}}) {
      *entry.slot = Compile(entry.name, entry.yuv, why);
      if (!*entry.slot) return false;
    }
    D3D11_QUERY_DESC query{};
    query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    (void)device->CreateQuery(&query, &disjoint);
    query.Query = D3D11_QUERY_TIMESTAMP;
    (void)device->CreateQuery(&query, &begin_stamp);
    (void)device->CreateQuery(&query, &end_stamp);
    return true;
  }

  [[nodiscard]] bool MakeTarget(Target& target, int w, int h, DXGI_FORMAT format, bool srv) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
    target = {};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.texture))) return false;
    if (FAILED(device->CreateUnorderedAccessView(target.texture.Get(), nullptr, &target.uav))) return false;
    if (srv && FAILED(device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv))) return false;
    return true;
  }

  [[nodiscard]] bool EnsureTargets(int w, int h, bool f32, bool transitions, std::string* why) {
    if (w != width || h != height) ReleaseTargets();
    // Half float keeps a layer at 8 bytes a pixel (a 1080p canvas is 16 MB) with a precision far finer than an 8 or 10
    // bit picture can show; the canvases alternate, because reading and writing one typed texture in one pass is not
    // something every device supports.
    const auto failed = [&] {
      ReleaseTargets();
      if (why != nullptr) *why = "not enough video memory for a " + std::to_string(w) + "x" + std::to_string(h) + " picture";
      return false;
    };
    if (!canvas[0].texture &&
        (!MakeTarget(canvas[0], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
         !MakeTarget(canvas[1], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true))) {
      return failed();
    }
    if (transitions && !layer_a.texture &&
        (!MakeTarget(layer_a, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
         !MakeTarget(layer_b, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true))) {
      return failed();
    }
    auto make_staging = [&](DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& target) {
      D3D11_TEXTURE2D_DESC staging{};
      staging.Width = static_cast<UINT>(w);
      staging.Height = static_cast<UINT>(h);
      staging.MipLevels = 1;
      staging.ArraySize = 1;
      staging.SampleDesc.Count = 1;
      staging.Usage = D3D11_USAGE_STAGING;
      staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging.Format = format;
      return SUCCEEDED(device->CreateTexture2D(&staging, nullptr, &target));
    };
    if (f32) {
      if (!out_f32.texture &&
          (!MakeTarget(out_f32, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, false) ||
           !make_staging(DXGI_FORMAT_R32G32B32A32_FLOAT, staging_f32))) {
        return failed();
      }
    } else if (!out_u8.texture &&
               (!MakeTarget(out_u8, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false) ||
                !make_staging(DXGI_FORMAT_R8G8B8A8_UNORM, staging_u8))) {
      return failed();
    }
    width = w;
    height = h;
    const auto pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    texture_bytes = pixels * (16 + (layer_a.texture ? 16 : 0) + (out_f32.texture ? 32 : 0) +
                              (out_u8.texture ? 8 : 0));
    return true;
  }

  void ReleaseTargets() {
    canvas[0] = canvas[1] = layer_a = layer_b = out_f32 = out_u8 = {};
    staging_f32.Reset();
    staging_u8.Reset();
    width = height = 0;
    texture_bytes = 0;
  }

  // A pooled texture for one picture in memory; textures are reused from frame to frame and the pool is trimmed when
  // sizes stop recurring.
  [[nodiscard]] PooledTexture* Acquire(int w, int h, DXGI_FORMAT format) {
    for (auto& entry : pool) {
      if (entry.used_frame != frame_counter && entry.width == w && entry.height == h && entry.format == format) {
        entry.used_frame = frame_counter;
        return &entry;
      }
    }
    // Drop textures not touched for a while before growing, so a change of media size does not strand memory.
    pool.erase(std::remove_if(pool.begin(), pool.end(), [&](const PooledTexture& entry) { return frame_counter - entry.used_frame > 240; }), pool.end());
    PooledTexture fresh;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &fresh.texture))) return nullptr;
    if (FAILED(device->CreateShaderResourceView(fresh.texture.Get(), nullptr, &fresh.view))) return nullptr;
    fresh.width = w;
    fresh.height = h;
    fresh.format = format;
    fresh.used_frame = frame_counter;
    pool.push_back(std::move(fresh));
    return &pool.back();
  }

  // Copies a picture into the device. Rows are copied one at a time because the two sides pad them differently.
  [[nodiscard]] PooledTexture* Upload(const media::VideoFrame& frame) {
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::size_t row_bytes = static_cast<std::size_t>(frame.width()) * 4;
    switch (frame.format()) {
      case media::PixelFormat::Rgba8: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
      case media::PixelFormat::Rgba16: format = DXGI_FORMAT_R16G16B16A16_UNORM; row_bytes *= 2; break;
      case media::PixelFormat::RgbaF32: format = DXGI_FORMAT_R32G32B32A32_FLOAT; row_bytes *= 4; break;
    }
    auto* entry = Acquire(frame.width(), frame.height(), format);
    if (entry == nullptr) return nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(entry->texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return nullptr;
    const auto* from = reinterpret_cast<const std::uint8_t*>(frame.data());
    auto* to = static_cast<std::uint8_t*>(mapped.pData);
    if (static_cast<std::ptrdiff_t>(mapped.RowPitch) == frame.stride()) {
      std::memcpy(to, from, static_cast<std::size_t>(frame.stride()) * static_cast<std::size_t>(frame.height()));
    } else {
      for (int y = 0; y < frame.height(); ++y) std::memcpy(to + static_cast<std::size_t>(y) * mapped.RowPitch, from + static_cast<std::ptrdiff_t>(y) * frame.stride(), row_bytes);
    }
    context->Unmap(entry->texture.Get(), 0);
    if (stats != nullptr) ++stats->sources_uploaded;
    return entry;
  }

  void WriteParams() {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      std::memcpy(mapped.pData, &params, sizeof(Params));
      context->Unmap(constants.Get(), 0);
    }
    ID3D11Buffer* buffers[] = {constants.Get()};
    context->CSSetConstantBuffers(0, 1, buffers);
  }

  void Dispatch(ID3D11ComputeShader* shader) {
    WriteParams();
    context->CSSetShader(shader, nullptr, 0);
    context->Dispatch(static_cast<UINT>((width + 15) / 16), static_cast<UINT>((height + 15) / 16), 1);
    if (stats != nullptr) ++stats->passes;
  }

  void Unbind() {
    ID3D11ShaderResourceView* no_srv[14] = {};
    ID3D11UnorderedAccessView* no_uav[2] = {};
    context->CSSetShaderResources(0, 14, no_srv);
    context->CSSetUnorderedAccessViews(0, 2, no_uav, nullptr);
  }

  void SetBaseParams() {
    params = Params{};
    params.sizes[0] = width;
    params.sizes[1] = height;
  }

  // The file a LUT effect names, as the software compositor finds it.
  [[nodiscard]] std::string LutKey(const SampledEffect& effect) const {
    if (effect.preset_name.empty()) return {};
    std::filesystem::path path(effect.preset_name);
    if (path.is_relative() && !asset_root.empty()) path = std::filesystem::path(asset_root) / path;
    return path.lexically_normal().string();
  }

  // The LUT of this effect on the card, loading and uploading it the first time. Nothing when it cannot be used (no
  // file, not a table, too large for the card): the software compositor then draws the effect and reports why.
  [[nodiscard]] GpuLut* ResolveLut(const SampledEffect& effect, std::string* why) {
    const auto key = LutKey(effect);
    if (key.empty()) {
      if (why != nullptr) *why = "a LUT effect with no file";
      return nullptr;
    }
    if (const auto found = luts.find(key); found != luts.end()) {
      found->second.used_frame = frame_counter;
      return &found->second;
    }
    std::error_code error;
    const auto exists = std::filesystem::exists(key, error) && !error;
    const auto modified = exists ? std::filesystem::last_write_time(key, error) : std::filesystem::file_time_type{};
    if (const auto failed = lut_failures.find(key); failed != lut_failures.end()) {
      if (failed->second.existed == exists && (!exists || (!error && failed->second.modified == modified))) {
        if (why != nullptr) *why = failed->second.message;
        return nullptr;
      }
      lut_failures.erase(failed);
    }
    const auto fail = [&](const std::string& message) -> GpuLut* {
      lut_failures[key] = {exists, modified, message};
      if (why != nullptr) *why = message;
      return nullptr;
    };
    try {
      GpuLut entry;
      entry.lut = CubeLut::Load(key);
      const bool cube = entry.lut.kind() == CubeLut::Kind::Cube3D;
      const int n = entry.lut.size();
      if (n > kMaxLutSize) return fail("a LUT larger than " + std::to_string(kMaxLutSize) + " points a side");
      // Red, green, blue and an unused fourth value, because a card reads four-component textures.
      std::vector<float> texels(entry.lut.entry_count() * 4);
      const float* from = entry.lut.data();
      for (std::size_t i = 0; i < entry.lut.entry_count(); ++i) {
        texels[i * 4] = from[i * 3];
        texels[i * 4 + 1] = from[i * 3 + 1];
        texels[i * 4 + 2] = from[i * 3 + 2];
        texels[i * 4 + 3] = 1.0f;
      }
      D3D11_TEXTURE3D_DESC desc{};
      desc.Width = static_cast<UINT>(n);
      desc.Height = cube ? static_cast<UINT>(n) : 1u;
      desc.Depth = cube ? static_cast<UINT>(n) : 1u;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      desc.Usage = D3D11_USAGE_IMMUTABLE;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data{};
      data.pSysMem = texels.data();
      data.SysMemPitch = static_cast<UINT>(n) * 16u;
      data.SysMemSlicePitch = data.SysMemPitch * desc.Height;
      if (FAILED(device->CreateTexture3D(&desc, &data, &entry.texture)) || FAILED(device->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.view))) {
        return fail("the card could not hold the LUT");
      }
      entry.used_frame = frame_counter;
      // A few at a time: the least recently used goes first.
      while (luts.size() >= static_cast<std::size_t>(kMaxCachedLuts)) {
        auto oldest = luts.begin();
        for (auto it = luts.begin(); it != luts.end(); ++it) {
          if (it->second.used_frame < oldest->second.used_frame) oldest = it;
        }
        luts.erase(oldest);
      }
      return &luts.emplace(key, std::move(entry)).first->second;
    } catch (const std::exception& problem) {
      return fail(std::string("lut: ") + problem.what());
    }
  }

  void FillOps(const std::vector<SampledEffect>& effects) {
    int count = 0;
    int lut_count = 0;
    current_luts.fill(nullptr);
    current_curves = nullptr;
    std::vector<float> curve_rows;
    for (const auto& effect : effects) {
      if (count >= kMaxOps) break;
      auto* block = params.ops[kOpStride * count];
      if (effect.effect_type == "opacity") {
        const auto scale = std::clamp(ScalarParameter(effect, "value", 1.0f), 0.0f, 1.0f);
        if (std::abs(scale - 1.0f) < 1e-6f) continue;
        block[0] = 1.0f;
        block[1] = scale;
        ++count;
      } else if (effect.effect_type == "grade" || effect.effect_type == "lumetri") {
        const auto grade = ReadGrade(effect);
        if (grade.identity()) continue;
        block[0] = 2.0f;
        block[1] = std::pow(2.0f, grade.exposure_stops);
        block[2] = grade.contrast;
        block[3] = grade.saturation;
        params.ops[kOpStride * count + 1][0] = grade.temperature / 100.0f;
        ++count;
      } else if (effect.effect_type == "lut") {
        const auto intensity = std::clamp(ScalarParameter(effect, "intensity", 1.0f), 0.0f, 1.0f);
        if (intensity <= 1e-6f) continue;
        auto* entry = ResolveLut(effect, nullptr);
        if (entry == nullptr) continue;
        const auto& lut = entry->lut;
        block[0] = 3.0f;
        block[1] = intensity;
        block[2] = static_cast<float>(lut.size());
        block[3] = lut.kind() == CubeLut::Kind::Curves1D ? 1.0f : 0.0f;
        const bool default_domain = lut.domain_min() == std::array<float, 3>{0.0f, 0.0f, 0.0f} && lut.domain_max() == std::array<float, 3>{1.0f, 1.0f, 1.0f};
        for (int i = 0; i < 3; ++i) {
          params.ops[kOpStride * count + 1][i] = lut.domain_min()[static_cast<std::size_t>(i)];
          params.ops[kOpStride * count + 2][i] = lut.domain_max()[static_cast<std::size_t>(i)] - lut.domain_min()[static_cast<std::size_t>(i)];
        }
        params.ops[kOpStride * count + 1][3] = default_domain ? 1.0f : 0.0f;
        params.ops[kOpStride * count + 2][3] = static_cast<float>(lut_count);
        current_luts[static_cast<std::size_t>(lut_count++)] = entry->view.Get();
        ++count;
      } else if (IsGpuColorEffect(effect.effect_type)) {
        const auto op = DescribeColorOp(effect);
        if (!op) continue;
        std::memcpy(block, op->block, sizeof(op->block));
        block[0] = static_cast<float>(static_cast<int>(op->kind));
        if (op->kind == GpuColorKind::Curves) {
          if (curve_rows.empty()) curve_rows.assign(static_cast<std::size_t>(5 * kMaxOps) * 256, 0.0f);
          std::copy(op->tables.begin(), op->tables.end(), curve_rows.begin() + static_cast<std::ptrdiff_t>(5 * count) * 256);
          block[2] = static_cast<float>(5 * count);
        }
        ++count;
      }
    }
    params.modes[1] = count;
    if (!curve_rows.empty() && UploadCurves(curve_rows)) current_curves = curve_view.Get();
  }

  [[nodiscard]] bool UploadCurves(const std::vector<float>& rows) {
    if (!curve_texture) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = 256;
      desc.Height = static_cast<UINT>(5 * kMaxOps);
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_R32_FLOAT;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &curve_texture)) || FAILED(device->CreateShaderResourceView(curve_texture.Get(), nullptr, &curve_view))) {
        curve_texture.Reset();
        curve_view.Reset();
        return false;
      }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(curve_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    for (int row = 0; row < 5 * kMaxOps; ++row) {
      std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch, rows.data() + static_cast<std::size_t>(row) * 256, 256 * sizeof(float));
    }
    context->Unmap(curve_texture.Get(), 0);
    return true;
  }

  // Binds what the next pass reads and writes.
  void Bind(ID3D11ShaderResourceView* source0, ID3D11ShaderResourceView* source1, ID3D11ShaderResourceView* canvas_in,
            ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, ID3D11UnorderedAccessView* output, ID3D11UnorderedAccessView* output8 = nullptr) {
    ID3D11ShaderResourceView* views[14] = {source0, source1, canvas_in, a, b};
    for (std::size_t i = 0; i < current_luts.size(); ++i) views[5 + i] = current_luts[i];
    views[13] = current_curves;
    context->CSSetShaderResources(0, 14, views);
    ID3D11UnorderedAccessView* uavs[2] = {output, output8};
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
  }

  void ClearTarget(Target& target, float r, float g, float b, float a) {
    const float colour[4] = {r, g, b, a};
    context->ClearUnorderedAccessViewFloat(target.uav.Get(), colour);
  }

  // The luma and chroma views of a hardware-decoded picture.
  [[nodiscard]] bool MakePlaneViews(const DeviceFrame& frame, ComPtr<ID3D11ShaderResourceView>& luma, ComPtr<ID3D11ShaderResourceView>& chroma) {
    if (frame.device != static_cast<void*>(device.Get())) return false;  // a picture from another device cannot be read here
    auto* texture = static_cast<ID3D11Texture2D*>(frame.texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    const bool ten = frame.format == DeviceFormat::P010;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = desc.ArraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (desc.ArraySize > 1) {
      view.Texture2DArray.MipLevels = 1;
      view.Texture2DArray.FirstArraySlice = static_cast<UINT>(frame.array_index);
      view.Texture2DArray.ArraySize = 1;
    } else {
      view.Texture2D.MipLevels = 1;
    }
    view.Format = ten ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    if (FAILED(device->CreateShaderResourceView(texture, &view, &luma))) return false;
    view.Format = ten ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    return SUCCEEDED(device->CreateShaderResourceView(texture, &view, &chroma));
  }
};

// ------------------------------------------------------------------ the plan ----

namespace {

bool IsNoOpEffect(const SampledEffect& effect) {
  const auto zero = [&](const char* name) { return std::abs(ScalarParameter(effect, name, 0.0f)) <= 1e-6f; };
  const auto& type = effect.effect_type;
  if (type == "blur" || type == "gaussian_blur") return zero("radius");
  if (type == "sharpen" || type == "unsharp_mask") return zero("amount");
  if (type == "vignette") return zero("amount");
  if (type == "directional_blur") return zero("length");
  if (type == "glow") return zero("intensity") || zero("radius");
  if (type == "drop_shadow") return zero("opacity");
  if (type == "noise_reduction") return zero("luma") && zero("chroma") && zero("temporal");
  if (type == "posterize") return ScalarParameter(effect, "levels", 256.0f) >= 255.5f;
  if (type == "wave_warp") return zero("amplitude");
  if (type == "bulge") return zero("amount");
  if (type == "rolling_shutter") {
    return zero("horizontal") && zero("vertical") && zero("rotation") && zero("curve");
  }
  if (type == "lens_correction" || type == "wide_angle") {
    return zero("distortion") && zero("quadratic") &&
           std::abs(ScalarParameter(effect, "scale", 100.0f) - 100.0f) <= 1e-6f;
  }
  if (type == "mesh_warp") {
    for (const auto& parameter : effect.parameters) {
      if (parameter.name.rfind("point_", 0) != 0) continue;
      for (int component = 0; component < parameter.value.dimension; ++component) {
        if (std::abs(parameter.value.components[static_cast<std::size_t>(component)]) > 1e-6) return false;
      }
    }
    return true;
  }
  if (type == "blend_mode") {
    return static_cast<int>(std::lround(ScalarParameter(effect, "mode", 0.0f))) == 0 &&
           ScalarParameter(effect, "opacity", 1.0f) >= 1.0f - 1e-6f;
  }
  return false;
}

bool HasOnlySupportedEffects(const std::vector<SampledEffect>& effects, bool sequence_level, std::string* reason,
                             const std::function<bool(const SampledEffect&, std::string*)>& lut_usable) {
  int ops = 0;
  int luts = 0;
  for (const auto& effect : effects) {
    if (IsNoOpEffect(effect)) continue;
    if (!effect.masks.empty()) {
      if (reason != nullptr) *reason = "an effect has a mask";
      return false;
    }
    const auto& type = effect.effect_type;
    if (type == "opacity" || type == "grade" || type == "lumetri" || type == "lut" || IsGpuColorEffect(type)) {
      if (++ops > kMaxOps) {
        if (reason != nullptr) *reason = "more than sixteen colour effects on one clip";
        return false;
      }
      if (type == "lut") {
        if (++luts > kMaxBoundLuts) {
          if (reason != nullptr) *reason = "more than eight LUTs on one clip";
          return false;
        }
        if (!lut_usable(effect, reason)) return false;
      }
      continue;
    }
    if (!sequence_level && (type == "motion" || type == "transform" || type == "stabilizer" || type == "crop" || type == "solid" || type == "time_remap")) continue;
    if (!sequence_level && type == "frame_interpolation") {
      if (static_cast<int>(std::lround(ScalarParameter(effect, "mode", 0.0f))) > 0) {
        if (reason != nullptr) *reason = "frame interpolation";
        return false;
      }
      continue;
    }
    if (reason != nullptr) *reason = "the " + type + " effect";
    return false;
  }
  return true;
}

}  // namespace

bool D3D11Compositor::Supports(const timeline::PlaybackPlan& plan, const CompositorConfig& config, std::string* reason) const {
  const auto refuse = [&](const std::string& why) {
    if (reason != nullptr) *reason = why;
    return false;
  };
  if (config.output_format != media::PixelFormat::Rgba8 && config.output_format != media::PixelFormat::RgbaF32) return refuse("the output format");
  if (model::RenderSemantics::For(plan.render_version).color_managed) {
    // Colour management matters only when the working and display spaces differ; a picture tagged with another space
    // than the working one is caught when it arrives (Unsupported).
    const auto working = color::ParseSpace(plan.working_color_space), display = color::ParseSpace(plan.display_color_space);
    if (!working || !display || !(*working == *display)) return refuse("colour management between different spaces");
  }
  if (!plan.captions.empty()) return refuse("burned-in captions");
  auto& s = *impl_;
  const std::lock_guard<std::mutex> lock(s.mutex);
  s.asset_root = config.asset_root;
  const std::function<bool(const SampledEffect&, std::string*)> lut_usable = [&](const SampledEffect& effect, std::string* why) { return s.ResolveLut(effect, why) != nullptr; };
  const int width = config.width > 0 ? config.width : static_cast<int>(plan.width);
  const int height = config.height > 0 ? config.height : static_cast<int>(plan.height);
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return refuse("the output size");
  std::string why;
  for (const auto& request : plan.video) {
    if (request.depth != 0) continue;
    if (FindEffect(request.effects, "graphic") != nullptr || FindEffect(request.effects, "motion_graphics_template") != nullptr) return refuse("a graphic");
    if (FindEffect(request.effects, "blend_mode") != nullptr) return refuse("a blend mode");
    if (!HasOnlySupportedEffects(request.effects, false, &why, lut_usable)) return refuse(why);
  }
  if (!HasOnlySupportedEffects(plan.sequence_effects, true, &why, lut_usable)) return refuse(why);
  // The card's mix pass is the straight dissolve; the rest of the transition library is drawn in software.
  for (const auto& transition : plan.transitions) {
    if (!IsStraightDissolve(transition.kind)) return refuse("the " + transition.kind + " transition");
  }
  return true;
}

// ------------------------------------------------------------------ the factory ----

D3D11Compositor::D3D11Compositor() : impl_(std::make_unique<Impl>()) {}
D3D11Compositor::~D3D11Compositor() = default;

std::vector<DeviceDescriptor> D3D11Compositor::EnumerateDevices() {
  std::vector<DeviceDescriptor> devices;
  for (auto& info : ListAdapters()) devices.push_back(std::move(info.descriptor));
  return devices;
}

std::unique_ptr<D3D11Compositor> D3D11Compositor::Create(const Options& options, std::string* reason) {
  const auto fail = [&](const std::string& why) -> std::unique_ptr<D3D11Compositor> {
    if (reason != nullptr) *reason = why;
    return nullptr;
  };
  auto adapters = ListAdapters();
  if (adapters.empty()) return fail("Windows lists no graphics adapter");
  std::vector<DeviceDescriptor> descriptors;
  for (const auto& info : adapters) {
    auto descriptor = info.descriptor;
    if (descriptor.vendor == Vendor::Software && !options.allow_software) descriptor.available = false;
    descriptors.push_back(std::move(descriptor));
  }
  std::size_t chosen = adapters.size();
  if (!options.adapter_id.empty()) {
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
      if (descriptors[i].id == options.adapter_id && descriptors[i].available) chosen = i;
    }
    if (chosen == adapters.size()) return fail("there is no usable adapter " + options.adapter_id);
  } else {
    DeviceRequirements requirements;
    requirements.allow_cpu_fallback = false;
    requirements.preferred = Flag(Capability::ZeroCopyDecode) | Flag(Capability::Float16);
    const auto selection = SelectDevice(descriptors, requirements);
    if (!selection) return fail("no graphics adapter can run the compositor" + std::string(options.allow_software ? "" : " (software rendering was not allowed)"));
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
      if (descriptors[i].id == selection->device.id) chosen = i;
    }
  }

  std::unique_ptr<D3D11Compositor> compositor(new D3D11Compositor());
  auto& impl = *compositor->impl_;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(adapters[chosen].adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &impl.device, nullptr, &impl.context);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(adapters[chosen].adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &impl.device, nullptr, &impl.context);
    if (FAILED(hr)) return fail("the adapter " + descriptors[chosen].name + " would not create a Direct3D 11 device (" + HrText(hr) + ")");
    descriptors[chosen].capabilities &= ~Flag(Capability::ZeroCopyDecode);
  }
  // A decoder on its own thread draws into this device's textures: the device's calls must be safe from both threads.
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(impl.device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
  impl.descriptor = descriptors[chosen];
  std::string why;
  if (!impl.Initialise(&why)) return fail(why);
  return compositor;
}

const DeviceDescriptor& D3D11Compositor::device() const { return impl_->descriptor; }
void* D3D11Compositor::native_device() const { return impl_->device.Get(); }

void D3D11Compositor::ReleaseResources() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->ReleaseTargets();
  impl_->pool.clear();
}

// ------------------------------------------------------------------ composing ----

media::VideoFrame D3D11Compositor::Compose(const timeline::PlaybackPlan& plan, const CompositorConfig& config, const FrameResolver& resolve,
                                           Statistics& statistics, GpuStatistics* gpu, const DeviceFrameResolver& device_resolve) {
  const auto started = Clock::now();
  auto& s = *impl_;
  const std::lock_guard<std::mutex> lock(s.mutex);
  GpuStatistics local;
  GpuStatistics& stats = gpu != nullptr ? *gpu : local;
  stats = {};
  s.stats = &stats;
  s.asset_root = config.asset_root;
  struct ResetStats { Impl& impl; ~ResetStats() { impl.stats = nullptr; } } reset_stats{s};

  const int width = config.width > 0 ? config.width : static_cast<int>(plan.width);
  const int height = config.height > 0 ? config.height : static_cast<int>(plan.height);
  if (width <= 0 || height <= 0) throw std::invalid_argument("Compositor has no output size");
  const bool f32 = config.output_format == media::PixelFormat::RgbaF32;
  std::string why;
  if (!s.EnsureTargets(width, height, f32, !plan.transitions.empty(), &why)) throw std::runtime_error(why);
  ++s.frame_counter;

  auto& context = *s.context.Get();
  const bool timed = s.disjoint && s.begin_stamp && s.end_stamp;
  if (timed) {
    context.Begin(s.disjoint.Get());
    context.End(s.begin_stamp.Get());
  }
  // Background, as the software compositor fills its canvas.
  s.current_canvas = 0;
  s.ClearTarget(s.canvas[0], config.background_red, config.background_green, config.background_blue, 1.0f);

  // Under colour management a picture tagged with another space than the working one needs converting first.
  const bool managed = model::RenderSemantics::For(plan.render_version).color_managed;
  const auto working = managed ? color::ParseSpace(plan.working_color_space) : std::optional<color::Space>{};
  const auto check_tags = [&](const std::string& primaries, const std::string& transfer) {
    if (!working) return;
    const auto tagged = color::SpaceFromTags(primaries, transfer);
    if (tagged.has_value() && !(*tagged == *working)) throw Unsupported("a picture is in another colour space than the working one");
  };
  const auto draw_request = [&](const timeline::SourceRequest& request, bool over_canvas, Target* layer_target) -> bool {
    s.SetBaseParams();
    s.FillOps(request.effects);
    const auto* solid = FindEffect(request.effects, "solid");
    PooledTexture* picture = nullptr;
    ComPtr<ID3D11ShaderResourceView> luma, chroma;
    bool yuv = false;
    if (solid != nullptr) {
      const auto colour = FindParameter(*solid, "color");
      s.params.modes[0] = kSolid;
      s.params.solid[0] = colour ? static_cast<float>(colour->components[0]) : 0.0f;
      s.params.solid[1] = colour ? static_cast<float>(colour->components[1]) : 0.0f;
      s.params.solid[2] = colour ? static_cast<float>(colour->components[2]) : 0.0f;
      s.params.solid[3] = 1.0f;
    } else {
      DeviceFrame on_device;
      if (device_resolve) on_device = device_resolve(request);
      int source_width = 0, source_height = 0;
      if (on_device.valid() && working && on_device.bt2020) throw Unsupported("a wide-gamut picture on a Rec.709 timeline");
      if (on_device.valid() && s.MakePlaneViews(on_device, luma, chroma)) {
        yuv = true;
        source_width = on_device.width;
        source_height = on_device.height;
        const bool ten = on_device.format == DeviceFormat::P010;
        // The conversion constants for the picture's own range and matrix.
        const double kr = on_device.bt2020 ? 0.2627 : (on_device.bt709 ? 0.2126 : 0.299);
        const double kb = on_device.bt2020 ? 0.0593 : (on_device.bt709 ? 0.0722 : 0.114);
        const double kg = 1.0 - kr - kb;
        const double levels = ten ? 1023.0 : 255.0;
        const double shift = ten ? 64.0 : 1.0;
        s.params.yuv2[0] = ten ? static_cast<float>(65535.0 / (levels * shift)) : 1.0f;
        if (on_device.full_range) {
          s.params.yuv0[0] = 1.0f;
          s.params.yuv0[1] = 0.0f;
          s.params.yuv0[2] = 1.0f;
        } else {
          const double y_range = ten ? 876.0 : 219.0, c_range = ten ? 896.0 : 224.0, black = ten ? 64.0 : 16.0;
          s.params.yuv0[0] = static_cast<float>(levels / y_range);
          s.params.yuv0[1] = static_cast<float>(black / levels);
          s.params.yuv0[2] = static_cast<float>(levels / c_range);
        }
        s.params.yuv0[3] = static_cast<float>((ten ? 512.0 : 128.0) / levels);
        s.params.yuv1[0] = static_cast<float>(2.0 * (1.0 - kr));
        s.params.yuv1[1] = static_cast<float>(-2.0 * (1.0 - kb) * kb / kg);
        s.params.yuv1[2] = static_cast<float>(-2.0 * (1.0 - kr) * kr / kg);
        s.params.yuv1[3] = static_cast<float>(2.0 * (1.0 - kb));
        s.params.modes[2] = 0;  // the views already start at the picture slice
        ++stats.sources_on_device;
      } else {
        const auto* frame = resolve ? resolve(request) : nullptr;
        if (frame == nullptr || !frame->valid()) {
          ++statistics.missing_frames;
          return false;
        }
        check_tags(frame->color.primaries, frame->color.transfer);
        const auto upload_started = Clock::now();
        picture = s.Upload(*frame);
        stats.upload_ms += MillisecondsSince(upload_started);
        if (picture == nullptr) {
          ++statistics.missing_frames;
          return false;
        }
        source_width = frame->width();
        source_height = frame->height();
      }
      // DrawFrame: the source is fitted to the output, then the motion is applied about the anchor.
      const auto transform = TransformFor(request.effects);
      const auto crop = CropFor(request.effects);
      const auto fw = static_cast<float>(source_width), fh = static_cast<float>(source_height);
      const auto crop_left = std::clamp(crop.left, 0.0f, 1.0f) * fw;
      const auto crop_top = std::clamp(crop.top, 0.0f, 1.0f) * fh;
      const auto crop_right = fw - std::clamp(crop.right, 0.0f, 1.0f) * fw;
      const auto crop_bottom = fh - std::clamp(crop.bottom, 0.0f, 1.0f) * fh;
      const auto fit = std::min(static_cast<float>(width) / fw, static_cast<float>(height) / fh);
      const auto scale_x = fit * transform.scale_x, scale_y = fit * transform.scale_y;
      s.params.sizes[2] = source_width;
      s.params.sizes[3] = source_height;
      if (crop_right <= crop_left || crop_bottom <= crop_top || std::abs(scale_x) < 1e-9f || std::abs(scale_y) < 1e-9f) {
        s.params.modes[0] = kNothing;
      } else {
        s.params.modes[0] = kPicture;
        s.params.geo0[0] = static_cast<float>(width) * 0.5f + transform.translate_x;
        s.params.geo0[1] = static_cast<float>(height) * 0.5f + transform.translate_y;
        s.params.geo0[2] = transform.anchor_x * fw;
        s.params.geo0[3] = transform.anchor_y * fh;
        s.params.geo1[0] = scale_x;
        s.params.geo1[1] = scale_y;
        s.params.geo1[2] = std::cos(-transform.rotation_radians);
        s.params.geo1[3] = std::sin(-transform.rotation_radians);
        s.params.crop[0] = crop_left;
        s.params.crop[1] = crop_top;
        s.params.crop[2] = crop_right;
        s.params.crop[3] = crop_bottom;
      }
    }
    const int in = s.current_canvas, out = 1 - s.current_canvas;
    ID3D11ShaderResourceView* source0 = yuv ? luma.Get() : (picture != nullptr ? picture->view.Get() : nullptr);
    ID3D11ShaderResourceView* source1 = yuv ? chroma.Get() : nullptr;
    if (over_canvas) {
      s.Bind(source0, source1, s.canvas[in].srv.Get(), nullptr, nullptr, s.canvas[out].uav.Get());
      s.Dispatch(yuv ? s.draw_over_yuv.Get() : s.draw_over.Get());
      s.current_canvas = out;
    } else {
      s.Bind(source0, source1, nullptr, nullptr, nullptr, layer_target->uav.Get());
      s.Dispatch(yuv ? s.draw_layer_yuv.Get() : s.draw_layer.Get());
    }
    s.Unbind();
    return true;
  };

  const auto ops_on_canvas = [&](const std::vector<SampledEffect>& effects) {
    s.SetBaseParams();
    s.FillOps(effects);
    if (s.params.modes[1] == 0) return;
    const int in = s.current_canvas, out = 1 - s.current_canvas;
    s.Bind(nullptr, nullptr, s.canvas[in].srv.Get(), nullptr, nullptr, s.canvas[out].uav.Get());
    s.Dispatch(s.ops_on_canvas.Get());
    s.Unbind();
    s.current_canvas = out;
  };

  // The walk is the software compositor's: requests of this sequence level grouped by track, bottom to top.
  std::unordered_map<std::string, const timeline::TransitionMix*> transitions;
  for (const auto& transition : plan.transitions) transitions.emplace(transition.track_id, &transition);
  std::map<std::pair<std::int64_t, std::string>, std::vector<const timeline::SourceRequest*>> by_track;
  for (const auto& request : plan.video) {
    if (request.depth == 0) by_track[{request.track_order, request.track_id}].push_back(&request);
  }
  for (const auto& [key, track_requests] : by_track) {
    const auto found = transitions.find(key.second);
    if (found != transitions.end()) {
      const auto* transition = found->second;
      const auto side = [&](const std::optional<std::string>& clip_id) -> const timeline::SourceRequest* {
        if (!clip_id.has_value()) return nullptr;
        for (const auto* request : track_requests) {
          if (request->clip_id == *clip_id) return request;
        }
        return nullptr;
      };
      const auto* from = side(transition->from_clip_id);
      const auto* to = side(transition->to_clip_id);
      // A missing side stays transparent: a one-sided transition reads as a fade of the other side over what is beneath.
      s.ClearTarget(s.layer_a, 0, 0, 0, 0);
      s.ClearTarget(s.layer_b, 0, 0, 0, 0);
      if (from != nullptr && from->source_kind != model::SourceKind::Adjustment) (void)draw_request(*from, false, &s.layer_a);
      if (to != nullptr && to->source_kind != model::SourceKind::Adjustment) (void)draw_request(*to, false, &s.layer_b);
      s.SetBaseParams();
      s.params.weight[0] = std::clamp(static_cast<float>(transition->progress), 0.0f, 1.0f);
      const int in = s.current_canvas, out = 1 - s.current_canvas;
      s.Bind(nullptr, nullptr, s.canvas[in].srv.Get(), s.layer_a.srv.Get(), s.layer_b.srv.Get(), s.canvas[out].uav.Get());
      s.Dispatch(s.mix_over.Get());
      s.Unbind();
      s.current_canvas = out;
      ++statistics.transitions_mixed;
      ++statistics.layers_composited;
      continue;
    }
    for (const auto* request : track_requests) {
      if (request->source_kind == model::SourceKind::Adjustment) {
        ops_on_canvas(request->effects);
        ++statistics.adjustment_layers;
        continue;
      }
      (void)draw_request(*request, true, nullptr);
      ++statistics.layers_composited;
    }
  }
  ops_on_canvas(plan.sequence_effects);

  // The finished picture: un-premultiplied, in the format asked for, then copied where the CPU can read it.
  s.SetBaseParams();
  {
    const int in = s.current_canvas;
    if (f32) {
      s.Bind(nullptr, nullptr, s.canvas[in].srv.Get(), nullptr, nullptr, s.out_f32.uav.Get());
      s.Dispatch(s.out32.Get());
    } else {
      s.Bind(nullptr, nullptr, s.canvas[in].srv.Get(), nullptr, nullptr, nullptr, s.out_u8.uav.Get());
      s.Dispatch(s.out8.Get());
    }
    s.Unbind();
  }
  auto& staging = f32 ? s.staging_f32 : s.staging_u8;
  context.CopyResource(staging.Get(), f32 ? s.out_f32.texture.Get() : s.out_u8.texture.Get());
  if (timed) {
    context.End(s.end_stamp.Get());
    context.End(s.disjoint.Get());
  }

  const auto readback_started = Clock::now();
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("could not read the finished picture back from the device");
  auto output = media::VideoFrame::Allocate(f32 ? media::PixelFormat::RgbaF32 : media::PixelFormat::Rgba8, width, height);
  const std::size_t row_bytes = static_cast<std::size_t>(width) * (f32 ? 16u : 4u);
  for (int y = 0; y < height; ++y) std::memcpy(output.row(y), static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, row_bytes);
  context.Unmap(staging.Get(), 0);
  stats.readback_ms = MillisecondsSince(readback_started);
  output.presentation_time = plan.sequence_time;
  output.color.range = model::ColorRange::Full;
  if (managed && working) {
    const auto tags = color::TagsOf(*working);
    output.color.primaries = tags.primaries;
    output.color.transfer = tags.transfer;
  }

  if (timed) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 begin = 0, end = 0;
    if (context.GetData(s.disjoint.Get(), &disjoint, sizeof(disjoint), 0) == S_OK && !disjoint.Disjoint &&
        context.GetData(s.begin_stamp.Get(), &begin, sizeof(begin), 0) == S_OK && context.GetData(s.end_stamp.Get(), &end, sizeof(end), 0) == S_OK) {
      stats.gpu_ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
    }
  }
  stats.texture_bytes = s.texture_bytes;
  stats.total_ms = MillisecondsSince(started);
  return output;
}

}  // namespace cutline::render::gpu

#else  // not Windows: there is no Direct3D 11; the software compositor does everything

namespace cutline::render::gpu {

struct D3D11Compositor::Impl {};
D3D11Compositor::D3D11Compositor() = default;
D3D11Compositor::~D3D11Compositor() = default;
std::vector<DeviceDescriptor> D3D11Compositor::EnumerateDevices() { return {}; }
std::unique_ptr<D3D11Compositor> D3D11Compositor::Create(const Options&, std::string* reason) {
  if (reason != nullptr) *reason = "Direct3D 11 is only available on Windows";
  return nullptr;
}
const DeviceDescriptor& D3D11Compositor::device() const { static const DeviceDescriptor none = CpuFallbackDevice(); return none; }
void* D3D11Compositor::native_device() const { return nullptr; }
bool D3D11Compositor::Supports(const timeline::PlaybackPlan&, const CompositorConfig&, std::string* reason) const {
  if (reason != nullptr) *reason = "no Direct3D 11";
  return false;
}
media::VideoFrame D3D11Compositor::Compose(const timeline::PlaybackPlan&, const CompositorConfig&, const FrameResolver&, Statistics&, GpuStatistics*, const DeviceFrameResolver&) {
  throw std::logic_error("no Direct3D 11");
}
void D3D11Compositor::ReleaseResources() {}

}  // namespace cutline::render::gpu

#endif
