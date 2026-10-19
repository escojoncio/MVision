/* Shaders del renderer nativo (NativeRenderer.swift). Dibujan la escena leída de la RAM del motor: mallas locales + matriz de mundo por pieza + color por
 * vértice (precalculado o calculado con la luz de la pieza) + niebla, con la misma semántica de texturas/mezcla que el rasterizador de referencia
 * (vr/native/ns_cpuref.c): VRAM 1024x512 de 16 bits (R16Uint), texturas de 4/8/15 bpp con CLUT, texel 0 transparente, modulación texel*color/128 en 5 bits,
 * el bit 15 del texel decide si un texel de una cara semitransparente se mezcla. Sin descarte de caras (doble cara) ni recorte en CPU.
 *
 * Dos rutas de vértices con la misma lógica (ns_vertex): por pieza (opacos: una llamada de dibujo por pieza, índice de pieza en un búfer de 4 bytes) y por lista
 * ordenada (semitransparentes: NsGpuOrder[] de atrás adelante, ya ordenada en CPU). Layout de los búferes: vr/native/ns_gpu_types.h. */
#include <metal_stdlib>
#include "ns_gpu_types.h"
using namespace metal;

constant uint fcNsPass [[function_constant(NSG_FC_PASS)]];
constant bool fcNsLinear [[function_constant(NSG_FC_LINEAR)]];
constant uint kNsPass = is_function_constant_defined(fcNsPass) ? fcNsPass : 0u;
constant bool kNsLinear = is_function_constant_defined(fcNsLinear) ? fcNsLinear : false;

struct NsVOut {
    float4 pos [[position]];
    float2 uv;               /* texels 0..255, perspectiva correcta */
    float3 col;              /* byte de color por vértice (128 = neutro), interpolado */
    uint tpage [[flat]];
    uint clutf [[flat]];     /* clut | flags << 16 */
};

static float3 ns_mul34(const device float *m, float3 p)
{
    return float3(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                  m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                  m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
}

static NsVOut ns_vertex(uint corner_index, uint piece_index, uint amp,
                        const device NsGpuVert *verts, const device NsGpuCorner *corners,
                        const device NsGpuPiece *pieces, constant NsGpuUniforms &U)
{
    const device NsGpuCorner &c = corners[corner_index];
    const device NsGpuPiece &P = pieces[piece_index];
    const device NsGpuVert &V = verts[c.v];

    /* posición en el mundo del motor; los vértices welded toman la posición transformada del vértice del padre */
    float3 lp = float3(V.p[0], V.p[1], V.p[2]);
    float3 wp = ns_mul34(&P.w[0], lp);
    if (V.weld != NSG_NO_WELD && P.parent >= 0 && V.weld < P.parent_nverts) {
        const device NsGpuPiece &Q = pieces[uint(P.parent)];
        const device NsGpuVert &PV = verts[Q.vert_base + V.weld];
        wp = ns_mul34(&Q.w[0], float3(PV.p[0], PV.p[1], PV.p[2]));
    }

    /* color: precalculado de la cara, luz de la pieza o neutro; después niebla (hacia negro) y byte entero */
    uint cf = uint(c.flags);
    float3 raw;
    if ((cf & NSG_CF_PAINT) != 0u) {
        uint pc = c.paint;
        raw = float3(float(pc & 255u), float((pc >> 8) & 255u), float((pc >> 16) & 255u));
    } else if ((P.flags & NSG_PF_SHADE) != 0u) {
        float3 n = float3(c.n[0], c.n[1], c.n[2]);
        float3 I = clamp(float3(dot(float3(P.l[0], P.l[1], P.l[2]), n),
                                dot(float3(P.l[3], P.l[4], P.l[5]), n),
                                dot(float3(P.l[6], P.l[7], P.l[8]), n)), 0.0, NSG_INTENSITY_MAX);
        raw = float3(P.k[3] + dot(float3(P.k[0], P.k[1], P.k[2]), I),
                     P.k[7] + dot(float3(P.k[4], P.k[5], P.k[6]), I),
                     P.k[11] + dot(float3(P.k[8], P.k[9], P.k[10]), I));
    } else {
        raw = float3(128.0);
    }
    raw = clamp(raw, 0.0, 255.0);
    float s = P.fog;
    if ((P.flags & NSG_PF_VFOG) != 0u) {
        float z = dot(U.fog_fwd.xyz, wp) + U.fog_fwd.w;
        s = 1.0 - clamp((z - U.fog_par.x) / max(U.fog_par.y, 1.0e-3), 0.0, 1.0);
    }
    float3 col = clamp(floor(raw * s), 0.0, 255.0);

    /* mundo del motor -> mundo VR (m) */
    float3 pv = float3(dot(U.tovr[0].xyz, wp) + U.tovr[0].w,
                       dot(U.tovr[1].xyz, wp) + U.tovr[1].w,
                       dot(U.tovr[2].xyz, wp) + U.tovr[2].w);
    /* sesgo de orden del motor (decals): se acerca el vértice al ojo a lo largo de la línea de visión (no cambia su posición en pantalla) */
    if (P.raise != 0.0) {
        float3 d = U.eye[amp].xyz - pv;
        float dl = length(d);
        float sh = min(abs(P.raise) * U.fog_par.w, 0.9 * dl);
        pv += d * ((P.raise > 0.0 ? sh : -sh) / max(dl, 1.0e-5));
    }

    NsVOut o;
    o.pos = U.view_proj[amp] * float4(pv, 1.0);
    o.uv = float2(float(c.u), float(c.t));
    o.col = col;
    o.tpage = uint(c.tpage);
    o.clutf = uint(c.clut) | (cf << 16);
    return o;
}

/* ---- opacos: una pieza por llamada ---- */
vertex NsVOut ns_vs_piece(uint vid [[vertex_id]],
                          const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                          const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                          const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                          constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]],
                          constant uint &pieceIx [[buffer(NSG_BUF_PIECEIX)]])
{
    return ns_vertex(vid, pieceIx, 0u, verts, corners, pieces, U);
}

