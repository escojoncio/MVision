/* Shaders del arma en mano (geometría 3D propia, fuera de la captura de GPU del juego). SIN COMPILAR en Metal (no hay
 * toolchain Apple en Linux): se valida la sintaxis como C++ con cabeceras simuladas (make -C vr/hands msl). Lo usa
 * app/Sources/RenderWeapon.swift dentro del MISMO pase de escena que PS1Renderer (mismas dimensiones de color/profundidad,
 * Z invertida con compare greaterEqual, MSAA, amplificación de vértices si layered).
 *
 * Vértice (36 B, == VrHandsVertex de vr/hands/vr_hands_frame.h): pos(3) nrm(3) uv(2) rgba(4, 128 = neutro como en la PS1:
 * el modulado es textura * rgba/128, así que 255 da ~2x, el rango normal de la consola). Los modelos originales traen
 * texturas oscuras y rgba = 255.
 * Iluminación: luz direccional fija en el mundo, lambert suave (media-lambert al cuadrado: sin zonas negras) + ambiente.
 * Sin brillos, sombras ni efectos modernos. Doble cara: la normal se invierte si el fragmento se ve "desde dentro" (no
 * depende del sentido de los triángulos, que varía entre modelos). Nearest por defecto; VR_WPN_FC_FILTER = bilineal.
 * Alfa-test: texels con alfa < 0,5 se descartan (el atlas puede traer negro puro transparente con key_black). */
#include <metal_stdlib>
using namespace metal;

#define VR_WPN_FC_FILTER 0      /* bool: muestreo bilineal del atlas */
#define VR_WPN_FC_LINEAR_OUT 1  /* bool: el destino es lineal/sRGB: convertir gamma -> lineal al escribir */

#define VR_WPN_BUF_VERTS 0
#define VR_WPN_BUF_FRAME 1      /* VrWpnFrame (constant) */
#define VR_WPN_BUF_DRAW 2       /* VrWpnDraw (constant), por malla */

/* Mismo layout en Swift (RenderWeapon.swift: WpnFrameU / WpnDrawU). */
struct VrWpnFrame {
    float4x4 view_proj[2];      /* clip = P * V por ojo (índice = amplification_id; sin amplificación se usa [0]) */
    float4 eye[2];              /* posición del ojo en mundo (xyz) */
    float4 light;               /* xyz = dirección HACIA la luz (unitaria), w = ambiente */
    float4 params;              /* x = difuso, y = alfa de corte, zw = reservado */
};
struct VrWpnDraw {
    float4x4 model;             /* local -> mundo VR; incluye escala uniforme */
};
struct VrWpnVtx {               /* 36 bytes */
    packed_float3 pos;
    packed_float3 nrm;
    packed_float2 uv;
    packed_uchar4 rgba;
};

constant bool wpnFcFilter [[function_constant(VR_WPN_FC_FILTER)]];
constant bool wpnFcLinear [[function_constant(VR_WPN_FC_LINEAR_OUT)]];
constant bool kWpnFilter = is_function_constant_defined(wpnFcFilter) ? wpnFcFilter : false;
constant bool kWpnLinear = is_function_constant_defined(wpnFcLinear) ? wpnFcLinear : false;

struct VrWpnVOut {
    float4 pos [[position]];
    float2 uv;
    float3 col;                 /* multiplicador de vértice (1.0 = 128) */
    float3 nrm;                 /* normal en mundo (sin normalizar: se normaliza por fragmento) */
    float3 to_eye;              /* del fragmento al ojo, en mundo */
};

inline VrWpnVOut vr_wpn_pack(const device VrWpnVtx &v, constant VrWpnFrame &f, constant VrWpnDraw &d, uint view) {
    float4 wp = d.model * float4(float3(v.pos), 1.0);
    float3 n = (d.model * float4(float3(v.nrm), 0.0)).xyz;
    VrWpnVOut o;
    o.pos = f.view_proj[view] * wp;
    o.uv = float2(v.uv);
    uchar4 c = uchar4(v.rgba);
    o.col = float3(float(c.x), float(c.y), float(c.z)) * (1.0 / 128.0);
    o.nrm = n;
    o.to_eye = f.eye[view].xyz - wp.xyz;
    return o;
}

