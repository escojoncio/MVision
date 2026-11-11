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
    if ((vrcam_active() || vrcam_wall_view()) && vrcam_config()->override_pad) A0 = (uint32_t)vrcam_pad_origin();   /* en la vista de pared del juego el mando conserva el rumbo con el que el jugador se pegó (el stick sigue empujando hacia la pared) */
    rt_orig(c, "vrcam_padorg");
}

/* Etapa de transformación de objetos: antes, oculta la cabeza del jugador (la etapa de recorte ya ha corrido) y pone los
 * brazos del modelo en la pose IK de las manos (vr/body; solo modifica las matrices de 6 piezas, se restauran en
 * hle_vrbody_sort al terminar el dibujado). */
void hle_vrcam_trans(Cpu *c) {
    if (vrcam_hide_head_now(emu_ram)) vrcam_hide_head(emu_ram);
    {   /* vrbody_apply corre una vez por frame DIBUJADO, no por VBlank: dt = VBlanks emulados desde el apply anterior (al menos uno) / vblank_hz */
        static uint32_t prev_vb; static int prev_ok;
        const VrCamRecord *r = vrcam_last_record();
        if (r) {
            uint32_t dv = prev_ok && r->vbl > prev_vb ? r->vbl - prev_vb : 1u;
            vrbody_set_dt((float)dv / vrcam_config()->vblank_hz);
            prev_vb = r->vbl; prev_ok = 1;
        } else prev_ok = 0;
    }
    vrbody_apply(emu_ram);
    rt_orig(c, "vrcam_trans");
}

/* Las 7 etapas del dibujado de un frame (pantalla, recorte, transformación, iluminación, primitivas, subdivisión,
 * orden): al terminar, las piezas de los brazos vuelven a las matrices que calculó el juego, para que su lógica
 * (efectos del arma, objetos en la mano, enemigos) no vea nunca la pose IK. Antes de devolverlas, respaldo de la captura de la escena nativa: si la envoltura de la etapa
 * «pantalla» no capturó en este fotograma (la entrada 0 de la tabla apunta a una copia en una overlay: introducciones), se captura aquí (ns_hook_rt.c). */
__attribute__((weak)) void ns_hook_backup_begin(void) {}      /* herramientas sin render nativo */
__attribute__((weak)) void ns_hook_backup_capture(void) {}
void hle_vrbody_sort(Cpu *c) {
    ns_hook_backup_begin();
    rt_orig(c, "vrbody_sort");
    ns_hook_backup_capture();
    vrbody_restore(emu_ram);
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
void vrcam_pack_stats(uint32_t *seen, uint32_t *fixed) { *seen = pk_seen; *fixed = pk_fixed; }
static int pk_enable = 1;
void vrcam_pack_enable(int on) { pk_enable = on; }   /* 0: la HLE solo ejecuta el original (pruebas A/B sin recorte) */
extern uint8_t emu_scratch[0x400];
void hle_vrcam_pack(Cpu *c) {
    uint32_t list = A0, pk = A1; int32_t n = (int32_t)A2;
    rt_orig(c, "vrcam_pack");
    if (!pk_enable || !vrcam_active() || n <= 0 || n > 4096 || (list & RAM_MASK) + 4u * (uint32_t)n > 0x200000u || (pk & RAM_MASK) + 52u * (uint32_t)n > 0x200000u) return;
    const int32_t hh = (int32_t)gte_cfc2(c, 26);          /* H: la división de la GTE desborda con 2*SZ <= H */
    const uint32_t zmin = (uint32_t)(hh > 0 ? hh / 2 + 4 : 80);
    for (int32_t i = 0; i < n; i++) {
        uint32_t w, hdr;
        memcpy(&w, emu_ram + ((list + 4u * (uint32_t)i) & RAM_MASK), 4);
        uint8_t *ph = emu_ram + ((pk + 52u * (uint32_t)i) & RAM_MASK);
        memcpy(&hdr, ph, 4);
        pk_seen++;
        if ((w & 0x80808080u) || (hdr & 0xFFFFu)) continue;
        const uint32_t o[4] = {(w << 2) & 0x1FCu, (w >> 6) & 0x1FCu, (w >> 14) & 0x1FCu, (w >> 22) & 0x1FCu};
        int bad = 0;
        uint32_t sz[4];
        for (int k = 0; k < 4; k++) {
            uint32_t sxy, z;
            memcpy(&sxy, emu_scratch + o[k], 4); memcpy(&z, emu_scratch + 0x200 + o[k], 4);
            int x = (int16_t)(sxy & 0xFFFF), y = (int16_t)(sxy >> 16);
            sz[k] = z & 0xFFFFu;
            if (sz[k] < zmin || x >= 1000 || x <= -1000 || y >= 1000 || y <= -1000) bad = 1;
        }
        if (!bad) continue;
        uint32_t otz = (sz[0] + sz[2]) >> 1;
        if (otz < 1) otz = 1;
        memcpy(ph, &otz, 4);
        pk_fixed++;
    }
}
