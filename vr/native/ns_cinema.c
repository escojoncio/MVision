#include "ns_cinema.h"
#include <math.h>
#include <string.h>

static int fin(const float *p, int n) { for (int i = 0; i < n; i++) if (!isfinite(p[i])) return 0; return 1; }

/* inversa de una afín en orden de columnas (la fila inferior se ignora y se toma (0,0,0,1)) */
static int inv_affine(const float g[16], double o[16]) {
    double a = g[0], d = g[1], gg = g[2];           /* columna 0 = (a, d, g) */
    double b = g[4], e = g[5], h = g[6];            /* columna 1 */
    double c = g[8], f = g[9], i = g[10];           /* columna 2 */
    double det = a * (e * i - f * h) - b * (d * i - f * gg) + c * (d * h - e * gg);
    if (!(fabs(det) > 1e-18)) return 0;
    double r[9] = { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                    (f * gg - d * i) / det, (a * i - c * gg) / det, (c * d - a * f) / det,
                    (d * h - e * gg) / det, (b * gg - a * h) / det, (a * e - b * d) / det };   /* fila-mayor de la inversa 3x3 */
    memset(o, 0, 16 * sizeof(double));
    for (int row = 0; row < 3; row++) for (int col = 0; col < 3; col++) o[col * 4 + row] = r[row * 3 + col];
    for (int row = 0; row < 3; row++) o[12 + row] = -(r[row * 3] * g[12] + r[row * 3 + 1] * g[13] + r[row * 3 + 2] * g[14]);
    o[15] = 1;
    return 1;
}

int ns_cinema_to_vr(const NsCamera *cam, const float gv[16], float out[12]) {
    if (!cam || !gv || !out || !fin(gv, 16) || !fin(cam->rot, 9) || !fin(cam->eye, 3)) return 0;
    double ginv[16];
    if (!inv_affine(gv, ginv)) return 0;
    const double ys = (cam->overscan_y > 0.5f && cam->overscan_y < 2.0f) ? cam->overscan_y : (double)NS_OVERSCAN_Y;
    /* V = S R^T (fila i de R^T = columna i de R) */
    double V[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) V[i * 3 + j] = cam->rot[j * 3 + i] * (i == 1 ? ys : 1.0);
    for (int row = 0; row < 3; row++) {
        double m[3];
        for (int j = 0; j < 3; j++) m[j] = gv[0 * 4 + row] * V[0 * 3 + j] + gv[1 * 4 + row] * V[1 * 3 + j] + gv[2 * 4 + row] * V[2 * 3 + j];
        for (int j = 0; j < 3; j++) out[row * 4 + j] = (float)m[j];
        out[row * 4 + 3] = (float)(gv[12 + row] - (m[0] * cam->eye[0] + m[1] * cam->eye[1] + m[2] * cam->eye[2]));
    }
    return fin(out, 12);
}

int ns_cinema_eye(const float gv[16], int eye, float ipd, float conv, float tan_x, float tan_y, float view[16], float tangents[4]) {
    if (!gv || !view || !tangents || !fin(gv, 16) || !(ipd >= 0.0f) || !isfinite(ipd) || !(conv > 0.1f) || !(tan_x > 0.05f) || !(tan_y > 0.05f)) return 0;
    double gi[16];
    if (!inv_affine(gv, gi)) return 0;
    const double s = 0.001, o = eye == 0 ? -0.5 * ipd : 0.5 * ipd;
    /* view = T(-o,0,0) * diag(s,-s,-s,1) * ginv */
    const double fd[3] = { s, -s, -s };
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 3; row++) view[col * 4 + row] = (float)(fd[row] * gi[col * 4 + row]);
        view[col * 4 + 3] = (float)(col == 3 ? 1.0 : 0.0);
    }
    view[12] = (float)(fd[0] * gi[12] - o);   /* T(-o) suma -o en x de la columna de traslación (la fila inferior de f*ginv es (0,0,0,1)) */
    const float sh = 0.5f * ipd / conv;
    if (eye == 0) { tangents[0] = tan_x - sh; tangents[1] = tan_x + sh; } else { tangents[0] = tan_x + sh; tangents[1] = tan_x - sh; }
    tangents[2] = tan_y; tangents[3] = tan_y;
    return fin(view, 16);
}

