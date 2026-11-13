/* Shaders del arma en mano (geometría 3D propia, fuera de la captura de GPU del juego). SIN COMPILAR en Metal (no hay
 * toolchain Apple en Linux): se valida la sintaxis como C++ con cabeceras simuladas (make -C vr/hands msl). Lo usa
 * app/Sources/RenderWeapon.swift dentro del MISMO pase de escena que RGRenderer (mismas dimensiones de color/profundidad,
 * Z invertida con compare greaterEqual, MSAA, amplificación de vértices si layered).
 *
 * Vértice (36 B, == VrHandsVertex de vr/hands/vr_hands_frame.h): pos(3) nrm(3) uv(2) rgba(4, 128 = neutro como en el original:
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
 * Rectángulo local: x, y en [-0,5; 0,5], cara visible +Z; uv = (x + 0,5; 0,5 - y). Mismo layout en Swift (RenderWeapon.swift: SpcDrawU, 192 bytes). */
#define VR_SPC_BUF_DRAW 2
#define VR_SPC_MODE_SCOPE 0.0
#define VR_SPC_MODE_HUD 1.0
#define VR_SPC_MODE_MISSILE 2.0
#define VR_SPC_MODE_AASCREEN 3.0   /* visor del antiaéreo en su pieza: imagen viva (textura 0) + HUD del juego (textura 1, subrectángulo uv de la vista 4:3 del juego, p0.z = premultiplicado) */

struct VrSpcDraw {
    float4x4 model;             /* local -> mundo VR; incluye el tamaño del rectángulo */
    float4 uv;                  /* x, y, ancho, alto del subrectángulo de la textura 0 (HUD); el resto de modos usan (0, 0, 1, 1) */
    float4 p0;                  /* x = modo, y = opacidad, z = ruido (misil) / la textura del HUD es la capa 2D fiel, de alfa PREMULTIPLICADO (modo 1), w = tiempo (s, módulo 64) */
    float4 p1;                  /* x = disco, y = grosor del marco oscuro de las pantallas LED (modos 2 y 3; fracción de la altura, 0 = sin marco), z = terminado, w = proporción ancho/alto */
    float4 p2;                  /* x = la imagen de la textura 0 ya es lineal (se lleva a gamma antes de mezclar); y, z = prismáticos (modo HUD con y > 0); z en el modo misil = HUD del juego encima (0 no, 1 alfa directo, 2 premultiplicado); y > 0,5 en el modo misil = pantalla LED en espera, ver el fragmento; w = nº de vértices de la máscara poligonal (mira; 0 = sin máscara) */
    float4 poly[4];             /* máscara poligonal convexa de la mira: vértices (u, v) sobre el rectángulo (v hacia abajo), 2 por float4 (xy, zw), hasta 8 */
};
/* Colocación del marco del juego en la textura del HUD (fragmento, buffer 3; Swift: SpcHudU, 48 bytes). Con la capa 2D fiel (HQ) la textura es la ventana visible (PAL 320x268) y el marco 320x256 va desplazado
 * (XR2DWindow.frameUV); con el HUD de reserva la textura es el marco / la vista entera y ambos rectángulos son (0, 0, 1, 1). */
