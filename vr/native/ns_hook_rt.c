/* ns_hook_rt: ver ns_hook_rt.h. */
#include "ns_hook_rt.h"
#include <stdatomic.h>
#include <string.h>
#include "ns_hook.h"
#include "rtpatch.h"

static _Atomic int installed;   /* lo lee la interfaz (mv_vrhost_native_available) desde otro hilo */
static _Atomic unsigned stale_n;
static uint64_t patch_hash;
static int last_rc = NS_HOOK_PATCH_FAIL - 1;
static NsStageCb stage_cb;

void ns_hook_set_stage_cb(NsStageCb cb) { stage_cb = cb; }
static NsPreCb pre_cb;
void ns_hook_set_precapture_cb(NsPreCb cb) { pre_cb = cb; }
int ns_hook_installed(void) { return atomic_load(&installed); }
unsigned ns_hook_stale_count(void) { return atomic_load(&stale_n); }
int ns_hook_last_rc(void) { return last_rc; }

static uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, emu_ram + (a & 0x1FFFFFu), 4); return v; }

/* Envoltura: el código original calcula las matrices de mundo de todos los objetos del canal; después se captura. a0 se lee antes (la original puede
 * reutilizar registros). Otros canales pasan sin tocar. `addr` = dirección de la rutina original (la estándar o la copia en overlay). */
static void wrap_common(Cpu *c, uint32_t addr, int dyn) {
    uint32_t chan = c->r[4];
    rt_call_orig(c, addr);
    if (chan != NS_MAIN_CHANNEL) return;
    if (pre_cb) pre_cb(emu_ram);
    if (dyn) ns_capture_dyn(emu_ram, emu_scratch); else ns_capture(emu_ram, emu_scratch);
    if (stage_cb) stage_cb(c);
}
static void stage_wrap(Cpu *c) { wrap_common(c, NS_STAGE_ADDR, 0); }

/* Envoltura dinámica: la entrada 0 de la tabla de etapas apunta a otra rutina (la copia de la overlay de la introducción, 0x8008240c). Solo hay una a la vez: su dirección va en dyn_addr. */
static uint32_t dyn_addr;
static uint64_t dyn_hash;
static _Atomic int dyn_on;
static _Atomic unsigned dyn_stale_n;
static void dyn_remove(void);
static void dyn_wrap(Cpu *c) { wrap_common(c, dyn_addr, 1); }

/* Respaldo (ver ns_hook.h): lo llama hle_vrbody_sort (vr/camera): begin antes del bucle de 7 etapas y capture tras él, antes de vrbody_restore. El extractor no lee el contador de visibilidad
 * (+0x4C: lo tocan hide_head, own_hands y hide_near) ni la matriz de pantalla; las matrices de mundo de los brazos/cuerpo con la pose IK ya están en la RAM y son las mismas que ns_hook_post_ik copia
 * a la escena de la envoltura, y la malla de la mano (puño) queda igual (fist_apply no se repite). Por eso se copia la IK con ns_hook_post_ik. NO se llama a stage_cb: el volcado del punto de
 * enganche lee los registros de la GTE de la CPU, que aquí son los del final de las 7 etapas y no los de la etapa «pantalla». */
void ns_hook_backup_begin(void) { ns_capture_backup_begin(); }
void ns_hook_backup_capture(void) {
    if (!ns_capture_backup_due()) return;
    if (pre_cb) pre_cb(emu_ram);
    ns_capture_backup(emu_ram, emu_scratch);
    ns_hook_post_ik(emu_ram);
}
uint32_t ns_hook_stage_entry(void) { return rd(NS_STAGE_TABLE); }

int ns_hook_install(void) {
    if (atomic_load(&installed)) return NS_HOOK_OK;
    uint32_t w = rd(NS_STAGE_ADDR);
    if ((w >> 16) != 0x27BDu || !(w & 0x8000u)) return last_rc = NS_HOOK_BAD_CODE;      /* addiu sp, sp, -N */
    uint32_t e = rd(NS_STAGE_TABLE);
    if (e != 0 && e != NS_STAGE_ADDR) return last_rc = NS_HOOK_BAD_TABLE;               /* otra rutina ya ocupa la entrada */
    RtPatchGuard g = {12, rt_code_hash(NS_STAGE_ADDR, 12)};
    if (rt_patch_fn(NS_STAGE_ADDR, stage_wrap, &g) != RT_PATCH_OK) return last_rc = NS_HOOK_PATCH_FAIL;
    patch_hash = g.hash;
    atomic_store(&installed, 1);
    return last_rc = NS_HOOK_OK;
}

void ns_hook_uninstall(void) {
    dyn_remove();
    if (!atomic_load(&installed)) return;
    rt_unpatch(NS_STAGE_ADDR);
    atomic_store(&installed, 0);
}

static void dyn_remove(void) {
    if (!atomic_load(&dyn_on)) return;
    rt_unpatch(dyn_addr);
    atomic_store(&dyn_on, 0);
    dyn_addr = 0; dyn_hash = 0;
}
/* Parche dinámico de la rutina a la que apunte la entrada 0 de la tabla cuando NO es la estándar: la retira si la entrada cambia, el parche caduca o el código de la overlay cambia (hash); la instala
 * (con su propio salto al original, rt_call_orig(dyn_addr)) si la entrada apunta a código con prólogo de pila. El respaldo (hle_vrbody_sort) sigue de red. */
static void dyn_poll(void) {
    uint32_t e = rd(NS_STAGE_TABLE);
    if (atomic_load(&dyn_on) && (e != dyn_addr || rt_patch_stale(dyn_addr) || rt_code_hash(dyn_addr, 12) != dyn_hash)) {
        dyn_remove();
        atomic_fetch_add(&dyn_stale_n, 1);
    }
    if (atomic_load(&dyn_on) || e == 0 || e == NS_STAGE_ADDR || (e & 3u) || (e >> 24) != 0x80u) return;
    uint32_t w = rd(e);
    if ((w >> 16) != 0x27BDu || !(w & 0x8000u)) return;        /* addiu sp, sp, -N */
    RtPatchGuard g = {12, rt_code_hash(e, 12)};
    dyn_addr = e;
    if (rt_patch_fn(e, dyn_wrap, &g) != RT_PATCH_OK) { dyn_addr = 0; return; }
    dyn_hash = g.hash;
    atomic_store(&dyn_on, 1);
}
int ns_hook_dyn_installed(void) { return atomic_load(&dyn_on); }
uint32_t ns_hook_dyn_addr(void) { return atomic_load(&dyn_on) ? dyn_addr : 0; }
unsigned ns_hook_dyn_stale_count(void) { return atomic_load(&dyn_stale_n); }

/* Reintento y detección de parche caducado: otra overlay encima de la etapa deja el parche inerte (rt_patch_stale); se retira y, si el código nuevo
 * tiene la forma esperada, se vuelve a instalar con su hash. Si la instalación falló (p. ej. la etapa aún no estaba cargada), se reintenta. */
int ns_hook_poll(void) {
    if (atomic_load(&installed) && (rt_patch_stale(NS_STAGE_ADDR) || rt_code_hash(NS_STAGE_ADDR, 12) != patch_hash)) {   /* la marca de caducidad solo se actualiza al entrar: se comprueba también el hash */
        rt_unpatch(NS_STAGE_ADDR);
        atomic_store(&installed, 0);
        atomic_fetch_add(&stale_n, 1);
    }
    if (!atomic_load(&installed)) ns_hook_install();
    dyn_poll();
    return atomic_load(&installed);
}
