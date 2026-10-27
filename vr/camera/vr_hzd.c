/* vr_hzd: ver vr_hzd.h. Solo se compila con VRHZD_STANDALONE (vr_camera.c lo define e incluye este .c; en Xcode/stage este archivo queda como unidad vacía para no duplicar símbolos). */
#ifdef VRHZD_STANDALONE
#include "vr_hzd.h"
#include <math.h>
#include <string.h>

#define HZ_RAM 0x200000u
static int okp(uint32_t p, uint32_t len) { return p >= 0x80000000u && p < 0x80000000u + HZ_RAM && len <= HZ_RAM && (p & 0x1FFFFFu) + len <= HZ_RAM; }
static uint32_t u32(const uint8_t *r, uint32_t a) { uint32_t v; memcpy(&v, r + (a & 0x1FFFFFu & ~3u), 4); return v; }
static int16_t s16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & 0x1FFFFFu & ~1u), 2); return v; }

int vrhzd_clamp_eye(const uint8_t *ram, int32_t bx, int32_t bz, float *ex, float *ez, float ey, float r) {
    if (!ram || !ex || !ez || !(r > 0.0f) || !isfinite(*ex) || !isfinite(*ez) || !isfinite(ey)) return 0;
    uint32_t ctl = u32(ram, 0x800ACB84u);
    if (!okp(ctl, 0x30)) return 0;
    uint32_t map = u32(ram, ctl + 0x2C);
    if (!okp(map, 12)) return 0;
    uint32_t wr = u32(ram, map + 8);
    if (!okp(wr, 4)) return 0;
    uint32_t hdr = u32(ram, wr);
    if (!okp(hdr, 20)) return 0;
    int ng = s16(ram, hdr + 10);
    uint32_t gp = u32(ram, hdr + 16), mask = u32(ram, 0x800ACB38u);
    if (ng <= 0 || ng > 32 || !okp(gp, 24u * (uint32_t)ng)) return 0;
    int pushes = 0;
    for (int pass = 0; pass < 4; pass++) {
        int changed = 0;
        for (int g = 0; g < ng; g++) {
            if (!((mask >> g) & 1u)) continue;
            uint32_t b = gp + 24u * (uint32_t)g;
            int nw = s16(ram, b + 2);
            uint32_t wp = u32(ram, b + 8);
            if (nw <= 0 || nw > 4096 || !okp(wp, 16u * (uint32_t)nw)) continue;
            for (int i = 0; i < nw; i++) {
                uint32_t w = wp + 16u * (uint32_t)i;
                float x1 = s16(ram, w), z1 = s16(ram, w + 2), y0 = s16(ram, w + 4), h1 = s16(ram, w + 6);
                float x2 = s16(ram, w + 8), z2 = s16(ram, w + 10), h2 = s16(ram, w + 14);
                float h = h1 > h2 ? h1 : h2;
                if (!(h > 0.0f) || ey < y0 - r || ey > y0 + h + r) continue;
                float dx = x2 - x1, dz = z2 - z1, L = sqrtf(dx * dx + dz * dz);
                if (L < 1.0f) continue;
                float nx = -dz / L, nz = dx / L;
                float sb = nx * ((float)bx - x1) + nz * ((float)bz - z1);
                if (sb < 0.0f) { nx = -nx; nz = -nz; sb = -sb; }
                if (sb < 1.0f) continue;                       /* el cuerpo está sobre la pared (puerta): no hay lado que defender */
                float c = sb < r ? sb : r;                     /* nunca más holgura de la que tiene el cuerpo */
                float t = (dx * (*ex - x1) + dz * (*ez - z1)) / L;
                float se = nx * (*ex - x1) + nz * (*ez - z1);
                if (t >= 0.0f && t <= L) {
                    if (se < c) { *ex += nx * (c - se); *ez += nz * (c - se); changed = 1; pushes++; }
                } else {                                       /* más allá de un extremo: cilindro de radio c alrededor del extremo */
                    float px = t < 0.0f ? x1 : x2, pz = t < 0.0f ? z1 : z2;
                    float vx = *ex - px, vz = *ez - pz, d = sqrtf(vx * vx + vz * vz);
                    if (d < c && d > 1e-3f) { *ex = px + vx / d * c; *ez = pz + vz / d * c; changed = 1; pushes++; }
                }
            }
        }
        if (!changed) break;
    }
    return pushes;
}
#endif /* VRHZD_STANDALONE */
