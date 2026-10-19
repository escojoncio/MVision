/* vr_missile: ver vr_missile.h. Núcleo sin dependencias del runtime (se prueba con RAM sintética). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vr_missile.h"

#define RAM_MASK 0x1FFFFFu
#define MA_POS     0x20u
#define MA_HEADING 0x6Eu
#define MA_PREV    0x108u
#define MA_DONE    0x112u
#define MA_AGE     0x118u
#define MA_STATE   0x11Cu
#define MA_SIZE    0x328u          /* tamaño del actor (808) */
#define STALL_VBLANKS 12           /* la edad avanza cada tick (2 VBlanks): sin avance en tantos VBlanks = terminado */
#define PAD_UP 0x10u
#define PAD_RIGHT 0x20u
#define PAD_DOWN 0x40u
#define PAD_LEFT 0x80u

static int ptr_ok(uint32_t a, uint32_t n) { return a >= 0x80010000u && a < 0x80200000u && n <= 0x200000u && (a & RAM_MASK) + n <= RAM_MASK + 1u; }
static int16_t rd_s16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & RAM_MASK), 2); return v; }
static int32_t rd_s32(const uint8_t *r, uint32_t a) { int32_t v; memcpy(&v, r + (a & RAM_MASK), 4); return v; }

static struct {
    VrMissile m;
    int last_age, stall;
    float st_y, st_p;            /* ejes pedidos */
    uint16_t pad; int pad_hold;  /* bits vigentes y VBlanks que faltan */
} S;

void vrmis_reset(void) { memset(&S, 0, sizeof S); }

void vrmis_begin(uint32_t actor) {
    uint32_t serial = S.m.serial + 1;
    float sy = S.st_y, sp = S.st_p;
    memset(&S, 0, sizeof S);
    S.m.serial = serial;
    S.st_y = sy; S.st_p = sp;
    S.m.actor = ptr_ok(actor, MA_SIZE) ? actor : 0;
    S.last_age = -1;
}

int vrmis_poll(const uint8_t *ram, VrMissile *out) {
    VrMissile *m = &S.m;
    if (ram && m->actor && m->ended == 0) {
        uint32_t a = m->actor;
        int16_t age = rd_s16(ram, a + MA_AGE);
        int done = ram[(a + MA_DONE) & RAM_MASK] != 0;
        int32_t state = rd_s32(ram, a + MA_STATE);
        int32_t p[3], q[3];
        for (int i = 0; i < 3; i++) { p[i] = rd_s16(ram, a + MA_POS + 2u * (uint32_t)i); q[i] = rd_s16(ram, a + MA_PREV + 2u * (uint32_t)i); }
        if (age != S.last_age) { S.stall = 0; S.last_age = age; } else S.stall++;
        m->age = age;
        m->heading = (uint16_t)rd_s16(ram, a + MA_HEADING) & 4095;
        for (int i = 0; i < 3; i++) m->pos[i] = p[i];
        float d[3] = {(float)(p[0] - q[0]), (float)(p[1] - q[1]), (float)(p[2] - q[2])};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 1.0f) for (int i = 0; i < 3; i++) m->dir[i] = d[i] / l;
        else if (m->dir[0] == 0 && m->dir[1] == 0 && m->dir[2] == 0) {      /* aún sin movimiento: la guiñada del juego (0 = +Z, 1024 = +X) */
            float yaw = (float)m->heading * (6.2831853f / 4096.0f);
            m->dir[0] = sinf(yaw); m->dir[2] = cosf(yaw);
        }
        if (done || state < 0) { m->ended = 1; m->alive = 0; }
        else if (S.stall >= STALL_VBLANKS) { m->ended = 2; m->alive = 0; }
        else m->alive = 1;
    }
    if (out) *out = *m;
    return m->actor != 0;
}
void vrmis_get(VrMissile *out) { *out = S.m; }

/* ---- guiado ----
 * Medido en el juego: mantener una dirección de la cruceta fija la guiñada OBJETIVO del misil (absoluta), que el misil sigue girando;
 * soltar la deja como estaba (sigue recto). La dirección se interpreta respecto al origen del mando (vrcam_pad_origin, la mirada de la
 * cabeza): arriba = origen + 2048, derecha = -1024 respecto a arriba, izquierda = +1024, abajo = +2048, y las diagonales a 512 (ocho
 * direcciones; las cuatro diagonales de abajo por simetría, no medidas). La guiñada crece hacia la izquierda (0 = +Z, 1024 = +X).
 * El control "eje -> cruceta" calcula la guiñada deseada = rumbo actual del misil - eje * 90 grados y pulsa la dirección cuyo rumbo se le acerca
 * más; si esa ya coincide con el rumbo actual no pulsa (el misil sigue recto). */
static const struct { uint16_t bits; int off; } DIR8[8] = {
    { PAD_UP, 0 }, { PAD_UP | PAD_RIGHT, -512 }, { PAD_RIGHT, -1024 }, { PAD_DOWN | PAD_RIGHT, -1536 },
    { PAD_DOWN, 2048 }, { PAD_DOWN | PAD_LEFT, 1536 }, { PAD_LEFT, 1024 }, { PAD_UP | PAD_LEFT, 512 },
};
static int wrap4096(int a) { a &= 4095; return a > 2048 ? a - 4096 : a; }

void vrmis_steer(float yaw, float pitch) {
    if (!(yaw == yaw)) yaw = 0;
    if (!(pitch == pitch)) pitch = 0;
    S.st_y = yaw < -1 ? -1 : (yaw > 1 ? 1 : yaw);
    S.st_p = pitch < -1 ? -1 : (pitch > 1 ? 1 : pitch);
}
uint16_t vrmis_pad(int pad_origin) {
    /* Se decide una vez por tick (2 VBlanks): la decisión vale este VBlank y el siguiente. */
    if (S.pad_hold > 0) { S.pad_hold--; return S.pad; }
    S.pad = 0;
    S.pad_hold = 1;
    if (!S.m.alive || fabsf(S.st_y) < 0.1f) return 0;
    int cur = (int)lrintf(atan2f(S.m.dir[0], S.m.dir[2]) * (4096.0f / 6.2831853f));
    int want = cur - (int)lrintf(S.st_y * 1024.0f);
    int best = -1, besterr = 1 << 20;
    for (int i = 0; i < 8; i++) {
        int h = pad_origin + 2048 + DIR8[i].off;
        int e = abs(wrap4096(h - want));
        if (e < besterr) { besterr = e; best = i; }
    }
    int h = pad_origin + 2048 + DIR8[best].off;
    if (abs(wrap4096(h - cur)) < 200) return 0;          /* ya coincide con el rumbo actual: no hace falta girar */
    S.pad = DIR8[best].bits;
    return S.pad;
}
int vrmis_pitch_supported(void) { return 0; }
