/* Ver vr_activity_rt.h. */
#include "vr_activity_rt.h"
#include <string.h>

int rt_patch(uint32_t addr, FuncPtr fn);
void rt_unpatch(uint32_t addr);
void rt_call_orig(Cpu *c, uint32_t addr);
int rt_hook_at(uint32_t addr, uint32_t (*fn)(Cpu *c, uint32_t r4, uint32_t r7));
void rt_unhook_at(uint32_t addr);

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

/* r4 = bloque de objeto 3D, r7 = bound_mode calculado por el motor. Lee flag y grupo del propio bloque. */
uint32_t vra_rt_bound_store(Cpu *c, uint32_t objs, uint32_t mode) {
    (void)c;
    S.bound_calls++;
    if (!G || !G->active || mode != 0) return mode;
    uint32_t o = objs & 0x1FFFFFFFu, flag = 0, group = 0;
    if ((objs & 0xE0000000u) != 0x80000000u || (objs & 3) || o + 0x34u > 0x200000u) return mode;   /* solo KSEG0 alineado */
    memcpy(&flag, psx_ram + o + 0x28, 4);
    uint16_t g16;
    memcpy(&g16, psx_ram + o + 0x2c, 2);
    group = g16;
    int m = vra_bound_mode(G, objs, flag, group, (int)mode);
    if (m != (int)mode) S.bound_changed++;
    return (uint32_t)m;
}

/* Alternativa SIN tocar código generado: parche de entrada de la función que consume el bound_mode justo después
 * del almacén (0x800188a4, a0 = bloque, a3 = bound_mode; la guarda en un registro salvado y NO la relee de RAM,
 * por eso hay que cambiar r7 además del campo). Equivale al gancho a mitad de función. */
void vra_rt_bound_draw(Cpu *c) {
    uint32_t objs = c->r[4];
    if (G && G->active && c->r[7] == 0) {
        uint32_t m = vra_rt_bound_store(c, objs, 0);
        if (m) {
            uint16_t v = (uint16_t)m;
            memcpy(psx_ram + (objs & 0x1FFFFFFFu) + 0x32, &v, 2);   /* vra_rt_bound_store ya validó objs */
            c->r[7] = m;
        }
    }
    rt_call_orig(c, VRA_EU_BOUND_DRAW);
}
int vra_rt_install_fnpatch(void) {
    if (rt_patch(VRA_EU_POINTCHECK, vra_rt_pointcheck)) return -1;
    if (rt_patch(VRA_EU_BOUND_DRAW, vra_rt_bound_draw)) { rt_unpatch(VRA_EU_POINTCHECK); return -2; }
    return 0;
}

int vra_rt_install(void) {
    if (rt_patch(VRA_EU_POINTCHECK, vra_rt_pointcheck)) return -1;
    if (rt_hook_at(VRA_EU_BOUND_STORE_AT, vra_rt_bound_store)) { rt_unpatch(VRA_EU_POINTCHECK); return -2; }
    return 0;
}
void vra_rt_uninstall(void) {
    rt_unpatch(VRA_EU_POINTCHECK);
    rt_unhook_at(VRA_EU_BOUND_STORE_AT);
    rt_unpatch(VRA_EU_BOUND_DRAW);
}
