/* GENERADO por tools/recomp/hletab.py desde runtime/hle*.txt: no editar (make -C runtime hle_table). */
#include "hlebind.h"

void hle_cb_dma(Cpu *c);
void hle_cb_irq(Cpu *c);
void hle_cb_reset(Cpu *c);
void hle_ctl_act_align(Cpu *c);
void hle_ctl_act_set(Cpu *c);
void hle_ctl_bios_init(Cpu *c);
void hle_ctl_bios_start(Cpu *c);
void hle_ctl_bios_stop(Cpu *c);
void hle_ctl_chk_multi(Cpu *c);
void hle_ctl_enable(Cpu *c);
void hle_ctl_info_act(Cpu *c);
void hle_ctl_info_comb(Cpu *c);
void hle_ctl_info_mode(Cpu *c);
void hle_ctl_init_direct(Cpu *c);
void hle_ctl_main_mode(Cpu *c);
void hle_ctl_start(Cpu *c);
void hle_ctl_state(Cpu *c);
void hle_ctl_stop(Cpu *c);
void hle_disc_ctl(Cpu *c);
void hle_disc_ctl_b(Cpu *c);
void hle_disc_ctl_f(Cpu *c);
void hle_disc_flush(Cpu *c);
void hle_disc_init(Cpu *c);
void hle_disc_mix(Cpu *c);
void hle_disc_ready(Cpu *c);
void hle_disc_reset(Cpu *c);
void hle_disc_sector(Cpu *c);
void hle_disc_sync(Cpu *c);
void hle_frame_cb(Cpu *c);
void hle_frame_wait(Cpu *c);
void hle_gfx_clear(Cpu *c);
void hle_gfx_clear2(Cpu *c);
void hle_gfx_copy(Cpu *c);
void hle_gfx_copy2(Cpu *c);
void hle_gfx_debug(Cpu *c);
void hle_gfx_dispmask(Cpu *c);
void hle_gfx_download(Cpu *c);
void hle_gfx_download2(Cpu *c);
void hle_gfx_field(Cpu *c);
void hle_gfx_list(Cpu *c);
void hle_gfx_list2(Cpu *c);
void hle_gfx_list_clear(Cpu *c);
void hle_gfx_list_clear_r(Cpu *c);
void hle_gfx_list_env(Cpu *c);
void hle_gfx_prim(Cpu *c);
void hle_gfx_put_disp(Cpu *c);
void hle_gfx_put_draw(Cpu *c);
void hle_gfx_queue(Cpu *c);
void hle_gfx_reset(Cpu *c);
void hle_gfx_sync(Cpu *c);
void hle_gfx_sync_cb(Cpu *c);
void hle_gfx_upload(Cpu *c);
void hle_gfx_upload2(Cpu *c);
void hle_jmp_restore(Cpu *c);
void hle_jmp_save(Cpu *c);
void hle_mc_first(Cpu *c);
void hle_mc_init(Cpu *c);
void hle_mc_start(Cpu *c);
void hle_mc_stop(Cpu *c);
void hle_sys_conf(Cpu *c);
void hle_vrbody_sort(Cpu *c);
void hle_vrcam_draw(Cpu *c);
void hle_vrcam_overscan(Cpu *c);
void hle_vrcam_pack(Cpu *c);
void hle_vrcam_padorg(Cpu *c);
void hle_vrcam_trans(Cpu *c);
void hle_vrcam_view(Cpu *c);
void hle_vrfire_aalock(Cpu *c);
void hle_vrfire_aaview(Cpu *c);
void hle_vrfire_amissile(Cpu *c);
void hle_vrfire_bullet(Cpu *c);
void hle_vrfire_charge(Cpu *c);
void hle_vrfire_flash_a(Cpu *c);
void hle_vrfire_flash_b(Cpu *c);
void hle_vrfire_grenade(Cpu *c);
void hle_vrfire_hide(Cpu *c);
void hle_vrfire_mine(Cpu *c);
void hle_vrfire_rmissile(Cpu *c);
void hle_vrradar_draw(Cpu *c);

