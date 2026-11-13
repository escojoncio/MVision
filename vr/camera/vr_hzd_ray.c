/* vr_hzd_ray: ver vr_hzd.h (vrhzd_raycast). Unidad propia (se compila siempre, no depende de vr_camera.c).
 * Fuente de colisión del láser: las paredes HZD del propio juego (las mismas que usa su línea de visión 0x80028D14, con la misma máscara de banderas), porque es el dato de colisión
 * del juego, llega hasta decenas de metros y solo cuesta leer unas centenas de paredes por consulta; el suelo, el techo y los objetos sólidos (puertas, cajas) no están ahí de forma
 * que podamos leer con seguridad, así que los aporta vr_headcol (vrhc_raycast_world) y el llamante se queda con el menor. */
#include "vr_hzd.h"
#include <math.h>
#include <string.h>

#define HZ_RAM 0x200000u
static int okp(uint32_t p, uint32_t len) { return p >= 0x80000000u && p < 0x80000000u + HZ_RAM && len <= HZ_RAM && (p & 0x1FFFFFu) + len <= HZ_RAM; }
static uint32_t u32(const uint8_t *r, uint32_t a) { uint32_t v; memcpy(&v, r + (a & 0x1FFFFFu & ~3u), 4); return v; }
static int16_t s16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & 0x1FFFFFu & ~1u), 2); return v; }

/* Una pared (16 B) contra el rayo; actualiza best (distancia) si la corta antes. */
static int seg_hit(const uint8_t *ram, uint32_t w, const float o[3], float dx, float dy, float dz, float *best) {
    float x1 = s16(ram, w), z1 = s16(ram, w + 2), y0 = s16(ram, w + 4), h1 = s16(ram, w + 6);
    float ex = s16(ram, w + 8) - x1, ez = s16(ram, w + 10) - z1, h2 = s16(ram, w + 14);
    float h = h1 > h2 ? h1 : h2;                             /* franja vertical y0..y0+h, la misma que usa vrhzd_clamp_eye */
    if (!(h > 0.0f)) return 0;
    float den = dx * ez - dz * ex;                           /* d x e (en el plano x,z) */
    if (fabsf(den) < 1e-6f) return 0;                        /* paralelo a la pared */
    float wx = x1 - o[0], wz = z1 - o[2];
    float t = (wx * ez - wz * ex) / den, s = (wx * dz - wz * dx) / den;
    if (!(t > 0.0f) || t >= *best || s < 0.0f || s > 1.0f) return 0;
    float y = o[1] + dy * t;
    if (y < y0 || y > y0 + h) return 0;
    *best = t;
    return 1;
}

float vrhzd_raycast(const uint8_t *ram, const float o[3], const float d[3], float max_units, unsigned skip) {
    if (!ram || !o || !d || !(max_units > 0.0f) || !isfinite(max_units)) return -1.0f;
    for (int i = 0; i < 3; i++) if (!isfinite(o[i]) || !isfinite(d[i])) return -1.0f;
    float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > 1e-6f)) return -1.0f;
    const float dx = d[0] / dl, dy = d[1] / dl, dz = d[2] / dl;
    if (fabsf(dx) + fabsf(dz) < 1e-6f) return -1.0f;                 /* rayo vertical: las paredes son verticales, nunca las cruza */
    uint32_t ctl = u32(ram, 0x800ACB84u);                            /* misma cadena que vrhzd_clamp_eye */
    if (!okp(ctl, 0x30)) return -1.0f;
    uint32_t map = u32(ram, ctl + 0x2C);
    if (!okp(map, 12)) return -1.0f;
    uint32_t wr = u32(ram, map + 8);
    if (!okp(wr, 4)) return -1.0f;
    uint32_t hdr = u32(ram, wr);
    if (!okp(hdr, 20)) return -1.0f;
    int ng = s16(ram, hdr + 10);
    uint32_t gp = u32(ram, hdr + 16), mask = u32(ram, 0x800ACB38u);
    float best = max_units;
    int found = 0;
    if (ng <= 0 || ng > 32 || !okp(gp, 24u * (uint32_t)ng)) ng = 0;       /* sin grupos legibles solo cuentan los segmentos dinámicos */
    for (int g = 0; g < ng; g++) {
        if (!((mask >> g) & 1u)) continue;
        uint32_t b = gp + 24u * (uint32_t)g;
        int nw = s16(ram, b + 2);
        uint32_t wp = u32(ram, b + 8), fp = u32(ram, b + 20);
        if (nw <= 0 || nw > 4096 || !okp(wp, 16u * (uint32_t)nw)) continue;
        int use_flags = skip && okp(fp, (uint32_t)nw);               /* sin tabla de banderas legible no se puede aplicar la máscara: la pared cuenta */
        for (int i = 0; i < nw; i++) {
            if (use_flags && (ram[(fp + (uint32_t)i) & 0x1FFFFFu] & skip)) continue;
            if (seg_hit(ram, wp + 16u * (uint32_t)i, o, dx, dy, dz, &best)) found = 1;
        }
    }
    /* Segmentos dinámicos (puertas, cajas...): HZD_CHK_D_SEGMENT de la línea de tiro del juego (libhzd online.c). Envoltura: +10 s16 nº, +18 s16 máximo, +32 puntero a la lista de punteros a pared,
     * +36 bytes de banderas (una por segmento, misma máscara skip que las fijas). Solo la envoltura del mapa del jugador (el juego recorre las de todos los mapas cargados). */
    {
        int nd = s16(ram, wr + 10), md = s16(ram, wr + 18);
        uint32_t dl = u32(ram, wr + 32), df = u32(ram, wr + 36);
        if (nd > 0 && nd <= md && md <= 512 && okp(dl, 4u * (uint32_t)nd)) {
            int use_flags = skip && okp(df, (uint32_t)nd);
            for (int i = 0; i < nd; i++) {
                uint32_t sp = u32(ram, dl + 4u * (uint32_t)i);
                if (!okp(sp, 16)) continue;
                if (use_flags && (ram[(df + (uint32_t)i) & 0x1FFFFFu] & skip)) continue;
                if (seg_hit(ram, sp, o, dx, dy, dz, &best)) found = 1;
            }
        }
    }
    return found ? best : -1.0f;
}

#include "vr_hzd_wall.h"

float vrhzd_wall_dist(const uint8_t *ram, const float p[3], float max_units) { return wall_core(ram, p, max_units, NULL); }
