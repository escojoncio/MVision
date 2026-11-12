/* mmpx: ver mmpx.h. Reglas de MMPX.glc (Copyright 2020 Morgan McGuire & Mara Gagiu, licencia MIT). */
#include "mmpx.h"
#include <string.h>

/* Entorno de un píxel: los 9 de 3x3 (A..I) y los 16 que consultan las reglas a distancia 2 y 3. */
enum { tA, tB, tC, tD, tE, tF, tG, tH, tI, tP, tS, tQ, tR, tR3, tQ3, tS3, tP3, tRu, tQu, tRd, tQd, tSr, tPr, tSl, tPl, NTAP };
static const signed char TAP[NTAP][2] = {
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    {0, -2}, {0, 2}, {-2, 0}, {2, 0}, {3, 0}, {-3, 0}, {0, 3}, {0, -3},
    {2, -1}, {-2, -1}, {2, 1}, {-2, 1}, {1, 2}, {1, -2}, {-1, 2}, {-1, -2}};

static inline uint32_t luma(uint32_t c) { return ((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF) + 1u) * (256u - (c >> 24)); }
static inline int eq2(int b, int a0, int a1) { return b == a0 && b == a1; }
static inline int eq3(int b, int a0, int a1, int a2) { return b == a0 && b == a1 && b == a2; }
static inline int eq4(int b, int a0, int a1, int a2, int a3) { return b == a0 && b == a1 && b == a2 && b == a3; }
static inline int any3(int b, int a0, int a1, int a2) { return b == a0 || b == a1 || b == a2; }
static inline int none2(int b, int a0, int a1) { return b != a0 && b != a1; }
static inline int none4(int b, int a0, int a1, int a2, int a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

/* canon: alfa 0 del plano 0 = transparente (un solo color, sale 0,0,0,0) */
static void scale(const uint8_t *src, int w, int h, int planes, int canon, uint8_t *dst) {
    if (!src || !dst || w <= 0 || h <= 0 || planes <= 0) return;
    const size_t plane = (size_t)w * (size_t)h * 4, dplane = plane * 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *at[NTAP];   /* píxel de origen de cada posición del entorno (plano 0) */
            uint32_t key[NTAP];        /* su valor en el plano 0 */
            int id[NTAP];              /* clase: la primera posición del entorno con el mismo píxel (en todos los planos) */
            for (int i = 0; i < NTAP; i++) {
                int sx = x + TAP[i][0], sy = y + TAP[i][1];
                sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx; sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
                at[i] = src + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
                key[i] = canon && !at[i][3] ? 0u : (uint32_t)at[i][0] | (uint32_t)at[i][1] << 8 | (uint32_t)at[i][2] << 16 | (uint32_t)at[i][3] << 24;
                id[i] = i;
                for (int j = 0; j < i; j++) {
                    if (key[j] != key[i]) continue;
                    int same = 1;
                    for (int p = 1; p < planes && same; p++) same = !memcmp(at[i] + p * plane, at[j] + p * plane, 4);
                    if (same) { id[i] = id[j]; break; }
                }
            }
            const int A = id[tA], B = id[tB], C = id[tC], D = id[tD], E = id[tE], F = id[tF], G = id[tG], H = id[tH], I = id[tI];
            int J = E, K = E, L = E, M = E;   /* salida: J K / L M */
            if (!eq4(E, A, B, C, D) || !eq4(E, F, G, H, I)) {
                const int P = id[tP], S = id[tS], Q = id[tQ], R = id[tR];
                const uint32_t Bl = luma(key[tB]), Dl = luma(key[tD]), El = luma(key[tE]), Fl = luma(key[tF]), Hl = luma(key[tH]);
                /* pendiente 1:1 */
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any3(E, A, C, G) && (El < Dl || A != D || E != P || E != Q)) J = D;
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any3(E, A, C, I) && (El < Bl || C != B || E != P || E != R)) K = B;
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any3(E, A, G, I) && (El < Hl || G != H || E != S || E != Q)) L = H;
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any3(E, C, G, I) && (El < Fl || I != H || E != R || E != S)) M = F;
                /* intersecciones */
                if ((E != F && eq4(E, C, I, D, Q) && eq2(F, B, H)) && F != id[tR3]) K = M = F;
                if ((E != D && eq4(E, A, G, F, R) && eq2(D, B, H)) && D != id[tQ3]) J = L = D;
                if ((E != H && eq4(E, G, I, B, P) && eq2(H, D, F)) && H != id[tS3]) L = M = H;
                if ((E != B && eq4(E, A, C, H, S) && eq2(B, D, F)) && B != id[tP3]) J = K = B;
                if (Bl < El && eq4(E, G, H, I, S) && none4(E, A, D, C, F)) J = K = B;
                if (Hl < El && eq4(E, A, B, C, P) && none4(E, D, G, I, F)) L = M = H;
                if (Fl < El && eq4(E, A, D, G, Q) && none4(E, B, C, I, H)) K = M = F;
                if (Dl < El && eq4(E, C, F, I, R) && none4(E, B, A, G, H)) J = L = D;
                /* pendiente 2:1 */
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (eq3(H, G, F, R) && none2(H, D, id[tRu])) L = M;
                        if (eq3(H, I, D, Q) && none2(H, F, id[tQu])) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (eq3(B, A, F, R) && none2(B, D, id[tRd])) J = K;
                        if (eq3(B, C, D, Q) && none2(B, F, id[tQd])) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (eq3(D, A, H, S) && none2(D, B, id[tSr])) J = L;
                        if (eq3(D, G, B, P) && none2(D, H, id[tPr])) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (eq3(F, C, H, S) && none2(F, B, id[tSl])) K = M;
                        if (eq3(F, I, B, P) && none2(F, H, id[tPl])) M = K;
                    }
                }
            }
            const int out[4] = {J, K, L, M};
            for (int q = 0; q < 4; q++) {
                uint8_t *o = dst + ((size_t)(2 * y + (q >> 1)) * (size_t)(2 * w) + (size_t)(2 * x + (q & 1))) * 4;
                if (canon && !key[out[q]]) { memset(o, 0, 4); continue; }
                for (int p = 0; p < planes; p++) memcpy(o + p * dplane, at[out[q]] + p * plane, 4);
            }
        }
}

void mmpx_scale2x(const uint8_t *src, int w, int h, uint8_t *dst) { scale(src, w, h, 1, 1, dst); }
void mmpx_scale2x_planes(const uint8_t *src, int w, int h, int planes, uint8_t *dst) { scale(src, w, h, planes, 0, dst); }
