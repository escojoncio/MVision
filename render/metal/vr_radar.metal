/* Quad del radar de muñeca (dispositivo en el antebrazo izquierdo). SIN COMPILAR (escrito sin toolchain Metal): revisar en la
 * primera compilación. Lo dibuja app/Sources/RadarPanel.swift dentro del pase de escena (MSAA, Z invertida con greaterEqual,
 * amplificación de vértices si layered). La textura del radar (rgba8, valores del original, no premultiplicada) la rellena el propio
 * RadarPanel con las funciones de vr_scene.metal (vr_vs_hud / vr_fs_scene); aquí solo se muestrea.
 *
 * Geometría: un quad de 4 vértices (triangle strip) por llamada, generado en el shader a partir de RadarQuadU; sin búfer de vértices.
 *   pantalla: center + right*sx*half_w + up*sy*half_h                       (sx, sy = ±1)
 *   marco:    center + right*sx*(half_w+bezel) + up*sy*(half_h+bezel) - normal*bezel_back   (detrás de la pantalla: sin z-fighting)
 * Con amplificación el índice de ojo (amplification_id) elige view_proj[amp], igual que vr_vs_scene_amp. */
#include <metal_stdlib>
#include "vr_types.h"
using namespace metal;

constant bool fcRadarLinear [[function_constant(VR_FC_LINEAR_OUT)]];
constant bool kRadarLinear = is_function_constant_defined(fcRadarLinear) ? fcRadarLinear : false;

/* Mismo layout en Swift (RadarQuadU): solo float4, 5 x 16 = 80 B. */
struct RadarQuadU {
    float4 center;   /* xyz centro (m, mundo VR), w = half_w */
    float4 right;    /* xyz eje derecho unitario, w = half_h */
    float4 up;       /* xyz eje arriba unitario, w = bezel (ancho del marco, m) */
    float4 params;   /* x = brillo (1 = normal; > 1 al mirarlo), y = bezel_back (m), z = 0 pantalla / 1 marco, w = opacidad 0..1 (fundido de vrradar_visibility) */
    float4 bezel_color; /* rgb del marco (en el espacio de la textura: gamma) */
};

struct RadarVOut {
    float4 pos [[position]];
    float2 uv;
};

inline RadarVOut radar_quad(uint vid, constant RadarQuadU &q, float4x4 vp) {
    float sx = (vid & 1u) ? 1.0 : -1.0;
    float sy = (vid & 2u) ? 1.0 : -1.0;
    float3 right = q.right.xyz, up = q.up.xyz;
    float3 n = cross(right, up);
    float hw = q.center.w, hh = q.right.w;
    float3 p;
    if (q.params.z > 0.5) {
        p = q.center.xyz + right * (sx * (hw + q.up.w)) + up * (sy * (hh + q.up.w)) - n * q.params.y;
    } else {
        p = q.center.xyz + right * (sx * hw) + up * (sy * hh);
    }
    RadarVOut o;
    o.pos = vp * float4(p, 1.0);
    o.uv = float2(sx * 0.5 + 0.5, 0.5 - sy * 0.5);
    return o;
}

vertex RadarVOut vr_vs_radar(uint vid [[vertex_id]], constant VrSceneUniforms &u [[buffer(VR_BUF_UNI)]],
                             constant RadarQuadU &q [[buffer(3)]]) {
    return radar_quad(vid, q, u.view_proj[0]);
}

vertex RadarVOut vr_vs_radar_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                                 constant VrSceneUniforms &u [[buffer(VR_BUF_UNI)]], constant RadarQuadU &q [[buffer(3)]]) {
    return radar_quad(vid, q, u.view_proj[amp]);
}

inline float radar_lin1(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

/* Pantalla: textura del radar x brillo; marco: color liso. La textura la muestrea el sampler pasado (nearest = fiel a los píxeles de la
 * original, linear con mips = más suave de lejos; lo elige RadarPanel.smooth). */
fragment float4 vr_fs_radar(RadarVOut in [[stage_in]], constant RadarQuadU &q [[buffer(3)]],
                            texture2d<float, access::sample> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    float3 rgb;
    if (q.params.z > 0.5) {
        rgb = q.bezel_color.rgb;
    } else {
        rgb = tex.sample(smp, in.uv).rgb * q.params.x;
    }
    rgb = clamp(rgb, float3(0.0), float3(1.0));
    if (kRadarLinear) rgb = float3(radar_lin1(rgb.x), radar_lin1(rgb.y), radar_lin1(rgb.z));
    float a = clamp(q.params.w, 0.0, 1.0);
    return float4(rgb * a, a);   /* premultiplicado: la tubería mezcla con uno / 1 - alfa de origen */
}
