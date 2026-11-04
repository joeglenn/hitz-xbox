/*
 * nv2a_d3d11.c -- GPU back end for the NV2A executor (Nv2aBackend.draw_x).
 *
 * HITZ_GPU=1 registers it. The executor still decodes the pushbuffer and runs
 * vertex programs on the CPU; this draws the resulting triangles on a real
 * D3D11 device, with the register combiners as an uber pixel shader (a direct
 * port of nv2a_rc_eval), so the image matches the CPU rasteriser's.
 *
 * Surfaces live on the GPU, keyed by guest address. A texture whose address is
 * a render target samples that target directly (render-to-texture). At every
 * flip each target drawn since the last flip is read back into guest memory,
 * so the toolkit's present path, screenshots and FB dumps keep working.
 *
 * HITZ_GPU_ADAPTER=<substring> picks the adapter (default: the hardware
 * adapter with the most dedicated video memory, i.e. the discrete GPU on a
 * laptop with two). HITZ_GPU_CULL=1 enables hardware culling (the CPU path does
 * not cull). Stats every 10 s as [D3D11].
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "nv2a_backend.h"
#include "d3d/d3d8_swizzle.h"
#include "platform/perf_log.h"
static void psc_init(void);           /* the shader cache, defined below */

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern ptrdiff_t xbox_GetMemoryOffset(void);
#define GUEST(va) ((uint8_t *)(xbox_GetMemoryOffset() + (ptrdiff_t)(va)))
#define KEY(va)   ((va) & 0x0FFFFFFFu)   /* 0x80xxxxxx and 0x00xxxxxx are the same RAM */

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;
static ID3D11VertexShader  *s_vs, *s_vs_prog;
static ID3D11PixelShader   *s_ps, *s_ps_flat;
static ID3D11InputLayout   *s_il;
static ID3D11Buffer        *s_vb, *s_cb;
static UINT                 s_vb_size, s_vb_pos;
static ID3D11RasterizerState *s_rs[4];      /* [cull] screen-space; [2+cull] programs (depth clip on) */
/* GPU vertex programs: raw attribute ring, index ring, constants, program. */
static ID3D11Buffer *s_raw, *s_ib, *s_vcb, *s_vpb;
static ID3D11ShaderResourceView *s_raw_srv;
static UINT s_raw_size, s_raw_pos, s_ib_size, s_ib_pos;
static uint32_t s_prog_hash;
typedef struct {
    float c[192][4];
    uint32_t attr[16][4];
    float aval[16][4];
    uint32_t vm[4];
    float vmf[4];
} VCData;
static int s_failed, s_cull;
static uint32_t s_frame = 1;
volatile int g_hitz_heavy;

/* ------------------------------------------------------------ statistics */
static struct { unsigned long long draws, vsh_draws, tris, tex_up, tex_up_bytes, tex_bc, tex_fast, tex_ms_cpu, rt_reads, rt_alias, flips;
                unsigned long long t_draw_ns, t_flip_ns, t_map_ns, gpu_ns, gpu_frames, rt_skipped, rb_late; } s_st;
static unsigned long long s_frame_draw_ns;   /* draw building since the last report */
/* The renderer's timers (draw/flip/texture ms, GPU timestamp queries) only
 * feed the log and the performance log: off without debug logging. */
static int s_tm = 1;
#define TNOW() (s_tm ? now_ns() : 0ull)
static unsigned long long now_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

/* ------------------------------------------------------------ shaders */
static const char s_hlsl[] =
"cbuffer CB : register(b0) {\n"
"  uint4 ICW[2]; uint4 AICW[2]; uint4 OCW[2]; uint4 AOCW[2]; uint4 F0[2]; uint4 F1[2];\n"
"  uint4 FIN;    /* final0 final1 final_c0 final_c1 */\n"
"  uint4 CTL;    /* control stage_program use_rc textured */\n"
"  uint4 MISC;   /* fog_color alpha_test alpha_func alpha_ref */\n"
"  uint4 MISC2;  /* clip_plane_mode */\n"
"  float4 RT;    /* width height 1/zmax 0 */\n"
"  float4 TS[4]; /* width height normalized cube */\n"
"};\n"
"Texture2D T2[4] : register(t0);\n"
"TextureCube TC[4] : register(t4);\n"
"SamplerState S[4] : register(s0);\n"
"struct VI { float4 p:POSITION; float4 d0:COLOR0; float4 d1:COLOR1; float fog:FOG;\n"
"            float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"struct VO { float4 p:SV_Position; noperspective float4 d0:COLOR0; noperspective float4 d1:COLOR1;\n"
"            noperspective float fog:FOG; float4 t0:TEXCOORD0; float4 t1:TEXCOORD1;\n"
"            float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"VO vs_main(VI i) {\n"
"  VO o; float w = i.p.w > 0 ? i.p.w : 1.0;\n"
"  float x = i.p.x / RT.x * 2.0 - 1.0, y = 1.0 - i.p.y / RT.y * 2.0, z = saturate(i.p.z * RT.z);\n"
"  o.p = float4(x * w, y * w, z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
"uint g8(uint4 a[2], uint i) { return a[i >> 2][i & 3]; }\n"
"float4 unpack(uint c) { return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0);\n"
"  uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p; if (m == 1) return 1.0 - saturate(x); if (m == 2) return 2.0 * p - 1.0;\n"
"  if (m == 3) return -2.0 * p + 1.0; if (m == 4) return p - 0.5; if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x; return -x;\n"
"}\n"
"float3 map_out(float3 x, uint m) {\n"
"  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0; if (m == 0x18) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20) return x * 4.0; if (m == 0x30) return x * 0.5; return x;\n"
"}\n"
"float4 tex2(uint st, float2 uv) {\n"
"  if (st == 0) return T2[0].SampleLevel(S[0], uv, 0); if (st == 1) return T2[1].SampleLevel(S[1], uv, 0);\n"
"  if (st == 2) return T2[2].SampleLevel(S[2], uv, 0); return T2[3].SampleLevel(S[3], uv, 0);\n"
"}\n"
"float4 texc(uint st, float3 d) {\n"
"  if (st == 0) return TC[0].SampleLevel(S[0], d, 0); if (st == 1) return TC[1].SampleLevel(S[1], d, 0);\n"
"  if (st == 2) return TC[2].SampleLevel(S[2], d, 0); return TC[3].SampleLevel(S[3], d, 0);\n"
"}\n"
"float4 sample_stage(uint st, float4 c, inout bool keep) {\n"
"  uint mode = (CTL.y >> (st * 5)) & 31;\n"
"  float4 sz = TS[st];\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2) {\n"
"    float q = c.w != 0 ? c.w : 1.0; float2 uv = c.xy / q;\n"
"    if (sz.z == 0) uv /= sz.xy;\n"
"    return tex2(st, uv);\n"
"  }\n"
"  if (mode == 3) {\n"
"    if (sz.w != 0) return texc(st, c.xyz);\n"
"    float2 uv = c.xy; if (sz.z == 0) uv /= sz.xy;\n"
"    return tex2(st, uv);\n"
"  }\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    for (uint j = 0; j < 4; j++) { bool ge = ((MISC2.x >> (st * 4 + j)) & 1) != 0;\n"
"      if (ge ? c[j] >= 0 : c[j] < 0) keep = false; }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"float3 rd_rgb(float4 R[16], uint b) { float4 r = R[b & 15]; float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b)); }\n"
"float rd_a(float4 R[16], uint b) { float4 r = R[b & 15]; return map_in((b & 0x10) ? r.a : r.b, b); }\n"
"float4 ps_main(VO i) : SV_Target {\n"
"  float4 col = i.d0, outc;\n"
"  if (CTL.z != 0) {\n"
"    bool keep = true; float4 t[4];\n"
"    float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"    [unroll] for (uint st = 0; st < 4; st++) t[st] = sample_stage(st, tc[st], keep);\n"
"    if (!keep) discard;\n"
"    float4 R[16];\n"
"    [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"    float4 fogc = unpack(MISC.x); /* R in bits 0-7: swap */\n"
"    R[3] = float4(fogc.b, fogc.g, fogc.r, i.fog);\n"
"    R[4] = col; R[5] = i.d1; R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
"    R[12].a = (CTL.y & 31) ? t[0].a : 1.0;\n"
"    uint n = min(CTL.x & 255, 8u), flags = CTL.x >> 8;\n"
"    for (uint s = 0; s < n; s++) {\n"
"      uint icw = g8(ICW, s), ocw = g8(OCW, s), aicw = g8(AICW, s), aocw = g8(AOCW, s);\n"
"      uint fl = ocw >> 12, afl = aocw >> 12;\n"
"      R[1] = unpack(g8(F0, (flags & 0x010) ? s : 0)); R[2] = unpack(g8(F1, (flags & 0x100) ? s : 0));\n"
"      bool mux = (flags & 1) ? R[12].a >= 0.5 : (((uint)(R[12].a * 255.0)) & 1) != 0;\n"
"      float3 A = rd_rgb(R, icw >> 24), B = rd_rgb(R, icw >> 16), C = rd_rgb(R, icw >> 8), D = rd_rgb(R, icw);\n"
"      float aA = rd_a(R, aicw >> 24), aB = rd_a(R, aicw >> 16), aC = rd_a(R, aicw >> 8), aD = rd_a(R, aicw);\n"
"      float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"      float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"      float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"      float3 ab = clamp(map_out(pab, fl & 0x38), -1, 1), cd = clamp(map_out(pcd, fl & 0x38), -1, 1);\n"
"      float3 ms = clamp(map_out(sum, fl & 0x38), -1, 1);\n"
"      float apab = aA * aB, apcd = aC * aD, asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"      float aab = clamp(map_out(apab.xxx, afl & 0x38).x, -1, 1), acd = clamp(map_out(apcd.xxx, afl & 0x38).x, -1, 1);\n"
"      float ams = clamp(map_out(asum.xxx, afl & 0x38).x, -1, 1);\n"
"      uint r;\n"
"      r = (ocw >> 4) & 15; if (r) { R[r].rgb = ab; if (fl & 0x80) R[r].a = ab.b; }\n"
"      r = ocw & 15;        if (r) { R[r].rgb = cd; if (fl & 0x40) R[r].a = cd.b; }\n"
"      r = (ocw >> 8) & 15; if (r) R[r].rgb = ms;\n"
"      r = (aocw >> 4) & 15; if (r) R[r].a = aab;\n"
"      r = aocw & 15;        if (r) R[r].a = acd;\n"
"      r = (aocw >> 8) & 15; if (r) R[r].a = ams;\n"
"      R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    if (FIN.x != 0 || FIN.y != 0) {\n"
"      uint f0 = FIN.x, f1 = FIN.y, ff = f1 & 255;\n"
"      R[1] = unpack(FIN.z); R[2] = unpack(FIN.w);\n"
"      float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb, b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"      R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0);\n"
"      float3 E = rd_rgb(R, f1 >> 24), F = rd_rgb(R, f1 >> 16);\n"
"      R[15] = float4(E * F, 0);\n"
"      float3 A = rd_rgb(R, f0 >> 24), B = rd_rgb(R, f0 >> 16), C = rd_rgb(R, f0 >> 8), D = rd_rgb(R, f0);\n"
"      outc = float4(D + C * (1.0 - A) + B * A, rd_a(R, f1 >> 8));\n"
"    } else outc = R[12];\n"
"    outc = saturate(outc);\n"
"  } else {\n"
"    outc = col;\n"
"    if (CTL.w != 0) { bool keep = true; outc = col * sample_stage(0, float4(i.t0.xy, 0, 1.0), keep); }\n"
"  }\n"
"  if (MISC.y != 0) {\n"
"    int v = (int)(outc.a * 255.0 + 0.5), rf = (int)(MISC.w & 255); bool ok;\n"
"    uint f = MISC.z; ok = true;\n"
"    if (f == 0x200) ok = false; else if (f == 0x201) ok = v < rf; else if (f == 0x202) ok = v == rf;\n"
"    else if (f == 0x203) ok = v <= rf; else if (f == 0x204) ok = v > rf; else if (f == 0x205) ok = v != rf;\n"
"    else if (f == 0x206) ok = v >= rf;\n"
"    if (!ok) discard;\n"
"  }\n"
"  return outc;\n"
"}\n"
"float4 ps_flat(VO i) : SV_Target { return i.d0; }\n"
"/* ---- vertex programs on the GPU (port of nv2a_vsh_interp.c) ---- */\n"
"cbuffer VC : register(b1) { float4 VCN[192]; uint4 ATTR[16]; float4 AVAL[16]; uint4 VM; float4 VMF; };\n"
"cbuffer VP : register(b2) { uint4 PROG[136]; };\n"
"ByteAddressBuffer VB : register(t8);\n"
"static float4 VIN[16];\n"
"static float4 VT[12];\n"
"static float4 VOP;\n"
"static float4 VOUT[13];\n"
"static int VA0;\n"
"uint ld32(uint a) { uint s = a & 3; uint lo = VB.Load(a - s); if (s == 0) return lo;\n"
"  uint hi = VB.Load(a - s + 4); return (lo >> (s * 8)) | (hi << (32 - s * 8)); }\n"
"float4 vfetch(uint a, uint vid) {\n"
"  uint4 d = ATTR[a];\n"
"  if (d.w == 0) return AVAL[a];\n"
"  uint p = d.x + vid * d.y;\n"
"  float4 o = float4(0, 0, 0, 1);\n"
"  if (d.z == 0) { uint v = ld32(p); return float4((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24) / 255.0; }\n"
"  if (d.z == 6) { uint v = ld32(p); o.x = (float)((int)(v << 21) >> 21) / 1023.0;\n"
"    o.y = (float)((int)(v << 10) >> 21) / 1023.0; o.z = (float)((int)v >> 22) / 511.0; return o; }\n"
"  uint n = min(d.w, 4u);\n"
"  [unroll] for (uint i = 0; i < 4; i++) if (i < n) {\n"
"    if (d.z == 2) o[i] = asfloat(ld32(p + 4 * i));\n"
"    else if (d.z == 4) o[i] = (ld32(p + i) & 255) / 255.0;\n"
"    else if (d.z == 1) o[i] = (float)((int)(ld32(p + 2 * i) << 16) >> 16) / 32767.0;\n"
"    else if (d.z == 5) o[i] = (float)((int)(ld32(p + 2 * i) << 16) >> 16);\n"
"  }\n"
"  return o;\n"
"}\n"
"uint fld(uint4 ins, uint w, uint lo, uint n) { return (ins[w] >> lo) & ((1u << n) - 1u); }\n"
"float4 vsrc(uint4 ins, uint which) {\n"
"  uint neg, sw, mux, reg;\n"
"  if (which == 0) { neg = fld(ins, 1, 8, 1); sw = fld(ins, 1, 0, 8); mux = fld(ins, 2, 26, 2); reg = fld(ins, 2, 28, 4); }\n"
"  else if (which == 1) { neg = fld(ins, 2, 25, 1); sw = fld(ins, 2, 17, 8); mux = fld(ins, 2, 11, 2); reg = fld(ins, 2, 13, 4); }\n"
"  else { neg = fld(ins, 2, 10, 1); sw = fld(ins, 2, 2, 8); mux = fld(ins, 3, 28, 2);\n"
"         reg = (fld(ins, 2, 0, 2) << 2) | fld(ins, 3, 30, 2); }\n"
"  float4 v;\n"
"  if (mux == 1) v = (reg == 12) ? VOP : VT[reg < 12 ? reg : 0];\n"
"  else if (mux == 2) v = VIN[fld(ins, 1, 9, 4)];\n"
"  else { int ci = (int)fld(ins, 1, 13, 8); if (fld(ins, 3, 1, 1)) ci += VA0;\n"
"         if (ci < 0 || ci >= 192) ci = 0; v = VCN[ci]; }\n"
"  float4 r = float4(v[(sw >> 6) & 3], v[(sw >> 4) & 3], v[(sw >> 2) & 3], v[sw & 3]);\n"
"  return neg ? -r : r;\n"
"}\n"
"float4 wm(float4 d, float4 v, uint m) { if (m & 8) d.x = v.x; if (m & 4) d.y = v.y; if (m & 2) d.z = v.z; if (m & 1) d.w = v.w; return d; }\n"
"float4 run_mac(uint op, float4 a, float4 b, float4 c) {\n"
"  if (op == 1) return a;\n"
"  if (op == 2) return a * b;\n"
"  if (op == 3) return a + c;\n"
"  if (op == 4) return a * b + c;\n"
"  if (op == 5) return dot(a.xyz, b.xyz).xxxx;\n"
"  if (op == 6) return (dot(a.xyz, b.xyz) + b.w).xxxx;\n"
"  if (op == 7) return dot(a, b).xxxx;\n"
"  if (op == 8) return float4(1.0, a.y * b.y, a.z, b.w);\n"
"  if (op == 9) return float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w);\n"
"  if (op == 10) return float4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w);\n"
"  if (op == 11) return float4(a < b);\n"
"  if (op == 12) return float4(a >= b);\n"
"  if (op == 13) { VA0 = (int)floor(a.x + 0.001); return a; }\n"
"  return a;\n"
"}\n"
"float4 run_ilu(uint op, float4 c) {\n"
"  float x = c.x;\n"
"  if (op == 1) return c;\n"
"  if (op == 2) return (x == 0 ? asfloat(0x7f800000) : 1.0 / x).xxxx;\n"
"  if (op == 3) { float f = 1.0 / x;\n"
"    if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
"    if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19; return f.xxxx; }\n"
"  if (op == 4) return (1.0 / sqrt(abs(x))).xxxx;\n"
"  if (op == 5) { float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
"  if (op == 6) { float ax = abs(x);\n"
"    if (ax == 0) return float4(-asfloat(0x7f800000), 1.0, -asfloat(0x7f800000), 1.0);\n"
"    float e = floor(log2(ax)); return float4(e, ax / exp2(e), log2(ax), 1.0); }\n"
"  if (op == 7) { float nl = max(c.x, 0.0), nh = max(c.y, 0.0), pw = clamp(c.w, -127.9961, 127.9961);\n"
"    return float4(1.0, nl, c.x > 0 ? pow(nh, pw) : 0.0, 1.0); }\n"
"  return c;\n"
"}\n"
"float fogf(float d) {\n"
"  if (VM.y == 0) return 1.0;\n"
"  float px = VMF.x, py = VMF.y, f; uint m = VM.z;\n"
"  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return saturate(f);\n"
"}\n"
"VO vs_prog(uint vid : SV_VertexID) {\n"
"  uint k;\n"
"  [loop] for (k = 0; k < 16; k++) VIN[k] = vfetch(k, vid);\n"
"  [unroll] for (k = 0; k < 12; k++) VT[k] = float4(0, 0, 0, 0);\n"
"  [unroll] for (k = 0; k < 13; k++) VOUT[k] = float4(0, 0, 0, 0);\n"
"  VOP = float4(0, 0, 0, 0); VA0 = 0;\n"
"  VOUT[3].w = 1; VOUT[4].w = 1; VOUT[9].w = 1; VOUT[10].w = 1; VOUT[11].w = 1; VOUT[12].w = 1;\n"
"  [loop] for (uint s = 0; s < VM.x; s++) {\n"
"    uint4 ins = PROG[s];\n"
"    uint mac = fld(ins, 1, 21, 4), ilu = fld(ins, 1, 25, 3);\n"
"    uint mm = fld(ins, 3, 24, 4), im = fld(ins, 3, 16, 4), td = fld(ins, 3, 20, 4);\n"
"    uint om = fld(ins, 3, 12, 4), oa = fld(ins, 3, 3, 8);\n"
"    float4 A = vsrc(ins, 0), B = vsrc(ins, 1), Cc = vsrc(ins, 2);\n"
"    float4 mr = float4(0, 0, 0, 0), ir = float4(0, 0, 0, 0);\n"
"    if (mac) mr = run_mac(mac, A, B, Cc);\n"
"    if (ilu) ir = run_ilu(ilu, Cc);\n"
"    if (mac && mac != 13 && mm) { if (td == 12) VOP = wm(VOP, mr, mm); else if (td < 12) VT[td] = wm(VT[td], mr, mm); }\n"
"    if (ilu && im) { uint it = mac ? 1u : td; if (it == 12) VOP = wm(VOP, ir, im); else if (it < 12) VT[it] = wm(VT[it], ir, im); }\n"
"    if (om && fld(ins, 3, 11, 1)) { float4 sv = fld(ins, 3, 2, 1) ? ir : mr;\n"
"      if (oa == 0) VOP = wm(VOP, sv, om); else if (oa < 13) VOUT[oa] = wm(VOUT[oa], sv, om); }\n"
"    if (fld(ins, 3, 0, 1)) break;\n"
"  }\n"
"  float4 pos = VOP;\n"
"  if (pos.w == 0 && asuint(pos.w) == 0) pos.w = 1.0;\n"
"  pos.x *= VMF.z; pos.y *= VMF.w;\n"
"  VO o;\n"
"  float x = pos.x / RT.x * 2.0 - 1.0, y = 1.0 - pos.y / RT.y * 2.0, z = pos.z * RT.z;\n"
"  o.p = float4(x * pos.w, y * pos.w, z * pos.w, pos.w);\n"
"  o.d0 = saturate(VOUT[3]); o.d1 = saturate(VOUT[4]); o.fog = fogf(VOUT[5].x);\n"
"  o.t0 = VOUT[9]; o.t1 = VOUT[10]; o.t2 = VOUT[11]; o.t3 = VOUT[12];\n"
"  return o;\n"
"}\n"
;

