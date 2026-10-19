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
