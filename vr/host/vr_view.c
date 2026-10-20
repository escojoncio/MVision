#include "vr_view.h"
#include <stdio.h>
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
    v->prev_reason = VRV_R_NO_GAME;
    v->to_comms = 12;              /* 240 ms de radio antes de dejar el mundo (= VRCAM_COMMS_GRACE) */
    v->pend_max = 150;             /* 3 s esperando la cámara VR tras una radio o una escena */
    v->cov_hi = 80;                /* medida buena: >= 80 % (cinemáticas con partida 67 - 99 %, muerte 22 - 98 %) */
    v->cov_lo = 62;                /* medida mala: < 62 % (introducciones 0,1 - 30 %; muerte medida en volcado: 58,9 % con 660 triángulos ausentes = el lugar de la muerte fuera de la cola) */
    v->cov_win = 4;                /* decide sobre las últimas 4 medidas (~4 s): las de una escena que barre zonas no cubiertas alternan buenas y malas */
    v->cov_need_bad = 2;           /* 2 malas de 4 pierden la fiabilidad (la cinemática sin medida o fiel es de fiar hasta entonces) */
    v->cov_need_good = 3;          /* 3 buenas de 4 la recuperan */
    v->cov_forget = 50;            /* 1 s sin 3D: la siguiente escena parte de cero */
}

int vrview_native_trusted(const VrViewIn *in) {
    if (!in) return 0;
    if (in->cover1 > 0) return (int)in->cover1 - 1 >= 45;      /* entrada ya filtrada por vrview_step: 101 = fiel, 1 = no fiel; crudo: umbral central */
    return in->game != 0;                                      /* sin medida: el extractor solo es de fiar con partida (puente completo y jugador) */
}

int vrview_kind_of(const VrViewIn *in) {
    if (!in) return VRV_K_UI;
    int r = vrview_reason(in);
    if (r == VRV_R_WORLD || r == VRV_R_SELECTOR || r == VRV_R_PAUSE_WORLD) return VRV_K_WORLD;
    /* FMV: con el decodificador de vídeo activo el fotograma sale de la VRAM (no hay escena que dibujar): gana al 3D que el motor deje detrás. */
    if (in->mdec && r != VRV_R_PAUSED) return VRV_K_VIDEO;
    /* Escena 3D del motor: sin control del jugador y con 3D; con o sin jugador en escena (las introducciones no lo tienen). Radio, pausa, detenido y ventana de
     * armas/objetos son 2D del juego aunque el motor dibuje 3D detrás. Pantalla estereoscópica solo si el render nativo es fiel; si no, respaldo (VRAM). */
    if (in->scene3d && (r == VRV_R_NO_GAME || r == VRV_R_CINEMATIC || r == VRV_R_LOST_CONTROL || r == VRV_R_DEAD))
        return vrview_native_trusted(in) ? VRV_K_ENGINE : VRV_K_BACKUP;
    if (in->video && r != VRV_R_PAUSED) return VRV_K_VIDEO;
    return VRV_K_UI;
}

const char *vrview_kind_name(int k) {
    switch (k) {
    case VRV_K_UI: return "2D";
    case VRV_K_WORLD: return "mundo";
    case VRV_K_ENGINE: return "cinemática del motor";
    case VRV_K_VIDEO: return "vídeo";
    case VRV_K_BACKUP: return "respaldo (imagen de la VRAM)";
    default: return "?";
    }
}

const char *vrview_cine_type(int k) {
    switch (k) {
    case VRV_K_UI: return "2d";
    case VRV_K_WORLD: return "mundo";
    case VRV_K_ENGINE: return "motor";
    case VRV_K_VIDEO: return "fmv";
    case VRV_K_BACKUP: return "respaldo";
    default: return "?";
    }
}