/* Sin amplificación (un pase por ojo): view_proj[0]/eye[0]. */
vertex VrWpnVOut vr_vs_weapon(uint vid [[vertex_id]], const device VrWpnVtx *verts [[buffer(VR_WPN_BUF_VERTS)]],
                              constant VrWpnFrame &f [[buffer(VR_WPN_BUF_FRAME)]],
                              constant VrWpnDraw &d [[buffer(VR_WPN_BUF_DRAW)]]) {
    return vr_wpn_pack(verts[vid], f, d, 0u);
}

/* Con amplificación de vértices (layered): el mapeo de vistas del encoder suma el índice de capa y de viewport. */
vertex VrWpnVOut vr_vs_weapon_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                                  const device VrWpnVtx *verts [[buffer(VR_WPN_BUF_VERTS)]],
                                  constant VrWpnFrame &f [[buffer(VR_WPN_BUF_FRAME)]],
                                  constant VrWpnDraw &d [[buffer(VR_WPN_BUF_DRAW)]]) {
    return vr_wpn_pack(verts[vid], f, d, uint(amp));
}

inline float vr_wpn_lin1(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

fragment float4 vr_fs_weapon(VrWpnVOut in [[stage_in]], constant VrWpnFrame &f [[buffer(VR_WPN_BUF_FRAME)]],
                             texture2d<float> atlas [[texture(0)]]) {
    constexpr sampler sNear(filter::nearest, address::clamp_to_edge);
    constexpr sampler sLin(filter::linear, address::clamp_to_edge);
    float4 t = kWpnFilter ? atlas.sample(sLin, in.uv) : atlas.sample(sNear, in.uv);
    if (t.w < f.params.y) discard_fragment();
    /* normal nula (vértice sin normal, o cortes que interpolan normales opuestas): normalize(0) daría NaN -> negro/ruido */
    float3 n = dot(in.nrm, in.nrm) > 1e-12 ? normalize(in.nrm) : normalize(in.to_eye);
    if (dot(n, in.to_eye) < 0.0) n = -n;                  /* cara vista desde dentro */
    float h = dot(n, f.light.xyz) * 0.5 + 0.5;             /* media-lambert: sin negro */
    float lit = f.light.w + f.params.x * h * h;
    float3 rgb = clamp(t.xyz * in.col * lit, float3(0.0), float3(1.0));
    if (kWpnLinear) rgb = float3(vr_wpn_lin1(rgb.x), vr_wpn_lin1(rgb.y), vr_wpn_lin1(rgb.z));
    return float4(rgb, 1.0);
}

/* ---- Vistas especiales del arma (mira óptica, visor antiaéreo, pantalla del misil): un rectángulo texturizado anclado al arma o a la cabeza ----
 * Mismo pase, mismo estado de profundidad que el arma pero con test siempre y mezcla alfa (iris de la mira, panel transparente del visor).
 * Rectángulo local: x, y en [-0,5; 0,5], cara visible +Z; uv = (x + 0,5; 0,5 - y). Mismo layout en Swift (RenderWeapon.swift: SpcDrawU, 128 bytes). */
#define VR_SPC_BUF_DRAW 2
#define VR_SPC_MODE_SCOPE 0.0
#define VR_SPC_MODE_HUD 1.0
#define VR_SPC_MODE_MISSILE 2.0

struct VrSpcDraw {
    float4x4 model;             /* local -> mundo VR; incluye el tamaño del rectángulo */
    float4 uv;                  /* x, y, ancho, alto del subrectángulo de la textura 0 (HUD); el resto de modos usan (0, 0, 1, 1) */
    float4 p0;                  /* x = modo, y = opacidad, z = ruido, w = tiempo (s, módulo 64) */
    float4 p1;                  /* x = disco, y = fijado, z = terminado, w = proporción ancho/alto */
    float4 p2;                  /* x = la imagen de la textura 0 ya es lineal (se lleva a gamma antes de mezclar) */
};
struct VrSpcOut {
    float4 pos [[position]];
    float2 q;                   /* 0..1 sobre el rectángulo, v hacia abajo */
};

inline VrSpcOut vr_spc_pack(uint vid, constant VrWpnFrame &f, constant VrSpcDraw &d, uint view) {
    const float2 c[6] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.5, 0.5), float2(-0.5, -0.5), float2(0.5, 0.5), float2(-0.5, 0.5) };
    float2 l = c[vid % 6u];
    VrSpcOut o;
    o.pos = f.view_proj[view] * (d.model * float4(l.x, l.y, 0.0, 1.0));
    o.q = float2(l.x + 0.5, 0.5 - l.y);
    return o;
}
vertex VrSpcOut vr_vs_spc(uint vid [[vertex_id]], constant VrWpnFrame &f [[buffer(VR_WPN_BUF_FRAME)]],
                          constant VrSpcDraw &d [[buffer(VR_SPC_BUF_DRAW)]]) {
    return vr_spc_pack(vid, f, d, 0u);
}
vertex VrSpcOut vr_vs_spc_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]], constant VrWpnFrame &f [[buffer(VR_WPN_BUF_FRAME)]],
                              constant VrSpcDraw &d [[buffer(VR_SPC_BUF_DRAW)]]) {
    return vr_spc_pack(vid, f, d, uint(amp));
}

