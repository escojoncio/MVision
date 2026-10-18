/* Shaders Metal del renderer de alta resolución: portan 1:1 la lógica de render/ref_raster.c.
 * SIN COMPILAR (escrito sin toolchain Metal): revisar en la primera compilación. Ver render/README.md.
 *
 * Recursos:
 *  - VRAM nativa: texture2d<ushort> R16Uint 1024x512 (5-5-5 + bit15 = máscara). Fuente de texturas/CLUT.
 *  - VRAM alta resolución: RGBA8Unorm de (1024*M)x(512*M) (M = N, o N*K si se supermuestrea; opcionalmente
 *    multimuestra x4 con sombreado por muestra). RGB = color, A = bit de máscara (0/1).
 *  - Mezcla programable (lectura del framebuffer con [[color(0)]]): semitransparencia de 4 modos y comprobación
 *    de máscara exactas por muestra. Las GPU Apple soportan lectura de color del framebuffer en cualquier formato.
 */
#include <metal_stdlib>
#include "ps1_types.h"
using namespace metal;

constant bool fcExact5 [[function_constant(PS1_FC_EXACT5)]];
constant bool fcPerSample [[function_constant(PS1_FC_PERSAMPLE)]];
constant bool fcMS [[function_constant(PS1_FC_MS)]];
constant bool kExact5 = is_function_constant_defined(fcExact5) ? fcExact5 : false;
constant bool kPerSample = is_function_constant_defined(fcPerSample) ? fcPerSample : false;
constant bool kMS = is_function_constant_defined(fcMS) ? fcMS : false;
constant bool kSS = !kMS;

constant int4 kDither[4] = {int4(-4, 0, -3, 1), int4(2, -2, 3, -1), int4(-3, 1, -4, 0), int4(3, -1, 2, -2)};

/* ---------------- utilidades ---------------- */
inline int ex8(int v5) { return (v5 << 3) | (v5 >> 2); }
inline int3 ex8v(int3 v5) { return int3(ex8(v5.x), ex8(v5.y), ex8(v5.z)); }

inline int3 unpack555(ushort c) { return int3(c & 31, (c >> 5) & 31, (c >> 10) & 31); }
inline ushort pack555(int3 c, bool mask) { return ushort(c.x | (c.y << 5) | (c.z << 10) | (mask ? 0x8000 : 0)); }

/* Mezcla semitransparente: b = fondo, f = primer plano, mismo dominio; maxv = 31 (5 bits) o 255. */
inline int3 ps1_blend(int3 b, int3 f, uint abr, int maxv) {
    int3 r;
    switch (abr) {
    case 0: r = (b + f) >> 1; break;
    case 1: r = b + f; break;
    case 2: r = b - f; break;
    default: r = b + (f >> 2); break;
    }
    return clamp(r, int3(0), int3(maxv));
}

/* Muestreo de textura de la VRAM nativa: idéntico a texel() de ref_raster.c (ventana de textura, 4/8/15 bits). */
inline ushort ps1_texel(texture2d<ushort, access::read> vram, constant PS1Prim &p, int u, int v) {
    uint tp = (p.modes >> 2) & 3;
    int twmx = p.window & 255, twmy = (p.window >> 8) & 255, twox = (p.window >> 16) & 255, twoy = (p.window >> 24) & 255;
    u &= 255; v &= 255;
    u = (u & ~(twmx * 8)) | ((twox & twmx) * 8);
    v = (v & ~(twmy * 8)) | ((twoy & twmy) * 8);
    int tx = p.tpage & 0xFFFF, ty = p.tpage >> 16, clx = p.clut & 0xFFFF, cly = p.clut >> 16;
    int y = (ty + v) & 511;
    if (tp == 0) {
        ushort w = vram.read(uint2((tx + (u >> 2)) & 1023, y)).r;
        int idx = (w >> ((u & 3) * 4)) & 15;
        return vram.read(uint2((clx + idx) & 1023, cly & 511)).r;
    } else if (tp == 1) {
        ushort w = vram.read(uint2((tx + (u >> 1)) & 1023, y)).r;
        int idx = (w >> ((u & 1) * 8)) & 255;
        return vram.read(uint2((clx + idx) & 1023, cly & 511)).r;
    }
    return vram.read(uint2((tx + u) & 1023, y)).r;
}

