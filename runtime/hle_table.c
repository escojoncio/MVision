/* GENERADO por tools/recomp/hletab.py desde runtime/hle*.txt: no editar (make -C runtime hle_table). */
#include "hlebind.h"

void hle_CdControl(Cpu *c);
void hle_CdControlB(Cpu *c);
void hle_CdControlF(Cpu *c);
void hle_CdFlush(Cpu *c);
void hle_CdGetSector(Cpu *c);
void hle_CdInit(Cpu *c);
void hle_CdMix(Cpu *c);
void hle_CdReady(Cpu *c);
void hle_CdReset(Cpu *c);
void hle_CdSync(Cpu *c);
void hle_ClearImage(Cpu *c);
void hle_ClearImage2(Cpu *c);
void hle_ClearOTag(Cpu *c);
void hle_ClearOTagR(Cpu *c);
void hle_DMACallback(Cpu *c);
void hle_DrawOTag(Cpu *c);
void hle_DrawOTag2(Cpu *c);
void hle_DrawOTagEnv(Cpu *c);
void hle_DrawPrim(Cpu *c);
void hle_DrawSync(Cpu *c);
void hle_DrawSyncCallback(Cpu *c);
void hle_GetODE(Cpu *c);
void hle_InitCARD(Cpu *c);
void hle_InitPAD(Cpu *c);
void hle_InterruptCallback(Cpu *c);
void hle_LoadImage(Cpu *c);
void hle_LoadImage2(Cpu *c);
void hle_MoveImage(Cpu *c);
void hle_MoveImage2(Cpu *c);
void hle_PadChkMtap(Cpu *c);
void hle_PadEnableCom(Cpu *c);
void hle_PadGetState(Cpu *c);
void hle_PadInfoAct(Cpu *c);
void hle_PadInfoComb(Cpu *c);
void hle_PadInfoMode(Cpu *c);
void hle_PadInitDirect(Cpu *c);
void hle_PadSetAct(Cpu *c);
void hle_PadSetActAlign(Cpu *c);
void hle_PadSetMainMode(Cpu *c);
void hle_PadStartCom(Cpu *c);
void hle_PadStopCom(Cpu *c);
void hle_PutDispEnv(Cpu *c);
void hle_PutDrawEnv(Cpu *c);
void hle_ResetCallback(Cpu *c);
void hle_ResetGraph(Cpu *c);
void hle_SetConf(Cpu *c);
void hle_SetDispMask(Cpu *c);
void hle_SetGraphDebug(Cpu *c);
void hle_SetGraphQueue(Cpu *c);
void hle_StartCARD(Cpu *c);
void hle_StartPAD(Cpu *c);
void hle_StopCARD(Cpu *c);
void hle_StopPAD(Cpu *c);
void hle_StoreImage(Cpu *c);
void hle_StoreImage2(Cpu *c);
void hle_VSync(Cpu *c);
void hle_VSyncCallback(Cpu *c);
void hle_firstfile(Cpu *c);
void hle_longjmp(Cpu *c);
void hle_setjmp(Cpu *c);
void hle_vrbody_sort(Cpu *c);
void hle_vrcam_draw(Cpu *c);
void hle_vrcam_overscan(Cpu *c);
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
    {"CdControl", hle_CdControl},
    {"CdControlB", hle_CdControlB},
    {"CdControlF", hle_CdControlF},
    {"CdFlush", hle_CdFlush},
    {"CdGetSector", hle_CdGetSector},
    {"CdInit", hle_CdInit},
    {"CdMix", hle_CdMix},
    {"CdReady", hle_CdReady},
    {"CdReset", hle_CdReset},
    {"CdSync", hle_CdSync},
    {"ClearImage", hle_ClearImage},
    {"ClearImage2", hle_ClearImage2},
    {"ClearOTag", hle_ClearOTag},
    {"ClearOTagR", hle_ClearOTagR},
    {"DMACallback", hle_DMACallback},
    {"DrawOTag", hle_DrawOTag},
    {"DrawOTag2", hle_DrawOTag2},
    {"DrawOTagEnv", hle_DrawOTagEnv},
    {"DrawPrim", hle_DrawPrim},
    {"DrawSync", hle_DrawSync},
    {"DrawSyncCallback", hle_DrawSyncCallback},
    {"GetODE", hle_GetODE},
    {"InitCARD", hle_InitCARD},
    {"InitPAD", hle_InitPAD},
    {"InterruptCallback", hle_InterruptCallback},
    {"LoadImage", hle_LoadImage},
    {"LoadImage2", hle_LoadImage2},
    {"MoveImage", hle_MoveImage},
    {"MoveImage2", hle_MoveImage2},
    {"PadChkMtap", hle_PadChkMtap},
    {"PadEnableCom", hle_PadEnableCom},
    {"PadGetState", hle_PadGetState},
    {"PadInfoAct", hle_PadInfoAct},
    {"PadInfoComb", hle_PadInfoComb},
    {"PadInfoMode", hle_PadInfoMode},
    {"PadInitDirect", hle_PadInitDirect},
    {"PadSetAct", hle_PadSetAct},
    {"PadSetActAlign", hle_PadSetActAlign},
    {"PadSetMainMode", hle_PadSetMainMode},
    {"PadStartCom", hle_PadStartCom},
    {"PadStopCom", hle_PadStopCom},
    {"PutDispEnv", hle_PutDispEnv},
    {"PutDrawEnv", hle_PutDrawEnv},
    {"ResetCallback", hle_ResetCallback},
    {"ResetGraph", hle_ResetGraph},
    {"SetConf", hle_SetConf},
    {"SetDispMask", hle_SetDispMask},
    {"SetGraphDebug", hle_SetGraphDebug},
    {"SetGraphQueue", hle_SetGraphQueue},
    {"StartCARD", hle_StartCARD},
    {"StartPAD", hle_StartPAD},
    {"StopCARD", hle_StopCARD},
    {"StopPAD", hle_StopPAD},
    {"StoreImage", hle_StoreImage},
    {"StoreImage2", hle_StoreImage2},
    {"VSync", hle_VSync},
    {"VSyncCallback", hle_VSyncCallback},
    {"firstfile", hle_firstfile},
    {"longjmp", hle_longjmp},
    {"setjmp", hle_setjmp},
    {"vrbody_sort", hle_vrbody_sort},
    {"vrcam_draw", hle_vrcam_draw},
    {"vrcam_overscan", hle_vrcam_overscan},
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
const unsigned hle_table_count = 78;