inline float vr_spc_enc1(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

inline float vr_spc_hash(float2 p, float t) {
    return fract(sin(dot(p + float2(t * 7.13, t * 3.71), float2(12.9898, 78.233))) * 43758.5453);
}

/* texture 0 = imagen (segunda vista renderizada o textura del HUD), texture 1 = abertura y retícula (solo la mira). */
fragment float4 vr_fs_spc(VrSpcOut in [[stage_in]], constant VrSpcDraw &d [[buffer(VR_SPC_BUF_DRAW)]],
                          texture2d<float> img [[texture(0)]], texture2d<float> ovl [[texture(1)]]) {
    constexpr sampler sLin(filter::linear, address::clamp_to_edge);
    float2 q = in.q;
    float2 r = q * 2.0 - 1.0;
    if (d.p1.x > 0.5 && dot(r, r) > 1.0) discard_fragment();            /* disco */
    float mode = d.p0.x;
    float3 rgb;
    float a = d.p0.y;
    if (mode < 0.5) {                                                    /* mira: imagen aumentada + abertura y retícula */
        rgb = img.sample(sLin, q).xyz;
        if (d.p2.x > 0.5) rgb = float3(vr_spc_enc1(rgb.x), vr_spc_enc1(rgb.y), vr_spc_enc1(rgb.z));
        float4 o = ovl.sample(sLin, q);
        rgb = mix(rgb, o.xyz, o.w);
    } else if (mode < 1.5) {                                             /* visor antiaéreo: el HUD del juego, sin premultiplicar */
        float2 u = float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w);
        if (u.x < 0.0 || u.y < 0.0 || u.x > 1.0 || u.y > 1.0) discard_fragment();
        float4 h = img.sample(sLin, u);
        rgb = float3(h.x, h.y, h.z);
        a *= h.w;
        if (a < 0.02) discard_fragment();
    } else {                                                             /* pantalla del misil: monocroma, estática, líneas y cruz */
        float2 px = q * float2(256.0 * d.p1.w, 256.0);
        float3 s = img.sample(sLin, q).xyz;
        if (d.p2.x > 0.5) s = float3(vr_spc_enc1(s.x), vr_spc_enc1(s.y), vr_spc_enc1(s.z));
        float lum = dot(s, float3(0.299, 0.587, 0.114));
        float n = vr_spc_hash(floor(px), d.p0.w);
        float noise = clamp(d.p0.z, 0.0, 1.0);
        lum = mix(lum, n, noise);
        if (d.p1.z > 0.5) lum = n * 0.8;                                 /* vuelo terminado: solo estática */
        lum *= 0.88 + 0.12 * sin(px.y * 3.14159265 * 1.0);                /* líneas de exploración */
        float v = 1.0 - 0.45 * dot(r, r);                                /* viñeta */
        rgb = float3(0.55, 1.0, 0.62) * lum * v;
        if (d.p1.z < 0.5) {
            float2 c = abs(px - float2(128.0 * d.p1.w, 128.0));
            float xh = (c.x < 0.6 && c.y > 6.0 && c.y < 22.0) || (c.y < 0.6 && c.x > 6.0 && c.x < 22.0) ? 1.0 : 0.0;
            rgb = mix(rgb, float3(0.7, 1.0, 0.7), xh * 0.9);
        }
        float e = max(abs(r.x), abs(r.y));
        rgb *= e > 0.97 ? 0.15 : 1.0;                                    /* marco */
    }
    if (kWpnLinear) rgb = float3(vr_wpn_lin1(rgb.x), vr_wpn_lin1(rgb.y), vr_wpn_lin1(rgb.z));
    return float4(rgb, clamp(a, 0.0, 1.0));
}