/* ---------------- vértice ---------------- */
struct VOut {
    float4 pos [[position]];
    float2 uv;     /* interpolación con corrección de perspectiva (por defecto). Con w = 1 es afín, como la PS1 */
    float3 col;
    uint prim [[flat]];
};

/* Objetivo = VRAM completa: x en [0,1024) -> NDC [-1,1], y hacia abajo. w = 1 => interpolación afín (como la PS1);
 * w = z de vista (alta precisión) => con corrección de perspectiva. */
vertex VOut ps1_vs_prim(uint vid [[vertex_id]], constant PS1Vertex *verts [[buffer(0)]],
                        constant PS1Frame &fr [[buffer(1)]]) {
    PS1Vertex v = verts[vid];
    VOut o;
    /* El oráculo evalúa la cobertura en la coordenada ENTERA (esquina); Metal en el centro del píxel: con
     * fr.corner se desplaza el vértice +0,5 para que (c+0,5) - (v+0,5) = c - v. */
    float off = fr.corner ? 0.5 : 0.0;
    float px = (v.x + off) * (2.0 / 1024.0) - 1.0, py = 1.0 - (v.y + off) * (2.0 / 512.0);
    if (fr.hiprec != 0u && v.iz > 0.0) {
        /* Geometría de alta precisión: w = z de vista => uv/color con corrección de perspectiva; clip.z = 1 => ndc.z = 1/z
         * (profundidad lineal en pantalla, mayor = más cerca). Ver comentario de PS1Frame en ps1_types.h. */
        float w = 1.0 / v.iz;
        o.pos = float4(px * w, py * w, 1.0, w);
    } else {
        o.pos = float4(px, py, 0.0, 1.0); /* 2D / pasada nativa: afín, z = 0 (borra la profundidad si está activa) */
    }
    o.uv = float2(v.u, v.v);
    o.col = float3(v.r, v.g, v.b);
    o.prim = v.prim;
    return o;
}

/* Sombreado común: devuelve false si el texel es transparente (0x0000). rgb: modulado, 0..~496 (8 bits). */
inline bool ps1_shade(constant PS1Prim &p, float2 uv, float3 col, texture2d<ushort, access::read> vram,
                      thread int3 &rgb, thread bool &semi, thread bool &mbit, bool exact) {
    bool tex = p.flags & PS1_F_TEXTURED, raw = p.flags & PS1_F_RAW;
    semi = p.flags & PS1_F_SEMI;
    mbit = false;
    float3 c = col; /* sin Gouraud el host repite el color de v[0] en todos los vértices */
    if (!tex) { rgb = int3(c); return true; }
    ushort t = ps1_texel(vram, p, int(floor(uv.x)), int(floor(uv.y)));
    if (t == 0) return false;
    int3 t5 = unpack555(t);
    if (exact) {
        if (raw) rgb = t5 << 3;
        else rgb = (t5 * int3(c)) >> 4;       /* oráculo: (tr*r)>>4 con r truncado */
    } else {
        int3 te = ex8v(t5);
        rgb = raw ? te : int3(float3(te) * c * (1.0 / 128.0));
    }
    semi = semi && (t & 0x8000);
    mbit = (t & 0x8000) != 0;
    return true;
}

inline bool ps1_clipped(constant PS1Prim &p, float2 frag, uint M) {
    int2 f = int2(frag);
    int x1 = int(p.clip1 & 0xFFFF) * int(M), y1 = int(p.clip1 >> 16) * int(M);
    int x2 = (int(p.clip2 & 0xFFFF) + 1) * int(M), y2 = (int(p.clip2 >> 16) + 1) * int(M);
    return f.x < x1 || f.x >= x2 || f.y < y1 || f.y >= y2;
}

