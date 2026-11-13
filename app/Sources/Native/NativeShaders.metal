/* Shaders del renderer nativo (NativeRenderer.swift). Dibujan la escena leída de la RAM del motor: mallas locales + matriz de mundo por pieza + color por
 * vértice (precalculado o calculado con la luz de la pieza) + niebla, con la misma semántica de texturas/mezcla que el rasterizador de referencia
 * (vr/native/ns_cpuref.c): VRAM 1024x512 de 16 bits (R16Uint), texturas de 4/8/15 bpp con CLUT, texel 0 transparente, modulación texel*color/128 en 5 bits,
 * el bit 15 del texel decide si un texel de una cara semitransparente se mezcla. Sin descarte de caras en el mundo en primera persona (doble cara; los reversos pierden los empates) ni recorte en CPU.
 *
 * Dos rutas de vértices con la misma lógica (ns_vertex): por pieza (opacos: una llamada de dibujo por pieza, índice de pieza en un búfer de 4 bytes) y por lista
 * ordenada (semitransparentes: NsGpuOrder[] de atrás adelante, ya ordenada en CPU). Layout de los búferes: vr/native/ns_gpu_types.h.
 * Profundidad: Z invertida en depth32Float (limpia a 0, compare >=). */
#include <metal_stdlib>
#include "ns_gpu_types.h"
using namespace metal;

constant uint fcNsPass [[function_constant(NSG_FC_PASS)]];
constant bool fcNsFlat [[function_constant(NSG_FC_FLAT)]];
constant bool kNsFlat = is_function_constant_defined(fcNsFlat) ? fcNsFlat : false;
constant bool fcNsLinear [[function_constant(NSG_FC_LINEAR)]];
constant uint kNsPass = is_function_constant_defined(fcNsPass) ? fcNsPass : 0u;
constant bool kNsLinear = is_function_constant_defined(fcNsLinear) ? fcNsLinear : false;

struct NsVOut {
    float4 pos [[position]];
    float2 uv;               /* texels 0..255, perspectiva correcta */
    float3 col;              /* byte de color por vértice (128 = neutro), interpolado */
    uint tpage [[flat]];
    uint clutf [[flat]];     /* clut | flags << 16 */
    float3 ew;               /* posición en el mundo del motor (el tinte del agua vive en el mundo: depende de ella, no de la cabeza) */
};