static const char s_ps_spec_src[] =
"/* ---- specialised pixel shader: K_* are literals prepended per combiner state ---- */\n"
"float4 sample_k(uint st, float4 c, inout bool keep) {\n"
"  uint mode = (K_SP >> (st * 5)) & 31;\n"
"  float4 sz = TS[st];\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2) {\n"
"    float q = c.w != 0 ? c.w : 1.0; float2 uv = c.xy / q;\n"
"    if (K_NORM[st] == 0) uv /= sz.xy;\n"
"    return tex2(st, uv);\n"
"  }\n"
"  if (mode == 3) {\n"
"    if (K_CUBE[st] != 0) return texc(st, c.xyz);\n"
"    float2 uv = c.xy; if (K_NORM[st] == 0) uv /= sz.xy;\n"
"    return tex2(st, uv);\n"
"  }\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) { bool ge = ((MISC2.x >> (st * 4 + j)) & 1) != 0;\n"
"      if (ge ? c[j] >= 0 : c[j] < 0) keep = false; }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"float4 ps_spec(VO i) : SV_Target {\n"
"  float4 col = i.d0, outc;\n"
"  if (K_USE_RC) {\n"
"    bool keep = true; float4 t[4];\n"
"    float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"    [unroll] for (uint st = 0; st < 4; st++) t[st] = sample_k(st, tc[st], keep);\n"
"    if (!keep) discard;\n"
"    float4 R[16];\n"
"    [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"    float4 fogc = unpack(MISC.x);\n"
"    R[3] = float4(fogc.b, fogc.g, fogc.r, i.fog);\n"
"    R[4] = col; R[5] = i.d1; R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
"    R[12].a = (K_SP & 31) ? t[0].a : 1.0;\n"
"    [unroll] for (uint s = 0; s < K_N; s++) {\n"
"      uint icw = K_ICW[s], ocw = K_OCW[s], aicw = K_AICW[s], aocw = K_AOCW[s];\n"
"      uint fl = ocw >> 12, afl = aocw >> 12;\n"
"      R[1] = unpack(g8(F0, (K_FLAGS & 0x010) ? s : 0)); R[2] = unpack(g8(F1, (K_FLAGS & 0x100) ? s : 0));\n"
"      bool mux = (K_FLAGS & 1) ? R[12].a >= 0.5 : (((uint)(R[12].a * 255.0)) & 1) != 0;\n"
"      float3 A = rd_rgb(R, icw >> 24), B = rd_rgb(R, icw >> 16), C = rd_rgb(R, icw >> 8), D = rd_rgb(R, icw);\n"
"      float aA = rd_a(R, aicw >> 24), aB = rd_a(R, aicw >> 16), aC = rd_a(R, aicw >> 8), aD = rd_a(R, aicw);\n"
"      float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"      float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"      float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"      float3 ab = clamp(map_out(pab, fl & 0x38), -1, 1), cd = clamp(map_out(pcd, fl & 0x38), -1, 1);\n"
"      float3 ms = clamp(map_out(sum, fl & 0x38), -1, 1);\n"
"      float apab = aA * aB, apcd = aC * aD, asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"      float aab = clamp(map_out(apab.xxx, afl & 0x38).x, -1, 1), acd = clamp(map_out(apcd.xxx, afl & 0x38).x, -1, 1);\n"
"      float ams = clamp(map_out(asum.xxx, afl & 0x38).x, -1, 1);\n"
"      uint r;\n"
"      r = (ocw >> 4) & 15; if (r) { R[r].rgb = ab; if (fl & 0x80) R[r].a = ab.b; }\n"
"      r = ocw & 15;        if (r) { R[r].rgb = cd; if (fl & 0x40) R[r].a = cd.b; }\n"
"      r = (ocw >> 8) & 15; if (r) R[r].rgb = ms;\n"
"      r = (aocw >> 4) & 15; if (r) R[r].a = aab;\n"
"      r = aocw & 15;        if (r) R[r].a = acd;\n"
"      r = (aocw >> 8) & 15; if (r) R[r].a = ams;\n"
"      R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    if (K_FIN0 != 0 || K_FIN1 != 0) {\n"
"      uint ff = K_FIN1 & 255;\n"
"      R[1] = unpack(FIN.z); R[2] = unpack(FIN.w);\n"
"      float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb, b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"      R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0);\n"
"      float3 E = rd_rgb(R, K_FIN1 >> 24), F = rd_rgb(R, K_FIN1 >> 16);\n"
"      R[15] = float4(E * F, 0);\n"
"      float3 A = rd_rgb(R, K_FIN0 >> 24), B = rd_rgb(R, K_FIN0 >> 16), C = rd_rgb(R, K_FIN0 >> 8), D = rd_rgb(R, K_FIN0);\n"
"      outc = float4(D + C * (1.0 - A) + B * A, rd_a(R, K_FIN1 >> 8));\n"
"    } else outc = R[12];\n"
"    outc = saturate(outc);\n"
"  } else {\n"
"    outc = col;\n"
"    if (K_TEX) { bool keep = true; outc = col * sample_k(0, float4(i.t0.xy, 0, 1.0), keep); }\n"
"  }\n"
"  if (K_AT) {\n"
"    int v = (int)(outc.a * 255.0 + 0.5), rf = (int)(MISC.w & 255); bool ok = true;\n"
"    if (K_AF == 0x200) ok = false; else if (K_AF == 0x201) ok = v < rf; else if (K_AF == 0x202) ok = v == rf;\n"
"    else if (K_AF == 0x203) ok = v <= rf; else if (K_AF == 0x204) ok = v > rf; else if (K_AF == 0x205) ok = v != rf;\n"
"    else if (K_AF == 0x206) ok = v >= rf;\n"
"    if (!ok) discard;\n"
"  }\n"
"  return outc;\n"
"}\n"
;

typedef struct {
    uint32_t icw[8], aicw[8], ocw[8], aocw[8], f0[8], f1[8];
    uint32_t fin[4], ctl[4], misc[4], misc2[4];
    float rt[4];
    float ts[4][4];
} CBData;

/* ------------------------------------------------------------ render targets */
typedef struct {
    uint32_t key, va, w, h, pitch, bpp, format, swizzled;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    int dirty, pending;           /* pending: copied to staging, not yet written to guest */
    uint32_t sampled_frame;       /* last frame it was read as a texture */
    uint32_t last_frame;
} RT;
#define MAX_RT 24
static RT s_rt[MAX_RT];

