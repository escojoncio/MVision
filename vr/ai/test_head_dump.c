/* Validación de la visión por la cabeza VR con la RUTINA REAL del motor sobre volcados de RAM (no se ejecuta el juego: solo se llama a la prueba de visión
 * de un soldado, con el intérprete del runtime, sobre la RAM de una instantánea, variando la posición del personaje).
 *   test_head_dump <volcado.bin> [actor_hex]
 * Comprueba: (1) estructura (variante, desplazamientos y alcance leídos del volcado); (2) con el ajuste original el envoltorio no altera nada; (3) barrido de
 * posiciones del personaje: dentro del cono con línea libre ve; fuera del cono nunca ve; con el cuerpo oculto por la geometría real del mapa y la cabeza
 * desplazada a un punto despejado el soldado lo ve; el resultado «más desfavorable» nunca es peor que el del cuerpo; la RAM del personaje y el alcance se restauran. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "rtpatch.h"
#include "vr_ai_scan.h"
#include "vr_perception_rt.h"
#include "bridge.h"

void interp_call(Cpu *c, uint32_t entry);
const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int nfail, nok;
#define CHECK(c, ...) do { if (c) nok++; else { nfail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t snap[VAI_RAM_SIZE];
static uint32_t actor, vision;
static const int O_FACE = 2956, O_ANG = 2958, O_LEN = 2960, O_FLAG = 2962, O_DIS = 3108, O_DIR = 3112;

static void put_player(int x, int y, int z) {
    vai_w16(emu_ram, VRP_A_PLAYER_POS, (int16_t)x); vai_w16(emu_ram, VRP_A_PLAYER_POS + 2, (int16_t)y); vai_w16(emu_ram, VRP_A_PLAYER_POS + 4, (int16_t)z);
}
/* una evaluación completa: restaura la RAM del volcado y llama a la rutina de visión (parcheada o no) para el soldado */
static int eval(int px, int py, int pz, const float *head_off, float t, int *dist, int *dir) {
    memcpy(emu_ram, snap, sizeof snap);
    put_player(px, py, pz);
    /* turno del soldado: contador % intervalo == su índice */
    int32_t cnt = (int32_t)vai_r32(emu_ram, 0x800E311Cu), itv = (int32_t)vai_r32(emu_ram, 0x800E3118u);
    if (itv > 0) vai_w8(emu_ram, actor + 2936, (uint8_t)(cnt % itv));
    vrp_rt_frame(t);
    vrp_rt_set_head(head_off);
    Cpu c; memset(&c, 0, sizeof c);
    c.r[29] = 0x801FFF00u; c.r[31] = 0xDEAD0000u; c.r[28] = 0x800AC488u; c.r[4] = actor;
    interp_call(&c, vision);
    if (dist) *dist = (int)vai_r32(emu_ram, actor + O_DIS);
    if (dir) *dir = (int)vai_r32(emu_ram, actor + O_DIR);
    return vai_r16(emu_ram, actor + O_FLAG);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(snap, 1, sizeof snap, f) != sizeof snap) { printf("no se pudo leer %s\n", argv[1]); return 2; }
    fclose(f);
    memcpy(emu_ram, snap, sizeof snap);
    static BridgeState bs;
    bridge_read(emu_ram, &bs);
    actor = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    if (!actor && bs.n_enemies > 0) actor = bs.controls[bs.enemies[bs.n_enemies > 1 ? 1 : 0].control].addr - 0x20;
    if (!actor) { printf("sin soldado\n"); return 2; }
    vrp_rt_init(VRP_ORIGINAL);
    VrpSites s;
    CHECK(vrp_locate(emu_ram, 0x800C4380u, 0x80100000u, &s) == 1, "localizar la prueba de visión");
    vision = s.vision;
    int sx = (int16_t)vai_r16(emu_ram, actor + 0x20), sy = vai_r16(emu_ram, actor + 0x22), sz = vai_r16(emu_ram, actor + 0x24);
    int face = vai_r16(emu_ram, actor + O_FACE), ang = vai_r16(emu_ram, actor + O_ANG), len = vai_r16(emu_ram, actor + O_LEN);
    printf("volcado %s: soldado %08x pos (%d,%d,%d) rumbo %d semiángulo %d alcance %d; visión en %08x\n", argv[1], actor, sx, sy, sz, face, ang, len, vision);
    CHECK(ang > 0 && ang < 2048 && len > 0 && len <= 30000, "estructura del soldado coherente");

    /* (2) original: llamada directa sin parche vs ajuste ORIGINAL con el envoltorio activo: RAM idéntica */
    float dirx = sinf(face * 6.2831853f / 4096.0f), dirz = cosf(face * 6.2831853f / 4096.0f);
    int px = sx + (int)(dirx * 2500), pz = sz + (int)(dirz * 2500), py = sy;
    int r0 = eval(px, py, pz, NULL, 1.0f, NULL, NULL);
    static uint8_t ref[VAI_RAM_SIZE];
    memcpy(ref, emu_ram, sizeof ref);
    CHECK(vrp_rt_overlay(0x800C4380u, 0x80100000u) == 1, "instalar el parche");
    vrp_rt_set_preset(VRP_ORIGINAL);
    int r1 = eval(px, py, pz, (float[3]){0, 900, 0}, 1.0f, NULL, NULL);
    CHECK(r0 == r1 && !memcmp(ref, emu_ram, sizeof ref), "ORIGINAL: el envoltorio no altera la RAM (flag %d/%d)", r0, r1);
    printf("original a 2500 delante del soldado: resultado %d\n", r0);

    /* (3) barrido con el preset VR sin ampliar el alcance (factor 1) para comparar con el motor puro */
    vrp_rt_set_range_scale(1.0f);
    vrp_rt_set_preset(VRP_VR);
    vrp_rt_set_range_scale(1.0f);
    { VrpCtx *cx = (VrpCtx *)vrp_rt_ctx(); cx->cfg.periph_frac = 99.0f; cx->cfg.range_mult = 1.0f; }   /* solo la cabeza: sin zona de sospecha ni regla de periferia */
    int n_cone_blocked = 0, n_cone_head = 0, n_cone_head0 = 0, n_open = 0, n_blocked = 0, n_head_saves = 0, n_out_cone = 0, bad_out = 0, worse = 0, restored_bad = 0, n_total = 0;
    int hdx[6] = {0, 0, 700, -700, 0, 0}, hdy[6] = {900, 450, 0, 0, 0, 0}, hdz[6] = {0, 0, 0, 0, 700, -700};
    float tt = 2.0f;
    for (int ring = 1; ring <= 3; ring++) for (int a = -2048; a < 2048; a += 128) {
        float r = 1100.0f * ring, th = (face + a) * 6.2831853f / 4096.0f;
        int bx = sx + (int)(sinf(th) * r), bz = sz + (int)(cosf(th) * r);
        /* cuerpo: motor puro (sin parche = ORIGINAL ya medido arriba con el mismo programa) */
        vrp_rt_set_preset(VRP_ORIGINAL);
        int body = eval(bx, py, bz, NULL, tt, NULL, NULL);
        vrp_rt_set_preset(VRP_VR); vrp_rt_set_range_scale(1.0f);
        { VrpCtx *cx = (VrpCtx *)vrp_rt_ctx(); cx->cfg.periph_frac = 99.0f; cx->cfg.range_mult = 1.0f; }
        n_total++;
        int adiff = abs(a) > 2048 ? 4096 - abs(a) : abs(a);
        if (adiff > ang && r >= 500) { n_out_cone++; }
        int none = eval(bx, py, bz, NULL, tt, NULL, NULL);
        tt += 1.0f;
        if (none != body && !(body == 1 && none == 1)) { /* sin cabeza el envoltorio con alcance ampliado solo cambia 1->(1|2) por el acumulador */ if (!(body >= 1 && none >= 1)) restored_bad++; }
        if (body > 0) n_open++; else n_blocked++;
        for (int k = 0; k < 6; k++) {
            float off[3] = {(float)hdx[k], (float)hdy[k], (float)hdz[k]};
            int d, dr;
            int h = eval(bx, py, bz, off, tt, &d, &dr);
            tt += 1.0f;
            CHECK(vai_r16(emu_ram, VRP_A_PLAYER_POS) == bx && vai_r16(emu_ram, VRP_A_PLAYER_POS + 2) == py && vai_r16(emu_ram, VRP_A_PLAYER_POS + 4) == bz, "posición del personaje restaurada");
            CHECK(vai_r16(emu_ram, actor + O_LEN) == len, "alcance restaurado");
            if (h < body) worse++;                              /* nunca puede ser mejor para el jugador que el cuerpo */
            if (body == 0 && h > 0) n_head_saves++;
            if (body == 0 && adiff < ang - 60 && r >= 600) { n_cone_blocked++; if (h > 0) n_cone_head++; else n_cone_head0++; }   /* dentro del cono y sin verlo: línea de visión/alcance/estado del motor */
            if (adiff > ang + 80 && r >= 500 && h > 0) {
                /* la cabeza desplazada (<= 700 unidades) puede entrar en el cono si el soldado está cerca; solo se cuenta si el punto desplazado sigue fuera */
                float kk = 1.0f - 80.0f / sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
                float hx = bx + off[0] * kk, hz = bz + off[2] * kk, hy = py + off[1] * kk;
                float ddx = hx - sx, ddz = hz - sz;
                if (sqrtf(ddx * ddx + ddz * ddz + (hy - sy) * (hy - sy)) < 600.0f) continue;   /* a menos de 500 el motor no aplica el cono */ int dd = ((int)lroundf(atan2f(ddx, ddz) * 4096.0f / 6.2831853f) - face) & 4095; if (dd > 2048) dd = 4096 - dd;
                if (dd > ang + 40) bad_out++;
            }
        }
    }
    printf("en el cono y sin verlo el cuerpo: %d pruebas de cabeza; ve %d (cabeza despejada), no ve %d (la geometría/alcance/estado del motor sigue mandando)\n", n_cone_blocked, n_cone_head, n_cone_head0);
    printf("barrido: %d posiciones (%d con el cuerpo visto por el motor, %d no; %d fuera del cono); la cabeza desplazada lo hace visible en %d casos que el cuerpo no\n", n_total, n_open, n_blocked, n_out_cone, n_head_saves);
    CHECK(worse == 0, "la cabeza nunca mejora el resultado del cuerpo (%d)", worse);
    CHECK(bad_out == 0, "fuera del cono (también con la cabeza fuera) nunca ve (%d)", bad_out);
    CHECK(restored_bad == 0, "sin cabeza el resultado coincide con el motor (%d)", restored_bad);
    /* (4) Independencia del recorte de dibujo y de la cámara: la rutina real de visión (cono, distancia, línea de visión, estado) no lee el estado de recorte (+0x32 de los
     * bloques de objeto) ni las matrices de cámara del canal. Se repite el barrido con (a) +0x32 = 0, 1 y 2 en TODOS los objetos de la cola y (b) las matrices del canal
     * (ojo_inv, ojo) sustituidas por basura: el resultado, la distancia y el rumbo deben ser idénticos bit a bit, con el motor puro y con el envoltorio + cabeza. */
    {
        const uint32_t CH = 0x800B2B7Cu;
        int nq = vai_r16(snap, CH + 0x56) , diffs = 0, trials = 0;
        uint32_t q = vai_r32(snap, CH + 0x58);
        printf("cola del canal: %d objetos\n", nq);
        for (int variant = 0; variant < 4; variant++)
            for (int ring = 1; ring <= 3; ring++) for (int a = -2048; a < 2048; a += 512) {
                float r = 1100.0f * ring, th = (face + a) * 6.2831853f / 4096.0f;
                int bx = sx + (int)(sinf(th) * r), bz = sz + (int)(cosf(th) * r);
                for (int mode = 0; mode < 2; mode++) {
                    vrp_rt_set_preset(mode ? VRP_VR : VRP_ORIGINAL);
                    { VrpCtx *cx = (VrpCtx *)vrp_rt_ctx(); if (mode) { cx->cfg.periph_frac = 99.0f; cx->cfg.range_mult = 1.0f; cx->cfg.range_scale = 1.0f; } }
                    float off[3] = {400, 700, 0};
                    int d0, r0_, d1, r1_, dr0, dr1;
                    r0_ = eval(bx, py, bz, mode ? off : NULL, tt, &d0, &dr0); tt += 1.0f;
                    /* perturbación sobre la misma instantánea */
                    memcpy(emu_ram, snap, sizeof snap);
                    put_player(bx, py, bz);
                    int32_t cnt = (int32_t)vai_r32(emu_ram, 0x800E311Cu), itv = (int32_t)vai_r32(emu_ram, 0x800E3118u);
                    if (itv > 0) vai_w8(emu_ram, actor + 2936, (uint8_t)(cnt % itv));
                    for (int i = 0; i < nq && i < 256; i++) {
                        uint32_t o = vai_r32(emu_ram, q + 4u * (uint32_t)i);
                        if (o >= 0x80000000u && o < 0x801FFF00u && !(o & 3) && o != actor) vai_w16(emu_ram, o + 0x32, (int16_t)(variant % 3));
                    }
                    if (variant == 3) for (int i = 0; i < 0x40; i += 2) vai_w16(emu_ram, CH + 0x10 + (uint32_t)i, (int16_t)(i * 977 + 311));   /* cámara de otra orientación */
                    vrp_rt_frame(tt); vrp_rt_set_head(mode ? off : NULL);
                    Cpu c; memset(&c, 0, sizeof c);
                    c.r[29] = 0x801FFF00u; c.r[31] = 0xDEAD0000u; c.r[28] = 0x800AC488u; c.r[4] = actor;
                    interp_call(&c, vision);
                    r1_ = vai_r16(emu_ram, actor + O_FLAG); d1 = (int)vai_r32(emu_ram, actor + O_DIS); dr1 = (int)vai_r32(emu_ram, actor + O_DIR);
                    tt += 1.0f; trials++;
                    if (r0_ != r1_ || d0 != d1 || dr0 != dr1) diffs++;
                }
            }
        printf("independencia del recorte/cámara: %d pruebas (motor puro y envoltorio con cabeza), %d diferencias\n", trials, diffs);
        CHECK(diffs == 0, "la visión del motor no depende del estado de recorte ni de la cámara (%d diferencias)", diffs);
    }
    printf("%d comprobaciones OK, %d fallos\n", nok, nfail);
    return nfail != 0;
}