#define VR_SPC_BUF_HUD 3
struct VrSpcHud {
    float4 frame;               /* x, y, ancho, alto (uv de la textura del HUD) del MARCO del juego: las fracciones del marco (iconos del HUD) se pasan a la textura con frame.xy + f * frame.zw */
    float4 view;                /* x, y, ancho, alto (uv de la textura del HUD) de la vista 3D 4:3 (GD_VIEW, 320x240), centrada en el marco: el recorte uv del misil es sobre ella, no sobre la textura entera */
    float4 nik;                 /* HUD propio del Nikita: x = anchura de la barra de combustible en píxeles del marco (< 0 = no se dibuja), yzw = posición del misil (cifras) */
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

/* Iconos del HUD del juego que no van en las pantallas LED del arma (Stinger y misil) (misma partición de la capa que XRPresent.hudRegions): vida / oxígeno (arriba izquierda), objeto equipado (L2, abajo izquierda) y arma equipada
 * con su munición (R2, abajo derecha). ff = fracciones del MARCO del juego (0..1), como hudRegions; la textura del HUD se pasa a ellas con VrSpcHud.frame. El resto (mira, marco de fijación, textos del misil) se conserva. */
/* LISTA BLANCA (antes una lista negra de tres esquinas que dejaba pasar el radar, la alerta, la vida enemiga...): de la capa 2D del juego solo se pinta lo propio del arma, en píxeles del MARCO 320x256 (ff * (320, 256)).
 * Desfase en y de las coordenadas del juego respecto al marco (la vista 240 va centrada en el marco 256): 0-2 px medido en auto06; radar 0. Medido en los volcados (radar x 235-305 y 15-60, alerta «EVASION» x 235-300 y 15-67, vida arriba izquierda y < 44, iconos L2/R2 y 213-245).
 *   Stinger (stnsight.c): escala de cabeceo con sus números a la izquierda (x 14-60, y 44-212), retícula y marco de fijación del misil (centro x 52-296, y 72-212) y «LOCK_ON» (180, 16; rectángulo desde y 16, glifos de 6 px + 0-2 de desfase). Limitaciones aceptadas: las etiquetas
 *   bajas de la escala (y 213-230) y sus líneas fuera de x 14-60 / y 44-212 quedan en el panel (conflicto con el icono L2 y la vida). La barra de tiempos de canal (bg_prim de stnsight.c) cae sobre la barra de vida del jugador
 *   (menu/life.c: filas 17-34 desde x 15) y no es separable: NO se incluye y se acepta que salga en el panel. Las líneas de convergencia borde a borde de sgtrect3.c tampoco son separables y no se tocan.
 *   Nikita (bullet/rmissile.c): solo «ENEMY» (116, 98 + 0-2; rectángulo desde y 96); combustible y cifras de posición las dibuja este sombreador (el juego solo las emite con ▲ mantenido). */
inline bool vr_spc_hud_keep(float2 ff, bool nikita) {
    float2 p = ff * float2(320.0, 256.0);
    if (nikita) return p.x >= 112.0 && p.x <= 176.0 && p.y >= 96.0 && p.y <= 120.0;
    return (p.x >= 14.0 && p.x <= 60.0 && p.y >= 44.0 && p.y <= 212.0) || (p.x >= 52.0 && p.x <= 296.0 && p.y >= 72.0 && p.y <= 212.0) || (p.x >= 176.0 && p.x <= 232.0 && p.y >= 16.0 && p.y <= 36.0);
}

/* Fuente 3x5 (15 bits, fila superior en los bits altos, columna izquierda en el bit alto de cada fila) para las cifras del Nikita */
inline int vr_spc_digit_glyph(int d) {
    switch (d) {
        case 0: return 31599; case 1: return 11415; case 2: return 29671; case 3: return 29647; case 4: return 23497;
        case 5: return 31183; case 6: return 31215; case 7: return 29257; case 8: return 31727; case 9: return 31695;
        default: return 0;
    }
}
/* Entero con signo en píxeles del marco: cada píxel de la fuente mide 2 x 1,5; avance de 8 px por carácter. 1 = pintado */
inline float vr_spc_num(float2 fp, float2 org, int v) {
    float2 t = (fp - org) / float2(2.0, 1.5);
    if (t.x < 0.0 || t.y < 0.0 || t.y >= 5.0) return 0.0;
    int ci = int(floor(t.x / 4.0));
    int cx = int(floor(t.x)) - 4 * ci;
    int cy = int(floor(t.y));
    int n = v < 0 ? -v : v;
    int nd = 1, p10 = 1;
    for (int i = 0; i < 4; i++) { if (n >= p10 * 10) { p10 *= 10; nd++; } }
    int neg = v < 0 ? 1 : 0;
    if (ci >= nd + neg || cx >= 3) return 0.0;
    int g = 448;                                                         /* signo menos */
    if (!(neg == 1 && ci == 0)) {
        int pw = 1;
        for (int i = 0; i < nd - 1 - (ci - neg); i++) pw *= 10;
        g = vr_spc_digit_glyph((n / pw) % 10);
    }
    return ((g >> (3 * (4 - cy) + (2 - cx))) & 1) != 0 ? 1.0 : 0.0;
}
/* Punto de mira: núcleo de radio rad (unidades de media altura útil) con aro oscuro; col = color del núcleo */
inline float3 vr_spc_dot(float3 rgb, float2 r, float aspect_in, float rad, float3 col) {
    float dist = length(float2(r.x * aspect_in, r.y));
    rgb = mix(rgb, float3(0.0, 0.0, 0.0), 0.7 * (1.0 - smoothstep(rad * 1.5, rad * 2.2, dist)));
    return mix(rgb, col, 1.0 - smoothstep(rad * 0.7, rad, dist));
}

inline float vr_spc_enc1(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

inline float vr_spc_hash(float2 p, float t) {
    return fract(sin(dot(p + float2(t * 7.13, t * 3.71), float2(12.9898, 78.233))) * 43758.5453);
}

/* texture 0 = imagen (segunda vista renderizada o textura del HUD), texture 1 = abertura y retícula (solo la mira). */
fragment float4 vr_fs_spc(VrSpcOut in [[stage_in]], constant VrSpcDraw &d [[buffer(VR_SPC_BUF_DRAW)]], constant VrSpcHud &hd [[buffer(VR_SPC_BUF_HUD)]],
                          texture2d<float> img [[texture(0)]], texture2d<float> ovl [[texture(1)]]) {
    constexpr sampler sLin(filter::linear, address::clamp_to_edge);
    float2 q = in.q;
    float2 r = q * 2.0 - 1.0;
    if (d.p1.x > 0.5 && dot(r, r) > 1.0) discard_fragment();            /* disco */
    if (d.p2.w > 2.5) {                                                  /* máscara poligonal convexa (abertura de la mira): se descarta si el punto cae a ambos lados de las aristas */
        int pn = min(int(d.p2.w + 0.5), 8);
        bool pos = false, neg = false;
        for (int i = 0; i < pn; i++) {
            int j = (i + 1 == pn) ? 0 : i + 1;
            float4 pa = d.poly[i >> 1], pb = d.poly[j >> 1];
            float2 a = (i & 1) == 0 ? pa.xy : pa.zw;
            float2 b = (j & 1) == 0 ? pb.xy : pb.zw;
            float cr = (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x);
            if (cr > 1e-6) pos = true;
            if (cr < -1e-6) neg = true;
        }
        if (pos && neg) discard_fragment();
    }
    float mode = d.p0.x;
    float3 rgb;
    float a = d.p0.y;
    if (mode > 1.5 && d.p1.y > 0.0) {                                    /* pantallas LED (misil y visor del antiaéreo): marco fino oscuro; p1.y = grosor como fracción de la altura; la imagen ocupa el interior */
        float2 bw = float2(d.p1.y / max(d.p1.w, 0.01), d.p1.y);
        float2 ee = min(q, 1.0 - q);
        if (ee.x < bw.x || ee.y < bw.y) {
            float3 fc = float3(0.015, 0.016, 0.02);
            if (kWpnLinear) fc = float3(vr_wpn_lin1(fc.x), vr_wpn_lin1(fc.y), vr_wpn_lin1(fc.z));
            return float4(fc, 1.0);
        }
        q = (q - bw) / (1.0 - 2.0 * bw);
        r = q * 2.0 - 1.0;
    }
    if (mode > 2.5) {                                                    /* visor del antiaéreo: imagen viva + mira y marco de fijación del HUD, ya recortado a la máscara poligonal */
        rgb = img.sample(sLin, q).xyz;
        if (d.p2.x > 0.5) rgb = float3(vr_spc_enc1(rgb.x), vr_spc_enc1(rgb.y), vr_spc_enc1(rgb.z));
        float2 hu = hd.view.xy + (float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w)) * hd.view.zw;     /* uv = recorte sobre la vista 4:3 del juego; se lleva a la vista dentro de la textura del HUD (como el misil) */
        float4 h = ovl.sample(sLin, hu);
        float al = h.w;
        float3 hc = float3(h.x, h.y, h.z);
        if (d.p0.z > 0.5) { al = max(h.w, max(h.x, max(h.y, h.z))); hc = hc / max(al, 1e-4); }   /* capa premultiplicada (lo aditivo lleva alfa 0 y color): a alfa directo */
        if (d.p2.z < 0.5 || hu.x < 0.0 || hu.y < 0.0 || hu.x > 1.0 || hu.y > 1.0 || !vr_spc_hud_keep((hu - hd.frame.xy) / hd.frame.zw, false)) al = 0.0;     /* solo el HUD propio del Stinger (lista blanca; hu es de la textura, la lista va en fracciones del marco); p2.z = 0: sin capa del HUD */
        rgb = mix(rgb, hc, clamp(al, 0.0, 1.0));
        float ain = (d.p1.w - 2.0 * d.p1.y) / max(1.0 - 2.0 * d.p1.y, 0.01);     /* proporción ancho/alto del interior (sin marco) */
        rgb = vr_spc_dot(rgb, r, ain, 0.0125 / max(0.5 * (1.0 - 2.0 * d.p1.y), 0.05), float3(1.0, 0.08, 0.05));      /* punto ROJO en el eje de la cámara (centro de la imagen); radio 1,25 % de la altura de la pantalla */
    } else if (mode < 0.5) {                                             /* mira: imagen aumentada + abertura y retícula */
        rgb = img.sample(sLin, q).xyz;
        if (d.p2.x > 0.5) rgb = float3(vr_spc_enc1(rgb.x), vr_spc_enc1(rgb.y), vr_spc_enc1(rgb.z));
        float4 o = ovl.sample(sLin, q);
        rgb = mix(rgb, o.xyz, o.w);
    } else if (mode < 1.5 && d.p2.y > 0.0) {                             /* prismáticos: la capa HUD del juego en vivo, tal cual, en un rectángulo; fuera de él, negro opaco */
        /* El quad es muy grande (cubre todo el campo). hq = posición en el rectángulo del HUD (0..1; p2.y, p2.z = medio lado del quad / ancho y alto del rectángulo). */
        float2 hq = float2(r.x * d.p2.y, r.y * d.p2.z) + 0.5;
        if (hq.x < 0.0 || hq.y < 0.0 || hq.x > 1.0 || hq.y > 1.0) return float4(0.0, 0.0, 0.0, 1.0);
        float4 h = img.sample(sLin, float2(d.uv.x, d.uv.y) + hq * float2(d.uv.z, d.uv.w));
        float al = h.w;
        float3 hc = float3(h.x, h.y, h.z);
        if (d.p0.z > 0.5) { al = max(h.w, max(h.x, max(h.y, h.z))); hc = hc / max(al, 1e-4); }   /* capa premultiplicada (lo aditivo lleva alfa 0 y color): a alfa directo */
        if (kWpnLinear) hc = float3(vr_wpn_lin1(hc.x), vr_wpn_lin1(hc.y), vr_wpn_lin1(hc.z));
        return float4(hc, clamp(al * a, 0.0, 1.0));
    } else if (mode < 1.5) {                                             /* visor antiaéreo: el HUD del juego, sin premultiplicar */
        float2 u = float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w);
        if (u.x < 0.0 || u.y < 0.0 || u.x > 1.0 || u.y > 1.0) discard_fragment();
        float4 h = img.sample(sLin, u);
        float al = h.w;
        rgb = float3(h.x, h.y, h.z);
        if (d.p0.z > 0.5) { al = max(h.w, max(h.x, max(h.y, h.z))); rgb = rgb / max(al, 1e-4); }   /* capa premultiplicada: a alfa directo */
        a *= al;
        if (a < 0.02) discard_fragment();
    } else {                                                             /* pantalla del misil: monocroma, estática, líneas y cruz */
        float2 px = q * float2(256.0 * d.p1.w, 256.0);
        float3 s = img.sample(sLin, float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w)).xyz;    /* uv = recorte de la imagen 4:3 sobre la pantalla (por defecto 0, 0, 1, 1) */
        if (d.p2.x > 0.5) s = float3(vr_spc_enc1(s.x), vr_spc_enc1(s.y), vr_spc_enc1(s.z));
        if (d.p2.y > 0.5) {                                              /* pantalla LED en espera (sin misil): fondo verde oscuro tenue con líneas de exploración y una pequeña cruz */
            rgb = float3(0.02, 0.07, 0.04) * (0.85 + 0.15 * sin(px.y * 3.14159265));
            float2 c = abs(px - float2(128.0 * d.p1.w, 128.0));
            float xh = (c.x < 0.6 && c.y > 6.0 && c.y < 14.0) || (c.y < 0.6 && c.x > 6.0 && c.x < 14.0) ? 1.0 : 0.0;
            rgb = mix(rgb, float3(0.25, 0.7, 0.3), xh * 0.8);
        } else {
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
        if (d.p2.z > 0.5 && d.p1.z < 0.5) {                              /* HUD del juego (textos y marcas del misil, capa 2D fiel) encima de la imagen; p2.z = 1 alfa directo, 2 premultiplicado. El recorte uv es sobre la imagen 4:3 (GD_VIEW): se lleva a la vista 4:3 DENTRO de la textura del HUD (hd.view; con la capa HQ la textura es la ventana 320x268, más alta que la vista 320x240) */
            float2 hu = hd.view.xy + (float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w)) * hd.view.zw;
            float4 h = ovl.sample(sLin, hu);
            float al = h.w;
            float3 hc = float3(h.x, h.y, h.z);
            if (d.p2.z > 1.5) { al = max(h.w, max(h.x, max(h.y, h.z))); hc = hc / max(al, 1e-4); }
            if (hu.x < 0.0 || hu.y < 0.0 || hu.x > 1.0 || hu.y > 1.0 || !vr_spc_hud_keep((hu - hd.frame.xy) / hd.frame.zw, true)) al = 0.0;     /* solo «ENEMY» del juego (lista blanca) */
            rgb = mix(rgb, hc, clamp(al, 0.0, 1.0));
        }
        if (d.p1.z < 0.5) {
            float2 hv = hd.view.xy + (float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w)) * hd.view.zw;
            float2 fp = ((hv - hd.frame.xy) / hd.frame.zw) * float2(320.0, 256.0) - float2(d.uv.x * 320.0, 8.0);     /* píxeles del juego (la vista 240 va centrada en el marco 256), con x desde el borde izquierdo VISIBLE: la pantalla recorta el ancho sobrante de la imagen 4:3 y a x = 8 las cifras quedarían fuera */
            if (hd.nik.x >= 0.0) {          /* HUD propio: barra de combustible (31, 39) 10 px de alto y cifras (8, 136 / 144 / 152), como DrawMissileHud */
                float3 bc = hd.nik.x < 15.0 ? float3(0.83, 0.31, 0.31) : float3(0.55, 0.95, 0.55);
                if (fp.x >= 31.0 && fp.x <= 31.0 + hd.nik.x && fp.y >= 39.0 && fp.y <= 49.0) rgb = mix(rgb, bc, 0.85);
                float tx = vr_spc_num(fp, float2(8.0, 136.0), int(hd.nik.y)) + vr_spc_num(fp, float2(8.0, 144.0), int(hd.nik.z)) + vr_spc_num(fp, float2(8.0, 152.0), int(hd.nik.w));
                rgb = mix(rgb, float3(0.8, 0.9, 0.8), clamp(tx, 0.0, 1.0));
            }
            float ain = (d.p1.w - 2.0 * d.p1.y) / max(1.0 - 2.0 * d.p1.y, 0.01);
            rgb = vr_spc_dot(rgb, r, ain, 0.0125 / max(0.5 * (1.0 - 2.0 * d.p1.y), 0.05), float3(0.2, 1.0, 0.25));      /* punto VERDE brillante en el eje del misil (centro de la imagen) */
        }
        float e = max(abs(r.x), abs(r.y));
        rgb *= e > 0.97 ? 0.15 : 1.0;                                    /* marco */
            }
    }
    if (kWpnLinear) rgb = float3(vr_wpn_lin1(rgb.x), vr_wpn_lin1(rgb.y), vr_wpn_lin1(rgb.z));
    return float4(rgb, clamp(a, 0.0, 1.0));
}
