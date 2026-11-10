/* Reglas de clasificación por procedencia (vr_prov.h): tablas de nombres de actor (ProvEntry.name, nodo+20) y de rutinas (ProvEntry.fn). Medido del juego (RAM del actor):
 *  - menuman.c (nodo 0x800be4e0, rutina 0x800394f8): gestor del HUD. Llama por jalr a los componentes de una tabla (0x800404e4, 0x8003f918, 0x8003d90c, 0x8003c238, 0x800431d4 y los pares de
 *    0x8009f460-0x8009f4a0); sus primitivas viven en MENUBUF (0x800ba4e0 / 0x800bc4e0). Esos componentes no son actores (su a0 no es un nodo con +8 = su rutina): llegan sin nombre y se
 *    reconocen por la dirección. El nombre de actor gana a la dirección cuando existen los dos.
 *  - cinema.c (rutina 0x800e1050): bandas de cinemática. motse.c: viñeta. telop.c y famaslit.c: créditos. scope.c (0x80063d54) y sight.c (0x800726bc): prismáticos / mira.
 *  - gas_efct.c (rutina 0x800de1f0 en su overlay: anillo del gas, 16 polígonos semitransparentes) y fadeio.c (0x800e15e8: fundido): efectos de pantalla que el renderer nativo hace de la RAM
 *    (ns_fx / ns_screen). Solo por nombre de actor: sus rutinas viven en overlays y la dirección sola no los identifica. Esta es la regla BASE: vpc_class_op solo los deja en FX_SCREEN
 *    mientras el anfitrión garantiza que el nativo pinta ese efecto (VrProvLearn.covered); si no, UNKNOWN.
 *  - Otros actores (o2_damge.c, breath.c: efectos; cualquier otro) y lo que no tiene procedencia: UNKNOWN (van al HUD, como antes, y se cuentan). Única excepción, en vpc_class_op: la
 *    primitiva que encadenó la ordenación del canal 3D y cuyo paquete tiene un elemento que el renderer nativo dibuja (VrProvLearn.covered) es WORLD; sus primitivas de pantalla, sin ese
 *    elemento, siguen siendo UNKNOWN. */
#include <string.h>
#include "vr_prov.h"

static const char *const N_LETTERBOX[] = {"cinema.c", NULL};
static const char *const N_VIGNETTE[] = {"motse.c", NULL};
static const char *const N_FULL[] = {"telop.c", "famaslit.c", NULL};
static const char *const N_SCOPE[] = {"scope.c", "sight.c", NULL};
static const char *const N_HUD[] = {"menuman.c", NULL};
static const char *const N_GAS[] = {"gas_efct.c", NULL};
static const char *const N_FADE[] = {"fadeio.c", NULL};

static const uint32_t F_LETTERBOX[] = {0x800e1050u, 0};
static const uint32_t F_SCOPE[] = {0x80063d54u, 0x800726bcu, 0};
static const uint32_t F_HUD[] = {
    0x800394f8u,                                                       /* menuman.c */
    0x800404e4u, 0x8003f918u, 0x8003d90cu, 0x8003c238u, 0x800431d4u,   /* componentes de la tabla */
    0x8003dc5cu, 0x8003c360u, 0x80043774u, 0x8003dbb0u, 0x8003fbc0u, 0x800407a0u, 0x800438e0u, 0x8004a8c4u, 0x8003c444u,   /* pares de 0x8009f460-0x8009f4a0 */
    0x80043790u, 0x8003dc3cu, 0x8003fc48u, 0x800407f0u, 0x80043a60u, 0};

/* Rutinas que solo ENCADENAN paquetes que escribieron otros (no dibujan): etapa de ordenación del canal principal (tabla de etapas 0x8009E534, docs/escena_nativa.md §10) y la ordenación de canales que la llama. */
static const uint32_t F_CHAIN[] = {0x8001da18u, 0x80018304u, 0};

static int has_name(const ProvEntry *e, const char *const *names) {
    if (!e->name[0]) return 0;
    for (; *names; names++) if (strstr(e->name, *names)) return 1;
    return 0;
}
static int has_fn(const ProvEntry *e, const uint32_t *fns) {
    if (!e->fn) return 0;
    for (; *fns; fns++) if (e->fn == *fns) return 1;
    return 0;
}

int vpc_is_chain(const ProvEntry *e) { return e && has_fn(e, F_CHAIN); }
int vpc_screen_kind(const ProvEntry *e) { return !e ? -1 : has_name(e, N_GAS) ? VPC_COV_GAS : has_name(e, N_FADE) ? VPC_COV_FADE : -1; }

int vpc_rule(const ProvEntry *e, const GpuOp *o) {
    if (!e) return VPC_UNKNOWN;
    if (has_name(e, N_LETTERBOX) || has_fn(e, F_LETTERBOX)) return VPC_FX_LETTERBOX;
    if (has_name(e, N_VIGNETTE)) return VPC_FX_VIGNETTE;
    if (has_name(e, N_FULL)) return VPC_FULL;
    if (has_name(e, N_SCOPE) || has_fn(e, F_SCOPE)) return o && (o->flags & GPU_F_TEXTURED) ? VPC_SCOPE_TEXT : VPC_SCOPE_FRAME;
    if (has_name(e, N_HUD) || has_fn(e, F_HUD)) return VPC_HUD_WORLD;
    if (vpc_screen_kind(e) >= 0) return VPC_FX_SCREEN;
    return VPC_UNKNOWN;
}