int ns_cinema_window(const NsCamera *cam, float *tan_x, float *tan_y, float shift[4]) {
    if (!cam || !(cam->h > 8.0f) || !isfinite(cam->h)) return 0;
    const float H = cam->h;
    float ox = (isfinite(cam->ofx) && cam->ofx > 0.0f && cam->ofx < 320.0f) ? cam->ofx : 160.0f;
    float oy = (isfinite(cam->ofy) && cam->ofy > 0.0f && cam->ofy < 240.0f) ? cam->ofy : 120.0f;
    if (tan_x) *tan_x = 160.0f / H;
    if (tan_y) *tan_y = 120.0f / H;
    if (shift) { const float dx = (ox - 160.0f) / H, dy = (oy - 120.0f) / H; shift[0] = dx; shift[1] = -dx; shift[2] = dy; shift[3] = -dy; }
    return 1;
}

int ns_cinema_stereo(float ipd, float z_near, float z_subj, float tan_x, float tan_screen, float dist, NsCinemaStereo *out) {
    if (!out || !(ipd >= 0.0f) || !isfinite(ipd) || !(tan_x > 0.0f) || !isfinite(tan_x) || !(tan_screen > 0.0f) || !isfinite(tan_screen) || !(dist > 0.0f) || !isfinite(dist)) return 0;
    const float conv = (z_subj > 0.0f && isfinite(z_subj)) ? (z_subj < 0.3f ? 0.3f : z_subj) : NS_CINEMA_CONV_DEFAULT;
    const float M = tan_screen / tan_x;
    const float pos = ipd / dist < NS_CINEMA_PAR_POS ? ipd / dist : NS_CINEMA_PAR_POS;
    const float kn = (z_near > 0.0f && z_near < conv) ? 1.0f / z_near - 1.0f / conv : 0.0f;   /* paralaje cruzado por metro de separación y unidad de M */
    float b = pos * conv / M;
    if (kn > 0.0f && b > NS_CINEMA_PAR_NEG / (M * kn)) b = NS_CINEMA_PAR_NEG / (M * kn);
    if (b > ipd) b = ipd;
    out->b = b; out->conv = conv;
    out->par_max = M * b / conv * 57.29578f;
    out->par_min = -M * b * kn * 57.29578f;
    return isfinite(b) && isfinite(out->par_max) && isfinite(out->par_min);
}

int ns_cinema_depth(const NsScene *sc, float *z_near, float *z_subj) {
    if (z_near) *z_near = 0.0f;
    if (z_subj) *z_subj = 0.0f;
    float tx, ty, sh[4];
    if (!sc || !sc->inst || !sc->piece || !fin(sc->cam.rot, 9) || !fin(sc->cam.eye, 3) || !ns_cinema_window(&sc->cam, &tx, &ty, sh)) return 0;
    const NsCamera *c = &sc->cam;
    const double ys = (c->overscan_y > 0.5f && c->overscan_y < 2.0f) ? c->overscan_y : (double)NS_OVERSCAN_Y;
    const uint32_t ni = sc->n_inst < sc->cap_inst ? sc->n_inst : sc->cap_inst, np = sc->n_piece < sc->cap_piece ? sc->n_piece : sc->cap_piece;
    double sw = 0, swz = 0, zmin = 0;
    for (uint32_t i = 0; i < ni; i++) {
        const NsInstance *ins = &sc->inst[i];
        if ((ins->flags & (NS_IF_STATIC | NS_IF_SHADOW)) || ins->first_piece + (uint32_t)ins->n_pieces > np) continue;
        for (uint32_t k = 0; k < ins->n_pieces; k++) {
            const NsPiece *p = &sc->piece[ins->first_piece + k];
            if (p->flags & NS_PF_HIDDEN) continue;
            const double d[3] = { p->world.m[3] - c->eye[0], p->world.m[7] - c->eye[1], p->world.m[11] - c->eye[2] };
            const double z = 0.001 * (c->rot[2] * d[0] + c->rot[5] * d[1] + c->rot[8] * d[2]);          /* vista = R^T (p - eye): columnas de R */
            if (!(z > 0.1)) continue;
            const double x = 0.001 * (c->rot[0] * d[0] + c->rot[3] * d[1] + c->rot[6] * d[2]) / z;      /* tangentes respecto al eje (x derecha, y abajo) */
            const double y = 0.001 * ys * (c->rot[1] * d[0] + c->rot[4] * d[1] + c->rot[7] * d[2]) / z;
            /* fracción 0 (centro de la ventana) .. 1 (borde): la ventana va de -(tx + sh[0]) a tx + sh[1] y de -(ty + sh[2]) a ty + sh[3] */
            const double ax = fabs(x + sh[0]) / tx, ay = fabs(y + sh[2]) / ty;
            if (!(ax < 1.0 && ay < 1.0)) continue;
            const double w = (1.0 - ax) * (1.0 - ay);
            sw += w; swz += w / z;
            if (zmin == 0 || z < zmin) zmin = z;
        }
    }
    if (!(sw > 0) || !(swz > 0)) return 0;
    if (z_subj) *z_subj = (float)(sw / swz);
    if (z_near) *z_near = (float)(zmin - 0.25 > 0.1 ? zmin - 0.25 : 0.1);
    return 1;
}