/* ---------------- primitivas -> objetivo de alta resolución (RGBA8, A = máscara) ---------------- */
fragment float4 ps1_fs_prim_hires(VOut in [[stage_in]], constant PS1Prim *prims [[buffer(0)]],
                                  constant PS1Frame &fr [[buffer(1)]],
                                  texture2d<ushort, access::read> vram [[texture(0)]],
                                  float4 dst [[color(0)]],
                                  uint sid [[sample_id, function_constant(kPerSample)]]) {
    constant PS1Prim &p = prims[in.prim];
    if (ps1_clipped(p, in.pos.xy, fr.scale)) discard_fragment();
    int3 rgb; bool semi, mbit;
    if (!ps1_shade(p, in.uv, in.col, vram, rgb, semi, mbit, kExact5)) discard_fragment();
    bool dmask = dst.a > 0.5;
    if ((p.flags & PS1_F_MASK_CHK) && dmask) discard_fragment();
    bool nm = mbit || (p.flags & PS1_F_MASK_SET);
    uint abr = p.modes & 3;
    int3 bd = int3(round(dst.rgb * 255.0));
    int3 outc;
    if (kExact5) {
        if (p.flags & PS1_F_DITHER) {
            int2 hp = int2(in.pos.xy) / int(fr.scale);
            rgb += kDither[hp.y & 3][hp.x & 3];
        }
        int3 c5 = clamp(rgb, int3(0), int3(255)) >> 3;
        if (semi) c5 = ps1_blend(bd >> 3, c5, abr, 31);
        outc = ex8v(c5);
    } else {
        int3 c8 = clamp(rgb, int3(0), int3(255));
        outc = semi ? ps1_blend(bd, c8, abr, 255) : c8;
    }
    return float4(float3(outc) * (1.0 / 255.0), nm ? 1.0 : 0.0);
}

/* ---------------- primitivas -> VRAM nativa 1x (R16Uint), exacto al oráculo ---------------- */
fragment ushort4 ps1_fs_prim_native(VOut in [[stage_in]], constant PS1Prim *prims [[buffer(0)]],
                                    texture2d<ushort, access::read> vram [[texture(0)]],
                                    ushort4 dst [[color(0)]]) {
    constant PS1Prim &p = prims[in.prim];
    if (ps1_clipped(p, in.pos.xy, 1)) discard_fragment();
    int3 rgb; bool semi, mbit;
    if (!ps1_shade(p, in.uv, in.col, vram, rgb, semi, mbit, true)) discard_fragment();
    if ((p.flags & PS1_F_MASK_CHK) && (dst.x & 0x8000)) discard_fragment();
    if (p.flags & PS1_F_DITHER) {
        int2 hp = int2(in.pos.xy);
        rgb += kDither[hp.y & 3][hp.x & 3];
    }
    int3 c5 = clamp(rgb, int3(0), int3(255)) >> 3;
    if (semi) c5 = ps1_blend(unpack555(dst.x), c5, p.modes & 3, 31);
    bool nm = mbit || (p.flags & PS1_F_MASK_SET);
    return ushort4(pack555(c5, nm), 0, 0, 0);
}

/* ---------------- eventos de VRAM sobre el objetivo de alta resolución ----------------
 * Se dibujan como un rectángulo (ps1_vs_rect) con el scissor/viewport del destino. */
struct RectOut { float4 pos [[position]]; };
vertex RectOut ps1_vs_rect(uint vid [[vertex_id]], constant PS1Event &ev [[buffer(0)]]) {
    float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0); /* tira de triángulos: 4 vértices */
    float2 p = float2(ev.x, ev.y) + c * float2(ev.w, ev.h);
    RectOut o;
    o.pos = float4(p.x * (2.0 / 1024.0) - 1.0, 1.0 - p.y * (2.0 / 512.0), 0.0, 1.0);
    return o;
}