int vrview_reason(const VrViewIn *in) {
    if (!in) return VRV_R_NO_GAME;
    if (!in->game) return VRV_R_NO_GAME;
    if (in->comms) return VRV_R_COMMS;          /* la radio es 2D del juego aunque el motor la abra durante una escena */
    if (in->cinematic) return VRV_R_CINEMATIC;
    if (in->stopped) return VRV_R_STOPPED;
    if (in->dead) return VRV_R_DEAD;
    /* Ventanas 2D del juego en partida: con la cámara VR activa el mundo no se toca (primera persona) y la ventana va en un panel. */
    if (in->paused) return in->cam_active ? VRV_R_PAUSE_WORLD : VRV_R_PAUSED;
    if (in->menu_open) return in->cam_active ? VRV_R_SELECTOR : VRV_R_MENU;
    if (!in->in_control) return VRV_R_LOST_CONTROL;
    if (!in->cam_active) return VRV_R_NO_CAM;
    return VRV_R_WORLD;
}

int vrview_step(VrView *v, const VrViewIn *in) {
    if (!v) return VRV_CINEMA;
    int r = vrview_reason(in);
    if (in && in->cam_active) v->cam_seen = 1;
    /* Regla de destino: sin cámara VR pero con control (fin de radio / de escena / pérdida de seguimiento) el destino es el MUNDO, no el cine: se congela la vista
     * (modo y clase) y la app oscurece, hasta que la cámara vuelve. Solo si la cámara ha existido alguna vez y por un tiempo acotado. */
    if (r == VRV_R_NO_CAM && v->cam_seen && v->pend_run < v->pend_max) { v->pend_run++; r = VRV_R_PENDING; }
    else if (r != VRV_R_NO_CAM) v->pend_run = 0;
    int old_mode = v->mode, old_kind = v->kind, old_reason = v->reason;
    /* Fidelidad del render nativo (solo cuenta con 3D): medidas de cobertura con histéresis. Sin 3D durante `cov_forget` fotogramas se olvidan. */
    VrViewIn eff;
    if (in) {
        eff = *in;
        if (in->scene3d) {
            v->cov_idle = 0;
            if (in->cover_new && in->cover1 > 0) {
                int pct = (int)in->cover1 - 1;
                unsigned wm = (1u << v->cov_win) - 1u;
                v->cov_bad_mask = ((v->cov_bad_mask << 1) | (pct < v->cov_lo ? 1u : 0u)) & wm;
                v->cov_good_mask = ((v->cov_good_mask << 1) | (pct >= v->cov_hi ? 1u : 0u)) & wm;
                if (__builtin_popcount(v->cov_bad_mask) >= v->cov_need_bad) v->cov_state = 2;
                else if (__builtin_popcount(v->cov_good_mask) >= v->cov_need_good) v->cov_state = 1;
            }
        } else if (++v->cov_idle >= v->cov_forget) {
            v->cov_state = 0; v->cov_good_mask = v->cov_bad_mask = 0; v->cov_idle = v->cov_forget;
        }
        eff.cover1 = v->cov_state == 1 ? 101 : v->cov_state == 2 ? 1 : 0;
        eff.cover_new = 0;
        in = &eff;
    }
    v->prev_reason = old_reason;
    v->reason = r;
    if (r == VRV_R_PENDING) {
        v->want = v->mode; v->run = 0; v->want_kind = v->kind; v->kind_run = 0; v->changed = r != old_reason ? VRV_CH_REASON : 0;
        return v->mode;
    }
    v->want = (r == VRV_R_WORLD || r == VRV_R_SELECTOR || r == VRV_R_PAUSE_WORLD) ? VRV_WORLD : VRV_CINEMA;
    if (v->want == v->mode) {
        v->run = 0;
    } else {
        int need = v->want == VRV_CINEMA ? (r == VRV_R_COMMS ? v->to_comms : v->to_cine) : v->to_world;
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
        if (v->kind == VRV_K_WORLD) need = r == VRV_R_COMMS ? v->to_comms : v->to_cine;
        if (need < 1) need = 1;
        if (++v->kind_run >= need) { v->kind = k; v->kind_run = 0; v->kind_switches++; }
    }
    v->changed = (v->mode != old_mode ? VRV_CH_MODE : 0) | (v->kind != old_kind ? VRV_CH_KIND : 0) | (r != old_reason ? VRV_CH_REASON : 0);
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
    case VRV_R_DEAD: return "muerte";
    case VRV_R_SELECTOR: return "selector de armas/objetos sobre el mundo";
    case VRV_R_PAUSE_WORLD: return "pausa sobre el mundo";
    case VRV_R_PENDING: return "transición: esperando cámara VR";
    default: return "?";
    }
}

