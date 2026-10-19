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
    i = world(); i.in_control = 0; i.menu_open = 1; CHECK(vrview_reason(&i) == VRV_R_SELECTOR, "ventana de armas con cámara VR = selector sobre el mundo");
    i.cam_active = 0; CHECK(vrview_reason(&i) == VRV_R_MENU, "ventana de armas sin cámara VR = cine");
    i = world(); i.in_control = 0; i.paused = 1; i.menu_open = 1; CHECK(vrview_reason(&i) == VRV_R_PAUSE_WORLD, "pausa gana a menú");
    i.cam_active = 0; CHECK(vrview_reason(&i) == VRV_R_PAUSED, "pausa sin cámara VR = cine");
    i = world(); i.in_control = 0; i.stopped = 1; i.comms = 1; CHECK(vrview_reason(&i) == VRV_R_COMMS, "radio gana a detenido");
    i = world(); i.in_control = 0; i.cinematic = 1; i.comms = 1; CHECK(vrview_reason(&i) == VRV_R_COMMS, "la radio gana a la cinemática");
    i.scene3d = 1; CHECK(vrview_kind_of(&i) == VRV_K_UI, "radio durante una escena con 3D = cine plano, nunca pantalla 3D");
    i = world(); i.in_control = 0; i.cinematic = 1; i.menu_open = 1; CHECK(vrview_reason(&i) == VRV_R_CINEMATIC, "cinemática gana a la ventana de armas");
    i = world(); i.in_control = 0; i.menu_open = 1; i.stopped = 1; CHECK(vrview_reason(&i) == VRV_R_STOPPED, "detenido gana a la ventana de armas");
    i = world(); i.in_control = 0; i.menu_open = 1; i.dead = 1; CHECK(vrview_reason(&i) == VRV_R_DEAD, "muerte gana a la ventana de armas");
    i = world(); i.in_control = 0; i.paused = 1; i.dead = 1; CHECK(vrview_reason(&i) == VRV_R_DEAD, "muerte gana a la pausa");
    i = world(); i.game = 0; i.cinematic = 1; CHECK(vrview_reason(&i) == VRV_R_NO_GAME, "sin partida gana a todo");
    CHECK(vrview_reason(NULL) == VRV_R_NO_GAME, "NULL");
    i = world(); i.in_control = 0; i.dead = 1; CHECK(vrview_reason(&i) == VRV_R_DEAD, "muerte");
    i = world(); i.in_control = 0; i.dead = 1; i.scene3d = 1; CHECK(vrview_kind_of(&i) == VRV_K_ENGINE, "muerte con 3D = pantalla 3D");
    i.scene3d = 0; CHECK(vrview_kind_of(&i) == VRV_K_UI, "muerte sin 3D = cine plano");
    i = world(); i.dead = 1; i.cinematic = 1; CHECK(vrview_reason(&i) == VRV_R_CINEMATIC, "cinemática gana a muerte");

    VrView v;
    vrview_reset(&v);
    CHECK(v.mode == VRV_CINEMA, "arranca en cine");
    i = world();
    CHECK(vrview_step(&v, &i) == VRV_CINEMA && vrview_step(&v, &i) == VRV_CINEMA, "histéresis a mundo (2 de 3)");
    CHECK(vrview_step(&v, &i) == VRV_WORLD, "mundo tras 3 fotogramas");
    v.to_comms = 3;                  /* umbral de radio igualado al resto para esta prueba de histéresis */
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
    for (int r = 0; r <= VRV_R_PENDING; r++) CHECK(vrview_reason_name(r)[0] != '?', "nombre %d", r);
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
    /* --- máquina de estados: selector de armas/objetos y pausa (el mundo no se interrumpe) --- */
    {
        VrView m; vrview_reset(&m);
        VrViewIn w2 = world(), sel = world(); sel.in_control = 0; sel.menu_open = 1; sel.scene3d = 1; w2.scene3d = 1;
        for (int n = 0; n < 3; n++) vrview_step(&m, &w2);
        CHECK(m.mode == VRV_WORLD && m.kind == VRV_K_WORLD, "partida en mundo");
        unsigned sw = m.switches, ksw = m.kind_switches;
        for (int n = 0; n < 200; n++) {                       /* selector abierto 4 s: ni un cambio de modo ni de clase */
            vrview_step(&m, &sel);
            CHECK(m.mode == VRV_WORLD && m.kind == VRV_K_WORLD, "selector: sigue en mundo (n=%d modo=%d clase=%d)", n, m.mode, m.kind);
        }
        CHECK(m.reason == VRV_R_SELECTOR && m.switches == sw && m.kind_switches == ksw, "selector: motivo %d, cambios de modo %u/%u clase %u/%u", m.reason, m.switches, sw, m.kind_switches, ksw);
        for (int n = 0; n < 5; n++) { vrview_step(&m, &w2); CHECK(m.mode == VRV_WORLD, "cerrar el selector no pasa por cine"); }
        /* pausa */
        VrViewIn ps = world(); ps.in_control = 0; ps.paused = 1; ps.scene3d = 1;
        for (int n = 0; n < 50; n++) { vrview_step(&m, &ps); CHECK(m.mode == VRV_WORLD && m.kind == VRV_K_WORLD, "pausa: sigue en mundo"); }
        CHECK(m.reason == VRV_R_PAUSE_WORLD, "pausa: motivo");
        /* si la cámara VR se apaga con el selector abierto (pérdida de seguimiento): tras 3 fotogramas cine, y vuelve al reactivarse */
        VrViewIn nc = sel; nc.cam_active = 0;
        for (int n = 0; n < 3; n++) vrview_step(&m, &nc);
        CHECK(m.mode == VRV_CINEMA && m.reason == VRV_R_MENU, "selector sin cámara VR: cine");
        for (int n = 0; n < 3; n++) vrview_step(&m, &sel);
        CHECK(m.mode == VRV_WORLD, "selector con cámara VR otra vez: mundo");
        /* radio desde el mundo: cine plano (no pantalla 3D) aunque el motor tenga 3D detrás; y vuelta al mundo */
        VrViewIn cm = world(); cm.in_control = 0; cm.comms = 1; cm.scene3d = 1; cm.stopped = 1;
        for (int n = 0; n < 40; n++) vrview_step(&m, &cm);
        CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_UI && m.reason == VRV_R_COMMS, "radio: cine plano (modo %d clase %d motivo %d)", m.mode, m.kind, m.reason);
        /* la radio abierta durante una escena del motor sigue siendo cine plano */
        VrViewIn cc = cm; cc.cinematic = 1;
        for (int n = 0; n < 40; n++) { vrview_step(&m, &cc); CHECK(m.kind == VRV_K_UI && m.mode == VRV_CINEMA, "radio en escena: cine plano"); }
        /* cinemática real: pantalla del motor */
        VrViewIn ci = world(); ci.in_control = 0; ci.cinematic = 1; ci.scene3d = 1;
        for (int n = 0; n < 40; n++) vrview_step(&m, &ci);
        CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_ENGINE && m.reason == VRV_R_CINEMATIC, "cinemática real: clase motor");
        /* muerte */
        VrViewIn dd = world(); dd.in_control = 0; dd.dead = 1; dd.scene3d = 1;
        for (int n = 0; n < 40; n++) vrview_step(&m, &dd);
        CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_ENGINE && m.reason == VRV_R_DEAD, "muerte: clase motor");
        /* registro: changed marca modo / clase / motivo y vrview_describe lo escribe */
        vrview_reset(&m);
        char buf[600];
        vrview_step(&m, &w2);
        CHECK(m.changed == VRV_CH_REASON, "changed: solo motivo (%u)", m.changed);
        vrview_step(&m, &w2); CHECK(m.changed == 0, "changed: sin cambio (%u)", m.changed);
        vrview_step(&m, &w2);
        CHECK(m.changed == (VRV_CH_MODE | VRV_CH_KIND), "changed: modo y clase (%u)", m.changed);
        vrview_step(&m, &w2);
        CHECK(m.changed == 0, "changed: nada");
        vrview_step(&m, &sel);
        CHECK(m.changed == VRV_CH_REASON, "changed: mundo -> selector solo cambia el motivo (%u)", m.changed);
        int len = vrview_describe(&m, &sel, buf, sizeof buf);
        CHECK(len > 20 && strstr(buf, "selector") && strstr(buf, "modo=mundo") && strstr(buf, "menu=1") && strstr(buf, "camVR=1"), "describe: %s", buf);
        CHECK(vrview_describe(&m, &sel, buf, 8) == 7 && buf[7] == 0, "describe trunca sin desbordar");
        CHECK(vrview_describe(NULL, &sel, buf, sizeof buf) == 0 && vrview_describe(&m, NULL, buf, sizeof buf) > 0, "describe NULL");
    }
    /* --- secuencias reales del registro: transiciones hacia/desde la radio y al acabar una escena --- */
    {
        /* Ninguna transición enseña la pantalla de cine plana salvo destino cine: "plana" = modo CINE con clase UI. Se registra por fotograma. */
        VrView m; vrview_reset(&m);
        VrViewIn w2 = world(); w2.scene3d = 1;
        for (int n = 0; n < 10; n++) vrview_step(&m, &w2);
        CHECK(m.mode == VRV_WORLD, "arranca en mundo");
        /* mundo -> radio: el mundo se mantiene to_comms-1 fotogramas y la cámara VR aguanta la gracia; luego cine */
        VrViewIn cm = world(); cm.in_control = 0; cm.comms = 1; cm.stopped = 1; cm.scene3d = 1; cm.cam_active = 0;
        for (int n = 0; n < m.to_comms - 1; n++) { vrview_step(&m, &cm); CHECK(m.mode == VRV_WORLD, "radio: el mundo se mantiene hasta que el códec esté en pantalla (n=%d)", n); }
        vrview_step(&m, &cm); CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_UI && m.reason == VRV_R_COMMS, "radio: cine tras %d fotogramas", m.to_comms);
        /* un parpadeo de la bandera de radio de 1 fotograma no mueve la vista */
        vrview_reset(&m); for (int n = 0; n < 10; n++) vrview_step(&m, &w2);
        vrview_step(&m, &cm); vrview_step(&m, &w2); CHECK(m.mode == VRV_WORLD && m.switches == 1, "parpadeo de radio");
        /* radio -> mundo: sin control no hay salto; con control y sin cámara VR (8 fotogramas + latencia, ~1-2 s medidos) la vista queda congelada en cine y marcada PENDIENTE (negro en la app) */
        vrview_reset(&m); for (int n = 0; n < 10; n++) vrview_step(&m, &w2);
        for (int n = 0; n < 40; n++) vrview_step(&m, &cm);
        CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_UI, "dentro de la radio");
        VrViewIn nc = world(); nc.cam_active = 0; nc.scene3d = 1;
        unsigned flat = 0, pend = 0, sw0 = m.switches, ksw0 = m.kind_switches;
        for (int n = 0; n < 70; n++) {      /* ~1,4 s sin cámara VR */
            vrview_step(&m, &nc);
            if (m.reason == VRV_R_PENDING) pend++;
            if (m.reason != VRV_R_PENDING) flat++;      /* cualquier fotograma que no sea pendiente aquí llevaría al cine plano */
            CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_UI && m.switches == sw0 && m.kind_switches == ksw0, "pendiente: vista congelada (n=%d)", n);
        }
        CHECK(pend == 70 && flat == 0, "todo el hueco sin cámara es PENDIENTE (%u de 70)", pend);
        for (int n = 0; n < 3; n++) vrview_step(&m, &w2);
        CHECK(m.mode == VRV_WORLD && m.kind == VRV_K_WORLD && m.reason == VRV_R_WORLD, "cámara VR de vuelta: mundo directo");
        /* fin de una escena del motor: ENGINE -> (control, sin cámara VR) -> mundo, sin pasar nunca por clase 2D */
        vrview_reset(&m); for (int n = 0; n < 10; n++) vrview_step(&m, &w2);
        VrViewIn ci = world(); ci.in_control = 0; ci.cinematic = 1; ci.scene3d = 1; ci.cam_active = 0;
        for (int n = 0; n < 60; n++) vrview_step(&m, &ci);
        CHECK(m.mode == VRV_CINEMA && m.kind == VRV_K_ENGINE, "escena: clase motor");
        VrViewIn after = world(); after.cam_active = 0; after.scene3d = 0;     /* escena terminada, control devuelto, 3D ya no es de escena */
        for (int n = 0; n < 60; n++) { vrview_step(&m, &after); CHECK(m.kind == VRV_K_ENGINE && m.reason == VRV_R_PENDING, "fin de escena: clase motor congelada, nunca 2D (n=%d kind=%d r=%d)", n, m.kind, m.reason); }
        for (int n = 0; n < 3; n++) vrview_step(&m, &w2);
        CHECK(m.mode == VRV_WORLD, "fin de escena: mundo");
        /* sin cámara VR disponible nunca (edición no reconocida): cine sin espera */
        vrview_reset(&m);
        VrViewIn none = world(); none.cam_active = 0;
        for (int n = 0; n < 5; n++) vrview_step(&m, &none);
        CHECK(m.reason == VRV_R_NO_CAM && m.mode == VRV_CINEMA, "sin cámara VR jamás: cine (motivo %d)", m.reason);
        /* la espera está acotada */
        vrview_reset(&m); for (int n = 0; n < 5; n++) vrview_step(&m, &w2);
        for (int n = 0; n < m.pend_max + 5; n++) vrview_step(&m, &none);
        CHECK(m.reason == VRV_R_NO_CAM && m.mode == VRV_CINEMA, "espera acotada: pasados %d fotogramas, cine", m.pend_max);
    }
    fprintf(stderr, fails ? "test_view: %d FALLOS\n" : "test_view: OK\n", fails);
    return fails ? 1 : 0;
}
