/* Prismáticos del juego (equip/scope.c, edición europea): salta la espera propia del original y mide el retardo hasta el zoom.
 *
 * Decompilación (scope.c Act): al equiparlos el actor «scope.c» (Work de 0xA0 bytes en el montón del motor) no deja ni dibujar el marco durante hud_delay = 16 ticks, y luego
 * desliza la rejilla 6 ticks (field_58 sube a 6 y pone un bit de flags por reloj GV_Clock; con los dos bits flags & 3 == 3 se lee el mando). Hasta entonces ○ / × no hacen nada. El zoom en sí es manual
 * (○ acerca, × aleja; GM_Camera.zoom += zoom / 32 por tick, 320 -> 3200 en unos 75 ticks).
 * En VR ese tiempo muerto (~23 ticks, ~0,9 s a 25 Hz) no aporta nada: se escribe hud_delay = 0 y flags |= 3. Es lo mismo que haría el original al cabo de ese tiempo (el Act sigue su camino normal:
 * crea la mira sight.c, dibuja el marco y atiende el mando).
 * Offsets del Work (g_struct.h + scope.c; la reserva 0xA0 de NewScope en MGS1.EXE confirma el tamaño): +0x00 GV_ACT {prev, next, act, die, free, filename, runtime, count}, +0x5C hud_delay (s16), +0x9C flags (s32).
 * Direcciones de la edición europea (MGS1.EXE: NewScope 0x80064434.. llama GV_SetNamedActor(work, 0x80063d54, 0x80064068, 0x80012a14)).
 * Seguridad: el actor solo se toca si está vivo en su lista (act/die/nombre esperados y prev->next == actor == next->prev: un bloque liberado ya no cumple lo último) y los valores previos son los de la espera.
 * Cabecera única (sin enlazar nada nuevo): la incluye vrhost.c; la prueba es vr/gameplay/test_bino.c. */
#ifndef VR_BINO_H
#define VR_BINO_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VRBINO_ACT_FN     0x80063d54u
#define VRBINO_DIE_FN     0x80064068u
#define VRBINO_NAME       0x80012a14u   /* "scope.c" en MGS1.EXE */
#define VRBINO_W_HUD      0x5Cu         /* s16 hud_delay (16 al crearse) */
#define VRBINO_W_FLAGS    0x9Cu         /* s32 flags: bits 0 y 1 = rejilla lista en cada reloj, 0x8000 = gafas montadas */
#define VRBINO_W_SIZE     0xA0u
#define VRBINO_HUD_DELAY  16
#define VRBINO_ZOOM_BASE  320           /* GM_Camera.zoom sin aumento */
#define VRBINO_PAD_CIRCLE (1u << 13)    /* ○ del mando que ve el juego: acerca el zoom (scope.c ManageZoom) */
#define VRBINO_WAIT_MAX   100           /* VBlanks esperando que el zoom se note tras pulsar ○ (sin cambio: ya en el tope, o el mando no llegó) */
#define VRBINO_VIS_RATIO  1.017f        /* la vista cambia 1 % cuando VrFx.scope_zoom sube 1,7 % (aumento del mundo = 2 * z^0,602, XRFrameEncoder.binocularWorldZoom) */
#define VRBINO_RAM_BASE   0x80010000u
#define VRBINO_RAM_END    0x80200000u

typedef struct {
    uint32_t act;                       /* dirección del Work vivo, 0 = ninguno */
    int has_eq, has_on, has_zoom;
    uint32_t eq_vbl, on_vbl, zoom_vbl;
    double eq_ms, on_ms, zoom_ms;
    int skipped;                        /* bit 0: hud_delay puesto a 0, bit 1: rejilla dada por lista */
} VrBino;
static VrBino vrbino_S;
static int vrbino_item;                 /* el objeto equipado son los prismáticos (IT_Scope = 1): el actor aparece enseguida, se busca en cada VBlank; sin ellos no se busca (el recorrido de 2 MB es caro) */
typedef struct {                        /* medida de la latencia de ○: pulsación -> zoom del juego -> zoom visible en la vista */
    int prev, wait, game_seen;
    uint32_t btn_vbl, game_vbl;
    int base_zoom;
    float base_fx;
    double btn_ms;
} VrBinoBtn;
static VrBinoBtn vrbino_B;