int vrview_describe(const VrView *v, const VrViewIn *in, char *buf, int n) {
    if (!v || !buf || n <= 0) return 0;
    VrViewIn z;
    if (!in) { memset(&z, 0, sizeof z); in = &z; }
    int r = snprintf(buf, (size_t)n,
        "[vista] cambio:%s%s%s modo=%s clase=%s motivo=%s (antes: %s) | puente: partida=%d control=%d camVR=%d escena=%d radio=%d pausa=%d menu=%d detenido=%d muerte=%d 3D=%d video=%d",
        (v->changed & VRV_CH_MODE) ? " modo" : "", (v->changed & VRV_CH_KIND) ? " clase" : "", (v->changed & VRV_CH_REASON) ? " motivo" : "",
        v->mode == VRV_WORLD ? "mundo" : "cine", vrview_kind_name(v->kind), vrview_reason_name(v->reason), vrview_reason_name(v->prev_reason),
        in->game, in->in_control, in->cam_active, in->cinematic, in->comms, in->paused, in->menu_open, in->stopped, in->dead, in->scene3d, in->video);
    if (r < 0) { buf[0] = 0; return 0; }
    return r >= n ? n - 1 : r;
}

int vrview_cine_describe(const VrView *v, const VrViewIn *in, char *buf, int n) {
    if (!v || !buf || n <= 0) return 0;
    VrViewIn z;
    if (!in) { memset(&z, 0, sizeof z); in = &z; }
    const char *why;
    switch (v->kind) {
    case VRV_K_VIDEO:
        why = in->mdec ? "decodificador de vídeo activo (MDEC): fotograma de la VRAM, pantalla de cine plana"
                       : "subidas de vídeo que cubren el área de pantalla de la VRAM y sin 3D";
        break;
    case VRV_K_ENGINE:
        why = v->cov_state == 1 ? "3D del motor y el render nativo representa la lista (cobertura medida buena): pantalla estereoscópica"
                                : "3D del motor con partida y sin medida de cobertura: se fía del extractor";
        break;
    case VRV_K_BACKUP:
        why = v->cov_state == 2 ? "3D del motor pero el render nativo no representa la lista (cobertura baja): imagen fiel de la VRAM"
                                : "3D del motor sin partida y sin medida fiable (el extractor no tiene escena apta en este modo): imagen fiel de la VRAM";
        break;
    case VRV_K_WORLD: why = "primera persona"; break;
    default: why = "2D del juego (menú, título, radio, carga...): pantalla de cine"; break;
    }
    char cov[24];
    if (in->cover1 > 0) snprintf(cov, sizeof cov, "%d%%", (int)in->cover1 - 1); else snprintf(cov, sizeof cov, "sin medir");
    int r = snprintf(buf, (size_t)n, "[cine] tipo=%s motivo=%s | razon=%s partida=%d 3D=%d mdec=%d subidas=%d cobertura=%s estado_cobertura=%d(0 sin medir, 1 fiel, 2 no fiel)",
                     vrview_cine_type(v->kind), why, vrview_reason_name(v->reason), in->game, in->scene3d, in->mdec, in->video, cov, v->cov_state);
    if (r < 0) { buf[0] = 0; return 0; }
    return r >= n ? n - 1 : r;
}

long long vrview_upload_area(int dx, int dy, int dw, int dh, int depth24, int x, int y, int w, int h) {
    if (dw <= 0) dw = 320;
    if (dh <= 0) dh = 240;
    if (depth24) dw = dw * 3 / 2;
    int x0 = x > dx ? x : dx, y0 = y > dy ? y : dy;
    int x1 = x + w < dx + dw ? x + w : dx + dw, y1 = y + h < dy + dh ? y + h : dy + dh;
    return (x1 > x0 && y1 > y0) ? (long long)(x1 - x0) * (y1 - y0) : 0;
}

int vrview_uploads_cover(long long sum, int dw, int dh, int depth24) {
    if (dw <= 0) dw = 320;
    if (dh <= 0) dh = 240;
    if (depth24) dw = dw * 3 / 2;
    return sum * 10 >= (long long)dw * dh * 6;
}