typedef struct {
    uint32_t key, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} DS;
#define MAX_DS 8
static DS s_ds[MAX_DS];

static void qflush(void);
static int s_q_ok;
static void rt_free(RT *r)
{
    if (s_q_ok > 0) qflush();
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    memset(r, 0, sizeof *r);
}

static uint32_t swz_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* Morton order as the NV2A swizzles: bits of x and y interleaved, x first,
     * until one dimension runs out. */
    uint32_t off = 0, bit = 0, mx = w > 1 ? w - 1 : 0, my = h > 1 ? h - 1 : 0, k;
    for (k = 0; (mx >> k) || (my >> k); k++) {
        if (mx >> k) { off |= ((x >> k) & 1u) << bit; bit++; }
        if (my >> k) { off |= ((y >> k) & 1u) << bit; bit++; }
    }
    return off;
}

static uint32_t px565_to_argb(uint16_t p)
{
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

/* Guest memory -> new render target, so a target starts with what the title
 * (or the CPU path) left there. */
static void rt_upload_guest(RT *r)
{
    uint32_t *buf = (uint32_t *)malloc((size_t)r->w * r->h * 4), x, y;
    const uint8_t *m = GUEST(r->va);
    if (!buf) return;
    for (y = 0; y < r->h; y++)
        for (x = 0; x < r->w; x++) {
            size_t o = r->swizzled ? (size_t)swz_offset(x, y, r->w, r->h) * r->bpp
                                   : (size_t)y * r->pitch + (size_t)x * r->bpp;
            buf[(size_t)y * r->w + x] = r->bpp == 4 ? *(const uint32_t *)(m + o)
                                                    : px565_to_argb(*(const uint16_t *)(m + o));
        }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL, buf, r->w * 4, 0);
    free(buf);
}

static RT *rt_get(const Nv2aSurface *s)
{
    uint32_t key = KEY(s->color_va), w, h;
    int i, free_i = -1, lru = 0;
    D3D11_TEXTURE2D_DESC td;

    if (s->swizzled) { w = s->swz_w; h = s->swz_h; }
    else {
        w = s->pitch / (s->bytes_per_pixel ? s->bytes_per_pixel : 4);
        h = s->clip_y + s->height;
        if (w < s->clip_x + s->width) w = s->clip_x + s->width;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    for (i = 0; i < MAX_RT; i++) {
        RT *r = &s_rt[i];
        if (!r->tex) { if (free_i < 0) free_i = i; continue; }
        if (r->key == key) {
            if (r->w >= w && r->h >= h && r->bpp == s->bytes_per_pixel
                && r->swizzled == s->swizzled && (s->swizzled ? r->w == w : r->pitch == s->pitch)) {
                r->last_frame = s_frame;
                return r;
            }
            rt_free(r);               /* reshaped: recreate */
            free_i = i;
            break;
        }
        if (r->last_frame < s_rt[lru].last_frame) lru = i;
    }
    if (free_i < 0) { rt_free(&s_rt[lru]); free_i = lru; }
    {
        RT *r = &s_rt[free_i];
        r->key = key; r->va = s->color_va; r->w = w; r->h = h; r->pitch = s->pitch;
        r->bpp = s->bytes_per_pixel; r->format = s->format; r->swizzled = s->swizzled;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex))) { memset(r, 0, sizeof *r); return NULL; }
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->rtv);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging);
        r->last_frame = s_frame;
        rt_upload_guest(r);
        fprintf(stderr, "[D3D11] render target 0x%08X %ux%u bpp %u%s\n", s->color_va, w, h, r->bpp,
                r->swizzled ? " swizzled" : "");
        return r;
    }
}

static RT *rt_find_key(uint32_t key)
{
    int i;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].key == key)
            return &s_rt[i];
    return NULL;
}

static DS *ds_get(uint32_t zeta_va, uint32_t w, uint32_t h)
{
    uint32_t key = KEY(zeta_va);
    int i, slot = -1;
    D3D11_TEXTURE2D_DESC td;
    if (w < 1280) w = 1280;
    if (h < 1024) h = 1024;
    for (i = 0; i < MAX_DS; i++) {
        if (s_ds[i].tex && s_ds[i].key == key) {
            if (s_ds[i].w >= w && s_ds[i].h >= h) return &s_ds[i];
            if (s_q_ok > 0) qflush();
            ID3D11DepthStencilView_Release(s_ds[i].dsv);
            ID3D11Texture2D_Release(s_ds[i].tex);
            memset(&s_ds[i], 0, sizeof s_ds[i]);
            slot = i;
            break;
        }
        if (!s_ds[i].tex && slot < 0) slot = i;
    }
    if (slot < 0) {
        slot = 0;
        if (s_q_ok > 0) qflush();
        ID3D11DepthStencilView_Release(s_ds[0].dsv);
        ID3D11Texture2D_Release(s_ds[0].tex);
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ds[slot].tex))) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_ds[slot].tex, NULL, &s_ds[slot].dsv);
    s_ds[slot].key = key; s_ds[slot].w = w; s_ds[slot].h = h;
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_ds[slot].dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return &s_ds[slot];
}

/* ------------------------------------------------------------ textures */
typedef struct {
    uint32_t key, w, h, color, pitch, cube, palette, bytes;
    uint64_t hash;
    uint32_t hashed_frame, last_frame;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} Tex;
#define MAX_TEX 4096
static Tex s_tex[MAX_TEX];
/* Lookup index: chains by guest address (index + 1; 0 ends a chain). */
#define TEX_BUCKETS 4096
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[MAX_TEX];
static uint32_t s_tex_used, s_tex_hint;
static uint32_t tex_bucket(uint32_t key) { return (key * 2654435761u) >> 20; }
static void tex_link(uint32_t i)
{
    uint32_t b = tex_bucket(s_tex[i].key);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
    s_tex_used++;
}
static void tex_drop(uint32_t i)                 /* release and unlink */
{
    if (s_q_ok > 0) qflush();
    uint32_t b = tex_bucket(s_tex[i].key);
    uint16_t *pp = &s_tex_head[b];
    while (*pp && *pp != i + 1) pp = &s_tex_next[*pp - 1];
    if (*pp) { *pp = s_tex_next[i]; s_tex_used--; }
    if (s_tex[i].srv) ID3D11ShaderResourceView_Release(s_tex[i].srv);
    if (s_tex[i].tex) ID3D11Texture2D_Release(s_tex[i].tex);
    memset(&s_tex[i], 0, sizeof s_tex[i]);
}
static uint32_t *s_decode;
static size_t s_decode_cap;
static int s_fast_off = -1, s_tex_verify = -1;   /* HITZ_GPU_FASTDECODE=0, HITZ_TEX_VERIFY=1 */

static uint64_t hash_mem(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* Decode the common uncompressed formats straight from guest memory, as
 * the toolkit's per-texel sampler would (same channel expansion, same BT.601
 * YUY2 maths), but a row at a time: the per-texel path (wrap, border, format
 * switch for each texel) cost ~2 ms per 640x480 movie frame and several ms
 * per in-match 256x256. Swizzled offsets are separable -- the Morton offset
 * of (x, y) is offset(x, 0) | offset(0, y) -- so two small tables do it.
 * Returns 0 for anything else (the caller uses the toolkit's decoder). */
static int fast_decode(const Nv2aStage *t, uint32_t *out)
{
    static uint32_t *ox, *oy;
    static uint32_t ox_cap, oy_cap;
    uint32_t fmt = t->color, w = t->width, h = t->height, x, y, bpp, swz;
    const uint8_t *m = GUEST(t->offset);
    if (t->cube || t->palette || !w || !h) return 0;
    switch (fmt) {
    case 0x02: case 0x04: case 0x05: case 0x03: bpp = 2; swz = 1; break;
    case 0x06: case 0x07:                       bpp = 4; swz = 1; break;
    case 0x10: case 0x11: case 0x1C: case 0x1D: bpp = 2; swz = 0; break;
    case 0x12: case 0x1E:                       bpp = 4; swz = 0; break;
    case 0x24: case 0x25:                       bpp = 2; swz = 0; break;
    default: return 0;
    }
    if (!swz && t->pitch < w * bpp) return 0;
    if (swz) {
        if (w > ox_cap) { free(ox); ox = (uint32_t *)malloc(w * 4); ox_cap = ox ? w : 0; }
        if (h > oy_cap) { free(oy); oy = (uint32_t *)malloc(h * 4); oy_cap = oy ? h : 0; }
        if (!ox || !oy) return 0;
        for (x = 0; x < w; x++) ox[x] = swz_offset(x, 0, w, h);
        for (y = 0; y < h; y++) oy[y] = swz_offset(0, y, w, h);
    }
    for (y = 0; y < h; y++) {
        uint32_t *d = out + (size_t)y * w;
        const uint8_t *row = swz ? m : m + (size_t)y * t->pitch;
        #define IDX(x_) (swz ? (ox[x_] | oy[y]) : (x_))
        switch (fmt) {
        case 0x06: case 0x12:
            if (!swz) { memcpy(d, row, (size_t)w * 4); break; }
            for (x = 0; x < w; x++) d[x] = ((const uint32_t *)row)[IDX(x)];
            break;
        case 0x07: case 0x1E:
            for (x = 0; x < w; x++) d[x] = ((const uint32_t *)row)[IDX(x)] | 0xFF000000u;
            break;
        case 0x02: case 0x10:
            for (x = 0; x < w; x++) {
                uint32_t v = ((const uint16_t *)row)[IDX(x)];
                d[x] = ((v & 0x8000u) ? 0xFF000000u : 0u) | (d3d8_expand_channel((v >> 10) & 0x1F, 5) << 16)
                     | (d3d8_expand_channel((v >> 5) & 0x1F, 5) << 8) | d3d8_expand_channel(v & 0x1F, 5);
            }
            break;
        case 0x03: case 0x1C:
            for (x = 0; x < w; x++) {
                uint32_t v = ((const uint16_t *)row)[IDX(x)];
                d[x] = 0xFF000000u | (d3d8_expand_channel((v >> 10) & 0x1F, 5) << 16)
                     | (d3d8_expand_channel((v >> 5) & 0x1F, 5) << 8) | d3d8_expand_channel(v & 0x1F, 5);
            }
            break;
        case 0x05: case 0x11:
            for (x = 0; x < w; x++) {
                uint32_t v = ((const uint16_t *)row)[IDX(x)];
                d[x] = 0xFF000000u | (d3d8_expand_channel((v >> 11) & 0x1F, 5) << 16)
                     | (d3d8_expand_channel((v >> 5) & 0x3F, 6) << 8) | d3d8_expand_channel(v & 0x1F, 5);
            }
            break;
        case 0x04: case 0x1D:
            for (x = 0; x < w; x++) {
                uint32_t v = ((const uint16_t *)row)[IDX(x)];
                d[x] = (d3d8_expand_channel((v >> 12) & 0x0F, 4) << 24) | (d3d8_expand_channel((v >> 8) & 0x0F, 4) << 16)
                     | (d3d8_expand_channel((v >> 4) & 0x0F, 4) << 8) | d3d8_expand_channel(v & 0x0F, 4);
            }
            break;
        case 0x24: case 0x25: {
            uint32_t yoff = fmt == 0x24 ? 0u : 1u;
            for (x = 0; x < w; x++) {
                const uint8_t *g = row + (size_t)(x & ~1u) * 2;
                int c = (int)g[(x & 1u) ? 2 + yoff : yoff] - 16;
                int cu = (int)g[1 - yoff] - 128, cv = (int)g[3 - yoff] - 128;
                int r = (298 * c + 409 * cv + 128) >> 8;
                int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
                int b = (298 * c + 516 * cu + 128) >> 8;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                d[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
            }
            break;
        }
        }
        #undef IDX
    }
    return 1;
}

static ID3D11ShaderResourceView *tex_get(int st, const Nv2aStage *t, int *is_cube)
{
    uint32_t key = KEY(t->offset), i, h, slot = MAX_TEX, lru = 0;
    uint64_t hash = 0;
    int have_hash = 0;
    RT *rt;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[6];
    uint32_t faces = t->cube ? 6 : 1, f;
    unsigned long long t_up0 = 0;
    size_t up_bytes = 0;            /* what actually goes to the GPU */

    *is_cube = 0;
    rt = rt_find_key(key);
    if (rt && !t->cube && rt->last_frame + 2 >= s_frame) {   /* render-to-texture */
        s_st.rt_alias++;
        rt->sampled_frame = s_frame;
        return rt->srv;
    }
    (void)h;
    {
        uint16_t c;
        for (c = s_tex_head[tex_bucket(key)]; c; c = s_tex_next[c - 1]) {
            Tex *x = &s_tex[c - 1];
            if (x->key == key && x->w == t->width && x->h == t->height && x->color == t->color
                && x->pitch == t->pitch && x->cube == t->cube && x->palette == t->palette) {
                if (x->hashed_frame != s_frame) {
                    unsigned long long th = TNOW();
                    hash = hash_mem(GUEST(t->offset), t->bytes);
                    if (t->palette) hash ^= hash_mem(GUEST(t->palette), 1024) * 31;
                    xbox_PerfCount(XBOX_PERF_HASH_MS, (TNOW() - th) / 1e6);
                    have_hash = 1;
                    x->hashed_frame = s_frame;
                    if (x->hash != hash) {        /* contents changed: re-upload */
                        tex_drop((uint32_t)(c - 1));
                        break;
                    }
                }
                x->last_frame = s_frame;
                *is_cube = (int)x->cube;
                return x->srv;
            }
        }
    }
    /* A free slot (or the least recently used one when full). */
    if (s_tex_used < MAX_TEX) {
        for (i = 0; i < MAX_TEX; i++, s_tex_hint = (s_tex_hint + 1) % MAX_TEX)
            if (!s_tex[s_tex_hint].tex) break;
        slot = s_tex_hint;
    } else {
        for (i = 0; i < MAX_TEX; i++)
            if (s_tex[i].last_frame < s_tex[lru].last_frame) lru = i;
        tex_drop(lru);
        slot = lru;
    }
    t_up0 = TNOW();
    if (!have_hash) {
        hash = hash_mem(GUEST(t->offset), t->bytes);
        if (t->palette) hash ^= hash_mem(GUEST(t->palette), 1024) * 31;
    }
    memset(&td, 0, sizeof td);
    td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = faces;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    {
        /* DXT1/3/5 are the GPU's own BC1/2/3: hand the blocks over as they
         * are instead of decoding every texel on the CPU (that decode was
         * most of each in-match texture hitch: 10-26 ms for 1-3 MB). The
         * Xbox stores the blocks row by row like D3D does; level 0 comes
         * first, and cube faces are one face stride apart. BC needs the top
         * level to be whole blocks; anything else takes the CPU path.
         * HITZ_GPU_BC=0 turns it off. */
        static int bc_on = -1;
        if (s_fast_off < 0) { const char *e = getenv("HITZ_GPU_FASTDECODE"); s_fast_off = e && *e == '0'; }
        if (s_tex_verify < 0) { const char *e = getenv("HITZ_TEX_VERIFY"); s_tex_verify = e && *e == '1'; }
        uint32_t bb = 0;
        DXGI_FORMAT bf = DXGI_FORMAT_UNKNOWN;
        if (bc_on < 0) { const char *e = getenv("HITZ_GPU_BC"); bc_on = !(e && *e == '0'); }
        switch (t->color) {
        case 0x0C: bb = 8;  bf = DXGI_FORMAT_BC1_UNORM; break;   /* DXT1 */
        case 0x0E: bb = 16; bf = DXGI_FORMAT_BC2_UNORM; break;   /* DXT3 */
        case 0x0F: bb = 16; bf = DXGI_FORMAT_BC3_UNORM; break;   /* DXT5 */
        default: break;
        }
        if (bc_on && bb && !(t->width & 3) && !(t->height & 3)
            && (size_t)(t->width / 4) * (t->height / 4) * bb * faces <= t->bytes) {
            const uint8_t *g = GUEST(t->offset);
            size_t face_stride = faces > 1 ? t->bytes / 6 : 0;
            td.Format = bf;
            for (f = 0; f < faces; f++) {
                sd[f].pSysMem = g + face_stride * f;
                sd[f].SysMemPitch = (t->width / 4) * bb;
                sd[f].SysMemSlicePitch = 0;
            }
            up_bytes = (size_t)(t->width / 4) * (t->height / 4) * bb * faces;
            s_st.tex_bc++;
        } else {
            unsigned long long td0 = TNOW();
            size_t need = (size_t)t->width * t->height * faces;
            {   /* which formats still take the CPU path, and how much */
                static unsigned fmt_n[256], fmt_px[256];
                fmt_n[t->color & 255]++; fmt_px[t->color & 255] += t->width * t->height * faces;
                if (getenv("HITZ_TEX_FMT_LOG") && (fmt_n[t->color & 255] & (fmt_n[t->color & 255] - 1)) == 0)
                    fprintf(stderr, "[D3D11] cpu-decoded format 0x%02X: %u textures, %u kpx so far (last %ux%u%s%s)\n",
                            t->color & 255, fmt_n[t->color & 255], fmt_px[t->color & 255] / 1000, t->width, t->height,
                            t->cube ? " cube" : "", t->palette ? " palette" : "");
            }
            if (need > s_decode_cap) {
                free(s_decode);
                s_decode = (uint32_t *)malloc(need * 4);
                s_decode_cap = s_decode ? need : 0;
            }
            if (!s_decode)
                return NULL;
            if (!s_fast_off && fast_decode(t, s_decode)) {
                s_st.tex_fast++;
                if (s_tex_verify > 0) {
                    /* HITZ_TEX_VERIFY=1: the toolkit's decoder must agree. */
                    static unsigned checked[256];
                    if (checked[t->color & 255]++ < 8) {
                        uint32_t *ref = (uint32_t *)malloc(need * 4);
                        if (ref && nv2a_backend_decode_stage(st, ref)) {
                            size_t i, bad = 0;
                            for (i = 0; i < need; i++) bad += ref[i] != s_decode[i];
                            fprintf(stderr, "[D3D11] fast decode check: format 0x%02X %ux%u %s (%zu of %zu texels differ)\n",
                                    t->color & 255, t->width, t->height, bad ? "MISMATCH" : "identical", bad, need);
                        }
                        free(ref);
                    }
                }
            } else if (!nv2a_backend_decode_stage(st, s_decode)) {
                return NULL;
            }
            for (f = 0; f < faces; f++) {
                sd[f].pSysMem = s_decode + (size_t)t->width * t->height * f;
                sd[f].SysMemPitch = t->width * 4;
                sd[f].SysMemSlicePitch = 0;
            }
            up_bytes = need * 4;
            s_st.tex_ms_cpu += TNOW() - td0;
        }
    }
    {
        Tex *x = &s_tex[slot];
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &x->tex))) { memset(x, 0, sizeof *x); return NULL; }
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)x->tex, NULL, &x->srv);
        x->key = key; x->w = t->width; x->h = t->height; x->color = t->color; x->pitch = t->pitch;
        x->cube = t->cube; x->palette = t->palette; x->bytes = t->bytes; x->hash = hash;
        x->hashed_frame = x->last_frame = s_frame;
        tex_link(slot);
        s_st.tex_up++;
        s_st.tex_up_bytes += up_bytes;
        xbox_PerfCount(XBOX_PERF_TEXTURES, 1);
        xbox_PerfCount(XBOX_PERF_TEX_BYTES, (double)up_bytes);
        xbox_PerfCount(XBOX_PERF_TEX_MS, (TNOW() - t_up0) / 1e6);
        *is_cube = (int)t->cube;
        return x->srv;
    }
}