static float3 ns_mul34(const device float *m, float3 p)
{
    return float3(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                  m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                  m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
}

/* Posición de mundo del motor de un vértice: la suya, o (soldadura) la del vértice del padre transformada con la matriz del padre, encadenada hasta NSG_WELD_DEPTH niveles
 * (si el vértice del padre también está soldado, se sigue al abuelo: mismo criterio que ns_adapt de la referencia). NSG_PF_NOWELD (soldadura inconsistente este fotograma) la ignora.
 * Réplica en C: ns_gpu_vert_world (ns_gpu.c). */
static float3 ns_world_pos(const device NsGpuVert &V, const device NsGpuPiece &P, const device NsGpuVert *verts, const device NsGpuPiece *pieces)
{
    float3 wp = ns_mul34(&P.w[0], float3(V.p[0], V.p[1], V.p[2]));
    if ((P.flags & NSG_PF_NOWELD) != 0u) return wp;
    uint w = V.weld;
    int pi = P.parent;
    uint pn = P.parent_nverts;
    for (int d = 0; d < NSG_WELD_DEPTH; d++) {
        if (w == NSG_NO_WELD || pi < 0 || w >= pn) break;
        const device NsGpuPiece &Q = pieces[uint(pi)];
        const device NsGpuVert &PV = verts[Q.vert_base + w];
        wp = ns_mul34(&Q.w[0], float3(PV.p[0], PV.p[1], PV.p[2]));
        if ((Q.flags & NSG_PF_NOWELD) != 0u) break;
        w = PV.weld; pi = Q.parent; pn = Q.parent_nverts;
    }
    return wp;
}

/* Red de seguridad contra «explosiones de vértices»: ¿se colapsa el triángulo que empieza en la esquina `first`? (solo piezas NSG_PF_NET, objetos dinámicos). Se colapsa si algún vértice
 * queda lejos del origen de su pieza (mas de NSG_NET_FAR_RATIO veces el radio de reposo de la malla + holgura) o si alguna arista mide mucho más que en reposo (NSG_NET_EDGE_RATIO veces + holgura).
 * Las tres esquinas toman la MISMA decisión (se calcula con el triángulo entero), así que el triángulo desaparece entero. Réplica en C: net_tri (ns_gpu.c). */
static bool ns_net_bad(uint first, uint piece_index, const device NsGpuVert *verts, const device NsGpuCorner *corners,
                       const device NsGpuPiece *pieces, constant NsGpuUniforms &U)
{
    const device NsGpuPiece &P = pieces[piece_index];
    float mpu = length(float3(U.tovr[0].x, U.tovr[1].x, U.tovr[2].x));      /* metros VR por unidad del motor */
    float slack = NSG_NET_SLACK_M / max(mpu, 1.0e-9);
    float3 org = float3(P.w[3], P.w[7], P.w[11]);
    float3 wp[3];
    float3 lp[3];
    for (uint k = 0u; k < 3u; k++) {
        const device NsGpuVert &V = verts[corners[first + k].v];
        lp[k] = float3(V.p[0], V.p[1], V.p[2]);
        wp[k] = ns_world_pos(V, P, verts, pieces);
    }
    bool bad = false;
    for (uint k = 0u; k < 3u; k++) {
        uint k2 = (k + 1u) % 3u;
        if (P.rest_r > 0.0 && !(length(wp[k] - org) <= NSG_NET_FAR_RATIO * P.rest_r + slack)) bad = true;
        if (!(length(wp[k] - wp[k2]) <= NSG_NET_EDGE_RATIO * length(lp[k] - lp[k2]) + slack)) bad = true;
    }
    return bad;
}

static NsVOut ns_vertex(uint corner_index, uint piece_index, uint amp,
                        const device NsGpuVert *verts, const device NsGpuCorner *corners,
                        const device NsGpuPiece *pieces, constant NsGpuUniforms &U)
{
    const device NsGpuCorner &c = corners[corner_index];
    const device NsGpuPiece &P = pieces[piece_index];
    const device NsGpuVert &V = verts[c.v];

    /* posición en el mundo del motor; los vértices welded toman la posición transformada del vértice del padre (encadenadas) */
    float3 wp = ns_world_pos(V, P, verts, pieces);
    /* red de seguridad (objetos dinámicos): un triángulo estirado hasta lejos se colapsa entero */
    bool net = false;
    if ((P.flags & NSG_PF_NET) != 0u) net = ns_net_bad(corner_index - corner_index % 3u, piece_index, verts, corners, pieces, U);

    uint cf = uint(c.flags);
    /* cara de una sola cara vista por detrás desde la cámara del juego (cinemáticas): el motor no la dibuja (techos vistos desde arriba, reversos de las salas). Los tres vértices deciden igual:
     * sentido del triángulo (A,B,D) / (B,C,D) como el del perímetro, dot(p0 - ojo, (p1-p0)x(p2-p0)) > 0 = anverso. Réplica en C: ns_gpu_tri_back (ns_gpu.c). */
    bool back = false;
    /* U.cull_eye.w = NSG_CULL_ALL (cinemáticas) o NSG_CULL_SEMI (cabeza bajo el agua: solo mallas semitransparentes, p. ej. la superficie del agua vista desde abajo). Réplica: ns_gpu_cull_applies */
    const uint cull = uint(U.cull_eye.w + 0.5);
    const bool one_sided = (cf & NSG_CF_ONESIDED) != 0u && (cull == uint(NSG_CULL_ALL) || (cull == uint(NSG_CULL_SEMI) && (cf & NSG_CF_SEMI) != 0u));
    /* Modo ENGINE (cámara del juego en una pantalla: pantalla 3D y cine; U.misc.y = H/2 > 0). Dentro de la ventana del motor la verdad es el fotograma del juego y lo reproduce la regla del motor
     * por triángulo ENTERO con su ojo: la GTE satura la división H/SZ en 2,0 cuando z de vista <= H/2, así que el vértice se proyecta como si estuviera en el plano z = H/2 (x e y de la vista
     * intactas); no hay polígono si ningún vértice está delante (z <= 0), si es el reverso de una cara de una sola cara (con los vértices ya ajustados) o si mide más de 1023 x 511 px (la tarjeta
     * no lo dibuja). La pantalla 3D enseña además campo que el juego no tiene (el otro ojo, los márgenes): el triángulo descartado por reverso o tamaño que no es cercano (algún vértice a
     * z >= H) se rescata: se dibuja con su geometría real, recortado por el plano cercano, y ns_fs descarta sus fragmentos dentro de la ventana del motor (NSG_CF_WINCUT); el de reverso, solo
     * si es anverso desde el ojo de la vista que se dibuja. «Caras cercanas: Original» (sin NSG_ENG_OFF_SPLIT): la cara de un objeto sin GBOUND (pieza sin NSG_PF_NOSPLIT) que el motor original parte en trozos
     * (libdg/divide.c: doble área de pantalla > NSG_DIV_AREA y profundidad media < NSG_DIV_Z, o NSG_DIV_Z_FAR con H > 1000) y que esta regla perdería por tamaño o deformaría (algún
     * vértice a z < H/2) se dibuja con su geometría real, recortada por el plano cercano, también dentro de la ventana; el reverso se decide antes, como el motor.
     * U.misc.z = reglas apagadas (NSG_ENG_OFF_*). Las tres esquinas deciden igual. Réplica en C: ns_gpu_engine_tri (ns_gpu.c). */
    const bool near_reject = U.misc.y > 0.0;
    const uint eng_off = uint(U.misc.z + 0.5);
    /* Reverso de una cara de una sola cara que NO se descarta (mundo en primera persona: doble cara): se dibuja, pero pierde el empate de profundidad con el anverso que comparte su plano
     * (paredes de dos hojas espalda con espalda: sin esto parpadean entre sus dos texturas). Se decide con el ojo de ESTA vista. Réplica en C: ns_gpu_back_z (ns_gpu.c). */
    const bool back_keep = (cf & NSG_CF_ONESIDED) != 0u && !one_sided;
    bool behind = false;
    float3 wpos = wp;   /* posición que se proyecta: la del vértice, o su ajuste al plano z = H/2 (modo ENGINE) */
    bool rescue = false;   /* modo ENGINE: descartado por el motor pero dibujado, con su geometría real, fuera de su ventana */
    if (one_sided || near_reject || back_keep) {
        uint f0 = corner_index - corner_index % 3u;
        float3 q0 = ns_world_pos(verts[corners[f0].v], P, verts, pieces);
        float3 q1 = ns_world_pos(verts[corners[f0 + 1u].v], P, verts, pieces);
        float3 q2 = ns_world_pos(verts[corners[f0 + 2u].v], P, verts, pieces);
        /* el mismo triángulo en el mundo VR (to_vr es una semejanza: el signo de su determinante dice si conserva el sentido) */
        float3 v0 = float3(dot(U.tovr[0].xyz, q0) + U.tovr[0].w, dot(U.tovr[1].xyz, q0) + U.tovr[1].w, dot(U.tovr[2].xyz, q0) + U.tovr[2].w);
        float3 v1 = float3(dot(U.tovr[0].xyz, q1) + U.tovr[0].w, dot(U.tovr[1].xyz, q1) + U.tovr[1].w, dot(U.tovr[2].xyz, q1) + U.tovr[2].w);
        float3 v2 = float3(dot(U.tovr[0].xyz, q2) + U.tovr[0].w, dot(U.tovr[1].xyz, q2) + U.tovr[1].w, dot(U.tovr[2].xyz, q2) + U.tovr[2].w);
        float sense = dot(U.tovr[0].xyz, cross(U.tovr[1].xyz, U.tovr[2].xyz)) < 0.0 ? -1.0 : 1.0;
        float3 nvr = cross(v1 - v0, v2 - v0);
        float3 g0 = q0, g1 = q1, g2 = q2;   /* vértices tras el ajuste de la GTE */
        if (near_reject) {
            const float3 zv = float3(dot(U.fog_fwd.xyz, q0), dot(U.fog_fwd.xyz, q1), dot(U.fog_fwd.xyz, q2)) + U.fog_fwd.w;   /* z de vista de la cámara del juego */
            const float zmax = max(max(zv.x, zv.y), zv.z);
            if ((eng_off & uint(NSG_ENG_OFF_BEHIND)) == 0u && zmax <= 0.0) back = true;   // ningún vértice delante de la cámara
            else {
                const float3 zc = max(zv, float3(U.misc.y));
                g0 = q0 + U.fog_fwd.xyz * (zc.x - zv.x); g1 = q1 + U.fog_fwd.xyz * (zc.y - zv.y); g2 = q2 + U.fog_fwd.xyz * (zc.z - zv.z);
                bool cull = one_sided && dot(g0 - U.cull_eye.xyz, cross(g1 - g0, g2 - g0)) <= 0.0;   // reverso para el motor
                const bool rev = cull;
                /* tamaño máximo del hardware: la GPU original no dibuja el triángulo de más de 1023 x 511 px de la pantalla del motor (coordenadas de la GTE saturadas a -1024..1023);
                 * zc >= H/2 > 0. Réplica en C: ns_gpu_tri_oversize */
                float3 r0 = g0 - U.cull_eye.xyz, r1 = g1 - U.cull_eye.xyz, r2 = g2 - U.cull_eye.xyz;
                float3 sx = clamp(U.cam_x.w * float3(dot(U.cam_x.xyz, r0), dot(U.cam_x.xyz, r1), dot(U.cam_x.xyz, r2)) / zc, NSG_HW_SXY_MIN, NSG_HW_SXY_MAX);
                float3 sy = clamp(U.cam_y.w * float3(dot(U.cam_y.xyz, r0), dot(U.cam_y.xyz, r1), dot(U.cam_y.xyz, r2)) / zc, NSG_HW_SXY_MIN, NSG_HW_SXY_MAX);
                if (!cull && (eng_off & uint(NSG_ENG_OFF_SIZE)) == 0u &&
                    (max(max(sx.x, sx.y), sx.z) - min(min(sx.x, sx.y), sx.z) > NSG_HW_MAX_W || max(max(sy.x, sy.y), sy.z) - min(min(sy.x, sy.y), sy.z) > NSG_HW_MAX_H)) cull = true;
                /* cara que el motor original parte: objeto sin GBOUND, no reverso, perdida por tamaño o con algún vértice a z < H/2, con área y profundidad media dentro de sus umbrales */
                const float zsum = max(zv.x, 0.0) + max(zv.y, 0.0) + max(zv.z, 0.0);
                const bool split = (P.flags & NSG_PF_NOSPLIT) == 0u && (eng_off & uint(NSG_ENG_OFF_SPLIT)) == 0u && !rev && (cull || min(min(zv.x, zv.y), zv.z) < U.misc.y) &&
                                   zsum < 3.0 * (U.cam_x.w > 1000.0 ? NSG_DIV_Z_FAR : NSG_DIV_Z) &&
                                   abs((sx.y - sx.x) * (sy.z - sy.x) - (sx.z - sx.x) * (sy.y - sy.x)) > NSG_DIV_AREA;
                if (split) {
                    /* se dibuja con su geometría real (wpos = wp), sin recorte de ventana */
                } else if (cull) {
                    /* rescate en el campo extra: no cercano y, si era reverso, anverso desde el ojo de ESTA vista (U.eye[amp]; sin amplificación, el del pase) */
                    const bool front = sense * dot(v0 - U.eye[amp].xyz, nvr) > 0.0;
                    rescue = zmax >= U.cam_x.w && (!rev || front);
                    back = !rescue;
                } else {
                    wpos = corner_index % 3u == 0u ? g0 : (corner_index % 3u == 1u ? g1 : g2);
                }
            }
        } else if (one_sided) {
            back = dot(q0 - U.cull_eye.xyz, cross(q1 - q0, q2 - q0)) <= 0.0;
        }
        if (back_keep) behind = sense * dot(v0 - U.eye[amp].xyz, nvr) <= 0.0;   /* mismo criterio, en el mundo VR y con el ojo de la vista */
    }

    /* color: precalculado de la cara, luz de la pieza o neutro; después niebla (hacia negro) y byte entero */
    float3 raw;
    if ((cf & NSG_CF_PAINT) != 0u) {
        uint pc = c.paint;
        raw = float3(float(pc & 255u), float((pc >> 8) & 255u), float((pc >> 16) & 255u));
    } else if ((P.flags & NSG_PF_SHADE) != 0u) {
        float3 n = float3(c.n[0], c.n[1], c.n[2]);
        float3 I = clamp(float3(dot(float3(P.l[0], P.l[1], P.l[2]), n),
                                dot(float3(P.l[3], P.l[4], P.l[5]), n),
                                dot(float3(P.l[6], P.l[7], P.l[8]), n)), 0.0, NSG_INTENSITY_MAX);
        raw = float3(P.k[3] + dot(float3(P.k[0], P.k[1], P.k[2]), I),
                     P.k[7] + dot(float3(P.k[4], P.k[5], P.k[6]), I),
                     P.k[11] + dot(float3(P.k[8], P.k[9], P.k[10]), I));
    } else {
        raw = float3(128.0);
    }
    raw = clamp(raw, 0.0, 255.0);
    float s = P.fog;
    if ((P.flags & NSG_PF_VFOG) != 0u) {
        float z = dot(U.fog_fwd.xyz, wp) + U.fog_fwd.w;
        s = 1.0 - clamp((z - U.fog_par.x) / max(U.fog_par.y, 1.0e-3), 0.0, 1.0);
    }
    float3 col = clamp(floor(raw * s), 0.0, 255.0);
    col = clamp(floor(col * U.tint.xyz), 0.0, 255.0);   /* depuración «solo nativo»: tinte; (1,1,1) = identidad */

    /* mundo del motor -> mundo VR (m) */
    float3 pv = float3(dot(U.tovr[0].xyz, wpos) + U.tovr[0].w,
                       dot(U.tovr[1].xyz, wpos) + U.tovr[1].w,
                       dot(U.tovr[2].xyz, wpos) + U.tovr[2].w);
    /* sesgo de orden del motor (decals): se acerca el vértice al ojo a lo largo de la línea de visión (no cambia su posición en pantalla) */
    if (P.raise != 0.0) {
        float3 d = U.eye[amp].xyz - pv;
        float dl = length(d);
        float sh = min(abs(P.raise) * U.fog_par.w, 0.9 * dl);
        pv += d * ((P.raise > 0.0 ? sh : -sh) / max(dl, 1.0e-5));
    }

    NsVOut o;
    o.pos = U.view_proj[amp] * float4(pv, 1.0);
    if (behind) o.pos.z *= NSG_BACK_Z;   /* Z invertida: un poco más lejos, sin cambiar la posición en pantalla ni el recorte */
    if (net || back) o.pos = float4(0.0, 0.0, 2.0, 1.0);   /* triángulo colapsado: los tres vértices en el mismo punto fuera del volumen de recorte */
    o.uv = float2(float(c.u), float(c.t));
    o.col = col;
    o.tpage = uint(c.tpage);
    /* visión térmica: los objetos que la admiten se dibujan con la CLUT térmica del motor (NSG_PF_IRCLUT, ver ns_gpu_make_piece) */
    uint cl = (P.flags & NSG_PF_IRCLUT) != 0u ? uint(NSG_IR_CLUT) : uint(c.clut);
    if (rescue) cf |= NSG_CF_WINCUT;
    o.clutf = cl | (cf << 16);
    o.ew = wp;
    return o;
}

/* ---- opacos dinámicos: una pieza por llamada; el índice de pieza es el instance_id (baseInstance de drawPrimitives, instanceCount 1) ---- */
vertex NsVOut ns_vs_piece(uint vid [[vertex_id]], uint iid [[instance_id]],
                          const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                          const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                          const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                          constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]])
{
    return ns_vertex(vid, iid, 0u, verts, corners, pieces, U);
}

