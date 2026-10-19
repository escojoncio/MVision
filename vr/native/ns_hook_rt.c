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
 * reutilizar registros). Otros canales pasan sin tocar. */
static void stage_wrap(Cpu *c) {
    uint32_t chan = c->r[4];
    rt_call_orig(c, NS_STAGE_ADDR);
    if (chan != NS_MAIN_CHANNEL) return;
    if (pre_cb) pre_cb(emu_ram);
    ns_capture(emu_ram, emu_scratch);
    if (stage_cb) stage_cb(c);
}

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
    if (!atomic_load(&installed)) return;
    rt_unpatch(NS_STAGE_ADDR);
    atomic_store(&installed, 0);
}

/* Reintento y detección de parche caducado: otra overlay encima de la etapa deja el parche inerte (rt_patch_stale); se retira y, si el código nuevo
 * tiene la forma esperada, se vuelve a instalar con su hash. Si la instalación falló (p. ej. la etapa aún no estaba cargada), se reintenta. */
int ns_hook_poll(void) {
    if (atomic_load(&installed) && (rt_patch_stale(NS_STAGE_ADDR) || rt_code_hash(NS_STAGE_ADDR, 12) != patch_hash)) {   /* la marca de caducidad solo se actualiza al entrar: se comprueba también el hash */
        rt_unpatch(NS_STAGE_ADDR);
        atomic_store(&installed, 0);
        atomic_fetch_add(&stale_n, 1);
    }
    if (!atomic_load(&installed)) ns_hook_install();
    return atomic_load(&installed);
}