/* ------------------------------------------------------------ state objects */
static D3D11_BLEND blend_f(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}
static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}
static D3D11_COMPARISON_FUNC cmp_f(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}
static D3D11_STENCIL_OP st_op(uint32_t o)
{
    switch (o) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

typedef struct { uint64_t key; void *obj; } Cache;
#define CACHE_N 512
static Cache s_bs[CACHE_N], s_dss[CACHE_N], s_ss[CACHE_N];
static void *cache_find(Cache *c, uint64_t key)
{
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 55) & (CACHE_N - 1), n;
    for (n = 0; n < CACHE_N; n++, i = (i + 1) & (CACHE_N - 1)) {
        if (!c[i].obj) return NULL;
        if (c[i].key == key) return c[i].obj;
    }
    return NULL;
}
static void cache_put(Cache *c, uint64_t key, void *obj)
{
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 55) & (CACHE_N - 1), n;
    for (n = 0; n < CACHE_N; n++, i = (i + 1) & (CACHE_N - 1))
        if (!c[i].obj) { c[i].key = key; c[i].obj = obj; return; }
}

static ID3D11BlendState *blend_state(const Nv2aRenderState *r)
{
    uint64_t key = ((uint64_t)r->blend_enable << 63) | ((uint64_t)(r->blend_src & 0xFFFF) << 40)
                 | ((uint64_t)(r->blend_dst & 0xFFFF) << 24) | ((uint64_t)(r->blend_eq & 0xFFFF) << 8)
                 | ((r->color_mask & 0x01000000u) ? 8 : 0) | ((r->color_mask & 0x00010000u) ? 4 : 0)
                 | ((r->color_mask & 0x100u) ? 2 : 0) | ((r->color_mask & 1u) ? 1 : 0);
    ID3D11BlendState *bs = (ID3D11BlendState *)cache_find(s_bs, key);
    D3D11_BLEND_DESC d;
    if (bs) return bs;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = r->blend_enable ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = blend_f(r->blend_src, 0);
    d.RenderTarget[0].DestBlend = blend_f(r->blend_dst, 0);
    d.RenderTarget[0].BlendOp = blend_op(r->blend_eq);
    d.RenderTarget[0].SrcBlendAlpha = blend_f(r->blend_src, 1);
    d.RenderTarget[0].DestBlendAlpha = blend_f(r->blend_dst, 1);
    d.RenderTarget[0].BlendOpAlpha = blend_op(r->blend_eq);
    d.RenderTarget[0].RenderTargetWriteMask =
        ((r->color_mask & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
      | ((r->color_mask & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
      | ((r->color_mask & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
      | ((r->color_mask & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &bs))) return NULL;
    cache_put(s_bs, key, bs);
    return bs;
}

static ID3D11DepthStencilState *depth_state(const Nv2aRenderState *r, int have_ds)
{
    uint64_t key;
    ID3D11DepthStencilState *ds;
    D3D11_DEPTH_STENCIL_DESC d;
    int test = have_ds && r->depth_test_enable, write = have_ds && r->depth_write;
    int sten = have_ds && r->stencil_enable;
    key = (uint64_t)test | ((uint64_t)write << 1) | ((uint64_t)sten << 2) | ((uint64_t)(r->depth_func & 0xF) << 3)
        | ((uint64_t)(r->stencil_func & 0xF) << 7) | ((uint64_t)(r->stencil_func_mask & 0xFF) << 11)
        | ((uint64_t)(r->stencil_write_mask & 0xFF) << 19) | ((uint64_t)(r->stencil_fail & 0xFFFF) << 27)
        | ((uint64_t)(r->stencil_zfail & 0xFFF) << 43) | ((uint64_t)(r->stencil_zpass & 0x1FF) << 55);
    if (sten) key ^= (uint64_t)r->stencil_zpass * 0x9E37ull ^ (uint64_t)r->stencil_zfail * 0x85EBull;
    ds = (ID3D11DepthStencilState *)cache_find(s_dss, key);
    if (ds) return ds;
    memset(&d, 0, sizeof d);
    d.DepthEnable = (test || write) ? TRUE : FALSE;
    d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = test ? cmp_f(r->depth_func) : D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = sten ? TRUE : FALSE;
    d.StencilReadMask = (UINT8)r->stencil_func_mask;
    d.StencilWriteMask = (UINT8)r->stencil_write_mask;
    d.FrontFace.StencilFunc = cmp_f(r->stencil_func);
    d.FrontFace.StencilFailOp = st_op(r->stencil_fail);
    d.FrontFace.StencilDepthFailOp = st_op(r->stencil_zfail);
    d.FrontFace.StencilPassOp = st_op(r->stencil_zpass);
    d.BackFace = d.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &ds))) return NULL;
    cache_put(s_dss, key, ds);
    return ds;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_m(uint32_t a)
{
    switch (a) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState *sampler(const Nv2aStage *t)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int lin = (mag == 2 || min == 2 || min == 4 || min == 6);
    uint64_t key = (uint64_t)lin | ((uint64_t)(t->addr_u & 7) << 1) | ((uint64_t)(t->addr_v & 7) << 4)
                 | ((uint64_t)(t->border_color ? t->border : 0xFFFFFFFFu) << 8) | ((uint64_t)t->border_color << 40);
    ID3D11SamplerState *ss = (ID3D11SamplerState *)cache_find(s_ss, key);
    D3D11_SAMPLER_DESC d;
    uint32_t b = t->border_color ? t->border : 0xFFFFFFFFu;
    if (ss) return ss;
    memset(&d, 0, sizeof d);
    d.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = addr_m(t->addr_u);
    d.AddressV = addr_m(t->addr_v);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    /* The CPU path clamps where the title asked for a border without a border
     * colour: do the same. */
    if (d.AddressU == D3D11_TEXTURE_ADDRESS_BORDER && !t->border_color) d.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (d.AddressV == D3D11_TEXTURE_ADDRESS_BORDER && !t->border_color) d.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.BorderColor[0] = ((b >> 16) & 255) / 255.0f; d.BorderColor[1] = ((b >> 8) & 255) / 255.0f;
    d.BorderColor[2] = (b & 255) / 255.0f; d.BorderColor[3] = (b >> 24) / 255.0f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &ss))) return NULL;
    cache_put(s_ss, key, ss);
    return ss;
}