int ns_cinema_depth_filter(NsCinemaDepthFilter *f, const NsScene *sc, float *z_near, float *z_subj) {
    if (z_near) *z_near = 0.0f;
    if (z_subj) *z_subj = 0.0f;
    if (!f) return 0;
    float tx, ty;
    if (!sc || !fin(sc->cam.rot, 9) || !fin(sc->cam.eye, 3) || !ns_cinema_window(&sc->cam, &tx, &ty, NULL)) { memset(f, 0, sizeof *f); return 0; }
    const NsCamera *c = &sc->cam;
    const float fwd[3] = { c->rot[2], c->rot[5], c->rot[8] };
    int cut = !f->have || sc->t_ns < f->t_ns;
    if (!cut) {
        const float d[3] = { c->eye[0] - f->eye[0], c->eye[1] - f->eye[1], c->eye[2] - f->eye[2] };
        const float jump = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float l = sqrtf((fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]) * (f->fwd[0] * f->fwd[0] + f->fwd[1] * f->fwd[1] + f->fwd[2] * f->fwd[2]));
        const float cs = (fwd[0] * f->fwd[0] + fwd[1] * f->fwd[1] + fwd[2] * f->fwd[2]) / l;
        if (!(jump <= NS_CINEMA_CUT_DIST) || !(cs >= cosf(NS_CINEMA_CUT_DEG * 0.0174533f))) cut = 1;   /* NaN => corte */
    }
    float zn, zs;
    const int raw = ns_cinema_depth(sc, &zn, &zs);
    if (cut || (raw && !(f->inv_subj > 0.0f))) {
        f->inv_near = raw ? 1.0f / zn : 0.0f; f->inv_subj = raw ? 1.0f / zs : 0.0f;
    } else if (raw) {
        const float a = 1.0f - expf(-(float)((double)(sc->t_ns - f->t_ns) * 1e-9) / NS_CINEMA_TAU_S);
        f->inv_near += a * (1.0f / zn - f->inv_near); f->inv_subj += a * (1.0f / zs - f->inv_subj);
    }
    f->have = 1; f->t_ns = sc->t_ns;
    memcpy(f->eye, c->eye, sizeof f->eye); memcpy(f->fwd, fwd, sizeof f->fwd);
    if (!(f->inv_subj > 0.0f)) return 0;
    if (z_near) *z_near = 1.0f / f->inv_near;
    if (z_subj) *z_subj = 1.0f / f->inv_subj;
    return 1;
}

void ns_cinema_projection(const float t[4], float n, float f, float out[16]) {
    const float l = t[0], r = t[1], u = t[2], d = t[3];
    const float sx = 2 / (l + r), sy = 2 / (u + d), cx = (r - l) / (l + r), cy = (u - d) / (u + d);
    const float a = n / (f - n), b = n * f / (f - n);
    memset(out, 0, 16 * sizeof(float));
    out[0] = sx; out[5] = sy; out[8] = cx; out[9] = cy; out[10] = a; out[11] = -1; out[14] = b;
}
