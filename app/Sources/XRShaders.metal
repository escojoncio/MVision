#include <metal_stdlib>
using namespace metal;

/* Pases propios del bucle inmersivo (no son de la escena):
 *  1) resolución de profundidad MSAA -> textura de profundidad del drawable (el compositor la usa para
 *     la reproyección y no admite multimuestra); se toma la muestra más cercana (Z invertida: el máximo);
 *  2) quads texturizados en el mundo (xr_q_*): pantalla de cine (tira curvable, muestreo nítido), paneles del HUD (recortes del
 *     panel 2D) y panel del menú VR; más el velo negro de las transiciones (xr_veil_*).
 * Capas: con layout .layered se dibuja con instancias (instance_id = vista) y render_target_array_index;
 * con .dedicated una pasada por vista, sin índice de capa. */

struct XRVOut {
    float4 pos [[position]];
};
struct XRVOutL {
    float4 pos [[position]];
    uint layer [[render_target_array_index]];
};

static float4 xr_fs_tri(uint vid)
{
    float2 p = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

vertex XRVOut xr_fs_vertex(uint vid [[vertex_id]])
{
    XRVOut o;
    o.pos = xr_fs_tri(vid);
    return o;
}

vertex XRVOutL xr_fs_vertex_layered(uint vid [[vertex_id]], uint iid [[instance_id]])
{
    XRVOutL o;
    o.pos = xr_fs_tri(vid);
    o.layer = iid;
    return o;
}

struct XRDepthOut {
    float depth [[depth(any)]];
};

fragment XRDepthOut xr_depth_resolve(XRVOut in [[stage_in]],
                                     texture2d_ms<float, access::read> src [[texture(0)]])
{
    uint2 p = uint2(in.pos.xy);
    uint n = src.get_num_samples();
    float d = 0.0;
    for (uint s = 0; s < n; ++s) d = max(d, src.read(p, s).r);
    XRDepthOut o;
    o.depth = d;
    return o;
}

fragment XRDepthOut xr_depth_resolve_layered(XRVOutL in [[stage_in]],
                                             texture2d_ms_array<float, access::read> src [[texture(0)]])
{
    uint2 p = uint2(in.pos.xy);
    uint n = src.get_num_samples();
    float d = 0.0;
    for (uint s = 0; s < n; ++s) d = max(d, src.read(p, in.layer, s).r);
    XRDepthOut o;
    o.depth = d;
    return o;
}

/* ---- Quads en el mundo: pantalla de cine, paneles del HUD, menú ----
 * Un solo vertex shader: tira de 2*(cols+1) vértices (cols = 1 para un panel plano). El modelo (rígido, en metros) lo pone la matriz mvp;
 * el tamaño y la curvatura van en `geom`. Curvatura: cilindro de radio D (el usuario en su eje) mezclado con el plano (cv 0..1). */
struct XRQuadU {
    float4x4 mvp[2];     /* proyección * vista * modelo, por vista */
    float4   uvrect;     /* u0, v0 (arriba), u1, v1 (abajo) */
    float4   geom;       /* x ancho (m), y alto (m), z distancia D (m, radio de la curvatura), w curvatura 0..1 */
    float4   tex;        /* x, y = tamaño de la imagen en texels (pantalla); z = nitidez 0 suave / 1 nítida / 2 píxel; w = opacidad */
    uint4    cfg;        /* x = índice de vista de la 1ª instancia, y = columnas, z = bit0 destino lineal / bit1 valores guardados lineales (pantalla 3D) */
    float4   fxTint;     /* pantalla 3D: tinte por canal (xyz) en espacio gamma */
    float4   fxCov[4];   /* pantalla 3D: recubrimientos (xyz = color, w = modo de mezcla original 0..3 o -1 = sin usar), en orden */
};

struct XRQVOut {
    float4 pos [[position]];
    float2 uv;
};
struct XRQVOutL {
    float4 pos [[position]];
    float2 uv;
    uint layer [[render_target_array_index]];
};

static void xr_quad_vertex(uint vid, uint view, constant XRQuadU &u, thread float4 &pos, thread float2 &uv)
{
    uint cols = max(u.cfg.y, 1u);
    uint col = min(vid >> 1, cols);
    bool top = (vid & 1u) == 0u;
    float uu = float(col) / float(cols);
    float W = u.geom.x, H = u.geom.y, D = max(u.geom.z, 0.01), cv = saturate(u.geom.w);
    float s = (uu - 0.5) * W;
    float a = s / D;
    float x = mix(s, D * sin(a), cv);
    float z = mix(0.0, D * (1.0 - cos(a)), cv);     /* +z = hacia el usuario */
    float y = top ? 0.5 * H : -0.5 * H;
    pos = u.mvp[view & 1u] * float4(x, y, z, 1.0);
    uv = float2(mix(u.uvrect.x, u.uvrect.z, uu), top ? u.uvrect.y : u.uvrect.w);
}

vertex XRQVOut xr_q_vertex(uint vid [[vertex_id]], constant XRQuadU &u [[buffer(0)]])
{
    XRQVOut o;
    xr_quad_vertex(vid, u.cfg.x, u, o.pos, o.uv);
    return o;
}

vertex XRQVOutL xr_q_vertex_layered(uint vid [[vertex_id]], uint iid [[instance_id]], constant XRQuadU &u [[buffer(0)]])
{
    XRQVOutL o;
    xr_quad_vertex(vid, u.cfg.x + iid, u, o.pos, o.uv);
    o.layer = iid;
    return o;
}

/* Muestreo de la imagen del juego. Modo 0: bilineal. Modo 1 (nítido): cada texel es plano y solo se mezcla con el vecino en una banda de
 * un píxel de pantalla alrededor de su borde (bilineal "de píxel fijo"); con minificación vuelve al bilineal. Modo 2: vecino más próximo. */
static float4 xr_screen_color(float2 uv, constant XRQuadU &u, texture2d<float> tex, sampler smp)
{
    float2 img = max(u.tex.xy, float2(1.0));
    float2 full = float2(tex.get_width(), tex.get_height());
    float2 t = uv * img;
    int mode = int(u.tex.z + 0.5);
    float2 p = t;
    if (mode >= 1) {
        float2 fw = fwidth(t);
        float2 n = floor(t);
        float2 f = t - n;
        float2 fe = (mode == 2) ? float2(1e-4) : max(fw, float2(1e-4));
        float2 a = clamp(0.5 - f / fe, 0.0, 0.5);
        float2 b = clamp(0.5 - (1.0 - f) / fe, 0.0, 0.5);
        float2 ps = n + 0.5 + b - a;
        float m = max(fw.x, fw.y);
        p = (mode == 2) ? ps : mix(ps, t, saturate(m - 1.0));
    }
    p = clamp(p, float2(0.5), max(img - 0.5, float2(0.5)));
    return float4(tex.sample(smp, p / full).rgb, 1.0);
}

static float4 xr_panel_color(float2 uv, constant XRQuadU &u, texture2d<float> tex, sampler smp)
{
    float4 c = tex.sample(smp, uv);
    float al = c.a * u.tex.w;
    if (al < 0.02) discard_fragment();                          /* no escribir profundidad en lo transparente */
    return float4(c.rgb * al, al);                              /* premultiplicado: el compositor lo espera así */
}

fragment float4 xr_screen_fragment(XRQVOut in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                   texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = xr_screen_color(in.uv, u, tex, smp);
    return float4(c.rgb * u.tex.w, u.tex.w);
}

fragment float4 xr_screen_fragment_layered(XRQVOutL in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                           texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = xr_screen_color(in.uv, u, tex, smp);
    return float4(c.rgb * u.tex.w, u.tex.w);
}

/* Pantalla 3D: el array de 2 capas (ojo izquierdo/derecho) lo dibuja RGRenderer con las cámaras del juego; cada ojo del usuario ve SU capa. Tinte y recubrimientos de pantalla
 * de la escena (fundidos, destellos, filtro de color del juego) en espacio gamma como el post-proceso de los ojos (vr_post_blend de vr_scene.metal). */
static float xr_s2l(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static float xr_l2s(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

static float4 xr_screen3d_color(float2 uv, constant XRQuadU &u, texture2d_array<float> tex, uint layer, sampler smp)
{
    float3 c = tex.sample(smp, uv, min(layer, 1u)).rgb;
    if ((u.cfg.z & 2u) != 0u) c = float3(xr_l2s(c.x), xr_l2s(c.y), xr_l2s(c.z));
    c = clamp(c * u.fxTint.xyz, 0.0, 1.0);
    for (int i = 0; i < 4; i++) {
        float m = u.fxCov[i].w;
        if (m < 0.0) break;
        float3 f = u.fxCov[i].xyz;
        int mi = int(m + 0.5);
        if (mi == 0) c = 0.5 * c + 0.5 * f;
        else if (mi == 1) c = c + f;
        else if (mi == 2) c = c - f;
        else c = c + 0.25 * f;
        c = clamp(c, 0.0, 1.0);
    }
    if ((u.cfg.z & 1u) != 0u) c = float3(xr_s2l(c.x), xr_s2l(c.y), xr_s2l(c.z));
    return float4(c, 1.0);
}

fragment float4 xr_screen3d_fragment(XRQVOut in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                     texture2d_array<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = xr_screen3d_color(in.uv, u, tex, u.cfg.x, smp);
    return float4(c.rgb * u.tex.w, u.tex.w);
}

fragment float4 xr_screen3d_fragment_layered(XRQVOutL in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                             texture2d_array<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = xr_screen3d_color(in.uv, u, tex, in.layer, smp);
    return float4(c.rgb * u.tex.w, u.tex.w);
}

fragment float4 xr_panel_fragment(XRQVOut in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                  texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    return xr_panel_color(in.uv, u, tex, smp);
}

fragment float4 xr_panel_fragment_layered(XRQVOutL in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                          texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    return xr_panel_color(in.uv, u, tex, smp);
}

/* Velo negro a pantalla completa (transiciones mundo <-> cine): c.x = opacidad. Va con el triángulo de xr_fs_vertex. */
fragment float4 xr_veil_fragment(XRVOut in [[stage_in]], constant float4 &c [[buffer(0)]])
{
    return float4(0.0, 0.0, 0.0, saturate(c.x));
}

fragment float4 xr_veil_fragment_layered(XRVOutL in [[stage_in]], constant float4 &c [[buffer(0)]])
{
    return float4(0.0, 0.0, 0.0, saturate(c.x));
}

/* Viñeta de confort: oscurece la periferia (negro con alfa) según el ángulo al eje óptico de cada ojo. p.x = intensidad 0..1, p.y = vista base (dedicated);
 * e0/e1 = por ojo (centro óptico x, y en píxeles del destino; tangentes por píxel x, y). Va con el triángulo de xr_fs_vertex. */
struct XRVigU {
    float4 p;
    float4 e0;
    float4 e1;
};

static float4 xr_vig_color(float2 px, uint eye, constant XRVigU &u)
{
    float4 e = eye == 0 ? u.e0 : u.e1;
    float2 t = (px - e.xy) * e.zw;
    float a = atan(length(t));                 // ángulo al eje (rad)
    float s = saturate(u.p.x);
    float inner = mix(1.00, 0.45, s);          // desde dónde oscurece: ~57 grados (casi nada) .. ~26 grados (fuerte)
    float alpha = smoothstep(inner, inner + 0.50, a) * (0.55 + 0.45 * s);
    return float4(0.0, 0.0, 0.0, saturate(alpha));
}

fragment float4 xr_vignette_fragment(XRVOut in [[stage_in]], constant XRVigU &u [[buffer(0)]])
{
    return xr_vig_color(in.pos.xy, uint(u.p.y), u);
}

fragment float4 xr_vignette_fragment_layered(XRVOutL in [[stage_in]], constant XRVigU &u [[buffer(0)]])
{
    return xr_vig_color(in.pos.xy, in.layer, u);
}