/* ------------------------------------------------------------ init */
static int init_device(void)
{
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *pick = NULL, *a = NULL;
    const char *want = getenv("HITZ_GPU_ADAPTER");
    SIZE_T pick_vram = 0;
    int pick_named = 0;
    UINT i;
    HRESULT hr;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    char name[128] = "?";

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac)))
        return 0;
    for (i = 0; IDXGIFactory1_EnumAdapters1(fac, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        char n[128];
        IDXGIAdapter1_GetDesc1(a, &d);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof n, NULL, NULL);
        fprintf(stderr, "[D3D11] adapter %u: %s (vendor %04X%s)\n", i, n, d.VendorId,
                (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "");
        fprintf(stderr, "[D3D11]   %u MB dedicated video memory\n",
                (unsigned)(d.DedicatedVideoMemory >> 20));
        /* A named adapter (the launcher's GPU choice) wins; otherwise the
         * hardware adapter with the most video memory. Windows lists the
         * integrated chip first on most laptops, and it is far too slow
         * for a match (it was picked before: 29 fps, then a frozen picture). */
        if (want && *want && strstr(n, want) && !pick_named) {
            if (pick) IDXGIAdapter1_Release(pick);
            pick = a; pick_named = 1; strcpy(name, n);
            continue;
        }
        if (!pick_named && d.VendorId != 0x1414 && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && (!pick || d.DedicatedVideoMemory > pick_vram)) {
            if (pick) IDXGIAdapter1_Release(pick);
            pick = a; pick_vram = d.DedicatedVideoMemory; strcpy(name, n);
            continue;
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(fac);
    hr = D3D11CreateDevice((IDXGIAdapter *)pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           NULL, 0, &fl, 1, D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
    if (pick) IDXGIAdapter1_Release(pick);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] device creation failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&s_ctx1);
    fprintf(stderr, "[D3D11] rendering on %s\n", pick ? name : "default adapter");
    xbox_PerfNote("gpu: %s", pick ? name : "default adapter");

    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a", NULL, NULL, "vs_main", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[D3D11] VS: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a", NULL, NULL, "ps_main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[D3D11] PS: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    {
        ID3DBlob *pvb = NULL;
        hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a", NULL, NULL, "vs_prog", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pvb, &err);
        if (FAILED(hr)) { fprintf(stderr, "[D3D11] VSP: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(pvb), ID3D10Blob_GetBufferSize(pvb), NULL, &s_vs_prog);
        ID3D10Blob_Release(pvb);
    }
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    {
        ID3DBlob *fb = NULL;
        if (SUCCEEDED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a", NULL, NULL, "ps_flat", "ps_5_0", 0, 0, &fb, NULL))) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(fb), ID3D10Blob_GetBufferSize(fb), NULL, &s_ps_flat);
            ID3D10Blob_Release(fb);
        }
        const char *e;
        e = getenv("HITZ_GPU_FLATPS");
        if (e && *e == '1' && s_ps_flat) {
            ID3D11PixelShader_Release(s_ps); s_ps = s_ps_flat; ID3D11PixelShader_AddRef(s_ps);
            fprintf(stderr, "[D3D11] DEBUG: flat pixel shader\n");
        }
    }
    {
        D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, d0), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, d1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Nv2aXVertex, fog), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, tex[0]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, tex[1]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, tex[2]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aXVertex, tex[3]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(ID3D11Device_CreateInputLayout(s_dev, il, 8, ID3D10Blob_GetBufferPointer(vsb),
                                                  ID3D10Blob_GetBufferSize(vsb), &s_il)))
            return 0;
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        s_vb_size = 32u << 20;
        bd.ByteWidth = s_vb_size; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) return 0;
        bd.ByteWidth = (sizeof(CBData) + 15) & ~15u; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb))) return 0;
        bd.ByteWidth = (sizeof(VCData) + 15) & ~15u;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vcb))) return 0;
        bd.ByteWidth = 136 * 16;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vpb))) return 0;
        s_ib_size = 16u << 20;
        bd.ByteWidth = s_ib_size; bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib))) return 0;
        s_raw_size = 64u << 20;
        bd.ByteWidth = s_raw_size; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_raw))) return 0;
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv;
            memset(&sv, 0, sizeof sv);
            sv.Format = DXGI_FORMAT_R32_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sv.BufferEx.FirstElement = 0;
            sv.BufferEx.NumElements = s_raw_size / 4;
            sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_raw, &sv, &s_raw_srv))) return 0;
        }
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE; rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs[0]);
        rd.CullMode = D3D11_CULL_BACK;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs[1]);
        rd.DepthClipEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs[3]);
        rd.CullMode = D3D11_CULL_NONE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs[2]);
    }
    s_cull = getenv("HITZ_GPU_CULL") && *getenv("HITZ_GPU_CULL") == '1';
    psc_init();
    return 1;
}

static int ready(void)
{
    static int state;                 /* 0 untried, 1 ok, -1 failed */
    if (!state) state = init_device() ? 1 : -1;
    if (state < 0 && !s_failed) { s_failed = 1; fprintf(stderr, "[D3D11] back end unavailable\n"); }
    return state > 0;
}

/* ------------------------------------------------------------ callbacks */
static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    RT *r;
    if (!ready()) return;
    if (s_q_ok > 0) qflush();
    if (trace_now())
        fprintf(stderr, "[TRACE] clear rt 0x%08X flags 0x%X argb %08X z %08X\n", s->color_va, flags, argb, zstencil);
    r = rt_get(s);
    if (!r) return;
    if (flags & 0xF0) {
        float c[4] = { ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f,
                       (argb & 255) / 255.0f, (argb >> 24) / 255.0f };
        D3D11_RECT rc = { (LONG)s->clip_x, (LONG)s->clip_y, (LONG)(s->clip_x + s->width), (LONG)(s->clip_y + s->height) };
        if (s->swizzled || (s->clip_x == 0 && s->clip_y == 0 && s->width >= r->w && s->height >= r->h) || !s_ctx1)
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        else
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)r->rtv, c, &rc, 1);
        r->dirty = 1;
    }
    if ((flags & 0x3) && rs->zeta_va) {
        DS *d = ds_get(rs->zeta_va, r->w, r->h);
        if (d) {
            float zmax = s->zeta_bits == 16 ? 65535.0f : 16777215.0f;
            float zv = (s->zeta_bits == 16 ? (float)(zstencil & 0xFFFF) : (float)(zstencil >> 8)) / zmax;
            UINT cf = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv, cf, zv > 1.0f ? 1.0f : zv,
                                                      (UINT8)(zstencil & 0xFF));
        }
    }
}

/* HITZ_GPU_PROF=1: GPU time per draw category (timestamp pair per draw,
 * summed 3 frames later) and pipeline statistics per frame. Diagnostic only:
 * the queries themselves cost a little. */
#define GP_MAX 8192
typedef struct {
    ID3D11Query *dj, *ps, *t[GP_MAX * 2];
    uint8_t cat[GP_MAX];
    uint32_t px_hint[GP_MAX];
    int n;
} GProfFrame;
static GProfFrame s_gp[4];
static int s_gp_on = -1, s_gp_frame;
static double s_gp_cat_ms[3];             /* 0 draw_x, 1 draw_vsh, 2 clear */
static unsigned long long s_gp_vs, s_gp_psi, s_gp_iav, s_gp_frames;
static int gprof_on(void)
{
    if (s_gp_on < 0) { const char *e = getenv("HITZ_GPU_PROF"); s_gp_on = e && *e == '1'; }
    return s_gp_on;
}
static void gprof_begin_draw(int cat)
{
    GProfFrame *f = &s_gp[s_gp_frame & 3];
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP, 0 };
    if (!gprof_on() || f->n >= GP_MAX) return;
    if (!f->t[f->n * 2]) {
        ID3D11Device_CreateQuery(s_dev, &qd, &f->t[f->n * 2]);
        ID3D11Device_CreateQuery(s_dev, &qd, &f->t[f->n * 2 + 1]);
    }
    f->cat[f->n] = (uint8_t)cat;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->t[f->n * 2]);
}
static void gprof_end_draw(void)
{
    GProfFrame *f = &s_gp[s_gp_frame & 3];
    if (!gprof_on() || f->n >= GP_MAX) return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->t[f->n * 2 + 1]);
    f->n++;
}
/* At flip: close this frame, collect the one from 3 flips ago, open the next. */
static void gprof_flip(void)
{
    GProfFrame *f;
    D3D11_QUERY_DESC qd;
    int k;
    if (!gprof_on()) return;
    memset(&qd, 0, sizeof qd);
    f = &s_gp[s_gp_frame & 3];
    if (f->dj) {
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->ps);
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->dj);
    }
    s_gp_frame++;
    f = &s_gp[s_gp_frame & 3];          /* oldest: collect, then reuse */
    if (f->dj && f->n) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS st;
        if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)f->dj, &d, sizeof d, 0) == S_OK
            && !d.Disjoint && d.Frequency) {
            for (k = 0; k < f->n; k++) {
                UINT64 a = 0, b = 0;
                if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)f->t[k * 2], &a, sizeof a, 0) == S_OK
                    && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)f->t[k * 2 + 1], &b, sizeof b, 0) == S_OK
                    && b >= a)
                    s_gp_cat_ms[f->cat[k] < 3 ? f->cat[k] : 0] += (double)(b - a) * 1e3 / (double)d.Frequency;
            }
            if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)f->ps, &st, sizeof st, 0) == S_OK) {
                s_gp_vs += st.VSInvocations; s_gp_psi += st.PSInvocations; s_gp_iav += st.IAVertices;
            }
            s_gp_frames++;
        }
    }
    if (!f->dj) {
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ID3D11Device_CreateQuery(s_dev, &qd, &f->dj);
        qd.Query = D3D11_QUERY_PIPELINE_STATISTICS; ID3D11Device_CreateQuery(s_dev, &qd, &f->ps);
    }
    f->n = 0;
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)f->dj);
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)f->ps);
}
static void gprof_report(void)
{
    if (!gprof_on() || !s_gp_frames) return;
    fprintf(stderr, "[D3D11-PROF] per frame: gpu draw_x %.1f ms, draw_vsh %.1f ms | IA verts %.0fk, VS inv %.0fk, PS inv %.0fk\n",
            s_gp_cat_ms[0] / s_gp_frames, s_gp_cat_ms[1] / s_gp_frames,
            s_gp_iav / 1e3 / s_gp_frames, s_gp_vs / 1e3 / s_gp_frames, s_gp_psi / 1e3 / s_gp_frames);
    s_gp_cat_ms[0] = s_gp_cat_ms[1] = s_gp_cat_ms[2] = 0;
    s_gp_vs = s_gp_psi = s_gp_iav = s_gp_frames = 0;
}

/* ---- specialised pixel-shader cache ----
 *
 * A pixel shader specialised to each combiner/alpha-test state is much
 * faster than the general one, but compiling one takes ~20 ms -- longer
 * than a frame. Compiled on the render thread the first time a state
 * appeared, each new state was a dropped frame: the "random" stutters, and
 * a burst of them when a scene (a goal replay) brings several new states.
 *
 *  - Misses are compiled on a background thread; until the shader is ready
 *    those draws use the general shader, which renders the same picture.
 *    HITZ_GPU_ASYNC_SHADERS=0 compiles on the render thread as before.
 *  - Every compiled shader is appended to cache\hitz_shaders.bin, and the
 *    file is loaded at start, so a state seen in any earlier session costs
 *    nothing. The file carries a hash of the shader source: a build whose
 *    shaders changed starts it afresh. HITZ_SHADER_CACHE=0 turns it off. */
typedef struct {
    uint32_t n, flags, sp, use_rc, tex, at, af, cube, norm, fin0, fin1;
    uint32_t icw[8], ocw[8], aicw[8], aocw[8];
} PsKey;
typedef struct { PsKey k; uint64_t h; ID3D11PixelShader *volatile ps; volatile LONG state; int used; } PsEnt;
enum { PS_NONE, PS_QUEUED, PS_READY, PS_FAILED };
#define PS_CACHE 2048
static PsEnt s_psc[PS_CACHE];
static int s_spec_on = -1;
static unsigned s_spec_compiled, s_spec_failed, s_spec_loaded, s_spec_async;
static double s_spec_ms;

#define PSC_FILE "cache\\hitz_shaders.bin"
#define PSC_MAGIC 0x3153505Au                   /* "ZPS1" */
static CRITICAL_SECTION s_psc_cs;               /* the file and the job ring */
static CONDITION_VARIABLE s_psc_cv;
static int s_psc_async = -1, s_psc_disk = -1;
static uint64_t s_psc_src_hash;
#define PSQ 256
static struct { int slot; char *src; size_t len; } s_psq[PSQ];
static unsigned s_psq_head, s_psq_tail;

static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) h = (h ^ *b++) * 0x100000001b3ull;
    return h;
}

static uint64_t key_hash(const PsKey *k)
{
    return fnv(k, sizeof *k, 0xcbf29ce484222325ull);
}

/* The slot for a key: its entry, or the free slot it would take (or -1 if
 * the table is full). *found says which. */
static int ps_slot(const PsKey *k, uint64_t h, int *found)
{
    uint32_t slot = (uint32_t)(h >> 40) & (PS_CACHE - 1);
    int i;
    for (i = 0; i < PS_CACHE; i++, slot = (slot + 1) & (PS_CACHE - 1)) {
        PsEnt *e = &s_psc[slot];
        if (!e->used) { *found = 0; return (int)slot; }
        if (e->h == h && !memcmp(&e->k, k, sizeof *k)) { *found = 1; return (int)slot; }
    }
    *found = 0;
    return -1;
}

static void psc_append(const PsKey *k, const void *code, uint32_t len)
{
    FILE *f;
    if (s_psc_disk <= 0)
        return;
    EnterCriticalSection(&s_psc_cs);
    if (fopen_s(&f, PSC_FILE, "ab") == 0 && f) {
        uint32_t ks = (uint32_t)sizeof *k;
        fwrite(&ks, 4, 1, f); fwrite(k, sizeof *k, 1, f);
        fwrite(&len, 4, 1, f); fwrite(code, 1, len, f);
        fclose(f);
    }
    LeaveCriticalSection(&s_psc_cs);
}

/* Compile one key's source into its entry (render thread or worker). */
static void ps_compile(int slot, const char *src, size_t len, int background)
{
    PsEnt *e = &s_psc[slot];
    ID3DBlob *bl = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    unsigned long long t0 = now_ns();
    double ms;
    if (SUCCEEDED(D3DCompile(src, len, "nv2a_spec", NULL, NULL, "ps_spec", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &bl, &err))) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(bl), ID3D10Blob_GetBufferSize(bl), NULL, &ps);
        if (ps)
            psc_append(&e->k, ID3D10Blob_GetBufferPointer(bl), (uint32_t)ID3D10Blob_GetBufferSize(bl));
        ID3D10Blob_Release(bl);
    } else if (s_spec_failed < 3) {
        fprintf(stderr, "[D3D11] spec PS failed: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
    }
    if (err) ID3D10Blob_Release(err);
    ms = (now_ns() - t0) / 1e6;
    EnterCriticalSection(&s_psc_cs);
    if (ps) s_spec_compiled++; else s_spec_failed++;
    s_spec_ms += ms;
    LeaveCriticalSection(&s_psc_cs);
    if (!background) {
        xbox_PerfCount(XBOX_PERF_SHADERS, 1);
        xbox_PerfCount(XBOX_PERF_SHADER_MS, ms);
    }
    e->ps = ps;
    InterlockedExchange(&e->state, ps ? PS_READY : PS_FAILED);
}

static DWORD WINAPI ps_worker(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        int slot;
        char *src;
        size_t len;
        EnterCriticalSection(&s_psc_cs);
        while (s_psq_head == s_psq_tail)
            SleepConditionVariableCS(&s_psc_cv, &s_psc_cs, INFINITE);
        slot = s_psq[s_psq_head].slot;
        src = s_psq[s_psq_head].src;
        len = s_psq[s_psq_head].len;
        s_psq_head = (s_psq_head + 1) % PSQ;
        LeaveCriticalSection(&s_psc_cs);
        ps_compile(slot, src, len, 1);
        free(src);
    }
}