/* Fill: ignora recorte y máscara (como el hardware). HQ: color de 8 bits original; si no, el de 5 bits. */
fragment float4 ps1_fs_fill_hires(RectOut in [[stage_in]], constant PS1Event &ev [[buffer(0)]]) {
    float3 c = float3(ev.color & 255, (ev.color >> 8) & 255, (ev.color >> 16) & 255) * (1.0 / 255.0);
    if (kExact5) c = float3(ex8v(unpack555(ushort(ev.fill15)))) * (1.0 / 255.0);
    return float4(c, 0.0);
}

/* Subida/copia ya aplicada a la VRAM nativa: replica el resultado (con la regla de máscara del evento) al
 * objetivo de alta resolución. Para copias que deben conservar detalle de alta resolución usar ps1_fs_copy_hires. */
fragment float4 ps1_fs_sync_hires(RectOut in [[stage_in]], constant PS1Event &ev [[buffer(0)]], constant PS1Frame &fr [[buffer(1)]],
                                  texture2d<ushort, access::read> vram [[texture(0)]], float4 dst [[color(0)]]) {
    if ((ev.flags & PS1_F_MASK_CHK) && dst.a > 0.5) discard_fragment();
    uint2 hp = uint2(in.pos.xy) / fr.scale;
    ushort c = vram.read(hp).r;
    return float4(float3(ex8v(unpack555(c))) * (1.0 / 255.0), ((c >> 15) | (ev.flags & PS1_F_MASK_SET)) ? 1.0 : 0.0);
}

/* Copia VRAM->VRAM a mitad de frame con detalle: lee de una INSTANTÁNEA del objetivo (blit previo del rectángulo
 * origen a una textura auxiliar; no se puede leer y escribir la misma textura). */
fragment float4 ps1_fs_copy_hires(RectOut in [[stage_in]], constant PS1Event &ev [[buffer(0)]], constant PS1Frame &fr [[buffer(1)]],
                                  texture2d<float, access::read> snap [[texture(0)]], float4 dst [[color(0)]]) {
    if ((ev.flags & PS1_F_MASK_CHK) && dst.a > 0.5) discard_fragment();
    int2 d = int2(in.pos.xy);
    int2 s = d + int2(ev.sx - ev.x, ev.sy - ev.y) * int(fr.scale);
    float4 c = snap.read(uint2(s));
    if (ev.flags & PS1_F_MASK_SET) c.a = 1.0;
    return c;
}

/* ---------------- kernels de cómputo sobre la VRAM nativa (R16Uint) ---------------- */
kernel void ps1_k_fill_native(texture2d<ushort, access::write> vram [[texture(0)]], constant PS1Event &ev [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(ev.w) || gid.y >= uint(ev.h)) return;
    vram.write(ushort4(ushort(ev.fill15), 0, 0, 0), uint2((ev.x + gid.x) & 1023, (ev.y + gid.y) & 511));
}

kernel void ps1_k_upload_native(texture2d<ushort, access::read_write> vram [[texture(0)]], constant PS1Event &ev [[buffer(0)]],
                                constant ushort *data [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(ev.w) || gid.y >= uint(ev.h)) return;
    uint idx = gid.y * uint(ev.w) + gid.x;
    if (idx >= ev.data_n) return; /* subida abortada (GP1 reset/01h): el oráculo solo escribió lo recibido */
    uint2 q = uint2((ev.x + gid.x) & 1023, (ev.y + gid.y) & 511);
    if ((ev.flags & PS1_F_MASK_CHK) && (vram.read(q).x & 0x8000)) return;
    ushort v = data[ev.data_off + idx] | ((ev.flags & PS1_F_MASK_SET) ? 0x8000 : 0);
    vram.write(ushort4(v, 0, 0, 0), q);
}

