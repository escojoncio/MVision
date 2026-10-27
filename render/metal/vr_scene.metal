/* Shaders de la escena VR (3D estéreo + panel HUD 2D). SIN COMPILAR (escrito sin toolchain Metal): revisar en la
 * primera compilación. Reimplementa el muestreo paletizado de rg_gpu.metal (sin tocar rg_*). Diseño: ver RenderRG.swift.
 *
 * Semitransparencia: el original mezcla en gamma (0,5B+0,5F, B+F, B-F, B+0,25F). Con vr_fs_scene_blend (VR_FC_BLEND 0..3) la mezcla la hace el shader leyendo el destino
 * ([[color(0)]], mezcla programable de la GPU de Apple; el pipeline lleva isBlendingEnabled = false); vr_fs_scene no lee el destino: si el destino es sRGB se pasa a gamma, se mezcla y se vuelve a lineal.
 * El bit 15 del texel decide por fragmento si un polígono texturizado semitransparente mezcla: se hace en dos pasadas del mismo lote con VR_FC_PASS
 * (1 = solo texels con bit15 -> pipeline con mezcla; 2 = solo texels sin bit15 -> pipeline opaco). */
#include <metal_stdlib>
#include "vr_types.h"
#include "vr_blend.h"
using namespace metal;

constant bool fcFilter [[function_constant(VR_FC_FILTER)]];
constant bool fcLinear [[function_constant(VR_FC_LINEAR_OUT)]];
constant uint fcPass [[function_constant(VR_FC_PASS)]];
constant int fcBlend [[function_constant(VR_FC_BLEND)]];
constant bool kFilter = is_function_constant_defined(fcFilter) ? fcFilter : false;
constant bool kLinear = is_function_constant_defined(fcLinear) ? fcLinear : false;
constant uint kPass = is_function_constant_defined(fcPass) ? fcPass : 0u;
constant int kBlend = is_function_constant_defined(fcBlend) ? fcBlend : -1;

struct VrVOut {
    float4 pos [[position]];
    float2 uv;             /* texels 0..255, perspectiva correcta */
    float3 col;            /* multiplicador (1.0 = 128 del original) */
    uint tpage [[flat]];
    uint clut_flags [[flat]]; /* clut | flags << 16 */
    uint tw [[flat]];         /* ventana de textura E2: twmx | twmy<<8 | twox<<16 | twoy<<24 (0 = sin ventana) */
    float2 orig;              /* HUD 2D: posición original interpolada (coordenadas de VRAM como los vértices); 0 en la escena 3D */
    float4 clip [[flat]];     /* recorte vigente de la primitiva (E3/E4) como [x1, y1, x2+1, y2+1) en las mismas coordenadas; sin recorte en la escena 3D */
};

inline VrVOut vr_pack(const device VrGpuVtx &v, const device VrGpuState *states, float4 clip, bool hud = false) {
    VrVOut o;
    o.pos = clip;
    o.orig = float2(0.0);
    o.clip = float4(-1.0e9, -1.0e9, 1.0e9, 1.0e9);
    o.uv = float2(v.uv[0], v.uv[1]);
    uint c = v.rgba;
    o.col = float3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) * (1.0 / 128.0);
    o.tpage = uint(v.tpage);
    o.clut_flags = uint(v.clut) | (uint(v.flags) << 16);
    o.tw = 0u;
    if (uint(v.pad) != 0xFFFFu) {
        const device VrGpuState &st = states[uint(v.pad)];
        o.tw = uint(st.tw[0]) | (uint(st.tw[1]) << 8) | (uint(st.tw[2]) << 16) | (uint(st.tw[3]) << 24);
        if (hud) o.clip = float4(float(st.clip[0]), float(st.clip[1]), float(st.clip[2]) + 1.0, float(st.clip[3]) + 1.0);
    }
    if (hud) o.orig = float2(v.pos[0], v.pos[1]);
    return o;
}

/* Carteles (VR_VF_BILLBOARD; mismas funciones en C: vr_eye_from_viewproj y vr_billboard_corner de vr/scene/vr_scene.c, con prueba en test_scene.c). Los resplandores de la PS1 son rectángulos de pantalla y salen como
 * cuadriláteros perpendiculares al eje de la cámara del juego: al girar la cabeza se verían de canto. El vértice se descompone en la base (R, D, N) de la cámara del juego respecto del centro C y se recompone con la
 * base girada por la rotación mínima que lleva N al rayo ojo->centro: mismos semiejes en metros, misma distancia al ojo, UV intacto. Sin ojo útil (sobre el centro o rayo opuesto a N) no cambia nada. */
