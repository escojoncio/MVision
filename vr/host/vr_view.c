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
    v->frozen_max = 3000;          /* 60 s de motor sin dibujar (una carga larga) antes de volver a la clasificación normal */
    v->hold_max = 100;             /* 2 s de render 3D sin cámara del motor reconocida antes de enseñar la pantalla plana */
    v->black_fresh = 2;            /* con la segunda escena publicada, la lista de la GPU de la primera (sus texturas) ya está cerrada */
    v->black_settle = 6;           /* 120 ms del motor dibujando (medido: huecos de 2 fotogramas en mitad del desmontaje) */
    v->black_2d = 25;              /* 0,5 s de pantalla 2D estable (medido: 9 fotogramas «sin partida» entre la carga y el mundo) */
    v->black_nat_max = 75;         /* 1,5 s esperando escenas nuevas con el destino ya nativo */
    v->black_max = v->frozen_max + 250;
}

/* Fundido a negro (política en vr_view.h). `r` = motivo de este fotograma ya con las retenciones; `window2d` = el juego pide una ventana 2D suya. Va antes de la histéresis: modo y clase
 * son los vigentes hasta el fotograma anterior, es decir, lo que la app tiene en pantalla. El negro solo se levanta con la vista vigente ya en su destino: si se levantara antes, la app
 * bajaría el velo sobre la vista retenida (vieja) o sobre la pantalla plana durante los fotogramas de histéresis. */
#define VRV_BLACK_PRE 10   /* VBlanks de parada del motor antes del negro: medido, las cargas duran >= 52 VBlanks y las paradas al empezar escena 5-9 */
static void black_step(VrView *v, const VrViewIn *in, int r, int window2d) {
    if (!v->black) {
        if (r != VRV_R_FROZEN) { v->black_pre = 0; return; }
        if (++v->black_pre < VRV_BLACK_PRE) return;   /* parada corta: no es una carga */
        v->black_pre = 0;
        v->black = 1; v->black_run = 0; v->black_draw = 0; v->black_nat = 0;
    }
    int out = 0;
    if (!in || ++v->black_run >= v->black_max) out = 1;
    else if (in->frozen && !window2d && !in->mdec) { v->black_draw = 0; v->black_nat = 0; }   /* el motor desmonta / carga */
    else {
        const int vig_world = v->mode == VRV_WORLD && v->kind == VRV_K_WORLD, vig_engine = v->mode == VRV_CINEMA && v->kind == VRV_K_ENGINE;
        const int k = vrview_kind_of(in);
        v->black_draw++;
        if (r == VRV_R_PENDING || r == VRV_R_HOLD) v->black_nat = 0;      /* vista retenida a la espera de la cámara: sigue el negro (acotadas por pend_max / hold_max) */
        else if (k == VRV_K_WORLD || k == VRV_K_ENGINE)                    /* destino nativo: hace falta la escena nueva (en el mundo, además apta para la primera persona: si no, el
                                                                            * render nativo repetiría la última válida, que es la de la zona anterior), con esa vista ya vigente */
            out = (in->fresh >= v->black_fresh && v->black_draw >= v->black_settle && (k == VRV_K_WORLD ? vig_world && in->native_ok : vig_engine)) || ++v->black_nat >= v->black_nat_max;
        else {                                                             /* destino 2D / vídeo: cuando la vista vigente ya no es la nativa retenida */
            const int wait = (window2d || in->mdec || k == VRV_K_VIDEO) ? 0 : in->list3d ? v->hold_max : v->black_2d;
            v->black_nat = 0;
            out = v->black_draw >= wait && !vig_world && !vig_engine;
        }
    }
    if (out) { v->black = 0; v->black_run = 0; }
}

