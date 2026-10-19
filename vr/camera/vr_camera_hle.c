#include <stdio.h>
#include <stdlib.h>
/* Sustitutos (HLE) de cinco funciones del motor para la cámara VR. Se enlazan con --hle hle_vrcam.txt y los
 * nombres de vr_names.json (ver Makefile). Cada sustituto delega en el código original con rt_orig() salvo en lo que
 * cambia, y solo cambia algo si vrcam_active() (decidido por el gancho de frame con el puente). Se ejecutan en el hilo
 * del juego, así que son seguros respecto a la fase del frame. */
#include <stdint.h>
#include <string.h>
#include "cpu.h"
#include "gpu.h"
#include "sys.h"
#include "vr_camera.h"
#include "vr_body.h"

#define RAM_MASK 0x1FFFFFu
#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]
#define A3 c->r[7]

static void w16(uint32_t a, int16_t v) { *(int16_t *)(emu_ram + (a & RAM_MASK & ~1u)) = v; }

/* Zona propia para los dos vectores de 4 s16 (ojo, objetivo) que se pasan al constructor: 16 bytes de memoria de kernel del
 * runtime (RAM baja libre, bios_kalloc; el juego no la usa), reservada la primera vez. Antes se escribían en la pila
 * del invitado justo por debajo de sp, que es la zona de argumentos/guardado del propio original (el original puede
 * volcar a0-a3 en [sp, sp+16) de su marco de llamada), y podía pisar sus propios vectores o datos vivos del llamador.
 * Si el runtime se reinicia (RAM y memoria de kernel nuevas), llamar a hle_vrcam_reset() para que se vuelva a reservar. */
static uint32_t vbuf;
static int in_view;      /* guarda de reentrada: el constructor no vuelve a entrar, pero si lo hiciera se usarían los argumentos originales */
void hle_vrcam_reset(void) { vbuf = 0; in_view = 0; }

/* Constructor de la matriz de cámara: (canal, *ojo, *objetivo, distancia de pantalla). Solo para el canal principal.
 * Preserva sp, a1-a3 y el resto de registros del llamador (el original conserva los suyos); solo cambian a1-a3 durante
 * la llamada y se restauran. Sin zona de vectores (sin memoria de kernel) la cámara VR no se usa, nunca la pila. */
void hle_vrcam_view(Cpu *c) {
    VrCamPlan p;
    int is_main = (A0 & RAM_MASK) == (0x800B2B7Cu & RAM_MASK);
    if (is_main && !vbuf && !in_view) vbuf = bios_kalloc(16);
    int vr = 0;
    if (is_main) vr = vrcam_plan(emu_ram, (int16_t)A3, vbuf && !in_view ? &p : NULL);   /* p == NULL: cámara del juego (y limpia "activo") */
    uint32_t a1 = A1, a2 = A2, a3 = A3;
    if (vr) {
        for (int i = 0; i < 3; i++) { w16(vbuf + 2 * i, p.from[i]); w16(vbuf + 8 + 2 * i, p.to[i]); }
        w16(vbuf + 6, 0); w16(vbuf + 14, 0);
        A1 = vbuf; A2 = vbuf + 8; A3 = (uint32_t)(int32_t)p.screen;
    }
    in_view++;
    rt_orig(c, "vrcam_view");
    in_view--;
    if (vr) { A1 = a1; A2 = a2; A3 = a3; }
    if (is_main) vrcam_commit(emu_ram, vr ? &p : NULL);
}

/* Origen angular del mando (el juego lo fija cada frame a la guiñada de su cámara + 2048). */
void hle_vrcam_padorg(Cpu *c) {
    if (vrcam_active() && vrcam_config()->override_pad) A0 = (uint32_t)vrcam_pad_origin();
    rt_orig(c, "vrcam_padorg");
}

/* Etapa de transformación de objetos: antes, oculta la cabeza del jugador (la etapa de recorte ya ha corrido) y pone los
 * brazos del modelo en la pose IK de las manos (vr/body; solo modifica las matrices de 6 piezas, se restauran en
 * hle_vrbody_sort al terminar el dibujado). */
void hle_vrcam_trans(Cpu *c) {
    if (vrcam_active() && vrcam_config()->hide_head) vrcam_hide_head(emu_ram);
    vrbody_apply(emu_ram);
    rt_orig(c, "vrcam_trans");
}