/* Ojo (mundo) de view_proj = P*V: el origen del espacio de vista es la intersección de los planos clip.x = 0, clip.y = 0, clip.w = 0 (filas 0, 1 y 3); no depende de la simetría de la proyección. */
inline float3 vr_eye_pos(float4x4 m, thread bool &ok) {
    float3 r0 = float3(m[0][0], m[1][0], m[2][0]), r1 = float3(m[0][1], m[1][1], m[2][1]), r3 = float3(m[0][3], m[1][3], m[2][3]);
    float3 c13 = cross(r1, r3), c30 = cross(r3, r0), c01 = cross(r0, r1);
    float det = dot(r0, c13);
    ok = abs(det) > 1.0e-9;
    return ok ? -(m[3][0] * c13 + m[3][1] * c30 + m[3][3] * c01) / det : float3(0.0);
}
inline float3 vr_billboard_pos(const device VrGpuState &st, float3 p, float3 eye) {
    float3 C = float3(st.bb[0], st.bb[1], st.bb[2]), R = float3(st.bb[3], st.bb[4], st.bb[5]), D = float3(st.bb[6], st.bb[7], st.bb[8]), N = float3(st.bb[9], st.bb[10], st.bb[11]);
    float3 f = C - eye;
    float len = length(f);
    if (!(len > 1.0e-4)) return p;
    f /= len;
    float c = dot(N, f);
    if (!(c > -0.999)) return p;
    float3 v = cross(N, f);
    float k = 1.0 / (1.0 + c);
    float3 d = p - C;
    float a = dot(d, R), b = dot(d, D), n = dot(d, N);
    float3 R2 = c * R + cross(v, R) + k * dot(v, R) * v;   /* rotación mínima N -> f (Rodrigues) */
    float3 D2 = c * D + cross(v, D) + k * dot(v, D) * v;
    return C + a * R2 + b * D2 + n * f;
}
inline float4 vr_clip_pos(const device VrGpuVtx &v, const device VrGpuState *states, float4x4 vp) {
    float3 p = float3(v.pos[0], v.pos[1], v.pos[2]);
    if ((uint(v.flags) & VR_VF_BILLBOARD) != 0u && uint(v.pad) != 0xFFFFu) {
        bool ok;
        float3 eye = vr_eye_pos(vp, ok);
        if (ok) p = vr_billboard_pos(states[uint(v.pad)], p, eye);
    }
    return vp * float4(p, 1.0);
}

/* Sin amplificación (un pase por ojo): view_proj[0]. */
vertex VrVOut vr_vs_scene(uint vid [[vertex_id]], const device VrGpuVtx *verts [[buffer(VR_BUF_VERTS)]],
                          constant VrSceneUniforms &u [[buffer(VR_BUF_UNI)]],
                          const device VrGpuState *states [[buffer(VR_BUF_STATES)]]) {
    const device VrGpuVtx &v = verts[vid];
    return vr_pack(v, states, vr_clip_pos(v, states, u.view_proj[0]));
}

/* Con amplificación de vértices (layered): el mapeo de vistas del encoder suma el índice de capa y de viewport. */
vertex VrVOut vr_vs_scene_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                              const device VrGpuVtx *verts [[buffer(VR_BUF_VERTS)]],
                              constant VrSceneUniforms &u [[buffer(VR_BUF_UNI)]],
                              const device VrGpuState *states [[buffer(VR_BUF_STATES)]]) {
    const device VrGpuVtx &v = verts[vid];
    return vr_pack(v, states, vr_clip_pos(v, states, u.view_proj[amp]));
}

/* HUD 2D: pos.xy en las coordenadas de dibujo del original -> NDC del panel (rect = origen y tamaño visibles). Sin profundidad. */
vertex VrVOut vr_vs_hud(uint vid [[vertex_id]], const device VrGpuVtx *verts [[buffer(VR_BUF_VERTS)]],
                        constant VrHudUniforms &h [[buffer(VR_BUF_UNI)]],
                        const device VrGpuState *states [[buffer(VR_BUF_STATES)]]) {
    const device VrGpuVtx &v = verts[vid];
    float x = (v.pos[0] - h.rect.x) / h.rect.z * 2.0 - 1.0;
    float y = 1.0 - (v.pos[1] - h.rect.y) / h.rect.w * 2.0;
    return vr_pack(v, states, float4(x, y, 0.0, 1.0), true);
}