vertex NsVOut ns_vs_piece_amp(uint vid [[vertex_id]], uint iid [[instance_id]], ushort amp [[amplification_id]],
                              const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                              const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                              const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                              constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]])
{
    return ns_vertex(vid, iid, uint(amp), verts, corners, pieces, U);
}

/* ---- lista de triángulos (semitransparentes ya ordenados, o el escenario opaco en una sola llamada) ---- */
vertex NsVOut ns_vs_order(uint vid [[vertex_id]],
                          const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                          const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                          const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                          constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]],
                          const device NsGpuOrder *order [[buffer(NSG_BUF_ORDER)]])
{
    NsGpuOrder o = order[vid / 3u];
    return ns_vertex(o.tri * 3u + (vid % 3u), o.piece, 0u, verts, corners, pieces, U);
}

vertex NsVOut ns_vs_order_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                              const device NsGpuVert *verts [[buffer(NSG_BUF_VERTS)]],
                              const device NsGpuCorner *corners [[buffer(NSG_BUF_CORNERS)]],
                              const device NsGpuPiece *pieces [[buffer(NSG_BUF_PIECES)]],
                              constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]],
                              const device NsGpuOrder *order [[buffer(NSG_BUF_ORDER)]])
{
    NsGpuOrder o = order[vid / 3u];
    return ns_vertex(o.tri * 3u + (vid % 3u), o.piece, uint(amp), verts, corners, pieces, U);
}


