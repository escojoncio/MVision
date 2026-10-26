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

/* ---- Escalado xBR-lv2 x3 de la capa 2D (una pasada) ----
 * Entrada: la imagen 2D a resolución nativa (RGBA8 no premultiplicado, tal como la deja vr_layer2d; el orden de canales es el de la textura y el espacio es el guardado: gamma, la vista sRGB decodifica al muestrear).
 * Salida: 3x3 por píxel, sin interpolar color en espacio lineal: solo se toma el píxel o una mezcla de dos vecinos en el borde de la diagonal.
 * Por cada una de las 4 esquinas del píxel E (rotando el marco 90 grados) se mira el entorno de 5x5 (vecinos B C D F G H I y I4 I5 H5 F4 de la esquina inferior derecha):
 *   edr  = hay un borde en diagonal (suma de distancias ponderadas YUV a un lado < al otro) y E difiere de F y de H;
 *   left / up = el borde es de pendiente 2:1 hacia G (shallow) o hacia C (steep), con las condiciones lv2;
 * y la subposición del píxel de salida decide cuánto del vecino más parecido (F o H) sustituye a E: 45 grados x + y > 1,5; 2:1 y + x/2 > 1 o x + y/2 > 1, con una rampa de 1/3 de píxel (casi sin mezcla).
 * Transparencia: alfa 0 es un color propio (rgb a 0 y alfa en la distancia): xBR nunca mezcla el color de un píxel transparente con uno opaco (en el borde elige uno u otro entero).
 * Es una implementación propia de xBR-lv2 con esa lógica de bordes, no una copia byte a byte del shader de Hyllian. */
constant float XBR_EQ = 0.1;

static float4 xbr_px(texture2d<float, access::read> src, int2 p, int2 sz)
{
    p = clamp(p, int2(0), sz - 1);
    float4 c = src.read(uint2(p));
    return c.a <= 0.0 ? float4(0.0) : c;
}

static float4 xbr_at(texture2d<float, access::read> src, int2 base, int2 r, int2 d, int a, int b, int2 sz)
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

static float4 xbr_mix(float4 a, float4 b, float t)
{
    if ((a.a <= 0.0) != (b.a <= 0.0)) return t >= 0.5 ? b : a;   /* transparente contra opaco: nunca se mezcla el color */
    float aa = mix(a.a, b.a, t);
    float3 c = (a.rgb * a.a * (1.0 - t) + b.rgb * b.a * t) / max(aa, 1e-4);
    return float4(c, aa);
}

kernel void xr_xbr3(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant uint2 &dims [[buffer(0)]],               /* tamaño de la imagen nativa (la textura puede ser mayor, como la del cine) */
                    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= dims.x * 3u || gid.y >= dims.y * 3u) return;
    int2 sz = int2(dims);
    int2 o = int2(gid);
    int2 base = int2(o.x / 3, o.y / 3);
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