/* ---------------- muestreo de VRAM ---------------- */
inline uint vr_texel(texture2d<ushort, access::read> vram, uint tpage, uint clut, uint tw, int u, int v) {
    int tx = int(tpage & 15u) * 64, ty = int((tpage >> 4) & 1u) * 256;
    uint tp = (tpage >> 7) & 3u;
    int clx = int(clut & 63u) * 16, cly = int((clut >> 6) & 511u);
    u &= 255; v &= 255;
    if (tw != 0u) { /* ventana de textura E2, igual que gpu.c texel() */
        int mx = int(tw & 255u), my = int((tw >> 8) & 255u), ox = int((tw >> 16) & 255u), oy = int((tw >> 24) & 255u);
        u = (u & ~(mx * 8)) | ((ox & mx) * 8);
        v = (v & ~(my * 8)) | ((oy & my) * 8);
    }
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

inline float3 vr_expand(uint t) {
    float3 c = float3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u));
    /* 5 -> 8 bits como el oráculo: (v<<3)|(v>>2) */
    return float3(float((uint(c.x) << 3) | (uint(c.x) >> 2)), float((uint(c.y) << 3) | (uint(c.y) >> 2)),
                  float((uint(c.z) << 3) | (uint(c.z) >> 2))) * (1.0 / 255.0);
}