/* Load cache\hitz_shaders.bin into the table (at device start). */
static void psc_load(void)
{
    FILE *f;
    uint32_t hdr[4], ks, len;
    unsigned long long t0 = now_ns();
    unsigned n = 0, bad = 0;
    long good_end;                              /* file offset after the last whole record */
    if (fopen_s(&f, PSC_FILE, "rb") != 0 || !f) {
        CreateDirectoryA("cache", NULL);
        goto fresh;
    }
    if (fread(hdr, 4, 4, f) != 4 || hdr[0] != PSC_MAGIC || hdr[1] != (uint32_t)sizeof(PsKey)
        || ((uint64_t)hdr[3] << 32 | hdr[2]) != s_psc_src_hash) {
        fclose(f);
        fprintf(stderr, "[D3D11] shader cache: built for other shaders; starting afresh\n");
        goto fresh;
    }
    good_end = ftell(f);
    for (;;) {
        PsKey k;
        void *code;
        int found, slot;
        if (fread(&ks, 4, 1, f) != 1) break;
        if (ks != sizeof k || fread(&k, sizeof k, 1, f) != 1 || fread(&len, 4, 1, f) != 1
            || len == 0 || len > (1u << 20)) { bad++; break; }
        code = malloc(len);
        if (!code || fread(code, 1, len, f) != len) { free(code); bad++; break; }
        {
            uint64_t h = key_hash(&k);
            slot = ps_slot(&k, h, &found);
            if (slot >= 0 && !found) {
                PsEnt *e = &s_psc[slot];
                ID3D11PixelShader *ps = NULL;
                if (SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, code, len, NULL, &ps)) && ps) {
                    e->used = 1; e->h = h; e->k = k; e->ps = ps; e->state = PS_READY;
                    n++;
                }
            }
        }
        free(code);
        good_end = ftell(f);
    }
    fclose(f);
    if (bad && good_end > 0) {
        /* A record cut short (the game was killed mid-write). Appends go after
         * it, so without this the shaders compiled from now on would sit
         * behind garbage and never load again. */
        int fd;
        if (_sopen_s(&fd, PSC_FILE, _O_RDWR | _O_BINARY, _SH_DENYRW, 0) == 0 && fd >= 0) {
            _chsize_s(fd, good_end);
            _close(fd);
        }
    }
    s_spec_loaded = n;
    fprintf(stderr, "[D3D11] shader cache: %u shaders loaded from %s in %.0f ms%s\n", n, PSC_FILE,
            (now_ns() - t0) / 1e6, bad ? " (a damaged tail was cut off; those shaders are rebuilt)" : "");
    xbox_PerfNote("shader cache: %u shaders loaded (states seen in earlier sessions cost nothing)", n);
    return;
fresh:
    if (fopen_s(&f, PSC_FILE, "wb") == 0 && f) {
        hdr[0] = PSC_MAGIC; hdr[1] = (uint32_t)sizeof(PsKey);
        hdr[2] = (uint32_t)s_psc_src_hash; hdr[3] = (uint32_t)(s_psc_src_hash >> 32);
        fwrite(hdr, 4, 4, f);
        fclose(f);
    } else {
        s_psc_disk = 0;
        fprintf(stderr, "[D3D11] shader cache: cannot write %s; not kept\n", PSC_FILE);
    }
    xbox_PerfNote("shader cache: empty (new shaders are compiled in the background and kept)");
}

static void psc_init(void)
{
    const char *e;
    InitializeCriticalSection(&s_psc_cs);
    InitializeConditionVariable(&s_psc_cv);
    e = getenv("HITZ_GPU_ASYNC_SHADERS");
    s_psc_async = !(e && *e == '0');
    e = getenv("HITZ_SHADER_CACHE");
    s_psc_disk = !(e && *e == '0');
    s_psc_src_hash = fnv(s_ps_spec_src, sizeof s_ps_spec_src - 1,
                         fnv(s_hlsl, sizeof s_hlsl - 1, 0xcbf29ce484222325ull));
    if (s_psc_disk)
        psc_load();
    if (s_psc_async) {
        HANDLE th = CreateThread(NULL, 0, ps_worker, NULL, 0, NULL);
        if (th) CloseHandle(th); else s_psc_async = 0;
    }
}

static ID3D11PixelShader *ps_for(const PsKey *k)
{
    uint64_t h = key_hash(k);
    int found, slot = ps_slot(k, h, &found);
    PsEnt *e;
    if (slot < 0) return NULL;
    e = &s_psc[slot];
    if (found) {
        LONG st = e->state;
        if (st == PS_READY) return e->ps;
        if (st != PS_NONE) return NULL;          /* queued or failed: general shader */
    } else {
        e->used = 1; e->h = h; e->k = *k; e->ps = NULL; e->state = PS_NONE;
    }
    {
        static char src[65536];
        char *o = src;
        size_t cap = sizeof src, hl = sizeof s_hlsl - 1, sl = sizeof s_ps_spec_src - 1;
        int j, w;
        #define EMIT(...) do { w = snprintf(o, cap, __VA_ARGS__); o += w; cap -= (size_t)w; } while (0)
        EMIT("static const uint K_N = %u, K_FLAGS = 0x%X, K_SP = 0x%X, K_USE_RC = %u, K_TEX = %u, K_AT = %u, K_AF = 0x%X;\n",
             k->n, k->flags, k->sp, k->use_rc, k->tex, k->at, k->af);
        EMIT("static const uint K_FIN0 = 0x%X, K_FIN1 = 0x%X;\n", k->fin0, k->fin1);
        EMIT("static const uint K_CUBE[4] = { %u, %u, %u, %u };\n", k->cube & 1, (k->cube >> 1) & 1, (k->cube >> 2) & 1, (k->cube >> 3) & 1);
        EMIT("static const uint K_NORM[4] = { %u, %u, %u, %u };\n", k->norm & 1, (k->norm >> 1) & 1, (k->norm >> 2) & 1, (k->norm >> 3) & 1);
        #define EMIT_ARR(nm, a) do { EMIT("static const uint %s[8] = {", nm); \
            for (j = 0; j < 8; j++) EMIT(" 0x%X%s", (a)[j], j < 7 ? "," : " };\n"); } while (0)
        EMIT_ARR("K_ICW", k->icw); EMIT_ARR("K_OCW", k->ocw); EMIT_ARR("K_AICW", k->aicw); EMIT_ARR("K_AOCW", k->aocw);
        #undef EMIT_ARR
        #undef EMIT
        if (hl + sl + 1 >= cap) { e->state = PS_FAILED; return NULL; }
        memcpy(o, s_hlsl, hl); o += hl;
        memcpy(o, s_ps_spec_src, sl); o += sl;
        *o = 0;
        if (s_psc_async > 0) {
            size_t len = (size_t)(o - src);
            char *copy = (char *)malloc(len);
            EnterCriticalSection(&s_psc_cs);
            if (copy && (s_psq_tail + 1) % PSQ != s_psq_head) {
                memcpy(copy, src, len);
                s_psq[s_psq_tail].slot = slot;
                s_psq[s_psq_tail].src = copy;
                s_psq[s_psq_tail].len = len;
                s_psq_tail = (s_psq_tail + 1) % PSQ;
                e->state = PS_QUEUED;
                s_spec_async++;
                copy = NULL;
                WakeConditionVariable(&s_psc_cv);
            }
            LeaveCriticalSection(&s_psc_cs);
            free(copy);                          /* ring full: retried next draw */
            if (e->state == PS_QUEUED)
                xbox_PerfCount(XBOX_PERF_SHADERS_ASYNC, 1);
            return NULL;
        }
        ps_compile(slot, src, (size_t)(o - src), 0);
        return e->state == PS_READY ? e->ps : NULL;
    }
}

/* Constant-buffer ring (D3D11.1 offsets): one Map(NO_OVERWRITE) region per
 * frame chunk instead of a WRITE_DISCARD rename per draw, which made the
 * driver throttle at ~15k renames/frame. Offsets are in 16-constant (256 B)
 * units. */
static ID3D11Buffer *s_cring;
static UINT s_cring_size, s_cring_pos, s_cring_gen;
static uint8_t *s_cring_map;
static int s_cring_ok = -1;
static int cring_init(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_FEATURE_DATA_D3D11_OPTIONS o;
    if (s_cring_ok >= 0) return s_cring_ok;
    s_cring_ok = 0;
    if (!s_ctx1) return 0;
    memset(&o, 0, sizeof o);
    ID3D11Device_CheckFeatureSupport(s_dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o);
    if (!o.ConstantBufferOffsetting || !o.MapNoOverwriteOnDynamicConstantBuffer) {
        fprintf(stderr, "[D3D11] no constant-buffer offsetting; per-draw renames\n");
        return 0;
    }
    memset(&bd, 0, sizeof bd);
    s_cring_size = 32u << 20;
    bd.ByteWidth = s_cring_size; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cring))) return 0;
    s_cring_ok = 1;
    fprintf(stderr, "[D3D11] constant-buffer ring %u MB\n", s_cring_size >> 20);
    return 1;
}

/* ---------------------------------------------------------------- draw queue
 * Draws are recorded, not issued: constants, vertices, raw attributes and
 * indices are copied into CPU-side windows of the GPU rings, and the state is
 * kept as object pointers. qflush() maps each ring once, copies the windows,
 * and replays the draws with redundant state filtered. A ring's window is
 * reserved when a batch starts, so absolute offsets are known while recording
 * (vertex-program constants embed raw-buffer offsets). Anything that releases
 * a view, or clears, reads back or flips, flushes first. */
typedef struct {
    ID3D11Buffer *buf;
    UINT size, pos;           /* ring size; where the next window starts */
    UINT base, cap, used;     /* current window */
    int discard, open;
    uint8_t *cpu;
} Ring;
static Ring s_rcb, s_rvb, s_rraw, s_rib;
static uint32_t s_qgen;       /* bumps per batch: cached program offsets */

static int ring_setup(Ring *r, ID3D11Buffer *buf, UINT size, UINT cap)
{
    r->buf = buf; r->size = size; r->cap = cap; r->pos = 0; r->open = 0;
    r->cpu = (uint8_t *)malloc(cap);
    return r->cpu != NULL;
}
static void ring_begin(Ring *r)
{
    if (r->open) return;
    if (r->pos + r->cap > r->size) r->pos = 0;
    r->discard = (r->pos == 0);
    r->base = r->pos; r->used = 0; r->open = 1;
}
/* Absolute offset of n bytes (aligned), or ~0u if the window is full. */
static UINT ring_alloc(Ring *r, UINT n, UINT align)
{
    UINT at, abs_;
    ring_begin(r);
    /* Any alignment (vertex strides are not powers of two), on the absolute
     * offset: Draw's start vertex is offset / stride. */
    abs_ = r->base + r->used;
    abs_ = (abs_ + align - 1) / align * align;
    at = abs_ - r->base;
    if (at + n > r->cap) return ~0u;
    r->used = at + n;
    return r->base + at;
}
static uint8_t *ring_ptr(Ring *r, UINT abs_off) { return r->cpu + (abs_off - r->base); }
static void ring_upload(Ring *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (!r->open) return;
    if (r->used && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->buf, 0,
                 r->discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
        memcpy((uint8_t *)m.pData + r->base, r->cpu, r->used);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->buf, 0);
    }
    r->pos = r->base + ((r->used + 255) & ~255u);
    r->open = 0;
}

typedef struct {
    uint8_t prog;                               /* 0 draw_x, 1 vertex program */
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11RasterizerState *rs;
    ID3D11PixelShader *ps;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
    UINT sref, vpw, vph;
    float bf[4];
    D3D11_RECT sc;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    UINT cb0, cb1, cb2;                         /* first constant (16-byte units) */
    UINT n0, n1, n2;
    UINT voff, count;                           /* draw_x: vb offset, vertex count */
    UINT ifirst, icount;                        /* program: first index, count */
} QDraw;
#define QMAX 8192
static QDraw *s_q;
static UINT s_qn;
static int s_q_ok = -1;  /* (declared above) */

static int q_init(void)
{
    if (s_q_ok >= 0) return s_q_ok;
    s_q_ok = 0;
    if (!cring_init()) return 0;
    s_q = (QDraw *)malloc(sizeof *s_q * QMAX);
    if (!s_q
        || !ring_setup(&s_rcb, s_cring, s_cring_size, 8u << 20)
        || !ring_setup(&s_rvb, s_vb, s_vb_size, 12u << 20)
        || !ring_setup(&s_rraw, s_raw, s_raw_size, 24u << 20)
        || !ring_setup(&s_rib, s_ib, s_ib_size, 6u << 20))
        return 0;
    s_q_ok = 1;
    return 1;
}

