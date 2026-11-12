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

/* Profundidad de relleno: escribe una profundidad constante (c = profundidad ya en el espacio del buffer, Z invertida). Con la prueba "mayor" solo sobrescribe los píxeles
 * que conservan la profundidad de borrado (0) o están más lejos que ella; con "siempre" (diagnóstico) aplana todo el buffer. Sin color. */
fragment XRDepthOut xr_depth_fill(XRVOut in [[stage_in]], constant float &c [[buffer(0)]])
{
    XRDepthOut o;
    o.depth = c;
    return o;
}

fragment XRDepthOut xr_depth_fill_layered(XRVOutL in [[stage_in]], constant float &c [[buffer(0)]])
{
    XRDepthOut o;
    o.depth = c;
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

/* Posición de muestreo «sharp-bilinear» (t en texels): cada texel es plano y solo se mezcla con el vecino en una banda de un píxel de pantalla alrededor de su borde; con minificación (m > 1) vuelve al
 * bilineal. mode 2: vecino más próximo (escala entera nítida). Misma fórmula que xr_screen_color. */
static float2 xr_sharp_pos(float2 t, int mode)
{
    float2 fw = fwidth(t);
    float2 n = floor(t);
    float2 f = t - n;
    float2 fe = (mode == 2) ? float2(1e-4) : max(fw, float2(1e-4));
    float2 a = clamp(0.5 - f / fe, 0.0, 0.5);
    float2 b = clamp(0.5 - (1.0 - f) / fe, 0.0, 0.5);
    float2 ps = n + 0.5 + b - a;
    float m = max(fw.x, fw.y);
    return (mode == 2) ? ps : mix(ps, t, saturate(m - 1.0));
}

static float4 xr_panel_color(float2 uv, constant XRQuadU &u, texture2d<float> tex, sampler smp)
{
    int pmode = int(u.cfg.w);                                   /* 0 bilineal (menú VR, HUD de reserva) / 1 sharp-bilinear (capa 2D con xBR) / 2 vecino más próximo */
    if (pmode >= 1) {
        float2 full = float2(tex.get_width(), tex.get_height());
        float2 p = xr_sharp_pos(uv * full, pmode);
        uv = clamp(p, float2(0.5), max(full - 0.5, float2(0.5))) / full;
    }
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

/* ---- Panel de la capa 2D fiel (vr/scene/vr_layer2d.h, vl2d_pack) ----
 * La capa llega en tres planos, sin sRGB: `tex` = color PREMULTIPLICADO c y alfa = 1 - t (lo que tapa del mundo); `pl` = dos planos apilados en vertical, resta d (mitad de arriba) y tope g (mitad de abajo).
 * Composición del original, por canal y en su espacio gamma:  salida = c + clamp(t * mundo - d, 0, g)  (los 4 modos de semitransparencia de la GPU original en cualquier orden y el texel 0).
 * El mundo es lo que ya hay en el destino: se lee con [[color(0)]] (mezcla programable) y el pase va SIN mezcla fija. Los planos se muestrean a mano (4 texels, misma posición «sharp-bilinear» que
 * xr_panel_color): interpolar valores premultiplicados no deja ribete oscuro en los bordes de textos e iconos. */
struct XRL2D { float3 c; float a; float3 d; float3 g; };

static XRL2D xr_l2d_texel(texture2d<float> tex, texture2d<float> pl, int2 p, int2 sz)
{
    p = clamp(p, int2(0), sz - 1);
    float4 ca = tex.read(uint2(p));
    XRL2D o;
    o.c = ca.rgb; o.a = ca.a;
    o.d = pl.read(uint2(p)).rgb;
    o.g = pl.read(uint2(p.x, p.y + sz.y)).rgb;
    return o;
}

static XRL2D xr_l2d_mix(XRL2D x, XRL2D y, float t)
{
    XRL2D o;
    o.c = mix(x.c, y.c, t); o.a = mix(x.a, y.a, t); o.d = mix(x.d, y.d, t); o.g = mix(x.g, y.g, t);
    return o;
}

static XRL2D xr_l2d_sample(float2 uv, int pmode, texture2d<float> tex, texture2d<float> pl)
{
    int2 sz = int2(tex.get_width(), tex.get_height());
    float2 t = uv * float2(sz);
    float2 p = ((pmode >= 1) ? xr_sharp_pos(t, pmode) : t) - 0.5;
    float2 f = floor(p);
    float2 w = p - f;
    int2 b = int2(f);
    XRL2D top = xr_l2d_mix(xr_l2d_texel(tex, pl, b, sz), xr_l2d_texel(tex, pl, b + int2(1, 0), sz), w.x);
    XRL2D bot = xr_l2d_mix(xr_l2d_texel(tex, pl, b + int2(0, 1), sz), xr_l2d_texel(tex, pl, b + int2(1, 1), sz), w.x);
    return xr_l2d_mix(top, bot, w.y);
}

static float3 xr_s2l3(float3 c) { return float3(xr_s2l(c.x), xr_s2l(c.y), xr_s2l(c.z)); }
static float3 xr_l2s3(float3 c) { return float3(xr_l2s(c.x), xr_l2s(c.y), xr_l2s(c.z)); }

/* dst = color que ya hay en el destino (el mundo). u.cfg.z bit0 = el destino guarda valores lineales (formato sRGB / flotante). */
static float4 xr_panel_l2d(float2 uv, constant XRQuadU &u, texture2d<float> tex, texture2d<float> pl, float4 dst)
{
    XRL2D s = xr_l2d_sample(uv, int(u.cfg.w), tex, pl);
    float op = saturate(u.tex.w);
    if (max(s.a, max(max(s.c.r, s.c.g), max(s.c.b, max(s.d.r, max(s.d.g, s.d.b))))) * op < 0.004) discard_fragment();   /* la capa no toca el píxel: ni color ni profundidad */
    bool lin = (u.cfg.z & 1u) != 0u;
    float3 w = saturate(dst.rgb);
    if (lin) w = xr_l2s3(w);
    float3 o = s.c + clamp((1.0 - s.a) * w - s.d, float3(0.0), s.g);
    o = saturate(mix(w, o, op));
    if (lin) o = xr_s2l3(o);
    float cov = s.a * op;
    return float4(o, cov + dst.a * (1.0 - cov));
}

/* MV_NO_FBFETCH (el banco de macOS, bench/build.sh): sin las funciones que leen el destino; XRFinalPass usa entonces la reserva de abajo. */
#ifndef MV_NO_FBFETCH
fragment float4 xr_panel_l2d_fragment(XRQVOut in [[stage_in]], float4 dst [[color(0)]], constant XRQuadU &u [[buffer(0)]],
                                      texture2d<float> tex [[texture(0)]], texture2d<float> pl [[texture(1)]])
{
    return xr_panel_l2d(in.uv, u, tex, pl, dst);
}

fragment float4 xr_panel_l2d_fragment_layered(XRQVOutL in [[stage_in]], float4 dst [[color(0)]], constant XRQuadU &u [[buffer(0)]],
                                              texture2d<float> tex [[texture(0)]], texture2d<float> pl [[texture(1)]])
{
    return xr_panel_l2d(in.uv, u, tex, pl, dst);
}
#endif

/* Reserva sin lectura del destino (XR2DCompose.framebufferFetch = false, o si el pipeline de arriba no se puede crear): mezcla fija «over» premultiplicada. Lo opaco, lo semitransparente B/2+F/2 y lo
 * aditivo salen bien salvo que la mezcla ocurre en el espacio del destino; la resta y el tope se pierden. */
static float4 xr_panel_l2d_over(float2 uv, constant XRQuadU &u, texture2d<float> tex, texture2d<float> pl)
{
    XRL2D s = xr_l2d_sample(uv, int(u.cfg.w), tex, pl);
    float op = saturate(u.tex.w);
    if (max(s.a, max(s.c.r, max(s.c.g, s.c.b))) * op < 0.004) discard_fragment();
    float3 c = s.c;
    if ((u.cfg.z & 1u) != 0u) c = s.a > 0.004 ? xr_s2l3(saturate(c / s.a)) * s.a : xr_s2l3(c);
    return float4(c * op, s.a * op);
}

fragment float4 xr_panel_l2d_over_fragment(XRQVOut in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                           texture2d<float> tex [[texture(0)]], texture2d<float> pl [[texture(1)]])
{
    return xr_panel_l2d_over(in.uv, u, tex, pl);
}

fragment float4 xr_panel_l2d_over_fragment_layered(XRQVOutL in [[stage_in]], constant XRQuadU &u [[buffer(0)]],
                                                   texture2d<float> tex [[texture(0)]], texture2d<float> pl [[texture(1)]])
{
    return xr_panel_l2d_over(in.uv, u, tex, pl);
}

/* ---- Escalado xBR-lv2 x3 de la capa 2D (una pasada) ----
 * Entrada: la imagen 2D a resolución nativa (RGBA8 de alfa PREMULTIPLICADO, tal como la deja vr_layer2d; el orden de canales es el de la textura y el espacio es el guardado: gamma, la vista sRGB decodifica al muestrear),
 * o los planos de resta y tope de la capa apilados en vertical (dims.z planos de dims.y filas: los vecinos no cruzan de un plano a otro).
 * Salida: 3x3 por píxel, sin interpolar color en espacio lineal: solo se toma el píxel o una mezcla de dos vecinos en el borde de la diagonal.
 * Por cada una de las 4 esquinas del píxel E (rotando el marco 90 grados) se mira el entorno de 5x5 (vecinos B C D F G H I y I4 I5 H5 F4 de la esquina inferior derecha):
 *   edr  = hay un borde en diagonal (suma de distancias ponderadas YUV a un lado < al otro) y E difiere de F y de H;
 *   left / up = el borde es de pendiente 2:1 hacia G (shallow) o hacia C (steep), con las condiciones lv2;
 * y la subposición del píxel de salida decide cuánto del vecino más parecido (F o H) sustituye a E: 45 grados x + y > 1,5; 2:1 y + x/2 > 1 o x + y/2 > 1, con una rampa de 1/3 de píxel (casi sin mezcla).
 * Transparencia: el alfa entra en la distancia y, al ser premultiplicado, la mezcla de un píxel transparente (0,0,0,0) con uno opaco es directa (no oscurece el color).
 * Es una implementación propia de xBR-lv2 con esa lógica de bordes, no una copia byte a byte del shader de Hyllian. */
constant float XBR_EQ = 0.1;

/* sz.xy = primer píxel del plano, sz.zw = último */
static float4 xbr_px(texture2d<float, access::read> src, int2 p, int4 sz)
{
    return src.read(uint2(clamp(p, sz.xy, sz.zw)));
}

static float4 xbr_at(texture2d<float, access::read> src, int2 base, int2 r, int2 d, int a, int b, int4 sz)
{
    return xbr_px(src, base + a * r + b * d, sz);
}

static float xbr_wd(float4 a, float4 b)
{
    float3 d = a.rgb - b.rgb;
    float y = dot(d, float3(0.299, 0.587, 0.114));
    float u = dot(d, float3(-0.169, -0.331, 0.5));
    float v = dot(d, float3(0.5, -0.419, -0.081));
    return 48.0 * abs(y) + 7.0 * abs(u) + 6.0 * abs(v) + 48.0 * abs(a.a - b.a);
}

static bool xbr_eq(float4 a, float4 b) { return xbr_wd(a, b) < XBR_EQ; }

static float4 xbr_mix(float4 a, float4 b, float t) { return mix(a, b, t); }   /* alfa premultiplicado */

kernel void xr_xbr3(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant uint4 &dims [[buffer(0)]],               /* x, y = tamaño de un plano de la imagen nativa (la textura puede ser mayor, como la del cine); z = planos apilados en vertical (1 = imagen normal) */
                    uint2 gid [[thread_position_in_grid]])
{
    uint np = max(dims.z, 1u);
    if (gid.x >= dims.x * 3u || gid.y >= dims.y * np * 3u) return;
    int2 o = int2(gid);
    int2 base = int2(o.x / 3, o.y / 3);
    int y0 = (base.y / int(dims.y)) * int(dims.y);
    int4 sz = int4(0, y0, int(dims.x) - 1, y0 + int(dims.y) - 1);
    float2 q = (float2(o - base * 3) + 0.5) / 3.0;                    /* subposición dentro del píxel, y hacia abajo */
    float4 e = xbr_px(src, base, sz);
    float4 res = e;
    const int2 rr[4] = { int2(1, 0), int2(0, 1), int2(-1, 0), int2(0, -1) };
    const int2 dd[4] = { int2(0, 1), int2(-1, 0), int2(0, -1), int2(1, 0) };
    for (int k = 0; k < 4; k++) {
        int2 r = rr[k], d = dd[k];
        float4 B = xbr_at(src, base, r, d, 0, -1, sz), C = xbr_at(src, base, r, d, 1, -1, sz);
        float4 D = xbr_at(src, base, r, d, -1, 0, sz), F = xbr_at(src, base, r, d, 1, 0, sz);
        float4 G = xbr_at(src, base, r, d, -1, 1, sz), H = xbr_at(src, base, r, d, 0, 1, sz), I = xbr_at(src, base, r, d, 1, 1, sz);
        float4 F4 = xbr_at(src, base, r, d, 2, 0, sz), I4 = xbr_at(src, base, r, d, 2, 1, sz);
        float4 H5 = xbr_at(src, base, r, d, 0, 2, sz), I5 = xbr_at(src, base, r, d, 1, 2, sz);
        bool edr = !xbr_eq(e, F) && !xbr_eq(e, H)
                && (xbr_wd(e, C) + xbr_wd(e, G) + xbr_wd(I, H5) + xbr_wd(I, F4) + 4.0 * xbr_wd(H, F))
                 < (xbr_wd(H, D) + xbr_wd(H, I5) + xbr_wd(F, B) + xbr_wd(F, I4) + 4.0 * xbr_wd(e, I));
        if (edr) {
            float fg = xbr_wd(F, G), hc = xbr_wd(H, C);
            bool left = (2.0 * fg) <= hc && !xbr_eq(e, G) && !xbr_eq(D, G);
            bool up = fg >= 2.0 * hc && !xbr_eq(e, C) && !xbr_eq(B, C);
            float a = smoothstep(-0.1667, 0.1667, q.x + q.y - 1.5);
            if (left) a = max(a, smoothstep(-0.1667, 0.1667, q.y + 0.5 * q.x - 1.0));
            if (up) a = max(a, smoothstep(-0.1667, 0.1667, q.x + 0.5 * q.y - 1.0));
            if (a > 0.0) res = xbr_mix(res, xbr_wd(e, F) <= xbr_wd(e, H) ? F : H, a);
        }
        q = float2(q.y, 1.0 - q.x);                                   /* siguiente esquina: el marco gira 90 grados */
    }
    dst.write(res, gid);
}

/* ---- Escalado MMPX x2 de la capa 2D (una pasada) ----
 * MMPX (M. McGuire y M. Gagiu, «MMPX Style-Preserving Pixel Art Magnification», JCGT 2021): cada píxel de salida es la COPIA de un píxel de la entrada (nunca una mezcla), elegida con las reglas de pendiente 1:1,
 * intersección y pendiente 2:1 sobre el entorno del píxel. Es el mismo algoritmo que vr/scene/mmpx.c (mmpx_scale2x_planes, con pruebas): las posiciones del entorno se agrupan en clases de píxeles idénticos y las
 * reglas comparan clases. Reglas tomadas de la implementación de referencia MMPX.glc de los autores (Copyright 2020 Morgan McGuire & Mara Gagiu, licencia MIT).
 * Entrada: `src` = imagen RGBA8 (color premultiplicado de la capa 2D, o el fotograma opaco del cine); `srcP` = planos de resta y tope de la capa apilados en vertical (solo si dims.z = 2). Dos píxeles son iguales
 * solo si lo son en todos los planos, y la salida copia el mismo píxel de origen en todos (los planos no se desalinean). La luminosidad sale de `src`: (r + g + b + 1) * (256 - alfa). */
static uint mm_read(texture2d<float, access::read> t, int2 p)
{
    uint4 b = uint4(t.read(uint2(p)) * 255.0 + 0.5);
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
static float4 mm_unpack(uint c) { return float4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float(c >> 24)) / 255.0; }
static uint mm_luma(uint c) { return ((c & 0xFFu) + ((c >> 8) & 0xFFu) + ((c >> 16) & 0xFFu) + 1u) * (256u - (c >> 24)); }
static bool mm_eq2(int b, int a0, int a1) { return b == a0 && b == a1; }
static bool mm_eq3(int b, int a0, int a1, int a2) { return b == a0 && b == a1 && b == a2; }
static bool mm_eq4(int b, int a0, int a1, int a2, int a3) { return b == a0 && b == a1 && b == a2 && b == a3; }
static bool mm_any3(int b, int a0, int a1, int a2) { return b == a0 || b == a1 || b == a2; }
static bool mm_none2(int b, int a0, int a1) { return b != a0 && b != a1; }
static bool mm_none4(int b, int a0, int a1, int a2, int a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

kernel void xr_mmpx2(texture2d<float, access::read> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     texture2d<float, access::read> srcP [[texture(2)]],
                     texture2d<float, access::write> dstP [[texture(3)]],
                     constant uint4 &dims [[buffer(0)]],              /* x, y = tamaño de la imagen nativa; z = planos de srcP (0 o 2) */
                     uint2 gid [[thread_position_in_grid]])           /* un hilo por píxel de ORIGEN: lee su entorno una vez y escribe sus 4 píxeles de salida */
{
    if (gid.x >= dims.x || gid.y >= dims.y) return;
    const int2 sz = int2(dims.xy);
    const int np = int(min(dims.z, 2u));
    const int2 base = int2(gid);
    /* entorno: A B C / D E F / G H I y los 16 vecinos a distancia 2 y 3 que consultan las reglas (mismo orden que TAP de mmpx.c) */
    const int2 tap[25] = { int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0), int2(0, 0), int2(1, 0), int2(-1, 1), int2(0, 1), int2(1, 1),
                           int2(0, -2), int2(0, 2), int2(-2, 0), int2(2, 0), int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3),
                           int2(2, -1), int2(-2, -1), int2(2, 1), int2(-2, 1), int2(1, 2), int2(1, -2), int2(-1, 2), int2(-1, -2) };
    int2 at[25];
    uint key[25], k1[25], k2[25];
    int id[25];
    for (int i = 0; i < 25; i++) {
        at[i] = clamp(base + tap[i], int2(0), sz - 1);
        key[i] = mm_read(src, at[i]);
        k1[i] = np > 0 ? mm_read(srcP, at[i]) : 0u;
        k2[i] = np > 1 ? mm_read(srcP, int2(at[i].x, at[i].y + sz.y)) : 0u;
        id[i] = i;
        for (int j = 0; j < i; j++) {
            if (key[j] == key[i] && k1[j] == k1[i] && k2[j] == k2[i]) { id[i] = id[j]; break; }
        }
    }
    const int A = id[0], B = id[1], C = id[2], D = id[3], E = id[4], F = id[5], G = id[6], H = id[7], I = id[8];
    int J = E, K = E, L = E, M = E;                                   /* salida: J K / L M */
    if (!mm_eq4(E, A, B, C, D) || !mm_eq4(E, F, G, H, I)) {
        const int P = id[9], S = id[10], Q = id[11], R = id[12];
        const uint Bl = mm_luma(key[1]), Dl = mm_luma(key[3]), El = mm_luma(key[4]), Fl = mm_luma(key[5]), Hl = mm_luma(key[7]);
        /* pendiente 1:1 */
        if ((D == B && D != H && D != F) && (El >= Dl || E == A) && mm_any3(E, A, C, G) && (El < Dl || A != D || E != P || E != Q)) J = D;
        if ((B == F && B != D && B != H) && (El >= Bl || E == C) && mm_any3(E, A, C, I) && (El < Bl || C != B || E != P || E != R)) K = B;
        if ((H == D && H != F && H != B) && (El >= Hl || E == G) && mm_any3(E, A, G, I) && (El < Hl || G != H || E != S || E != Q)) L = H;
        if ((F == H && F != B && F != D) && (El >= Fl || E == I) && mm_any3(E, C, G, I) && (El < Fl || I != H || E != R || E != S)) M = F;
        /* intersecciones */
        if ((E != F && mm_eq4(E, C, I, D, Q) && mm_eq2(F, B, H)) && F != id[13]) { K = F; M = F; }
        if ((E != D && mm_eq4(E, A, G, F, R) && mm_eq2(D, B, H)) && D != id[14]) { J = D; L = D; }
        if ((E != H && mm_eq4(E, G, I, B, P) && mm_eq2(H, D, F)) && H != id[15]) { L = H; M = H; }
        if ((E != B && mm_eq4(E, A, C, H, S) && mm_eq2(B, D, F)) && B != id[16]) { J = B; K = B; }
        if (Bl < El && mm_eq4(E, G, H, I, S) && mm_none4(E, A, D, C, F)) { J = B; K = B; }
        if (Hl < El && mm_eq4(E, A, B, C, P) && mm_none4(E, D, G, I, F)) { L = H; M = H; }
        if (Fl < El && mm_eq4(E, A, D, G, Q) && mm_none4(E, B, C, I, H)) { K = F; M = F; }
        if (Dl < El && mm_eq4(E, C, F, I, R) && mm_none4(E, B, A, G, H)) { J = D; L = D; }
        /* pendiente 2:1 */
        if (H != B) {
            if (H != A && H != E && H != C) {
                if (mm_eq3(H, G, F, R) && mm_none2(H, D, id[17])) L = M;
                if (mm_eq3(H, I, D, Q) && mm_none2(H, F, id[18])) M = L;
            }
            if (B != I && B != G && B != E) {
                if (mm_eq3(B, A, F, R) && mm_none2(B, D, id[19])) J = K;
                if (mm_eq3(B, C, D, Q) && mm_none2(B, F, id[20])) K = J;
            }
        }
        if (F != D) {
            if (D != I && D != E && D != C) {
                if (mm_eq3(D, A, H, S) && mm_none2(D, B, id[21])) J = L;
                if (mm_eq3(D, G, B, P) && mm_none2(D, H, id[22])) L = J;
            }
            if (F != E && F != A && F != G) {
                if (mm_eq3(F, C, H, S) && mm_none2(F, B, id[23])) K = M;
                if (mm_eq3(F, I, B, P) && mm_none2(F, H, id[24])) M = K;
            }
        }
    }
    const int pick[4] = { J, K, L, M };
    for (uint q = 0u; q < 4u; q++) {
        const uint2 o = gid * 2u + uint2(q & 1u, q >> 1);
        dst.write(mm_unpack(key[pick[q]]), o);
        if (np > 0) dstP.write(mm_unpack(k1[pick[q]]), o);
        if (np > 1) dstP.write(mm_unpack(k2[pick[q]]), uint2(o.x, o.y + dims.y * 2u));
    }
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