inline float vr_lin1(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
inline float vr_gam1(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

/* Color del fragmento (espacio gamma, 0..1) con el muestreo de VRAM; descarta los fragmentos recortados/transparentes. Cuerpo común de vr_fs_scene y vr_fs_scene_blend. */
inline float3 vr_scene_rgb(VrVOut in, texture2d<ushort, access::read> vram) {
    /* recorte de la primitiva (solo HUD 2D; en 3D el recorte es todo el plano): las ventanas, cuadros de texto y letras con recorte no se salen de su caja */
    if (in.orig.x < in.clip.x || in.orig.y < in.clip.y || in.orig.x >= in.clip.z || in.orig.y >= in.clip.w) discard_fragment();
    uint flags = in.clut_flags >> 16;
    uint clut = in.clut_flags & 0xFFFFu;
    float3 rgb;
    if (flags & VR_VF_TEXTURED) {
        float3 tex;
        float2 uv = in.uv;
        /* rect volteado: el texel del píxel i es u0 - i, pero el centro del píxel interpola a u0 - i - 0,5 (floor daría u0 - i - 1) */
        if (flags & VR_VF_RECT) {
            if (flags & VR_VF_FLIPX) uv.x += 1.0;
            if (flags & VR_VF_FLIPY) uv.y += 1.0;
        }
        uint tn;                       /* texel "más cercano": decide transparencia y bit15 */
        if (kFilter) {
            float2 f = uv - 0.5;
            float2 fl = floor(f);
            float2 fr = f - fl;
            int u0 = int(fl.x), v0 = int(fl.y);
            uint t[4];
            t[0] = vr_texel(vram, in.tpage, clut, in.tw, u0, v0);
            t[1] = vr_texel(vram, in.tpage, clut, in.tw, u0 + 1, v0);
            t[2] = vr_texel(vram, in.tpage, clut, in.tw, u0, v0 + 1);
            t[3] = vr_texel(vram, in.tpage, clut, in.tw, u0 + 1, v0 + 1);
            float w[4];
            w[0] = (1.0 - fr.x) * (1.0 - fr.y);
            w[1] = fr.x * (1.0 - fr.y);
            w[2] = (1.0 - fr.x) * fr.y;
            w[3] = fr.x * fr.y;
            float3 acc = float3(0.0);
            float cov = 0.0;
            for (int i = 0; i < 4; i++) {
                if (t[i] != 0u) { acc += vr_expand(t[i]) * w[i]; cov += w[i]; }
            }
            if (cov < 0.5) discard_fragment();
            tex = acc / cov;
            tn = t[(fr.y >= 0.5 ? 2 : 0) + (fr.x >= 0.5 ? 1 : 0)];
        } else {
            tn = vr_texel(vram, in.tpage, clut, in.tw, int(floor(uv.x)), int(floor(uv.y)));
            if (tn == 0u) discard_fragment();
            tex = vr_expand(tn);
        }
        bool b15 = (tn & 0x8000u) != 0u;
        if (kPass == 1u && !b15) discard_fragment();
        if (kPass == 2u && b15) discard_fragment();
        rgb = (flags & VR_VF_RAW) ? tex : tex * in.col;
    } else {
        if (kPass == 2u) discard_fragment();
        rgb = in.col * (128.0 / 255.0);
    }
    return clamp(rgb, float3(0.0), float3(1.0));
}

fragment float4 vr_fs_scene(VrVOut in [[stage_in]], texture2d<ushort, access::read> vram [[texture(0)]]) {
    float3 rgb = vr_scene_rgb(in, vram);
    if (kLinear) rgb = float3(vr_lin1(rgb.x), vr_lin1(rgb.y), vr_lin1(rgb.z));
    return float4(rgb, 1.0);
}

/* Igual que vr_fs_scene pero mezcla en el shader (modo kBlend 0..3) sobre el destino, que se lee con [[color(0)]] (pipeline sin mezcla de función fija). */
fragment float4 vr_fs_scene_blend(VrVOut in [[stage_in]], texture2d<ushort, access::read> vram [[texture(0)]], float4 dst [[color(0)]]) {
    float3 rgb = vr_scene_rgb(in, vram);
    float3 b = clamp(dst.rgb, float3(0.0), float3(1.0));
    if (kLinear) b = float3(vr_gam1(b.x), vr_gam1(b.y), vr_gam1(b.z));
    float3 m = float3(vr_blend1(kBlend, b.x, rgb.x), vr_blend1(kBlend, b.y, rgb.y), vr_blend1(kBlend, b.z, rgb.z));
    if (kLinear) m = float3(vr_lin1(m.x), vr_lin1(m.y), vr_lin1(m.z));
    return float4(m, kBlend == 0 ? 0.5 + 0.5 * dst.a : dst.a);   /* alfa como la mezcla fija de antes: modo 0 = 0,5·1 + 0,5·Ad; el resto, Ad */
}

/* ---------------- HUD: de alfa premultiplicado (interno, rgba16f) a NO premultiplicado ---------------- */
struct VrFullOut { float4 pos [[position]]; float2 uv; };
vertex VrFullOut vr_vs_full(uint vid [[vertex_id]]) {
    float2 p = float2((vid == 1u) ? 3.0 : -1.0, (vid == 2u) ? 3.0 : -1.0);
    VrFullOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    return o;
}

/* Lo aditivo (alfa 0, rgb > 0) no es representable sobre la escena: se aproxima con alfa = canal máximo. */
fragment float4 vr_fs_unpremul(VrFullOut in [[stage_in]], texture2d<float, access::read> src [[texture(0)]]) {
    uint2 p = uint2(in.pos.xy);
    float4 c = src.read(p);
    float a = max(c.a, max(c.r, max(c.g, c.b)));
    if (a <= 0.0) return float4(0.0);
    return float4(clamp(c.rgb / a, float3(0.0), float3(1.0)), clamp(a, 0.0, 1.0));
}

/* ---------------- post-proceso del filtro de pantalla (efecto submarino) ----------------
 * Entrada: la escena de ese ojo ya RESUELTA (una muestra; el MSAA se resolvió al terminar la escena). Se lee con muestreo bilineal y
 * dirección clamp. Salida: el drawable. No usa profundidad. El tinte y el recubrimiento van en el espacio gamma original (como el resto de
 * la escena): si el destino es lineal (VR_FC_LINEAR_OUT) se convierte a gamma, se opera y se vuelve a lineal. */
struct VrPostOut { float4 pos [[position]]; };
struct VrPostOutL { float4 pos [[position]]; uint layer [[render_target_array_index]]; };

vertex VrPostOut vr_vs_post(uint vid [[vertex_id]]) {
    VrPostOut o;
    o.pos = float4((vid == 1u) ? 3.0 : -1.0, (vid == 2u) ? 3.0 : -1.0, 0.0, 1.0);
    return o;
}

vertex VrPostOutL vr_vs_post_layered(uint vid [[vertex_id]], uint iid [[instance_id]]) {
    VrPostOutL o;
    o.pos = float4((vid == 1u) ? 3.0 : -1.0, (vid == 2u) ? 3.0 : -1.0, 0.0, 1.0);
    o.layer = iid;
    return o;
}

/* Fila fuente (coordenada continua en píxeles) para la fila de salida yc del ojo `eye`. */
inline float vr_post_ysrc(float yc, uint eye, constant VrPostUniforms &u, constant float *dv) {
    float d = 0.0;
    int nrows = int(u.cfg.z);
    if (nrows > 0 && u.eye[eye].y > 1.0 && u.cfg.x > 0.0) {
        float t = (yc - u.eye[eye].x) / u.eye[eye].y;
        float r = u.cfg.y + u.cfg.x * t;                  /* fila equivalente de la imagen original (continua) */
        if (r >= 0.0 && r < float(VR_POST_ROWS)) {
            float rr = r - 0.5;                           /* las filas tienen su centro en i + 0,5 */
            float f = floor(rr);
            int i0 = int(f);
            float fr = rr - f;
            int i1 = min(i0 + 1, VR_POST_ROWS - 1);
            i0 = max(i0, 0);
            d = mix(dv[i0], dv[i1], fr);
        }
        return yc + u.eye[eye].y * d / u.cfg.x;
    }
    return yc;
}

/* Mezcla de un color de recubrimiento/línea (f) sobre c con el modo de mezcla original m (0: B/2+F/2, 1: B+F, 2: B-F, 3: B+F/4) y peso w (1 = completo). */
inline float3 vr_post_blend(float3 c, float3 f, int m, float w) {
    if (m == 0) return mix(c, 0.5 * c + 0.5 * f, w);
    if (m == 1) return c + f * w;
    if (m == 2) return c - f * w;
    return c + 0.25 * f * w;
}

/* Peso (0..1) de las líneas de exploración en la fila de salida yc del ojo `eye` (fila original equivalente r = cy_row + proj_h*t). Con las líneas más finas que
 * ~3 píxeles del ojo se usa la media (alto/periodo) para no producir muaré. Sin líneas: 0. */
inline float vr_post_scan(float yc, uint eye, constant VrPostUniforms &u) {
    if (u.scan.w < 0.0 || u.scan.x < 1.0 || u.eye[eye].y <= 1.0 || u.cfg.x <= 0.0) return 0.0;
    float pitch = u.scan.x;
    float duty = clamp(u.scan.z / pitch, 0.0, 1.0);
    float r = u.cfg.y + u.cfg.x * ((yc - u.eye[eye].x) / u.eye[eye].y);
    float t = fract((r - u.scan.y) / pitch);          /* 0..1 dentro del periodo; la línea cubre [0, duty) */
    float pe = pitch * u.eye[eye].y / u.cfg.x;        /* píxeles del ojo por periodo */
    float e = pitch / max(pe, 1e-3);                  /* filas originales por píxel del ojo */
    float edge = max(e, 1e-4);
    float w = clamp((duty - t) * pitch / edge + 0.5, 0.0, 1.0) * clamp(t * pitch / edge + 0.5, 0.0, 1.0);
    return mix(w, duty, clamp((3.0 - pe) * 0.5, 0.0, 1.0));
}

/* Resta de la máscara de visión en el píxel (px,py) del ojo `eye`: fuera de la imagen original = 1 (negro). */
inline float vr_post_frame(float2 pix, uint eye, constant VrPostUniforms &u, texture2d<float> frm) {
    if (u.frame.z < 0.5 || u.eye[eye].y <= 1.0 || u.eye[eye].w <= 1.0 || u.cfg.x <= 0.0 || u.frame.x <= 0.0 || u.frame.y <= 0.0) return 0.0;
    float k = u.cfg.x;
    float cx = u.cfg.w + k * ((pix.x - u.eye[eye].z) / u.eye[eye].w);
    float cy = u.cfg.y + k * ((pix.y - u.eye[eye].x) / u.eye[eye].y);
    float2 uv = float2(cx / u.frame.x, cy / u.frame.y);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    constexpr sampler fsm(coord::normalized, address::clamp_to_edge, filter::linear);
    return clamp(frm.sample(fsm, uv, level(0)).r, 0.0, 1.0);
}

inline float4 vr_post_finish(float4 px, constant VrPostUniforms &u, float scanw, float framesub) {
    float3 c = clamp(px.rgb, float3(0.0), float3(1.0));
    if (kLinear) c = float3(vr_gam1(c.x), vr_gam1(c.y), vr_gam1(c.z));
    c *= u.tint.xyz;
    bool scanPending = scanw > 0.0;
    int scanAfter = int(u.misc.x + 0.5);
    for (int i = 0; i < VR_POST_COVERS; i++) {
        if (scanPending && i >= scanAfter) {         /* las líneas se dibujaron tras `scanAfter` recubrimientos */
            c = clamp(vr_post_blend(c, u.scan_rgb.xyz, int(u.scan.w + 0.5), scanw * u.scan_rgb.w), float3(0.0), float3(1.0));
            scanPending = false;
        }
        float m = u.covers[i].w;
        if (m < 0.0) break;
        c = vr_post_blend(c, u.covers[i].xyz, int(m + 0.5), 1.0);
        c = clamp(c, float3(0.0), float3(1.0));      /* el original satura tras cada primitiva */
    }
    if (scanPending) c = clamp(vr_post_blend(c, u.scan_rgb.xyz, int(u.scan.w + 0.5), scanw * u.scan_rgb.w), float3(0.0), float3(1.0));
    if (framesub > 0.0) {
        c = max(c - float3(framesub), float3(0.0));
        c += u.misc.yzw * smoothstep(0.85, 1.0, framesub);   /* interior cerrado (caja: cartón oscuro; resto: 0 = negro) */
    }
    c = clamp(c, float3(0.0), float3(1.0));
    if (kLinear) c = float3(vr_lin1(c.x), vr_lin1(c.y), vr_lin1(c.z));
    return float4(c, px.a);   /* conserva el alfa de la escena */
}

fragment float4 vr_fs_post(VrPostOut in [[stage_in]], texture2d<float> src [[texture(0)]], texture2d<float> frm [[texture(1)]],
                           constant VrPostUniforms &u [[buffer(0)]], constant float *dv [[buffer(VR_BUF_POST_DV)]]) {
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);
    float ys = vr_post_ysrc(in.pos.y, 0u, u, dv);
    float2 uv = float2(in.pos.x / float(src.get_width()), ys / float(src.get_height()));
    return vr_post_finish(src.sample(smp, uv), u, vr_post_scan(in.pos.y, 0u, u), vr_post_frame(in.pos.xy, 0u, u, frm));
}

fragment float4 vr_fs_post_layered(VrPostOutL in [[stage_in]], texture2d_array<float> src [[texture(0)]], texture2d<float> frm [[texture(1)]],
                                   constant VrPostUniforms &u [[buffer(0)]], constant float *dv [[buffer(VR_BUF_POST_DV)]]) {
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);
    uint eye = min(in.layer, 1u);
    float ys = vr_post_ysrc(in.pos.y, eye, u, dv);
    float2 uv = float2(in.pos.x / float(src.get_width()), ys / float(src.get_height()));
    return vr_post_finish(src.sample(smp, uv, in.layer), u, vr_post_scan(in.pos.y, eye, u), vr_post_frame(in.pos.xy, eye, u, frm));
}