static uint32_t vrbino_rd32(const uint8_t *r, uint32_t a) { uint32_t v; memcpy(&v, r + (a & 0x1FFFFFu), 4); return v; }
static int16_t vrbino_rd16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & 0x1FFFFFu), 2); return v; }
static void vrbino_wr32(uint8_t *r, uint32_t a, uint32_t v) { memcpy(r + (a & 0x1FFFFFu), &v, 4); }
static void vrbino_wr16(uint8_t *r, uint32_t a, int16_t v) { memcpy(r + (a & 0x1FFFFFu), &v, 2); }
static double vrbino_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }
static int vrbino_ptr(uint32_t p) { return p >= VRBINO_RAM_BASE && p + 8u <= VRBINO_RAM_END && !(p & 3u); }

/* 1 si `a` es el Work vivo del actor scope.c (en su lista de actores). */
static int vrbino_live(const uint8_t *ram, uint32_t a) {
    if (!vrbino_ptr(a) || a + VRBINO_W_SIZE > VRBINO_RAM_END) return 0;
    if (vrbino_rd32(ram, a + 8) != VRBINO_ACT_FN || vrbino_rd32(ram, a + 0xC) != VRBINO_DIE_FN || vrbino_rd32(ram, a + 0x14) != VRBINO_NAME) return 0;
    uint32_t p = vrbino_rd32(ram, a), n = vrbino_rd32(ram, a + 4);
    if (!vrbino_ptr(p) || !vrbino_ptr(n)) return 0;
    return vrbino_rd32(ram, p + 4) == a && vrbino_rd32(ram, n) == a;
}

static uint32_t vrbino_find(const uint8_t *ram) {
    for (uint32_t w = VRBINO_RAM_BASE + 8u; w + VRBINO_W_SIZE <= VRBINO_RAM_END; w += 4u)
        if (vrbino_rd32(ram, w) == VRBINO_ACT_FN && vrbino_live(ram, w - 8u)) return w - 8u;
    return 0;
}

__attribute__((unused)) static void vrbino_reset(void) { memset(&vrbino_S, 0, sizeof vrbino_S); memset(&vrbino_B, 0, sizeof vrbino_B); vrbino_item = 0; }

/* Una vez por fotograma, antes de vrbino_frame: 1 si el objeto equipado son los prismáticos (solo entonces busca el actor, en cada VBlank desde el primero). */
__attribute__((unused)) static void vrbino_set_item(int scope) {
    vrbino_item = scope != 0;
}

/* Una vez por fotograma de juego. vbl: contador de fotogramas; cam_zoom: GM_Camera.zoom (s16, 320 = sin aumento); view_on: el marco de los prismáticos está en la lista (VrFx.scope_on && binocular);
 * skip: saltar la espera del original. Devuelve bits: 1 = se escribió hud_delay, 2 = se escribió flags. */
