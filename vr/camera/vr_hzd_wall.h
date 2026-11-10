/* Núcleo compartido de vrhzd_wall_dist (vr_hzd_ray.c) y vrhzd_wall_near (vr_hzd.c): distancia del punto a la pared de colisión HZD más cercana (y su normal al lado libre). Solo se incluye desde esos
 * dos .c, después de definir okp / u32 / s16 (static; cada unidad tiene los suyos). Solo lee la RAM. */
/* Núcleo de vrhzd_wall_dist / vrhzd_wall_near. Con n != NULL se ignoran las paredes sobre las que está el punto (sin lado libre) y n sale unitaria, horizontal y hacia el lado del punto. */
static float wall_core(const uint8_t *ram, const float p[3], float max_units, float *n) {
    if (!ram || !p || !(max_units > 0.0f) || !isfinite(max_units)) return -1.0f;
    for (int i = 0; i < 3; i++) if (!isfinite(p[i])) return -1.0f;
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
    if (ng <= 0 || ng > 32 || !okp(gp, 24u * (uint32_t)ng)) return -1.0f;
    float best = max_units, bn[2] = {0.0f, 0.0f};
    int found = 0;
    for (int g = 0; g < ng; g++) {
        if (!((mask >> g) & 1u)) continue;
        uint32_t b = gp + 24u * (uint32_t)g;
        int nw = s16(ram, b + 2);
        uint32_t wp = u32(ram, b + 8);
        if (nw <= 0 || nw > 4096 || !okp(wp, 16u * (uint32_t)nw)) continue;
        for (int i = 0; i < nw; i++) {
            uint32_t w = wp + 16u * (uint32_t)i;
            float x1 = s16(ram, w), z1 = s16(ram, w + 2), y0 = s16(ram, w + 4), h1 = s16(ram, w + 6);
            float ex = s16(ram, w + 8) - x1, ez = s16(ram, w + 10) - z1, h2 = s16(ram, w + 14);
            float h = h1 > h2 ? h1 : h2;                             /* franja vertical y0..y0+h, la misma que usa vrhzd_clamp_eye */
            if (!(h > 0.0f) || p[1] < y0 || p[1] > y0 + h) continue;
            float L2 = ex * ex + ez * ez, t = L2 > 0.0f ? ((p[0] - x1) * ex + (p[2] - z1) * ez) / L2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            float dx = p[0] - (x1 + ex * t), dz = p[2] - (z1 + ez * t), d = sqrtf(dx * dx + dz * dz);
            if (!(d < best)) continue;
            if (n) {
                float L = sqrtf(L2);
                if (L < 1.0f) continue;
                float nx = -ez / L, nz = ex / L, sb = nx * (p[0] - x1) + nz * (p[2] - z1);
                if (sb < 0.0f) { nx = -nx; nz = -nz; sb = -sb; }
                if (sb < 1.0f) continue;                             /* el punto está sobre la pared (puerta): no hay lado libre */
                bn[0] = nx; bn[1] = nz;
            }
            best = d; found = 1;
        }
    }
    if (found && n) { n[0] = bn[0]; n[1] = bn[1]; }
    return found ? best : -1.0f;
}