/* ---------------- onda de pantalla del agua (render nativo, cabeza bajo el agua) ----------------
 * Post-proceso por ojo sobre la imagen final: cada fila de pantalla se muestrea desplazada en horizontal dx = amp * (ancho/1888) * sin(freq * fila + fase), con la fila del motor
 * medida desde el centro óptico (ns_screen_wave_row / ns_screen_wave_dx de ns_screen.c). Solo copia y desplaza (sin tinte: el tinte del agua lo aplica ns_fs); no toca la profundidad. */
inline float vr_wave_dx(float yc, uint eye, constant VrPostUniforms &u, float width) {
    if (u.eye[eye].y <= 1.0 || u.wave.w <= 0.0) return 0.0;
    float row = (yc - u.eye[eye].x) / u.eye[eye].y * u.wave.w;
    return u.wave.x * (width / 1888.0) * sin(u.wave.y * row + u.wave.z);
}

fragment float4 vr_fs_wave(VrPostOut in [[stage_in]], texture2d<float> src [[texture(0)]], constant VrPostUniforms &u [[buffer(0)]]) {
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);
    float2 uv = float2((in.pos.x + vr_wave_dx(in.pos.y, 0u, u, float(src.get_width()))) / float(src.get_width()), in.pos.y / float(src.get_height()));
    return src.sample(smp, uv);
}

fragment float4 vr_fs_wave_layered(VrPostOutL in [[stage_in]], texture2d_array<float> src [[texture(0)]], constant VrPostUniforms &u [[buffer(0)]]) {
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);
    uint eye = min(in.layer, 1u);
    float2 uv = float2((in.pos.x + vr_wave_dx(in.pos.y, eye, u, float(src.get_width()))) / float(src.get_width()), in.pos.y / float(src.get_height()));
    return src.sample(smp, uv, in.layer);
}