/* ---- efectos (carteles, cintas, cuadriláteros y triángulos de mundo): vértices ya en el mundo VR, orientados a la cabeza en CPU (ns_gpu_fx_build). Misma ns_fs. ---- */
static NsVOut ns_fx_vertex(uint vid, uint amp, const device NsGpuFxVert *fv, constant NsGpuUniforms &U)
{
    const device NsGpuFxVert &V = fv[vid];
    NsVOut o;
    /* polígono de una cara que este ojo no ve: triángulo degenerado (los tres vértices llevan la misma máscara). Sin amplificación el ojo del pase viene en U.misc.x */
    const uint eye = amp + uint(U.misc.x);
    o.pos = ((V.eyes >> eye) & 1u) != 0u ? U.view_proj[amp] * float4(V.p[0], V.p[1], V.p[2], 1.0) : float4(0.0);
    const float3 pv = float3(V.p[0], V.p[1], V.p[2]);
    o.ew = float3(dot(U.fromvr[0].xyz, pv) + U.fromvr[0].w, dot(U.fromvr[1].xyz, pv) + U.fromvr[1].w, dot(U.fromvr[2].xyz, pv) + U.fromvr[2].w);
    o.uv = float2(V.u, V.v);
    o.col = clamp(floor(clamp(float3(V.col[0], V.col[1], V.col[2]), 0.0, 255.0) * U.tint.xyz), 0.0, 255.0);
    o.tpage = V.tpage;
    o.clutf = V.clutf;
    return o;
}
vertex NsVOut ns_vs_fx(uint vid [[vertex_id]],
                       const device NsGpuFxVert *fv [[buffer(NSG_BUF_FX)]],
                       constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]])
{
    return ns_fx_vertex(vid, 0u, fv, U);
}
vertex NsVOut ns_vs_fx_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]],
                           const device NsGpuFxVert *fv [[buffer(NSG_BUF_FX)]],
                           constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]])
{
    return ns_fx_vertex(vid, uint(amp), fv, U);
}