const HleEntry hle_table[] = {
    {"cb_dma", hle_cb_dma},
    {"cb_irq", hle_cb_irq},
    {"cb_reset", hle_cb_reset},
    {"ctl_act_align", hle_ctl_act_align},
    {"ctl_act_set", hle_ctl_act_set},
    {"ctl_bios_init", hle_ctl_bios_init},
    {"ctl_bios_start", hle_ctl_bios_start},
    {"ctl_bios_stop", hle_ctl_bios_stop},
    {"ctl_chk_multi", hle_ctl_chk_multi},
    {"ctl_enable", hle_ctl_enable},
    {"ctl_info_act", hle_ctl_info_act},
    {"ctl_info_comb", hle_ctl_info_comb},
    {"ctl_info_mode", hle_ctl_info_mode},
    {"ctl_init_direct", hle_ctl_init_direct},
    {"ctl_main_mode", hle_ctl_main_mode},
    {"ctl_start", hle_ctl_start},
    {"ctl_state", hle_ctl_state},
    {"ctl_stop", hle_ctl_stop},
    {"disc_ctl", hle_disc_ctl},
    {"disc_ctl_b", hle_disc_ctl_b},
    {"disc_ctl_f", hle_disc_ctl_f},
    {"disc_flush", hle_disc_flush},
    {"disc_init", hle_disc_init},
    {"disc_mix", hle_disc_mix},
    {"disc_ready", hle_disc_ready},
    {"disc_reset", hle_disc_reset},
    {"disc_sector", hle_disc_sector},
    {"disc_sync", hle_disc_sync},
    {"frame_cb", hle_frame_cb},
    {"frame_wait", hle_frame_wait},
    {"gfx_clear", hle_gfx_clear},
    {"gfx_clear2", hle_gfx_clear2},
    {"gfx_copy", hle_gfx_copy},
    {"gfx_copy2", hle_gfx_copy2},
    {"gfx_debug", hle_gfx_debug},
    {"gfx_dispmask", hle_gfx_dispmask},
    {"gfx_download", hle_gfx_download},
    {"gfx_download2", hle_gfx_download2},
    {"gfx_field", hle_gfx_field},
    {"gfx_list", hle_gfx_list},
    {"gfx_list2", hle_gfx_list2},
    {"gfx_list_clear", hle_gfx_list_clear},
    {"gfx_list_clear_r", hle_gfx_list_clear_r},
    {"gfx_list_env", hle_gfx_list_env},
    {"gfx_prim", hle_gfx_prim},
    {"gfx_put_disp", hle_gfx_put_disp},
    {"gfx_put_draw", hle_gfx_put_draw},
    {"gfx_queue", hle_gfx_queue},
    {"gfx_reset", hle_gfx_reset},
    {"gfx_sync", hle_gfx_sync},
    {"gfx_sync_cb", hle_gfx_sync_cb},
    {"gfx_upload", hle_gfx_upload},
    {"gfx_upload2", hle_gfx_upload2},
    {"jmp_restore", hle_jmp_restore},
    {"jmp_save", hle_jmp_save},
    {"mc_first", hle_mc_first},
    {"mc_init", hle_mc_init},
    {"mc_start", hle_mc_start},
    {"mc_stop", hle_mc_stop},
    {"sys_conf", hle_sys_conf},
    {"vrbody_sort", hle_vrbody_sort},
    {"vrcam_draw", hle_vrcam_draw},
    {"vrcam_overscan", hle_vrcam_overscan},
    {"vrcam_pack", hle_vrcam_pack},
    {"vrcam_padorg", hle_vrcam_padorg},
    {"vrcam_trans", hle_vrcam_trans},
    {"vrcam_view", hle_vrcam_view},
    {"vrfire_aalock", hle_vrfire_aalock},
    {"vrfire_aaview", hle_vrfire_aaview},
    {"vrfire_amissile", hle_vrfire_amissile},
    {"vrfire_bullet", hle_vrfire_bullet},
    {"vrfire_charge", hle_vrfire_charge},
    {"vrfire_flash_a", hle_vrfire_flash_a},
    {"vrfire_flash_b", hle_vrfire_flash_b},
    {"vrfire_grenade", hle_vrfire_grenade},
    {"vrfire_hide", hle_vrfire_hide},
    {"vrfire_mine", hle_vrfire_mine},
    {"vrfire_rmissile", hle_vrfire_rmissile},
    {"vrradar_draw", hle_vrradar_draw},
    {0, 0}
};
const unsigned hle_table_count = 79;
