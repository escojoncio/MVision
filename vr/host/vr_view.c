#include "vr_view.h"
#include <string.h>

void vrview_reset(VrView *v) {
    if (!v) return;
    memset(v, 0, sizeof *v);
    v->mode = VRV_CINEMA;          /* se arranca en cine: título, cargas y vídeos no tienen mundo */
    v->want = VRV_CINEMA;
    v->reason = VRV_R_NO_GAME;
    v->to_cine = 3;                /* 60 ms */
    v->to_world = 3;
}

int vrview_reason(const VrViewIn *in) {
    if (!in) return VRV_R_NO_GAME;
    if (!in->game) return VRV_R_NO_GAME;
    if (in->cinematic) return VRV_R_CINEMATIC;
    if (in->comms) return VRV_R_COMMS;
    if (in->stopped) return VRV_R_STOPPED;
    if (in->paused) return VRV_R_PAUSED;
    if (in->menu_open) return VRV_R_MENU;
    if (!in->in_control) return VRV_R_LOST_CONTROL;
    if (!in->cam_active) return VRV_R_NO_CAM;
    return VRV_R_WORLD;
}

int vrview_step(VrView *v, const VrViewIn *in) {
    if (!v) return VRV_CINEMA;
    int r = vrview_reason(in);
    v->reason = r;
    v->want = r == VRV_R_WORLD ? VRV_WORLD : VRV_CINEMA;
    if (v->want == v->mode) {
        v->run = 0;
    } else {
        int need = v->want == VRV_CINEMA ? v->to_cine : v->to_world;
        if (need < 1) need = 1;
        if (++v->run >= need) {
            v->mode = v->want;
            v->run = 0;
            v->switches++;
        }
    }
    return v->mode;
}

const char *vrview_reason_name(int r) {
    switch (r) {
    case VRV_R_WORLD: return "mundo";
    case VRV_R_NO_GAME: return "sin partida";
    case VRV_R_CINEMATIC: return "cinemática";
    case VRV_R_COMMS: return "radio";
    case VRV_R_STOPPED: return "detenido";
    case VRV_R_PAUSED: return "pausa";
    case VRV_R_MENU: return "ventana de armas/objetos";
    case VRV_R_LOST_CONTROL: return "sin control";
    case VRV_R_NO_CAM: return "sin cámara VR";
    default: return "?";
    }
}