static int vrbino_frame(uint8_t *ram, int cam_zoom, int view_on, uint32_t vbl, int skip) {
    VrBino *S = &vrbino_S;
    int wrote = 0;
    if (!ram) return 0;
    if (S->act && !vrbino_live(ram, S->act)) { memset(S, 0, sizeof *S); }
    if (!S->act) {
        if (!vrbino_item) return 0;
        uint32_t a = vrbino_find(ram);
        if (!a) return 0;
        memset(S, 0, sizeof *S);
        S->act = a; S->has_eq = 1; S->eq_vbl = vbl; S->eq_ms = vrbino_ms();
        fprintf(stderr, "[prismaticos] equipado_vbl=%u (actor scope.c 0x%08x, espera_del_juego=%d ticks)\n", vbl, a, VRBINO_HUD_DELAY + 7);
    }
    if (skip) {
        int16_t hd = vrbino_rd16(ram, S->act + VRBINO_W_HUD);
        if (hd > 0 && hd <= VRBINO_HUD_DELAY) { vrbino_wr16(ram, S->act + VRBINO_W_HUD, 0); wrote |= 1; }
        uint32_t fl = vrbino_rd32(ram, S->act + VRBINO_W_FLAGS);
        if ((fl & 3u) != 3u && (fl & ~0x8003u) == 0) { vrbino_wr32(ram, S->act + VRBINO_W_FLAGS, fl | 3u); wrote |= 2; }
        S->skipped |= wrote;
    }
    if (!S->has_on && view_on) {
        S->has_on = 1; S->on_vbl = vbl; S->on_ms = vrbino_ms();
        fprintf(stderr, "[prismaticos] activo_vbl=%u zoom_inicio_vbl=- | equipado->activo=%d fotogramas, %.0f ms | espera_saltada=%d\n", vbl, (int)(vbl - S->eq_vbl), S->on_ms - S->eq_ms, S->skipped);
    }
    if (!S->has_zoom && cam_zoom > VRBINO_ZOOM_BASE && cam_zoom < 4000) {
        S->has_zoom = 1; S->zoom_vbl = vbl; S->zoom_ms = vrbino_ms();
        fprintf(stderr, "[prismaticos] activo_vbl=%d zoom_inicio_vbl=%u | equipado->activo=%d, activo->zoom=%d, equipado->zoom=%d fotogramas (%.0f ms) | espera_saltada=%d (la pulsacion de O es del usuario)\n",
                S->has_on ? (int)S->on_vbl : -1, vbl, S->has_on ? (int)(S->on_vbl - S->eq_vbl) : -1, S->has_on ? (int)(vbl - S->on_vbl) : -1, (int)(vbl - S->eq_vbl), S->zoom_ms - S->eq_ms, S->skipped);
    }
    return wrote;
}

/* Latencia de ○ por tramos (una vez por fotograma, después de vrbino_frame). pad: mando que ve el juego (bit 13 = ○); cam_zoom: GM_Camera.zoom del puente (<= 0 desconocido);
 * fx_zoom: VrFx.scope_zoom que se publica a la vista (con él XRFrameEncoder fija el aumento del mundo). Registra, por cada pulsación nueva de ○ con el actor vivo, los fotogramas de
 * pulsación -> zoom del juego (GM_Camera.zoom cambia) -> zoom visible (la vista cambia 1 %). Solo mide: no escribe nada. */
__attribute__((unused)) static void vrbino_pad(uint32_t vbl, int cam_zoom, uint32_t pad, float fx_zoom) {
    VrBinoBtn *B = &vrbino_B;
    if (!vrbino_S.act) { memset(B, 0, sizeof *B); return; }
    const int down = (pad & VRBINO_PAD_CIRCLE) != 0;
    if (down && !B->prev) {
        memset(B, 0, sizeof *B);
        B->wait = 1; B->btn_vbl = vbl; B->btn_ms = vrbino_ms(); B->base_zoom = cam_zoom; B->base_fx = fx_zoom;
    }
    B->prev = down;
    if (!B->wait) return;
    if (!B->game_seen && cam_zoom > 0 && B->base_zoom > 0 && cam_zoom != B->base_zoom) { B->game_seen = 1; B->game_vbl = vbl; }
    const int vis = B->base_fx > 0.0f && fx_zoom > B->base_fx * VRBINO_VIS_RATIO;
    if (vis || vbl - B->btn_vbl >= VRBINO_WAIT_MAX) {
        fprintf(stderr, "[prismaticos] boton_vbl=%u zoom_juego_vbl=%d zoom_visible_vbl=%d | boton->juego=%d boton->visible=%d fotogramas (%.0f ms) | zoom_juego %d->%d vista %.3f->%.3f%s\n",
                B->btn_vbl, B->game_seen ? (int)B->game_vbl : -1, vis ? (int)vbl : -1, B->game_seen ? (int)(B->game_vbl - B->btn_vbl) : -1, vis ? (int)(vbl - B->btn_vbl) : -1,
                vrbino_ms() - B->btn_ms, B->base_zoom, cam_zoom, (double)B->base_fx, (double)fx_zoom, vis ? "" : " | sin_cambio_visible");
        B->wait = 0;
    }
}
#endif
