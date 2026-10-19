/* Prueba de vr_view: motivos, histéresis y transiciones. make -C vr/host test */
#include <stdio.h>
#include <string.h>
#include "vr_view.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static VrViewIn world(void) { VrViewIn i; memset(&i, 0, sizeof i); i.game = i.in_control = i.cam_active = 1; return i; }

int main(void) {
    VrViewIn i = world();
    CHECK(vrview_reason(&i) == VRV_R_WORLD, "mundo");
    i.cam_active = 0; CHECK(vrview_reason(&i) == VRV_R_NO_CAM, "sin cámara");
    i = world(); i.in_control = 0; CHECK(vrview_reason(&i) == VRV_R_LOST_CONTROL, "sin control");
    i = world(); i.in_control = 0; i.menu_open = 1; CHECK(vrview_reason(&i) == VRV_R_MENU, "ventana de armas");
    i = world(); i.in_control = 0; i.paused = 1; i.menu_open = 1; CHECK(vrview_reason(&i) == VRV_R_PAUSED, "pausa gana a menú");
    i = world(); i.in_control = 0; i.stopped = 1; i.comms = 1; CHECK(vrview_reason(&i) == VRV_R_COMMS, "radio gana a detenido");
    i = world(); i.in_control = 0; i.cinematic = 1; i.comms = 1; CHECK(vrview_reason(&i) == VRV_R_CINEMATIC, "cinemática gana a todo");
    i = world(); i.game = 0; i.cinematic = 1; CHECK(vrview_reason(&i) == VRV_R_NO_GAME, "sin partida gana a todo");
    CHECK(vrview_reason(NULL) == VRV_R_NO_GAME, "NULL");

    VrView v;
    vrview_reset(&v);
    CHECK(v.mode == VRV_CINEMA, "arranca en cine");
    i = world();
    CHECK(vrview_step(&v, &i) == VRV_CINEMA && vrview_step(&v, &i) == VRV_CINEMA, "histéresis a mundo (2 de 3)");
    CHECK(vrview_step(&v, &i) == VRV_WORLD, "mundo tras 3 fotogramas");
    VrViewIn c = world(); c.in_control = 0; c.comms = 1;
    CHECK(vrview_step(&v, &c) == VRV_WORLD && vrview_step(&v, &c) == VRV_WORLD, "histéresis a cine");
    CHECK(vrview_step(&v, &i) == VRV_WORLD, "un parpadeo no cambia la vista");
    CHECK(vrview_step(&v, &c) == VRV_WORLD && vrview_step(&v, &c) == VRV_WORLD, "la cuenta se reinició");
    CHECK(vrview_step(&v, &c) == VRV_CINEMA, "cine tras 3 fotogramas seguidos");
    CHECK(v.reason == VRV_R_COMMS && v.switches == 2, "motivo %d, cambios %u", v.reason, v.switches);
    /* umbrales de 1 y entradas inválidas */
    v.to_cine = 0; v.to_world = 0;
    CHECK(vrview_step(&v, &i) == VRV_WORLD, "umbral 0 se trata como 1");
    CHECK(vrview_step(NULL, &i) == VRV_CINEMA, "NULL");
    for (int r = 0; r <= VRV_R_NO_CAM; r++) CHECK(vrview_reason_name(r)[0] != '?', "nombre %d", r);
    /* clasificación: mundo / cinemática del motor / vídeo / 2D */
    {
        VrViewIn k;
        memset(&k, 0, sizeof k);
        CHECK(vrview_kind_of(&k) == VRV_K_UI, "título sin 3D = 2D");
        k.scene3d = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "sin partida y con 3D = cinemática del motor (introducción)");
        k.game = 1; k.cinematic = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "cinemática con jugador en escena");
        k.cinematic = 0; k.comms = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_UI, "radio con 3D detrás = 2D");
        k.comms = 0; k.paused = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_UI, "pausa = 2D");
        k.paused = 0; k.menu_open = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_UI, "ventana de armas = 2D");
        memset(&k, 0, sizeof k);
        k.video = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_VIDEO, "vídeo pregrabado");
        k.scene3d = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "con 3D no es vídeo");
        k = world();
        CHECK(vrview_kind_of(&k) == VRV_K_WORLD, "mundo");
        VrView w;
        vrview_reset(&w);
        VrViewIn e;
        memset(&e, 0, sizeof e);
        e.scene3d = 1;
        for (int n = 0; n < 2; n++) vrview_step(&w, &e);
        CHECK(w.kind == VRV_K_UI, "entrar en cinemática exige %d fotogramas", w.to_engine);
        vrview_step(&w, &e);
        CHECK(w.kind == VRV_K_ENGINE && w.mode == VRV_CINEMA, "cinemática del motor tras 3 fotogramas (la vista sigue siendo cine, no mundo)");
        e.scene3d = 0;
        for (int n = 0; n < 9; n++) vrview_step(&w, &e);
        CHECK(w.kind == VRV_K_ENGINE, "un fundido/corte sin 3D no la corta (9 de 10)");
        e.scene3d = 1; vrview_step(&w, &e); e.scene3d = 0;
        for (int n = 0; n < 9; n++) vrview_step(&w, &e);
        CHECK(w.kind == VRV_K_ENGINE, "la cuenta se reinicia");
        vrview_step(&w, &e);
        CHECK(w.kind == VRV_K_UI, "sale tras 10 fotogramas sin 3D");
        e.video = 1;
        for (int n = 0; n < 3; n++) vrview_step(&w, &e);
        CHECK(w.kind == VRV_K_VIDEO, "vídeo");
        for (int q = 0; q <= VRV_K_VIDEO; q++) CHECK(vrview_kind_name(q)[0] != '?', "nombre %d", q);
    }
    fprintf(stderr, fails ? "test_view: %d FALLOS\n" : "test_view: OK\n", fails);
    return fails ? 1 : 0;
}
