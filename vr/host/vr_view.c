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
    v->kind = VRV_K_UI;
    v->want_kind = VRV_K_UI;
    v->to_engine = 3;              /* entrar en cinemática de motor: 60 ms de 3D seguido */
    v->from_engine = 10;           /* salir: 200 ms sin 3D (un fundido o un corte no deben parpadear) */
    v->to_other = 3;
}

int vrview_kind_of(const VrViewIn *in) {
    if (!in) return VRV_K_UI;
    int r = vrview_reason(in);
    if (r == VRV_R_WORLD) return VRV_K_WORLD;
    /* Cinemática del motor: sin control del jugador y con 3D; con o sin jugador en escena (las introducciones no lo tienen). Radio, pausa,
     * detenido y ventana de armas/objetos son 2D del juego aunque el motor dibuje 3D detrás. */
    if (in->scene3d && (r == VRV_R_NO_GAME || r == VRV_R_CINEMATIC || r == VRV_R_LOST_CONTROL)) return VRV_K_ENGINE;
    if (in->video && r != VRV_R_PAUSED) return VRV_K_VIDEO;
    return VRV_K_UI;
}

const char *vrview_kind_name(int k) {
    switch (k) {
    case VRV_K_UI: return "2D";
    case VRV_K_WORLD: return "mundo";
    case VRV_K_ENGINE: return "cinemática del motor";
    case VRV_K_VIDEO: return "vídeo";
    default: return "?";
    }
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
    int k = vrview_kind_of(in);
    v->want_kind = k;
    if (k == v->kind) {
        v->kind_run = 0;
    } else {
        int need = k == VRV_K_ENGINE ? v->to_engine : v->kind == VRV_K_ENGINE ? v->from_engine : v->to_other;
        if (k == VRV_K_WORLD) need = v->to_world;      /* el mundo y el modo de vista usan el mismo umbral */
        if (v->kind == VRV_K_WORLD) need = v->to_cine;
        if (need < 1) need = 1;
        if (++v->kind_run >= need) { v->kind = k; v->kind_run = 0; v->kind_switches++; }
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
