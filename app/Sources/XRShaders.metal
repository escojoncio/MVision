#include <metal_stdlib>
using namespace metal;

/* Pases propios del bucle inmersivo (no son de la escena):
 *  1) resolución de profundidad MSAA -> textura de profundidad del drawable (el compositor la usa para
 *     la reproyección y no admite multimuestra); se toma la muestra más cercana (Z invertida: el máximo);
 *  2) panel 2D (HUD) como quad texturizado en el mundo.
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

/* ---- Panel 2D ---- */
struct XRHudUniforms {
    float4x4 mvp[2];     /* proyección * vista * modelo del panel, por vista */
    uint     baseView;   /* índice de vista de la 1ª instancia (dedicated: la vista de la pasada) */
    float    opacity;
    float2   pad;
};

struct XRHudVOut {
    float4 pos [[position]];
    float2 uv;
};
struct XRHudVOutL {
    float4 pos [[position]];
    float2 uv;
    uint layer [[render_target_array_index]];
};

/* Quad unidad centrado en el origen del modelo (x,y en [-0.5,0.5]); el tamaño va en la matriz modelo. */
static void xr_hud_corner(uint vid, thread float2 &p, thread float2 &uv)
{
    p = float2((vid & 1) ? 0.5 : -0.5, (vid & 2) ? 0.5 : -0.5);
    uv = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 0.0 : 1.0);
}

vertex XRHudVOut xr_hud_vertex(uint vid [[vertex_id]], constant XRHudUniforms &u [[buffer(0)]])
{
    float2 p, uv;
    xr_hud_corner(vid, p, uv);
    XRHudVOut o;
    o.pos = u.mvp[u.baseView & 1] * float4(p, 0.0, 1.0);
    o.uv = uv;
    return o;
}

vertex XRHudVOutL xr_hud_vertex_layered(uint vid [[vertex_id]], uint iid [[instance_id]],
                                        constant XRHudUniforms &u [[buffer(0)]])
{
    float2 p, uv;
    xr_hud_corner(vid, p, uv);
    uint v = (u.baseView + iid) & 1;
    XRHudVOutL o;
    o.pos = u.mvp[v] * float4(p, 0.0, 1.0);
    o.uv = uv;
    o.layer = iid;
    return o;
}

fragment float4 xr_hud_fragment(XRHudVOut in [[stage_in]], constant XRHudUniforms &u [[buffer(0)]],
                                texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = tex.sample(smp, in.uv);
    if (c.a * u.opacity < 0.02) discard_fragment();            /* no escribir profundidad en lo transparente */
    return float4(c.rgb * c.a * u.opacity, c.a * u.opacity);   /* premultiplicado: el compositor lo espera así */
}

fragment float4 xr_hud_fragment_layered(XRHudVOutL in [[stage_in]], constant XRHudUniforms &u [[buffer(0)]],
                                        texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = tex.sample(smp, in.uv);
    if (c.a * u.opacity < 0.02) discard_fragment();
    return float4(c.rgb * c.a * u.opacity, c.a * u.opacity);
}