static void qflush(void)
{
    static QDraw last;
    static int have_last;
    UINT i;
    UINT stride = sizeof(Nv2aXVertex), zero = 0;
    if (!s_qn) {
        ring_upload(&s_rcb); ring_upload(&s_rvb); ring_upload(&s_rraw); ring_upload(&s_rib);
        return;
    }
    ring_upload(&s_rcb); ring_upload(&s_rvb); ring_upload(&s_rraw); ring_upload(&s_rib);
    have_last = 0;
    /* Fixed for the whole replay. */
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &zero);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShaderResources(s_ctx, 8, 1, &s_raw_srv);
    for (i = 0; i < s_qn; i++) {
        QDraw *d = &s_q[i];
        int f = !have_last;
        if (f || d->rtv != last.rtv || d->dsv != last.dsv)
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &d->rtv, d->dsv);
        if (f || d->vpw != last.vpw || d->vph != last.vph) {
            D3D11_VIEWPORT vp = { 0, 0, (float)d->vpw, (float)d->vph, 0, 1 };
            ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        }
        if (f || memcmp(&d->sc, &last.sc, sizeof d->sc))
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &d->sc);
        if (f || d->rs != last.rs) ID3D11DeviceContext_RSSetState(s_ctx, d->rs);
        if (f || d->ps != last.ps) ID3D11DeviceContext_PSSetShader(s_ctx, d->ps, NULL, 0);
        if (f || memcmp(d->srv, last.srv, sizeof d->srv))
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, d->srv);
        if (f || memcmp(d->smp, last.smp, sizeof d->smp))
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, d->smp);
        if (f || d->bs != last.bs || memcmp(d->bf, last.bf, sizeof d->bf))
            ID3D11DeviceContext_OMSetBlendState(s_ctx, d->bs, d->bf, 0xFFFFFFFFu);
        if (f || d->dss != last.dss || d->sref != last.sref)
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, d->dss, d->sref);
        if (f || d->cb0 != last.cb0) {
            ID3D11DeviceContext1_VSSetConstantBuffers1(s_ctx1, 0, 1, &s_cring, &d->cb0, &d->n0);
            ID3D11DeviceContext1_PSSetConstantBuffers1(s_ctx1, 0, 1, &s_cring, &d->cb0, &d->n0);
        }
        if (f || d->prog != last.prog) {
            ID3D11DeviceContext_IASetInputLayout(s_ctx, d->prog ? NULL : s_il);
            ID3D11DeviceContext_VSSetShader(s_ctx, d->prog ? s_vs_prog : s_vs, NULL, 0);
        }
        if (d->prog) {
            if (f || d->cb1 != last.cb1 || d->cb2 != last.cb2 || !last.prog) {
                ID3D11Buffer *rb[2] = { s_cring, s_cring };
                UINT ff[2] = { d->cb1, d->cb2 }, nn[2] = { d->n1, d->n2 };
                ID3D11DeviceContext1_VSSetConstantBuffers1(s_ctx1, 1, 2, rb, ff, nn);
            }
            gprof_begin_draw(1);
            ID3D11DeviceContext_DrawIndexed(s_ctx, d->icount, d->ifirst, 0);
        } else {
            gprof_begin_draw(0);
            ID3D11DeviceContext_Draw(s_ctx, d->count, d->voff / (UINT)sizeof(Nv2aXVertex));
        }
        gprof_end_draw();
        last = *d;
        have_last = 1;
    }
    s_qn = 0;
    s_qgen++;
}

/* Record the state part of a draw into q (no device calls except state-object
 * creation, texture uploads and render-target creation). Returns the target. */
static RT *prepare(const Nv2aSurface *s, const Nv2aBatchX *b, int prog, QDraw *q, CBData *cbp)
{
    const Nv2aRenderState *rs = b->state;
    RT *r;
    DS *d = NULL;
#define cb (*cbp)
    int st;
    uint32_t k_cube = 0, k_norm = 0;
    ID3D11PixelShader *ps = s_ps;

    if (!ready() || !q_init()) return NULL;
    r = rt_get(s);
    if (!r) return NULL;
    if (rs->zeta_va && (rs->depth_test_enable || rs->depth_write || rs->stencil_enable))
        d = ds_get(rs->zeta_va, r->w, r->h);
    memset(q, 0, sizeof *q);

    memset(&cb, 0, sizeof cb);
    if (b->rc) {
        memcpy(cb.icw, b->rc->color_icw, 32); memcpy(cb.aicw, b->rc->alpha_icw, 32);
        memcpy(cb.ocw, b->rc->color_ocw, 32); memcpy(cb.aocw, b->rc->alpha_ocw, 32);
        memcpy(cb.f0, b->rc->factor0, 32);    memcpy(cb.f1, b->rc->factor1, 32);
        cb.fin[0] = b->rc->final0; cb.fin[1] = b->rc->final1;
        cb.fin[2] = b->rc->final_c0; cb.fin[3] = b->rc->final_c1;
        cb.ctl[0] = b->rc->control; cb.ctl[1] = b->rc->stage_program;
    }
    cb.ctl[2] = b->use_rc; cb.ctl[3] = b->textured;
    if (!b->use_rc) cb.ctl[1] = b->textured ? 1 : 0;      /* stage 0 as 2D for the modulate path */
    cb.misc[0] = b->fog_color; cb.misc[1] = rs->alpha_test_enable;
    cb.misc[2] = rs->alpha_func; cb.misc[3] = rs->alpha_ref;
    cb.misc2[0] = b->clip_plane_mode;
    cb.rt[0] = (float)r->w; cb.rt[1] = (float)r->h;
    cb.rt[2] = 1.0f / (s->zeta_bits == 16 ? 65535.0f : 16777215.0f);

    for (st = 0; st < 4; st++) {
        const Nv2aStage *t = &b->stage[st];
        uint32_t mode = (cb.ctl[1] >> (st * 5)) & 31;
        int cube = 0;
        ID3D11ShaderResourceView *v;
        if (!t->valid || !mode || mode > 3) continue;
        v = tex_get(st, t, &cube);
        if (v == r->srv) v = NULL;          /* sampling the target being drawn */
        if (!v) continue;
        if (cube) q->srv[4 + st] = v; else q->srv[st] = v;
        q->smp[st] = sampler(t);
        cb.ts[st][0] = (float)t->width; cb.ts[st][1] = (float)t->height;
        cb.ts[st][2] = t->normalized ? 1.0f : 0.0f;
        cb.ts[st][3] = cube ? 1.0f : 0.0f;
        if (cube) k_cube |= 1u << st;
        if (t->normalized) k_norm |= 1u << st;
    }

    q->rtv = r->rtv;
    q->dsv = d ? d->dsv : NULL;
    q->vpw = r->w; q->vph = r->h;
    q->sc.left = (LONG)s->clip_x; q->sc.top = (LONG)s->clip_y;
    q->sc.right = (LONG)(s->clip_x + s->width); q->sc.bottom = (LONG)(s->clip_y + s->height);
    if (s->swizzled) { q->sc.left = 0; q->sc.top = 0; q->sc.right = (LONG)r->w; q->sc.bottom = (LONG)r->h; }
    q->rs = s_rs[(prog ? 2 : 0) + (s_cull && rs->cull_enable ? 1 : 0)];
    if (s_spec_on < 0) { const char *e = getenv("HITZ_GPU_UBER"); s_spec_on = !(e && *e == '1'); }
    if (s_spec_on && s_ps != s_ps_flat) {
        PsKey k;
        uint32_t n = cb.ctl[0] & 0xFF;
        memset(&k, 0, sizeof k);
        k.use_rc = cb.ctl[2]; k.tex = cb.ctl[3]; k.sp = cb.ctl[1];
        k.at = rs->alpha_test_enable ? 1 : 0; k.af = k.at ? rs->alpha_func : 0;
        k.cube = k_cube; k.norm = k_norm;
        if (k.use_rc) {
            k.n = n > 8 ? 8 : n; k.flags = cb.ctl[0] >> 8; k.fin0 = cb.fin[0]; k.fin1 = cb.fin[1];
            memcpy(k.icw, cb.icw, 4 * k.n); memcpy(k.ocw, cb.ocw, 4 * k.n);
            memcpy(k.aicw, cb.aicw, 4 * k.n); memcpy(k.aocw, cb.aocw, 4 * k.n);
        }
        {
            ID3D11PixelShader *sp_ = ps_for(&k);
            if (sp_) ps = sp_;
        }
    }
    q->ps = ps;
    {
        uint32_t c = rs->blend_color;
        if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
            q->bf[0] = q->bf[1] = q->bf[2] = q->bf[3] = (c >> 24) / 255.0f;
        else {
            q->bf[0] = ((c >> 16) & 255) / 255.0f; q->bf[1] = ((c >> 8) & 255) / 255.0f;
            q->bf[2] = (c & 255) / 255.0f; q->bf[3] = (c >> 24) / 255.0f;
        }
    }
    q->bs = blend_state(rs);
    q->dss = depth_state(rs, d != NULL);
    q->sref = rs->stencil_ref & 0xFF;
    q->prog = (uint8_t)prog;
    q->n0 = ((sizeof(CBData) + 255) & ~255u) / 16;
    r->dirty = 1;
    return r;
#undef cb
}

/* HITZ_GPU_TRACE=1: log every draw/clear of one light frame (<= 8 draws)
 * every ~5 s, to debug screens that come out wrong. */
static int s_trace_on = -1, s_trace_frame;
static uint32_t s_trace_last;
static int trace_now(void)
{
    if (s_trace_on < 0) { const char *e = getenv("HITZ_GPU_TRACE"); s_trace_on = e && *e == '1'; }
    return s_trace_on && s_trace_frame;
}
static void trace_draw(const char *kind, const Nv2aSurface *s, const Nv2aBatchX *b, const Nv2aXVertex *v0)
{
    const Nv2aRenderState *rs = b->state;
    int st;
    fprintf(stderr, "[TRACE] %s rt 0x%08X %ux%u+%u+%u swz %u fmt %u zeta 0x%08X | use_rc %u tex %u sp 0x%X ctl 0x%X fin %08X/%08X"
            " | z test %u func 0x%X write %u sten %u | blend %u 0x%X/0x%X eq 0x%X mask %08X | at %u 0x%X ref %u | cull %u\n",
            kind, s->color_va, s->width, s->height, s->clip_x, s->clip_y, s->swizzled, s->format, rs->zeta_va,
            b->use_rc, b->textured, b->rc ? b->rc->stage_program : 0, b->rc ? b->rc->control : 0,
            b->rc ? b->rc->final0 : 0, b->rc ? b->rc->final1 : 0,
            rs->depth_test_enable, rs->depth_func, rs->depth_write, rs->stencil_enable,
            rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq, rs->color_mask,
            rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref, rs->cull_enable);
    for (st = 0; st < 4; st++)
        if (b->stage[st].valid)
            fprintf(stderr, "[TRACE]   stage %d 0x%08X %ux%u color 0x%X norm %u cube %u\n", st, b->stage[st].offset,
                    b->stage[st].width, b->stage[st].height, b->stage[st].color, b->stage[st].normalized, b->stage[st].cube);
    if (v0)
        fprintf(stderr, "[TRACE]   v0 pos %.1f %.1f %.4f %.3f d0 %.2f %.2f %.2f %.2f t0 %.3f %.3f %.3f %.3f\n",
                v0->pos[0], v0->pos[1], v0->pos[2], v0->pos[3], v0->d0[0], v0->d0[1], v0->d0[2], v0->d0[3],
                v0->tex[0][0], v0->tex[0][1], v0->tex[0][2], v0->tex[0][3]);
}

static void be_draw_x(const Nv2aSurface *s, const Nv2aBatchX *b)
{
    if (trace_now()) trace_draw("draw_x", s, b, b->count ? &b->v[0] : NULL);
    unsigned long long t0 = TNOW();
    UINT bytes = b->count * (UINT)sizeof(Nv2aXVertex), off = ~0u, cbo = ~0u;
    QDraw qd;
    CBData cb;
    int j;

    if (!b->count || !q_init() || bytes > s_rvb.cap) return;
    if (!prepare(s, b, 0, &qd, &cb)) return;         /* may flush: nothing reserved yet */
    for (j = 0; j < 2; j++) {
        if (s_qn >= QMAX) qflush();
        cbo = ring_alloc(&s_rcb, sizeof cb, 256);
        off = ring_alloc(&s_rvb, bytes, sizeof(Nv2aXVertex) * 16);
        if (cbo != ~0u && off != ~0u) break;
        qflush();
    }
    if (j == 2) return;
    memcpy(ring_ptr(&s_rcb, cbo), &cb, sizeof cb);
    memcpy(ring_ptr(&s_rvb, off), b->v, bytes);
    qd.cb0 = cbo / 16;
    qd.voff = off; qd.count = b->count;
    s_q[s_qn++] = qd;
    s_st.draws++;
    s_st.tris += b->count / 3;
    {
        unsigned long long dt = TNOW() - t0;
        s_st.t_draw_ns += dt;
        s_frame_draw_ns += dt;
    }
}

/* Program batches: raw attribute ranges (overlapping ranges of an
 * interleaved buffer copied once), indices, constants and the program. */