/* ---------------- muestreo de VRAM (igual que ns_sample_texel de la referencia, sin ventana de textura) ---------------- */
static uint ns_texel(texture2d<ushort, access::read> vram, uint tpage, uint clut, int u, int v)
{
    int tx = int(tpage & 15u) * 64, ty = int((tpage >> 4) & 1u) * 256;
    uint tp = (tpage >> 7) & 3u;
    int clx = int(clut & 63u) * 16, cly = int((clut >> 6) & 511u);
    u &= 255; v &= 255;
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

/* ---------------- filtro de texturas MMPX (ajuste «Filtro de texturas 3D»; U.misc.w > 0,5) ----------------
 * MMPX, McGuire y Gagiu 2020 (licencia MIT, Copyright 2020 Morgan McGuire & Mara Gagiu): reglas de vr/scene/mmpx.c. Réplica línea a línea en C: ns_mmpx de vr/native/test_ns_mmpx.c (probada contra mmpx_scale2x).
 * Para la UV del fragmento (coordenadas de texel con fracción) toma el texel de origen E y sus vecinos con ns_texel (misma página, CLUT, envoltura a 256; sin ventana de textura), decide el subtexel 2x J K / L M
 * con las reglas y devuelve el texel de VRAM de ese subtexel (0 = transparente: ns_fs lo descarta como siempre). Los texels se comparan por su valor de 16 bits ya decodificado (no por el índice de la CLUT). */
static uint ns_mm_luma(uint t)
{
    return t == 0u ? 256u : (((t & 31u) + ((t >> 5) & 31u) + ((t >> 10) & 31u)) * 8u + (t >> 15) + 1u);
}
static bool ns_mm_eq2(uint b, uint a0, uint a1) { return b == a0 && b == a1; }
static bool ns_mm_eq3(uint b, uint a0, uint a1, uint a2) { return b == a0 && b == a1 && b == a2; }
static bool ns_mm_eq4(uint b, uint a0, uint a1, uint a2, uint a3) { return b == a0 && b == a1 && b == a2 && b == a3; }
static bool ns_mm_any3(uint b, uint a0, uint a1, uint a2) { return b == a0 || b == a1 || b == a2; }
static bool ns_mm_none2(uint b, uint a0, uint a1) { return b != a0 && b != a1; }
static bool ns_mm_none4(uint b, uint a0, uint a1, uint a2, uint a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

#define NS_TAP(dx, dy) ns_texel(vram, tpage, clut, bu + (dx), bv + (dy))
static uint ns_mmpx(texture2d<ushort, access::read> vram, uint tpage, uint clut, float2 uv)
{
    const float2 fu = uv + 1.0e-4;
    const float2 bf = floor(fu);
    const int bu = int(bf.x), bv = int(bf.y);
    const float2 fr = fu - bf;
    const uint A = NS_TAP(-1, -1), B = NS_TAP(0, -1), C = NS_TAP(1, -1);
    const uint D = NS_TAP(-1, 0), E = NS_TAP(0, 0), F = NS_TAP(1, 0);
    const uint G = NS_TAP(-1, 1), H = NS_TAP(0, 1), I = NS_TAP(1, 1);
    uint J = E, K = E, L = E, M = E;
    if (!ns_mm_eq4(E, A, B, C, D) || !ns_mm_eq4(E, F, G, H, I)) {
        const uint P = NS_TAP(0, -2), S = NS_TAP(0, 2), Q = NS_TAP(-2, 0), R = NS_TAP(2, 0);
        const uint Bl = ns_mm_luma(B), Dl = ns_mm_luma(D), El = ns_mm_luma(E), Fl = ns_mm_luma(F), Hl = ns_mm_luma(H);
        /* pendiente 1:1 */
        if ((D == B && D != H && D != F) && (El >= Dl || E == A) && ns_mm_any3(E, A, C, G) && (El < Dl || A != D || E != P || E != Q)) J = D;
        if ((B == F && B != D && B != H) && (El >= Bl || E == C) && ns_mm_any3(E, A, C, I) && (El < Bl || C != B || E != P || E != R)) K = B;
        if ((H == D && H != F && H != B) && (El >= Hl || E == G) && ns_mm_any3(E, A, G, I) && (El < Hl || G != H || E != S || E != Q)) L = H;
        if ((F == H && F != B && F != D) && (El >= Fl || E == I) && ns_mm_any3(E, C, G, I) && (El < Fl || I != H || E != R || E != S)) M = F;
        /* intersecciones */
        if ((E != F && ns_mm_eq4(E, C, I, D, Q) && ns_mm_eq2(F, B, H)) && F != NS_TAP(3, 0)) { K = F; M = F; }
        if ((E != D && ns_mm_eq4(E, A, G, F, R) && ns_mm_eq2(D, B, H)) && D != NS_TAP(-3, 0)) { J = D; L = D; }
        if ((E != H && ns_mm_eq4(E, G, I, B, P) && ns_mm_eq2(H, D, F)) && H != NS_TAP(0, 3)) { L = H; M = H; }
        if ((E != B && ns_mm_eq4(E, A, C, H, S) && ns_mm_eq2(B, D, F)) && B != NS_TAP(0, -3)) { J = B; K = B; }
        if (Bl < El && ns_mm_eq4(E, G, H, I, S) && ns_mm_none4(E, A, D, C, F)) { J = B; K = B; }
        if (Hl < El && ns_mm_eq4(E, A, B, C, P) && ns_mm_none4(E, D, G, I, F)) { L = H; M = H; }
        if (Fl < El && ns_mm_eq4(E, A, D, G, Q) && ns_mm_none4(E, B, C, I, H)) { K = F; M = F; }
        if (Dl < El && ns_mm_eq4(E, C, F, I, R) && ns_mm_none4(E, B, A, G, H)) { J = D; L = D; }
        /* pendiente 2:1 */
        if (H != B) {
            if (H != A && H != E && H != C) {
                if (ns_mm_eq3(H, G, F, R) && ns_mm_none2(H, D, NS_TAP(2, -1))) L = M;
                if (ns_mm_eq3(H, I, D, Q) && ns_mm_none2(H, F, NS_TAP(-2, -1))) M = L;
            }
            if (B != I && B != G && B != E) {
                if (ns_mm_eq3(B, A, F, R) && ns_mm_none2(B, D, NS_TAP(2, 1))) J = K;
                if (ns_mm_eq3(B, C, D, Q) && ns_mm_none2(B, F, NS_TAP(-2, 1))) K = J;
            }
        }
        if (F != D) {
            if (D != I && D != E && D != C) {
                if (ns_mm_eq3(D, A, H, S) && ns_mm_none2(D, B, NS_TAP(1, 2))) J = L;
                if (ns_mm_eq3(D, G, B, P) && ns_mm_none2(D, H, NS_TAP(1, -2))) L = J;
            }
            if (F != E && F != A && F != G) {
                if (ns_mm_eq3(F, C, H, S) && ns_mm_none2(F, B, NS_TAP(-1, 2))) K = M;
                if (ns_mm_eq3(F, I, B, P) && ns_mm_none2(F, H, NS_TAP(-1, -2))) M = K;
            }
        }
    }
    return fr.x < 0.5 ? (fr.y < 0.5 ? J : L) : (fr.y < 0.5 ? K : M);
}
#undef NS_TAP

static float ns_lin1(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

/* Pase 0 (opaco): caras no semitransparentes completas; en las semitransparentes solo los texels con bit15 = 0 (las sin textura no entran).
 * Pase 1 (semitransparente): solo los texels con bit15 = 1 de las texturizadas y todo lo sin textura. Alfa = cobertura (1 donde se escribe). */
fragment float4 ns_fs(NsVOut in [[stage_in]], texture2d<ushort, access::read> vram [[texture(0)]],
                      constant NsGpuUniforms &U [[buffer(NSG_BUF_UNI)]])
{
    uint flags = in.clutf >> 16;
    uint clut = in.clutf & 0xFFFFu;
    if ((flags & NSG_CF_WINCUT) != 0u) {
        /* triángulo rescatado del descarte del motor (ns_vertex): dentro de la ventana del motor manda el fotograma del juego, que no lo tiene. Réplica en C: ns_gpu_engine_win */
        const float3 er = in.ew - U.cull_eye.xyz;
        const float ez = dot(U.fog_fwd.xyz, er);
        if (ez > 0.0 && abs(U.cam_x.w * dot(U.cam_x.xyz, er)) <= NSG_ENG_WIN_X * ez && abs(U.cam_y.w * dot(U.cam_y.xyz, er)) <= NSG_ENG_WIN_Y * ez) discard_fragment();
    }
    bool semi = (flags & NSG_CF_SEMI) != 0u;
    float3 col = clamp(in.col, 0.0, 255.0);
    /* agua (ns_screen_water_ref): tinte multiplicativo en el volumen, o en todo si la cabeza está dentro; depende solo de la posición del mundo. La ondulación NO va aquí: es un post-proceso de pantalla (vr_fs_wave). */
    bool wet = false;
    float3 wtint = float3(1.0);
    if (U.water_min.w > 0.5) {
        bool inside = all(in.ew >= U.water_min.xyz) && all(in.ew <= U.water_max.xyz);
        wet = inside || U.water_tint.w > 0.5;
        if (wet) wtint = U.water_tint.xyz;
    }
    float3 rgb;
    if (kNsFlat) {
        /* malla opaca sin textura: ni lectura de VRAM ni descarte */
        rgb = col / 255.0;
    } else if ((flags & NSG_CF_TEX) != 0u) {
        /* U.misc.w > 0,5: filtro de texturas MMPX (ns_mmpx: subtexel 2x del texel de origen); si no, el texel de origen tal cual. Uniforme: la rama es la misma para todo el dibujo. */
        uint tn = U.misc.w > 0.5 ? ns_mmpx(vram, in.tpage, clut, in.uv)
                                 : ns_texel(vram, in.tpage, clut, int(floor(in.uv.x + 1.0e-4)), int(floor(in.uv.y + 1.0e-4)));
        if (tn == 0u) discard_fragment();
        bool b15 = (tn & 0x8000u) != 0u;
        if (kNsPass == 0u) { if (semi && b15) discard_fragment(); }
        else if (!b15) discard_fragment();
        float3 t5 = float3(float(tn & 31u), float((tn >> 5) & 31u), float((tn >> 10) & 31u));
        rgb = clamp(t5 * col / 128.0, 0.0, 31.0) / 31.0;
    } else {
        if (kNsPass == 0u && semi) discard_fragment();
        rgb = col / 255.0;
    }
    if (wet) rgb = clamp(rgb * wtint, 0.0, 1.0);
    if (kNsLinear) rgb = float3(ns_lin1(rgb.x), ns_lin1(rgb.y), ns_lin1(rgb.z));
    return float4(rgb, 1.0);
}

/* ---------------- recubrimientos de pantalla del ojo (ns_screen_overlay_ref) ----------------
 * Un triángulo que cubre la pantalla; el fragmento reconstruye las tangentes del ojo (t = (ndc + c) / s con la proyección del ojo) y de ellas los píxeles del motor
 * (px = t * H): no dependen de la resolución del ojo y siguen a la cabeza como en la pantalla del motor. Sin prueba de profundidad. */
struct NsOvOut {
    float4 pos [[position]];
    float2 ndc;
    uint eye [[flat]];
};
static NsOvOut ns_overlay_vertex(uint vid, uint eye)
{
    float2 p = float2((vid << 1) & 2u, vid & 2u);
    NsOvOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.ndc = p * 2.0 - 1.0;
    o.eye = eye;
    return o;
}
vertex NsOvOut ns_vs_overlay(uint vid [[vertex_id]])
{
    return ns_overlay_vertex(vid, 0u);
}
vertex NsOvOut ns_vs_overlay_amp(uint vid [[vertex_id]], ushort amp [[amplification_id]])
{
    return ns_overlay_vertex(vid, uint(amp));
}
static float ns_sstep(float e0, float e1, float x)
{
    if (e1 <= e0) return x >= e1 ? 1.0 : 0.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
static float ns_frame_sub(texture2d<float> frm, NsGpuOverlay ov, float2 t)
{
    const float H = ov.p[3] > 0.0 ? ov.p[3] : 320.0;
    const float2 uv = float2((ov.p[0] + t.x * H) / max(ov.rgb2[0], 1.0), (ov.rgb2[2] - t.y * H) / max(ov.rgb2[1], 1.0));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    constexpr sampler fsm(coord::normalized, address::clamp_to_edge, filter::linear);
    return clamp(frm.sample(fsm, uv, level(0)).r, 0.0, 1.0);
}
fragment float4 ns_fs_overlay(NsOvOut in [[stage_in]], constant NsGpuOverlayU &U [[buffer(NSG_BUF_OVERLAY)]], texture2d<float> frm [[texture(0)]])
{
    const float4 ep = in.eye == 0u ? float4(U.eye0[0], U.eye0[1], U.eye0[2], U.eye0[3]) : float4(U.eye1[0], U.eye1[1], U.eye1[2], U.eye1[3]);   /* P00, P11, P02, P12 */
    const float3 upv = in.eye == 0u ? float3(U.up0[0], U.up0[1], U.up0[2]) : float3(U.up1[0], U.up1[1], U.up1[2]);   /* eje Y del mundo VR en el espacio de vista del ojo */
    const float2 t = (in.ndc + ep.zw) / ep.xy;
    const float H = U.ov.p[3] > 0.0 ? U.ov.p[3] : 320.0;
    const float px = t.x * H, py = t.y * H;
    float3 F = float3(U.ov.rgb[0], U.ov.rgb[1], U.ov.rgb[2]);
    float a = U.ov.amount;
    if (U.ov.kind == NSG_OV_RING) {
        /* viñeta circular: invariante al giro de la cabeza sobre su eje */
        float r = sqrt(px * px + py * py);
        float c = ns_sstep(U.ov.p[0], U.ov.p[1], r);
        F = mix(F, float3(U.ov.rgb2[0], U.ov.rgb2[1], U.ov.rgb2[2]), c);
        a *= c;
    } else if (U.ov.kind == NSG_OV_SCAN) {
        /* líneas ancladas al MUNDO: la fase sale de la elevación del rayo en el mundo VR (ángulo * H = filas del motor), no de la fila de pantalla */
        float per = U.ov.p[0] > 0.0 ? U.ov.p[0] : 2.0;
        float yd = clamp(dot(upv, normalize(float3(t.x, t.y, -1.0))), -1.0, 1.0);
        float ph = asin(yd) * H / per + U.ov.p[1];
        a *= (ph - floor(ph)) < U.ov.p[2] ? 1.0 : 0.0;
    } else if (U.ov.kind == NSG_OV_FRAME) {
        /* máscara de visión de la caja (ns_screen_frame_sub): resta por celda de la imagen original (columna = p0 + tx*H, fila = centro - ty*H), anclada a la cabeza; fuera de la imagen = cerrado */
        const float sub = ns_frame_sub(frm, U.ov, t);
        if (U.ov.p[1] < 0.5) a = sub;                                   /* pasada 0: resta (abr 2) */
        else F = F * ns_sstep(0.85, 1.0, sub);                          /* pasada 1: relleno del interior cerrado (abr 1), a = 1 */
    } else if (U.ov.kind == NSG_OV_BARS) {
        /* las barras de bajo el agua pasan a viñeta circular (no giran con la cabeza) */
        float lim = 120.0 - U.ov.p[0];
        float r = sqrt(px * px + py * py);
        a *= ns_sstep(lim - 10.0 * U.ov.p[1], lim + 10.0 * U.ov.p[1], r);
    }
    if (kNsLinear) F = float3(ns_lin1(F.x), ns_lin1(F.y), ns_lin1(F.z));
    /* mezcla de fábrica: abr0 (alfa = a/2, mezcla de color con el destino), abr1/abr2 (suma/resta de F*a), abr3 (suma de F*a/4) */
    if (U.ov.abr == 0u) return float4(F, 0.5 * a);
    if (U.ov.abr == 3u) return float4(F * a * 0.25, 1.0);
    return float4(F * a, 1.0);
}
