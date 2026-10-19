/* Ver vr_activity_rt.h. */
#include "vr_activity_rt.h"
#include <string.h>
#include "rtpatch.h"


static VrActivity *G;
static VraRtStats S;

void vra_rt_attach(VrActivity *a) { G = a; }
VrActivity *vra_rt_instance(void) { return G; }
VraRtStats vra_rt_stats(void) { return S; }

/* a0 = SVECTOR*, a1 = nº de puntos. El original proyecta con la cámara original y escribe `pad`. */
void vra_rt_pointcheck(Cpu *c) {
    uint32_t pts = c->r[4], n = c->r[5];
    rt_call_orig(c, VRA_EU_POINTCHECK);
    S.pc_calls++;
    if (G && G->active && G->cfg.mode[VRA_CLASS_SOLDIER] == VRA_VR && (int32_t)n > 0 && n <= 256)
        S.pc_points_changed += (uint64_t)vra_pointcheck_fixup(G, psx_ram, pts, (int)n);
}

/* r4 = bloque de objeto 3D, r7 = clip_state calculado por el motor. Lee flag y grupo del propio bloque. */
uint32_t vra_rt_clip_store(Cpu *c, uint32_t objs, uint32_t mode) {
    (void)c;
    S.clip_calls++;
    if (!G || !G->active || mode != 0) return mode;
    uint32_t o = objs & 0x1FFFFFFFu, flag = 0, group = 0;
    if ((objs & 0xE0000000u) != 0x80000000u || (objs & 3) || o + 0x34u > 0x200000u) return mode;   /* solo KSEG0 alineado */
    memcpy(&flag, psx_ram + o + 0x28, 4);
    uint16_t g16;
    memcpy(&g16, psx_ram + o + 0x2c, 2);
    group = g16;
    int m = vra_clip_state(G, objs, flag, group, (int)mode);
    if (m != (int)mode) S.clip_changed++;
    return (uint32_t)m;
}

/* Parche de entrada de la función que consume el estado de recorte justo después de calcularlo (a0 = bloque, a3 = clip_state;
 * la guarda en un registro salvado y NO la relee de RAM, por eso hay que cambiar r7 además del campo). */
void vra_rt_clip_draw(Cpu *c) {
    uint32_t objs = c->r[4];
    if (G && G->active && c->r[7] == 0) {
        uint32_t m = vra_rt_clip_store(c, objs, 0);
        if (m) {
            uint16_t v = (uint16_t)m;
            memcpy(psx_ram + (objs & 0x1FFFFFFFu) + 0x32, &v, 2);   /* vra_rt_clip_store ya validó objs */
            c->r[7] = m;
        }
    }
    rt_call_orig(c, VRA_EU_CLIP_DRAW);
}
int vra_rt_install_fnpatch(void) {
    const RtPatchGuard g1 = {VRA_EU_POINTCHECK_NW, VRA_EU_POINTCHECK_HASH}, g2 = {VRA_EU_CLIP_DRAW_NW, VRA_EU_CLIP_DRAW_HASH};
    int r = rt_patch_fn(VRA_EU_POINTCHECK, vra_rt_pointcheck, &g1);
    if (r) return r;
    r = rt_patch_fn(VRA_EU_CLIP_DRAW, vra_rt_clip_draw, &g2);
    if (r == RT_PATCH_HASH) {   /* con el parche de recorte de la cámara VR aplicado el código es otro */
        const RtPatchGuard g3 = {VRA_EU_CLIP_DRAW_NW, VRA_EU_CLIP_DRAW_HASH_VRCAM};
        r = rt_patch_fn(VRA_EU_CLIP_DRAW, vra_rt_clip_draw, &g3);
    }
    if (r) { rt_unpatch(VRA_EU_POINTCHECK); return r; }
    return 0;
}
void vra_rt_uninstall(void) {
    rt_unpatch(VRA_EU_POINTCHECK);
    rt_unpatch(VRA_EU_CLIP_DRAW);
}