int vrview_kind_of(const VrViewIn *in) {
    if (!in) return VRV_K_UI;
    int r = vrview_reason(in);
    if (r == VRV_R_WORLD || r == VRV_R_SELECTOR || r == VRV_R_PAUSE_WORLD) return VRV_K_WORLD;
    /* FMV: con el decodificador de vídeo activo el fotograma sale de la VRAM (no hay escena que dibujar): gana al 3D que el motor deje detrás. */
    if (in->mdec && r != VRV_R_PAUSED) return VRV_K_VIDEO;
    /* Escena 3D del motor (bandera de escena/demo/muerte del motor) con 3D; con o sin jugador en escena (las introducciones no lo tienen). Sin control y sin cámara VR (o sin mundo nativo
     * apto con ella) es pantalla 3D solo si hay escena nativa apta (native_ok: no basta el puente ni 3D basura de una carga); si no, 2D. Radio, pausa, detenido y ventana de armas/objetos son 2D
     * del juego aunque el motor dibuje 3D detrás. Siempre pantalla estereoscópica del render nativo (sin respaldo plano). */
    if (in->scene3d && (r == VRV_R_NO_GAME || r == VRV_R_CINEMATIC || r == VRV_R_DEAD || r == VRV_R_WALL || r == VRV_R_EVENT || (r == VRV_R_LOST_CONTROL && in->native_ok)))
        return VRV_K_ENGINE;
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

const char *vrview_cine_type(int k) {
    switch (k) {
    case VRV_K_UI: return "2d";
    case VRV_K_WORLD: return "mundo";
    case VRV_K_ENGINE: return "motor";
    case VRV_K_VIDEO: return "fmv";
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
    if (!in->in_control) return (in->cam_active && in->native_ok) ? VRV_R_WORLD : VRV_R_LOST_CONTROL;   /* sin control pero con cámara VR y mundo nativo apto (ascensor): se sigue en VR */
    if (in->wall) return VRV_R_WALL;            /* pegado a la pared: la cámara VR está apagada a propósito; no es una transición que esperar */
    if (in->event) return VRV_R_EVENT;          /* cámara de evento del juego con el mando retirado (panel del ascensor): igual */
    if (!in->cam_active) return VRV_R_NO_CAM;
    return VRV_R_WORLD;
}

int vrview_step(VrView *v, const VrViewIn *in) {
    if (!v) return VRV_CINEMA;
    int r = vrview_reason(in);
    if (in && in->cam_active) v->cam_seen = 1;
    /* Regla de destino: sin cámara VR pero con control (fin de radio / de escena / pérdida de seguimiento) el destino es el MUNDO, no el cine: se congela la vista
     * (modo y clase) y la app oscurece, hasta que la cámara vuelve. Solo si la cámara ha existido alguna vez y por un tiempo acotado. */
    /* Lo que se ve sale del render nativo (mundo, o pantalla 3D en modo cine), no del fotograma de la PS1. Modo y clase tienen histéresis separadas: las mezclas (cine con clase mundo) son
     * la pantalla plana para la app y no son una vista nativa que retener. */
    const int native_view = (v->mode == VRV_WORLD && v->kind == VRV_K_WORLD) || (v->mode == VRV_CINEMA && v->kind == VRV_K_ENGINE);
    /* El motor no dibuja (carga / cambio de zona): lo que queda en su pantalla es un fotograma viejo. Con una vista nativa se congela lo que se estuviera viendo (el render nativo repite su
     * última escena dibujada), salvo con un vídeo en curso (lo pinta el decodificador, no el motor) o con una ventana 2D del juego pedida (radio, pausa, armas/objetos, juego detenido: se
     * enseñan igual). Desde una pantalla 2D o un vídeo no hay nada que retener: clasificación normal. Va antes de la espera de la cámara VR: una carga no gasta ese plazo. */
    const int window2d = r == VRV_R_COMMS || r == VRV_R_STOPPED || r == VRV_R_PAUSED || r == VRV_R_MENU || r == VRV_R_SELECTOR || r == VRV_R_PAUSE_WORLD;
    if (in && in->frozen && !in->mdec && native_view && !window2d && v->frozen_run < v->frozen_max) { v->frozen_run++; r = VRV_R_FROZEN; }
    else {
        if (!in || !in->frozen) v->frozen_run = 0;
        if (r == VRV_R_NO_CAM && v->cam_seen && v->pend_run < v->pend_max) { v->pend_run++; r = VRV_R_PENDING; }
        else if (r != VRV_R_NO_CAM) v->pend_run = 0;
    }
    /* La pantalla del juego trae un render 3D de PS1 sin la cámara del motor reconocida (arranque de una escena o de la vista de pared, cuyas primeras listas aún salen con la cámara VR;
     * zona recién cargada sin jugador) y se venía de una vista nativa: la clase sería 2D, es decir, ese fotograma de baja resolución en la pantalla plana. Se retiene la vista ENTERA (modo
     * y clase: la app solo deja de usar la pantalla plana con la pantalla 3D o el mundo) hasta que llegue una lista con la cámara del motor, una 2D de verdad o el mundo; acotado por
     * hold_max. Solo con motivos en los que el motor dibuja su propia escena 3D: la radio, la pausa, las ventanas, el juego detenido y la muerte (menú de fin de partida) son 2D del juego. */
    if (r != VRV_R_PENDING && r != VRV_R_FROZEN) {
        int base = in && !in->mdec && native_view && (r == VRV_R_NO_GAME || r == VRV_R_CINEMATIC || r == VRV_R_LOST_CONTROL || r == VRV_R_WALL || r == VRV_R_EVENT) && vrview_kind_of(in) == VRV_K_UI;
        if (base && in->list3d && v->hold_run < v->hold_max) { v->hold_run++; r = VRV_R_HOLD; }
        else if (!base) v->hold_run = 0;       /* el tope solo se rearma cuando la situación termina; una lista suelta sin 3D no lo rearma */
    }
    int old_mode = v->mode, old_kind = v->kind, old_reason = v->reason;
    const int old_black = v->black;
    black_step(v, in, r, window2d);
    const unsigned ch_black = v->black != old_black ? VRV_CH_BLACK : 0;
    v->prev_reason = old_reason;
    v->reason = r;
    if (r == VRV_R_PENDING || r == VRV_R_FROZEN || r == VRV_R_HOLD) {
        v->want = v->mode; v->run = 0; v->want_kind = v->kind; v->kind_run = 0; v->changed = (r != old_reason ? VRV_CH_REASON : 0) | ch_black;
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
    v->changed = (v->mode != old_mode ? VRV_CH_MODE : 0) | (v->kind != old_kind ? VRV_CH_KIND : 0) | (r != old_reason ? VRV_CH_REASON : 0) | ch_black;
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
    case VRV_R_WALL: return "pegado a la pared: cámara del juego";
    case VRV_R_FROZEN: return "el motor no dibuja: vista congelada";
    case VRV_R_HOLD: return "render 3D sin cámara del motor: vista retenida";
    case VRV_R_EVENT: return "cámara de evento del juego (panel): cámara del juego";
    default: return "?";
    }
}

int vrview_describe(const VrView *v, const VrViewIn *in, char *buf, int n) {
    if (!v || !buf || n <= 0) return 0;
    VrViewIn z;
    if (!in) { memset(&z, 0, sizeof z); in = &z; }
    int r = snprintf(buf, (size_t)n,
        "[vista] cambio:%s%s%s%s modo=%s clase=%s motivo=%s (antes: %s) | puente: partida=%d control=%d camVR=%d escena=%d radio=%d pausa=%d menu=%d detenido=%d muerte=%d 3D=%d video=%d nativo=%d pared=%d sin_dibujo=%d lista3D=%d negro=%d nuevas=%d evento=%d",
        (v->changed & VRV_CH_MODE) ? " modo" : "", (v->changed & VRV_CH_KIND) ? " clase" : "", (v->changed & VRV_CH_REASON) ? " motivo" : "", (v->changed & VRV_CH_BLACK) ? " negro" : "",
        v->mode == VRV_WORLD ? "mundo" : "cine", vrview_kind_name(v->kind), vrview_reason_name(v->reason), vrview_reason_name(v->prev_reason),
        in->game, in->in_control, in->cam_active, in->cinematic, in->comms, in->paused, in->menu_open, in->stopped, in->dead, in->scene3d, in->video, in->native_ok, in->wall, in->frozen, in->list3d,
        v->black, in->fresh, in->event);
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
    case VRV_K_ENGINE: why = "3D del motor dibujado por el render nativo (escena leída de la RAM): pantalla estereoscópica"; break;
    case VRV_K_WORLD: why = "primera persona"; break;
    default: why = "2D del juego (menú, título, radio, carga...): pantalla de cine"; break;
    }
    int r = snprintf(buf, (size_t)n, "[cine] tipo=%s motivo=%s | razon=%s partida=%d 3D=%d mdec=%d subidas=%d",
                     vrview_cine_type(v->kind), why, vrview_reason_name(v->reason), in->game, in->scene3d, in->mdec, in->video);
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