vertex NsVOut ns_vs_piece_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                              const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                              const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                              const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                              constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]],
                              constant uint &pieceIx [[buffer(NSG_BUF_PIECEIX)]])
{
    return ns_vertex(vid, pieceIx, uint(amp), verts, corners, pieces, U);
}

/* ---- semitransparentes: lista de triángulos ya ordenada ---- */
vertex NsVOut ns_vs_order(uint vid [[vertex_id]],
                          const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                          const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                          const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                          constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]],
                          const device NsGpuOrder *order [[buffer(NSG_BUF_ORDER)]])
{
    NsGpuOrder o = order[vid / 3u];
    return ns_vertex(o.tri * 3u + (vid % 3u), o.piece, 0u, verts, corners, pieces, U);
}

vertex NsVOut ns_vs_order_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                              const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                              const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                              const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                              constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]],
                              const device NsGpuOrder *order [[buffer(NSG_BUF_ORDER)]])
{
    NsGpuOrder o = order[vid / 3u];
    return ns_vertex(o.tri * 3u + (vid % 3u), o.piece, uint(amp), verts, corners, pieces, U);
}

/* ---------------- muestreo de VRAM (igual que ns_sample_texel de la referencia, sin ventana de textura) ---------------- */
static uint ns_texel(texture2d<ushort, access::read> vram, uint tpage, uint clut, int u, int v)
{
    int tx = int(tpage & 15u) * 64, ty = int((tpage >> 4) & 1u) * 256;
    uint tp = (tpage >> 7) & 3u;
    int clx = int(clut & 63u) * 16, cly = int((clut >> 6) & 511u);
    u &= 255; v &= 255;
    int y = (ty + v) & 511;
    if (tp == 0u) {
        uint w = uint(vram.read(uint2(uint((tx + (u >> 2)) & 1023), uint(y))).x);
        uint idx = (w >> uint((u & 3) * 4)) & 15u;
        return uint(vram.read(uint2(uint((clx + int(idx)) & 1023), uint(cly))).x);
    } else if (tp == 1u) {
        uint w = uint(vram.read(uint2(uint((tx + (u >> 1)) & 1023), uint(y))).x);
        uint idx = (w >> uint((u & 1) * 8)) & 255u;
        return uint(vram.read(uint2(uint((clx + int(idx)) & 1023), uint(cly))).x);
    }
    return uint(vram.read(uint2(uint((tx + u) & 1023), uint(y))).x);
}

static float ns_lin1(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

/* Pase 0 (opaco): caras no semitransparentes completas; en las semitransparentes solo los texels con bit15 = 0 (las sin textura no entran).
 * Pase 1 (semitransparente): solo los texels con bit15 = 1 de las texturizadas y todo lo sin textura. Alfa = cobertura (1 donde se escribe). */
fragment float4 ns_fs(NsVOut in [[stage_in]], texture2d<ushort, access::read> vram [[texture(0)]])
{
    uint flags = in.clutf >> 16;
    uint clut = in.clutf & 0xFFFFu;
    bool semi = (flags & NSG_CF_SEMI) != 0u;
    float3 col = clamp(in.col, 0.0, 255.0);
    float3 rgb;
    if ((flags & NSG_CF_TEX) != 0u) {
        uint tn = ns_texel(vram, in.tpage, clut, int(floor(in.uv.x + 1.0e-4)), int(floor(in.uv.y + 1.0e-4)));
        if (tn == 0u) discard_fragment();
        bool b15 = (tn & 0x8000u) != 0u;
        if (kNsPass == 0u) { if (semi && b15) discard_fragment(); }
        else if (!b15) discard_fragment();
        float3 t5 = float3(float(tn & 31u), float((tn >> 5) & 31u), float((tn >> 10) & 31u));
        rgb = clamp(t5 * col / 128.0, 0.0, 31.0) / 31.0;
    } else {
        if (kNsPass == 0u && semi) discard_fragment();
        rgb = col / 255.0;
    }
    if (kNsLinear) rgb = float3(ns_lin1(rgb.x), ns_lin1(rgb.y), ns_lin1(rgb.z));
    return float4(rgb, 1.0);
}