/* Las 7 etapas del dibujado de un frame (pantalla, recorte, transformación, iluminación, primitivas, subdivisión,
 * orden): al terminar, las piezas de los brazos vuelven a las matrices que calculó el juego, para que su lógica
 * (efectos del arma, objetos en la mano, enemigos) no vea nunca la pose IK. */
void hle_vrbody_sort(Cpu *c) {
    rt_orig(c, "vrbody_sort");
    vrbody_restore(emu_ram);
    vrcam_drawclip_restore(emu_ram);   /* el estado de recorte de lógica (+0x32) vuelve a su valor original antes de que corra la lógica */
}

/* Gancho de entrada del constructor de paquetes de un bloque de objeto 3D (r4 = bloque, r7 = modo): estado de recorte de DIBUJO separado del de
 * lógica (vr_camera.h). Lo llama la sustitución de esa función (vr_activity_rt.c, que es la dueña de la dirección). */
void vrcam_dc_builder_pre(Cpu *c) {
    if (!vrcam_active()) return;
    uint32_t m = c->r[7], nm = vrcam_drawclip_pre(emu_ram, c->r[4], m);
    if (nm != m) c->r[7] = nm;
}

/* Envío del frame al GPU: solo cuenta (para asociar la lista capturada con su registro de cámara). */
void hle_vrcam_draw(Cpu *c) {
    uint32_t s0 = gpu_capture_seq();
    rt_orig(c, "vrcam_draw");
    vrcam_note_draw(s0, gpu_capture_seq());
}

/* Ajuste vertical de matrices de pantalla: con la cámara VR no se aplica (las posiciones de vista salen métricas). */
void hle_vrcam_overscan(Cpu *c) {
    if (vrcam_active() && !vrcam_config()->keep_overscan) return;
    rt_orig(c, "vrcam_overscan");
}

/* Etapa de polígono de la transformación (0x8001e1fc): (lista de índices de vértice, paquetes, nº). Para cada paquete decide si se emite
 * (cabecera de 16 bits baja = z de ordenación != 0) con la prueba de orientación de la GTE (NCLIP) sobre las coordenadas de pantalla.
 * Si algún vértice está detrás de la cámara, demasiado cerca (división desbordada) o con coordenadas saturadas, esas coordenadas son
 * basura y la orientación aleatoria: el motor descartaba esos polígonos (la mitad de lo que hay alrededor del jugador, según hacia donde mire).
 * Con VR activa el original corre tal cual y después se recuperan SOLO los descartados con algún vértice poco fiable (la orientación de los
 * fiables sigue decidiendo, igual que antes); el renderer recorta contra el plano cercano y decide la cara con las posiciones 3D de vista.
 * Paquetes indirectos (bits altos de los índices) y todo lo demás: intactos. Contadores en vrcam_pack_stats(). */
