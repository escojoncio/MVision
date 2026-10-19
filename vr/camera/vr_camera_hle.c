/* Sustitutos (HLE) de cinco funciones del motor para la cámara VR. Se enlazan con --hle hle_vrcam.txt y los
 * nombres de vr_names.json (ver Makefile). Cada sustituto delega en el código original con rt_orig() salvo en lo que
 * cambia, y solo cambia algo si vrcam_active() (decidido por el gancho de frame con el puente). Se ejecutan en el hilo
 * del juego, así que son seguros respecto a la fase del frame. */
#include <stdint.h>
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

static void w16(uint32_t a, int16_t v) { *(int16_t *)(psx_ram + (a & RAM_MASK & ~1u)) = v; }

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
    if (is_main) vr = vrcam_plan(psx_ram, (int16_t)A3, vbuf && !in_view ? &p : NULL);   /* p == NULL: cámara del juego (y limpia "activo") */
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
    if (is_main) vrcam_commit(psx_ram, vr ? &p : NULL);
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
    if (vrcam_active() && vrcam_config()->hide_head) vrcam_hide_head(psx_ram);
    vrbody_apply(psx_ram);
    rt_orig(c, "vrcam_trans");
}

/* Las 7 etapas del dibujado de un frame (pantalla, recorte, transformación, iluminación, primitivas, subdivisión,
 * orden): al terminar, las piezas de los brazos vuelven a las matrices que calculó el juego, para que su lógica
 * (efectos del arma, objetos en la mano, enemigos) no vea nunca la pose IK. */
void hle_vrbody_sort(Cpu *c) {
    rt_orig(c, "vrbody_sort");
    vrbody_restore(psx_ram);
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
