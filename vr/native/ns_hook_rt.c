/* ns_hook_rt: ver ns_hook_rt.h. */
#include "ns_hook_rt.h"
#include <string.h>
#include "ns_hook.h"
#include "rtpatch.h"

static int installed;
static NsStageCb stage_cb;

void ns_hook_set_stage_cb(NsStageCb cb) { stage_cb = cb; }
int ns_hook_installed(void) { return installed; }

static uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, emu_ram + (a & 0x1FFFFFu), 4); return v; }

/* Envoltura: el código original calcula las matrices de mundo de todos los objetos del canal; después se captura. a0 se lee antes (la original puede
 * reutilizar registros). Otros canales pasan sin tocar. */
static void stage_wrap(Cpu *c) {
    uint32_t chan = c->r[4];
    rt_call_orig(c, NS_STAGE_ADDR);
    if (chan != NS_MAIN_CHANNEL) return;
    ns_capture(emu_ram, emu_scratch);
    if (stage_cb) stage_cb(c);
}

int ns_hook_install(void) {
    if (installed) return NS_HOOK_OK;
    uint32_t w = rd(NS_STAGE_ADDR);
    if ((w >> 16) != 0x27BDu || !(w & 0x8000u)) return NS_HOOK_BAD_CODE;      /* addiu sp, sp, -N */
    uint32_t e = rd(NS_STAGE_TABLE);
    if (e != 0 && e != NS_STAGE_ADDR) return NS_HOOK_BAD_TABLE;               /* otra rutina ya ocupa la entrada */
    RtPatchGuard g = {12, rt_code_hash(NS_STAGE_ADDR, 12)};
    if (rt_patch_fn(NS_STAGE_ADDR, stage_wrap, &g) != RT_PATCH_OK) return NS_HOOK_PATCH_FAIL;
    installed = 1;
    return NS_HOOK_OK;
}

void ns_hook_uninstall(void) {
    if (!installed) return;
    rt_unpatch(NS_STAGE_ADDR);
    installed = 0;
}