static uint32_t pk_seen, pk_fixed;
static uint32_t pk_ind, pk_ind_drop, pk_dir_drop, pk_reg;   /* indirectos vistos / descartados por el original, directos descartados (tras la re-emisión), registrados */
void vrcam_pack_stats(uint32_t *seen, uint32_t *fixed) { *seen = pk_seen; *fixed = pk_fixed; }
void vrcam_pack_stats2(uint32_t *indirect, uint32_t *indirect_dropped, uint32_t *direct_dropped, uint32_t *registered) { *indirect = pk_ind; *indirect_dropped = pk_ind_drop; *direct_dropped = pk_dir_drop; *registered = pk_reg; }
static int pk_enable = 1;
static uint32_t pk_front_rescue;   /* frontales exactos que el motor había descartado y se emiten */
static uint32_t pk_bad_cull;      /* poco fiables descartados por cara trasera exacta */
static uint32_t pk_val_agree, pk_val_dis;   /* validación: orientación exacta vs la del motor en paquetes fiables con los tres vértices delante */
static int pk_val_on = 1;
static uint32_t pk_tab[2][2][2];
void vrcam_pack_tab(uint32_t out[8]) { memcpy(out, pk_tab, sizeof pk_tab); }
static int pk_exact_cull = -1;    /* MV_BADCULL=0 apaga el descarte exacto (pruebas A/B) */
void vrcam_pack_stats3(uint32_t *bad_cull, uint32_t *rescued, uint32_t *agree, uint32_t *disagree) { *bad_cull = pk_bad_cull; *rescued = pk_front_rescue; *agree = pk_val_agree; *disagree = pk_val_dis; }
static int pk_ind_reemit = -1;   /* MV_INDFIX=0 apaga la re-emisión de indirectos (pruebas A/B) */
void vrcam_pack_enable(int on) { pk_enable = on; }   /* 0: la HLE solo ejecuta el original (pruebas A/B sin recorte) */
extern uint8_t emu_scratch[0x400];
void hle_vrcam_pack(Cpu *c) {
    uint32_t list = A0, pk = A1; int32_t n = (int32_t)A2;
    rt_orig(c, "vrcam_pack");
    if (n <= 0 || n > 4096 || (list & RAM_MASK) + 4u * (uint32_t)n > 0x200000u || (pk & RAM_MASK) + 52u * (uint32_t)n > 0x200000u) return;
    if (pk_exact_cull < 0) { const char *e = getenv("MV_BADCULL"); pk_exact_cull = !(e && e[0] == '0'); }
    if (pk_ind_reemit < 0) { const char *e = getenv("MV_INDFIX"); pk_ind_reemit = !(e && e[0] == '0'); }
    const int do_fix = pk_enable && vrcam_active();
    const int do_reg = gte_hp_enabled() && vrcam_active();
    if (!do_fix && !do_reg) return;
    const int32_t hh = (int32_t)gte_cfc2(c, 26);          /* H: la división de la GTE desborda con 2*SZ <= H */
    const uint32_t zmin = (uint32_t)(hh > 0 ? hh / 2 + 4 : 80);
    uint32_t base = 0;
    if (do_reg) {   /* ranuras del scratchpad usadas por esta lista -> vértices exactos del log de la GTE */
        uint32_t mx = 0;
        for (int32_t i = 0; i < n; i++) {
            uint32_t w; memcpy(&w, emu_ram + ((list + 4u * (uint32_t)i) & RAM_MASK), 4);
            if (w & 0x80808080u) continue;
            uint32_t o[4] = {(w << 2) & 0x1FCu, (w >> 6) & 0x1FCu, (w >> 14) & 0x1FCu, (w >> 22) & 0x1FCu};
            for (int k = 0; k < 4; k++) if (o[k] > mx) mx = o[k];
        }
        base = gte_hp_align(emu_scratch, (int)(mx >> 2) + 1);
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t w, hdr;
        memcpy(&w, emu_ram + ((list + 4u * (uint32_t)i) & RAM_MASK), 4);
        uint8_t *ph = emu_ram + ((pk + 52u * (uint32_t)i) & RAM_MASK);
        memcpy(&hdr, ph, 4);
        const uint32_t hdr0 = hdr;   /* decisión original del motor (para los diagnósticos) */
        pk_seen++;
        if (w & 0x80808080u) {
            pk_ind++;
            if (!(hdr & 0xFFFFu)) {
                pk_ind_drop++;
                if (do_fix && pk_ind_reemit) { uint32_t one = (hdr & ~0xFFFFu) | 1u; memcpy(ph, &one, 4); }
            }
            continue;
        }
        const uint32_t o[4] = {(w << 2) & 0x1FCu, (w >> 6) & 0x1FCu, (w >> 14) & 0x1FCu, (w >> 22) & 0x1FCu};
        if (!(hdr & 0xFFFFu) && do_fix) {
            /* El motor descartó el paquete (NCLIP <= 0 sobre las coordenadas de pantalla). Con las posiciones de vista EXACTAS (log de la GTE) se decide por la
             * orientación real (signo del triple producto de los tres vértices que usa el motor: índices 0, 1 y 3), no por el NCLIP:
             *   - frontal -> se emite aunque el NCLIP fuera 0 o aleatorio: los polígonos lejanos o rasantes (área de pantalla < 1 píxel tras redondear a enteros, ~7 % de
             *     los frontales) o con vértices detrás/saturados se perdían, y con la cabeza inclinada y la vista ampliada SÍ se ven;
             *   - trasero de una cara -> se descarta igual que el motor (si no, aparecería al girar lo que el juego nunca dibuja); de dos caras -> se emite.
             * Sin posiciones exactas (sin registro): solo se recuperan los de vértices poco fiables (SZ < H/2+4 o coordenadas saturadas), como antes. */
            int emit = 0, decided = 0;
            uint32_t sz[4];
            for (int k = 0; k < 4; k++) { uint32_t z; memcpy(&z, emu_scratch + 0x200 + o[k], 4); sz[k] = z & 0xFFFFu; }
            if (base && pk_exact_cull) {
                GpuHpInfo q[3];
                if (gte_hp_log_get(base + (o[0] >> 2), &q[0]) && gte_hp_log_get(base + (o[1] >> 2), &q[1]) && gte_hp_log_get(base + (o[3] >> 2), &q[2])) {
                    double det = (double)q[0].vx * ((double)q[1].vy * q[2].vz - (double)q[1].vz * q[2].vy)
                               - (double)q[0].vy * ((double)q[1].vx * q[2].vz - (double)q[1].vz * q[2].vx)
                               + (double)q[0].vz * ((double)q[1].vx * q[2].vy - (double)q[1].vy * q[2].vx);
                    uint16_t two; memcpy(&two, emu_scratch + 0x1FC, 2);
                    decided = 1;
                    emit = det > 0.0 || (two && det < 0.0);
                    if (!emit) pk_bad_cull++;
                    else if (det > 0.0) pk_front_rescue++;
                }
            }
            if (!decided) {
                for (int k = 0; k < 4; k++) {
                    uint32_t sxy; memcpy(&sxy, emu_scratch + o[k], 4);
                    int x = (int16_t)(sxy & 0xFFFF), y = (int16_t)(sxy >> 16);
                    if (sz[k] < zmin || x >= 1000 || x <= -1000 || y >= 1000 || y <= -1000) emit = 1;
                }
            }
            if (emit) {
                uint32_t otz = (sz[0] + sz[2]) >> 1;
                if (otz < 1) otz = 1;
                memcpy(ph, &otz, 4);
                hdr = otz;
                pk_fixed++;
            }
        }
        if (base && pk_val_on && !(w & 0x80808080u)) {   /* tabla de diagnóstico: orientación exacta x emitido x una/dos caras, solo vértices normales y delante */
            GpuHpInfo q[3];
            if (gte_hp_log_get(base + (o[0] >> 2), &q[0]) && gte_hp_log_get(base + (o[1] >> 2), &q[1]) && gte_hp_log_get(base + (o[3] >> 2), &q[2]) &&
                !q[0].flags && !q[1].flags && !q[2].flags && q[0].vz > 0 && q[1].vz > 0 && q[2].vz > 0) {
                double det = (double)q[0].vx * ((double)q[1].vy * q[2].vz - (double)q[1].vz * q[2].vy)
                           - (double)q[0].vy * ((double)q[1].vx * q[2].vz - (double)q[1].vz * q[2].vx)
                           + (double)q[0].vz * ((double)q[1].vx * q[2].vy - (double)q[1].vy * q[2].vx);
                uint16_t two; memcpy(&two, emu_scratch + 0x1FC, 2);
                pk_tab[det > 0.0][(hdr0 & 0xFFFFu) != 0][two != 0]++;
            }
        }
        if (base && (hdr0 & 0xFFFFu) && pk_val_on) {   /* validación del signo exacto: paquete fiable que sí se emite, con los tres vértices delante */
            GpuHpInfo q[3];
            if (gte_hp_log_get(base + (o[0] >> 2), &q[0]) && gte_hp_log_get(base + (o[1] >> 2), &q[1]) && gte_hp_log_get(base + (o[3] >> 2), &q[2]) &&
                !q[0].flags && !q[1].flags && !q[2].flags && q[0].vz > 0 && q[1].vz > 0 && q[2].vz > 0) {
                double det = (double)q[0].vx * ((double)q[1].vy * q[2].vz - (double)q[1].vz * q[2].vy)
                           - (double)q[0].vy * ((double)q[1].vx * q[2].vz - (double)q[1].vz * q[2].vx)
                           + (double)q[0].vz * ((double)q[1].vx * q[2].vy - (double)q[1].vy * q[2].vx);
                int front_hw = !(hdr0 & 0x10000u);
                if ((det > 0.0) == front_hw) pk_val_agree++; else pk_val_dis++;
            }
        }
        if (!(hdr & 0xFFFFu)) pk_dir_drop++;
        if (base && (hdr & 0xFFFFu)) {   /* paquete que se enviará al GPU: registrar sus vértices exactos */
            int sx[4], sy[4]; uint32_t id[4];
            for (int k = 0; k < 4; k++) {
                uint32_t sxy; memcpy(&sxy, emu_scratch + o[k], 4);
                sx[k] = (int16_t)(sxy & 0xFFFF); sy[k] = (int16_t)(sxy >> 16); id[k] = base + (o[k] >> 2);
            }
            if (gte_hp_poly_reg(4, sx, sy, id) || gte_hp_poly_reg(3, sx, sy, id)) pk_reg++;
        }
    }
}