/* snap: copia del rectángulo origen (blit previo) para respetar la semántica de "leer antes de escribir". */
kernel void ps1_k_copy_native(texture2d<ushort, access::read_write> vram [[texture(0)]],
                              texture2d<ushort, access::read> snap [[texture(1)]], constant PS1Event &ev [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(ev.w) || gid.y >= uint(ev.h)) return;
    uint2 q = uint2((ev.x + gid.x) & 1023, (ev.y + gid.y) & 511);
    if ((ev.flags & PS1_F_MASK_CHK) && (vram.read(q).x & 0x8000)) return;
    ushort v = snap.read(gid).x | ((ev.flags & PS1_F_MASK_SET) ? 0x8000 : 0);
    vram.write(ushort4(v, 0, 0, 0), q);
}

/* Alta resolución -> nativa para el rectángulo ev (px nativos): lo que se dibujó a escala pasa a ser
 * textura legible (render a textura, copias de pantalla). Toma la media de las muestras del bloque. */
kernel void ps1_k_resolve_native(texture2d<float, access::read> hr [[texture(0), function_constant(kSS)]],
                                 texture2d_ms<float, access::read> hrms [[texture(1), function_constant(kMS)]],
                                 texture2d<ushort, access::write> vram [[texture(2)]], constant PS1Event &ev [[buffer(0)]],
                                 constant PS1Frame &fr [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(ev.w) || gid.y >= uint(ev.h)) return;
    uint2 q = uint2((ev.x + gid.x) & 1023, (ev.y + gid.y) & 511);
    float4 acc = 0; int n = 0;
    for (uint j = 0; j < fr.scale; j++)
        for (uint i = 0; i < fr.scale; i++) {
            uint2 s = q * fr.scale + uint2(i, j);
            if (kMS) { for (uint k = 0; k < fr.samples; k++) { acc += hrms.read(s, k); n++; } }
            else { acc += hr.read(s); n++; }
        }
    float4 a = acc / float(n);
    int3 c5 = clamp(int3(round(a.rgb * 31.0)), int3(0), int3(31));
    vram.write(ushort4(pack555(c5, a.a > 0.5), 0, 0, 0), q);
}

/* Presentación: área de display -> RGBA8 de (w*N, h*N). Resuelve K x K (o las muestras MSAA) por píxel de salida. */
kernel void ps1_k_display(texture2d<float, access::read> hr [[texture(0), function_constant(kSS)]],
                          texture2d_ms<float, access::read> hrms [[texture(1), function_constant(kMS)]],
                          texture2d<ushort, access::read> vram [[texture(2)]], texture2d<float, access::write> out [[texture(3)]],
                          constant PS1Present &pr [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    uint W = uint(pr.w) * pr.scale, H = uint(pr.h) * pr.scale;
    if (gid.x >= W || gid.y >= H) return;
    uint2 px = gid / pr.scale; /* píxel nativo dentro del display */
    if (pr.depth24) { /* 24 bits (FMV): 3 bytes por píxel empaquetados en la fila de VRAM, replicado */
        float3 c;
        for (int k = 0; k < 3; k++) {
            uint byteaddr = (uint(pr.x) * 2 + px.x * 3 + k) & 2047;
            ushort w = vram.read(uint2(byteaddr >> 1, (uint(pr.y) + px.y) & 511)).x;
            c[k] = float((byteaddr & 1) ? (w >> 8) : (w & 255)) * (1.0 / 255.0);
        }
        out.write(float4(c, 1.0), gid);
        return;
    }
    float3 acc = 0; int n = 0;
    uint M = pr.scale * pr.ssaa;
    uint2 base = uint2(((uint(pr.x) + px.x) & 1023) * M + (gid.x % pr.scale) * pr.ssaa,
                       ((uint(pr.y) + px.y) & 511) * M + (gid.y % pr.scale) * pr.ssaa);
    for (uint j = 0; j < pr.ssaa; j++)
        for (uint i = 0; i < pr.ssaa; i++) {
            uint2 s = base + uint2(i, j);
            if (kMS) { for (uint k = 0; k < pr.samples; k++) { acc += hrms.read(s, k).rgb; n++; } }
            else { acc += hr.read(s).rgb; n++; }
        }
    out.write(float4(acc / float(n), 1.0), gid);
}