static void be_draw_vsh(const Nv2aSurface *s, const Nv2aBatchVsh *b)
{
    if (trace_now()) trace_draw("draw_vsh", s, &b->x, NULL);
    unsigned long long t0 = TNOW();
    struct { const uint8_t *p; uint32_t n; int a; } iv[16], tmp;
    uint32_t niv = 0, i, j, total = 0, ib_bytes = b->icount * 4;
    uint32_t base[16];
    static VCData vc;
    static struct { const uint8_t *p; uint32_t n, off; } run[16];
    uint32_t runs = 0, h, raw_at, ib_at, vc_at;
    static UINT pfirst, pnum, pgen = ~0u;
    static uint32_t phash;
    QDraw qd, *q = &qd;
    CBData cb;
    UINT cbo = ~0u, pa = ~0u;
    int need_prog = 0;

    if (!b->icount || !q_init() || ib_bytes > s_rib.cap) return;
    if (!prepare(s, &b->x, 1, &qd, &cb)) return;     /* may flush: nothing reserved yet */
    h = 2166136261u;
    for (i = 0; i < (b->program_len > 136 ? 136u : b->program_len) * 4; i++) h = (h ^ b->program[i]) * 16777619u;
    h ^= (b->program_len > 136 ? 136u : b->program_len);
    for (i = 0; i < 16; i++)
        if (b->attr[i].data && b->attr[i].size) {
            iv[niv].p = b->attr[i].data; iv[niv].n = b->attr[i].bytes; iv[niv].a = (int)i; niv++;
        }
    for (i = 1; i < niv; i++)                          /* sort by address */
        for (j = i; j > 0 && iv[j - 1].p > iv[j].p; j--) { tmp = iv[j]; iv[j] = iv[j - 1]; iv[j - 1] = tmp; }
    {   /* merge into runs */
        const uint8_t *rs_ = NULL, *re = NULL;
        uint32_t run_off = 0;
        for (i = 0; i < niv; i++) {
            if (!rs_ || iv[i].p > re) {
                if (rs_) { run[runs].p = rs_; run[runs].n = (uint32_t)(re - rs_); run[runs].off = run_off;
                           run_off += (run[runs].n + 15) & ~15u; runs++; }
                rs_ = iv[i].p; re = iv[i].p + iv[i].n;
            } else if (iv[i].p + iv[i].n > re) re = iv[i].p + iv[i].n;
            base[iv[i].a] = runs;
        }
        if (rs_) { run[runs].p = rs_; run[runs].n = (uint32_t)(re - rs_); run[runs].off = run_off;
                   run_off += (run[runs].n + 15) & ~15u; runs++; }
        total = run_off;
    }
    if (total > s_rraw.cap) return;
    /* Reserve everything this draw needs in the current windows; flush and
     * retry once if any is full (the reservation is then in fresh windows). */
    for (j = 0; j < 2; j++) {
        if (s_qn >= QMAX) qflush();
        need_prog = (h != phash || pgen != s_qgen);
        raw_at = total ? ring_alloc(&s_rraw, total, 16) : 0;
        ib_at = ring_alloc(&s_rib, ib_bytes, 256);
        cbo = ring_alloc(&s_rcb, sizeof cb, 256);
        vc_at = ring_alloc(&s_rcb, sizeof vc, 256);
        pa = need_prog ? ring_alloc(&s_rcb, 136 * 16, 256) : 0;
        if (raw_at != ~0u && ib_at != ~0u && cbo != ~0u && vc_at != ~0u && pa != ~0u)
            break;
        qflush();
    }
    if (j == 2) return;
    memcpy(ring_ptr(&s_rcb, cbo), &cb, sizeof cb);
    qd.cb0 = cbo / 16;
    for (i = 0; i < runs; i++)
        memcpy(ring_ptr(&s_rraw, raw_at) + run[i].off, run[i].p, run[i].n);
    for (i = 0; i < 16; i++) {
        const Nv2aAttrib *at = &b->attr[i];
        if (at->data && at->size) {
            uint32_t ri = base[i];
            vc.attr[i][0] = raw_at + run[ri].off + (uint32_t)(at->data - run[ri].p);
            vc.attr[i][1] = at->stride; vc.attr[i][2] = at->type; vc.attr[i][3] = at->size;
        } else {
            vc.attr[i][0] = vc.attr[i][1] = vc.attr[i][2] = vc.attr[i][3] = 0;
            memcpy(vc.aval[i], at->value, 16);
        }
    }
    memcpy(vc.c, b->constants, sizeof vc.c);
    vc.vm[0] = b->program_len > 136 ? 136 : b->program_len;
    vc.vm[1] = b->fog_enable; vc.vm[2] = b->fog_mode; vc.vm[3] = 0;
    vc.vmf[0] = b->fog_param[0]; vc.vmf[1] = b->fog_param[1]; vc.vmf[2] = b->aa_sx; vc.vmf[3] = b->aa_sy;
    memcpy(ring_ptr(&s_rcb, vc_at), &vc, sizeof vc);
    if (need_prog) {                                /* program: once per window */
        memset(ring_ptr(&s_rcb, pa), 0, 136 * 16);
        memcpy(ring_ptr(&s_rcb, pa), b->program, vc.vm[0] * 16);
        pfirst = pa / 16; pnum = (136 * 16 + 255) / 256 * 16;
        phash = h; pgen = s_qgen;
    }
    memcpy(ring_ptr(&s_rib, ib_at), b->indices, ib_bytes);
    q->cb1 = vc_at / 16; q->n1 = ((sizeof vc + 255) & ~255u) / 16;
    q->cb2 = pfirst; q->n2 = pnum;
    q->ifirst = ib_at / 4; q->icount = b->icount;
    s_q[s_qn++] = qd;
    s_st.draws++;
    s_st.vsh_draws++;
    s_st.tris += b->icount / 3;
    {
        unsigned long long dt = TNOW() - t0;
        s_st.t_draw_ns += dt;
        s_frame_draw_ns += dt;
    }
}

static uint16_t argb_to_565(uint32_t c)
{
    return (uint16_t)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31));
}

static int s_copies_queued;   /* this flip queued readback copies: flush them */

static void be_flip(void)
{
    unsigned long long t0;
    int i;
    if (!s_dev) return;
    t0 = TNOW();
    if (s_q_ok > 0) qflush();
    /* Unbind so render targets can be copied out cleanly. */
    {
        ID3D11ShaderResourceView *none[8] = { 0 };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, none);
    }
    /* Asynchronous readback, one flip late: last flip's copies are written to
     * guest memory now (the GPU has had a whole frame to finish them), then
     * this frame's dirty targets are queued. HITZ_GPU_SYNC_READBACK=1 restores
     * the old wait-at-flip behaviour. */
    {
        static int sync = -1;
        if (sync < 0) { const char *e = getenv("HITZ_GPU_SYNC_READBACK"); sync = e && *e == '1'; }
        for (int pass = sync ? 1 : 0; pass < 2; pass++) {
            for (i = 0; i < MAX_RT; i++) {
                RT *r = &s_rt[i];
                if (!r->tex || r->swizzled) continue;
                /* A target the GPU also samples stays on the GPU (no copy-out)
                 * unless it is full-screen: those can be the frame on screen,
                 * which the window reads from guest memory. Skipping them
                 * froze the picture on "LOADING" through the pre-game intro
                 * (the game samples the frame it presents) while the match
                 * ran and played sound. */
                if (pass == 1 && r->dirty && !sync && r->sampled_frame && r->sampled_frame + 120 > s_frame
                    && !(r->w >= 640 && r->h >= 448)) {
                    r->dirty = 0;                /* a GPU-side target (sampled): no copy-out */
                    s_st.rt_skipped++;
                    continue;
                }
                if (pass == 1 && r->dirty && r->pending)
                    continue;                    /* previous copy not written yet: next flip */
                if (pass == 1 && r->dirty) {
                    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging, (ID3D11Resource *)r->tex);
                    r->dirty = 0;
                    r->pending = 1;
                    s_copies_queued = 1;
                    if (!sync) continue;
                }
                if (r->pending && (pass == 0 || sync)) {
                    D3D11_MAPPED_SUBRESOURCE m;
                    uint32_t y, x;
                    uint8_t *g;
                    unsigned long long tm = TNOW();
                    /* Wait for last flip's copy (it was flushed to the GPU
                     * then, so this is normally already done). Polling with
                     * DO_NOT_WAIT used to fail on about half the in-match
                     * flips on NVIDIA; each miss dropped or repeated a frame
                     * on screen: smooth 60 fps by the counters, visibly
                     * jittery. HITZ_GPU_READBACK_POLL=1 restores polling. */
                    static int poll = -1;
                    if (poll < 0) { const char *e = getenv("HITZ_GPU_READBACK_POLL"); poll = e && *e == '1'; }
                    HRESULT mh = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0, D3D11_MAP_READ,
                                                         (sync || !poll) ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
                    if (mh == DXGI_ERROR_WAS_STILL_DRAWING) { s_st.rb_late++; continue; }
                    if (FAILED(mh))
                        continue;
                    s_st.t_map_ns += TNOW() - tm;
                    g = GUEST(r->va);
                    for (y = 0; y < r->h; y++) {
                        const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                        uint8_t *dst = g + (size_t)y * r->pitch;
                        if (r->bpp == 4)
                            memcpy(dst, src, (size_t)r->w * 4);
                        else
                            for (x = 0; x < r->w; x++)
                                ((uint16_t *)dst)[x] = argb_to_565(((const uint32_t *)src)[x]);
                    }
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
                    r->pending = 0;
                    s_st.rt_reads++;
                }
            }
        }
    }
    /* Send this flip's copies to the GPU now, so they are finished by the
     * next flip's Map instead of waiting in the driver's command buffer. */
    if (s_copies_queued) {
        s_copies_queued = 0;
        ID3D11DeviceContext_Flush(s_ctx);
    }
    /* GPU frame time: [disjoint, t_start] at frame begin, [t_end, end disjoint]
     * at its flip; results collected 3 frames later. */
    if (s_tm) {
        static ID3D11Query *dj[4], *t0q[4], *t1q[4];
        static int qinit, qn;
        int k;
        if (!qinit) {
            D3D11_QUERY_DESC qd;
            memset(&qd, 0, sizeof qd);
            for (k = 0; k < 4; k++) {
                qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ID3D11Device_CreateQuery(s_dev, &qd, &dj[k]);
                qd.Query = D3D11_QUERY_TIMESTAMP;          ID3D11Device_CreateQuery(s_dev, &qd, &t0q[k]);
                ID3D11Device_CreateQuery(s_dev, &qd, &t1q[k]);
            }
            qinit = 1;
        }
        if (qn > 0) {                                   /* close the frame just flipped */
            k = (qn - 1) & 3;
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)t1q[k]);
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)dj[k]);
        }
        if (qn >= 4) {                                  /* collect frame qn-4 (slot reused now) */
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
            UINT64 a = 0, b = 0;
            k = qn & 3;
            if (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)dj[k], &d, sizeof d, 0) == S_OK
                && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)t0q[k], &a, sizeof a, 0) == S_OK
                && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)t1q[k], &b, sizeof b, 0) == S_OK
                && !d.Disjoint && d.Frequency && b > a) {
                s_st.gpu_ns += (unsigned long long)((double)(b - a) * 1e9 / (double)d.Frequency);
                s_st.gpu_frames++;
            }
        }
        k = qn & 3;                                     /* open the next frame */
        ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)dj[k]);
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)t0q[k]);
        qn++;
    }
    {   /* for HITZ_HOSTPROF_HEAVY: profile only frames with >1000 draws */
        static unsigned long long prev_draws;
        g_hitz_heavy = (s_st.draws - prev_draws) > 1000;
        prev_draws = s_st.draws;
    }
    ID3D11DeviceContext_Flush(s_ctx);   /* submit the readback copies so they finish by next flip */
    {   /* trace: arm for the next frame if this one was light and 5 s passed */
        static unsigned long long prev;
        unsigned long long dn = s_st.draws - prev;
        prev = s_st.draws;
        if (s_trace_frame) { s_trace_frame = 0; fprintf(stderr, "[TRACE] --- flip\n"); }
        else if (trace_now() || (s_trace_on > 0 && dn <= 8 && s_frame - s_trace_last > 300)) {
            s_trace_frame = 1; s_trace_last = s_frame;
            fprintf(stderr, "[TRACE] --- frame %u\n", s_frame);
        }
    }
    gprof_flip();
    s_frame++;
    s_st.flips++;
    s_st.t_flip_ns += TNOW() - t0;
    xbox_PerfCount(XBOX_PERF_FLUSH_MS, (TNOW() - t0) / 1e6);
    xbox_PerfCount(XBOX_PERF_DRAW_MS, s_frame_draw_ns / 1e6);
    s_frame_draw_ns = 0;
    {
        static unsigned long long last;
        unsigned long long n = now_ns();
        if (!last) last = n;
        if (n - last > 10000000000ull) {
            double sec = (n - last) / 1e9;
            fprintf(stderr, "[D3D11] %.1f fps | %.0f draws/frame (%.0f gpu-vsh), %.0f tris/frame | draw cpu %.1f ms/frame, "
                            "flip %.1f ms/frame (map wait %.1f, late %llu, gpu-only %llu) | gpu %.1f ms/frame | tex uploads %llu (%llu compressed, %llu fast, %.1f MB; cpu decode %.1f ms), rt-as-texture %llu\n",
                    s_st.flips / sec, s_st.flips ? (double)s_st.draws / s_st.flips : 0.0,
                    s_st.flips ? (double)s_st.vsh_draws / s_st.flips : 0.0,
                    s_st.flips ? (double)s_st.tris / s_st.flips : 0.0,
                    s_st.flips ? s_st.t_draw_ns / 1e6 / s_st.flips : 0.0,
                    s_st.flips ? s_st.t_flip_ns / 1e6 / s_st.flips : 0.0,
                    s_st.flips ? s_st.t_map_ns / 1e6 / s_st.flips : 0.0, s_st.rb_late, s_st.rt_skipped,
                    s_st.gpu_frames ? s_st.gpu_ns / 1e6 / s_st.gpu_frames : 0.0,
                    s_st.tex_up, s_st.tex_bc, s_st.tex_fast, s_st.tex_up_bytes / 1048576.0, s_st.tex_ms_cpu / 1e6, s_st.rt_alias);
            gprof_report();
            if (s_spec_compiled || s_spec_failed || s_spec_loaded)
                fprintf(stderr, "[D3D11] specialised pixel shaders: %u from the cache, %u compiled (%u in the background, %.0f ms total), %u failed\n",
                        s_spec_loaded, s_spec_compiled, s_spec_async, s_spec_ms, s_spec_failed);
            memset(&s_st, 0, sizeof s_st);
            last = n;
            g_hitz_heavy = 0;
        }
    }
}

static const Nv2aBackend s_backend_d3d11 = { be_clear, NULL, be_flip, be_draw_x, be_draw_vsh };
static const Nv2aBackend s_backend_d3d11_novsh = { be_clear, NULL, be_flip, be_draw_x, NULL };

void hitz_gpu_backend_init(void)
{
    const char *e = getenv("HITZ_GPU");
    if (!e || *e != '1')
        return;
    s_tm = g_hitz_log || xbox_PerfEnabled();
    {
        const char *v = getenv("HITZ_GPU_VSH");      /* 0: vertex programs on the CPU */
        int gpu_vsh = !(v && *v == '0');
        nv2a_backend_register(gpu_vsh ? &s_backend_d3d11 : &s_backend_d3d11_novsh);
        fprintf(stderr, "[D3D11] NV2A GPU back end registered (HITZ_GPU=1, vertex programs on the %s)\n",
                gpu_vsh ? "GPU" : "CPU");
    }
}
