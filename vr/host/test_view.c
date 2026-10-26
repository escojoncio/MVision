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
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "sin partida y con 3D = escena del motor (siempre nativo, nunca imagen plana de la VRAM)");
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
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "con 3D no es vídeo (sin MDEC): escena del motor");
        k.mdec = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_VIDEO, "decodificador de vídeo activo = FMV aunque quede 3D detrás");
        k.paused = 0; k.game = 1; k.paused = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_UI, "pausa con MDEC = 2D");
        k = world();
        CHECK(vrview_kind_of(&k) == VRV_K_WORLD, "mundo");
        VrView w;
        vrview_reset(&w);
        VrViewIn e;
        memset(&e, 0, sizeof e);
        e.game = 1; e.scene3d = 1;       /* con partida: el extractor es de fiar sin medidas (cinemática real) */
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
        for (int q = 0; q <= VRV_K_VIDEO; q++) CHECK(vrview_kind_name(q)[0] != '?' && vrview_cine_type(q)[0] != '?', "nombre %d", q);
    }
    /* --- el cine tiene dos casos del motor/2D y el vídeo; NUNCA una imagen plana del 3D (el mundo lo dibuja solo el render nativo) --- */
    {
        char buf[700];
        VrView m; vrview_reset(&m);
        /* introducción sin partida (antes: respaldo plano): escena del motor a los 3 fotogramas */
        VrViewIn in; memset(&in, 0, sizeof in); in.scene3d = 1;
        for (int n = 0; n < 3; n++) vrview_step(&m, &in);
        CHECK(m.kind == VRV_K_ENGINE && m.mode == VRV_CINEMA && (m.changed & VRV_CH_KIND), "intro: motor a los 3 fotogramas (clase %d)", m.kind);
        vrview_cine_describe(&m, &in, buf, sizeof buf);
        CHECK(strstr(buf, "[cine] tipo=motor motivo=") && strstr(buf, "sin partida") && !strstr(buf, "respaldo"), "registro intro: %s", buf);
        /* no existe ninguna combinación de entradas que dé una clase fuera de UI/WORLD/ENGINE/VIDEO */
        for (unsigned bits = 0; bits < 2048; bits++) {
            VrViewIn z; memset(&z, 0, sizeof z);
            z.game = bits & 1; z.in_control = (bits >> 1) & 1; z.cam_active = (bits >> 2) & 1; z.cinematic = (bits >> 3) & 1; z.comms = (bits >> 4) & 1;
            z.paused = (bits >> 5) & 1; z.dead = (bits >> 6) & 1; z.scene3d = (bits >> 7) & 1; z.video = (bits >> 8) & 1; z.mdec = (bits >> 9) & 1; z.menu_open = (bits >> 10) & 1;
            int k = vrview_kind_of(&z);
            CHECK(k >= VRV_K_UI && k <= VRV_K_VIDEO, "clase %d fuera de rango (entrada %u)", k, bits);
            if (!z.mdec && !z.video && !z.paused && !z.menu_open && !z.comms && z.scene3d && !z.game) CHECK(k == VRV_K_ENGINE, "3D sin partida y sin vídeo = motor (entrada %u)", bits);
        }
        /* cinemática y muerte con partida: motor y se quedan (no hay medida de cobertura que las pase a imagen plana) */
        vrview_reset(&m);
        VrViewIn ci = world(); ci.in_control = 0; ci.cinematic = 1; ci.scene3d = 1;
        for (int n = 0; n < 200; n++) { vrview_step(&m, &ci); CHECK(n < 3 || m.kind == VRV_K_ENGINE, "cinemática: sigue en motor (n=%d)", n); }
        VrViewIn dd = world(); dd.in_control = 0; dd.dead = 1; dd.scene3d = 1;
        vrview_reset(&m); for (int n = 0; n < 50; n++) vrview_step(&m, &dd);
        CHECK(m.kind == VRV_K_ENGINE && m.reason == VRV_R_DEAD, "muerte con 3D: motor");

        /* (a) FMV: el decodificador activo gana al 3D y a la falta de partida; 12 fotogramas de gracia los pone el host */
        vrview_reset(&m);
        VrViewIn fv; memset(&fv, 0, sizeof fv); fv.mdec = 1;
        for (int n = 0; n < 3; n++) vrview_step(&m, &fv);
        CHECK(m.kind == VRV_K_VIDEO && m.mode == VRV_CINEMA, "FMV (MDEC) sin partida: vídeo");
        vrview_cine_describe(&m, &fv, buf, sizeof buf);
        CHECK(strstr(buf, "[cine] tipo=fmv motivo=") && strstr(buf, "MDEC"), "registro fmv: %s", buf);
        fv.scene3d = 1;
        for (int n = 0; n < 20; n++) { vrview_step(&m, &fv); CHECK(m.kind == VRV_K_VIDEO, "FMV con restos de 3D detrás: sigue siendo vídeo"); }
        fv.mdec = 0; fv.scene3d = 0; fv.video = 1;
        for (int n = 0; n < 5; n++) vrview_step(&m, &fv);
        CHECK(m.kind == VRV_K_VIDEO, "FMV detectado por subidas a pantalla completa");
        vrview_cine_describe(&m, &fv, buf, sizeof buf);
        CHECK(strstr(buf, "tipo=fmv") && strstr(buf, "subidas"), "registro fmv por subidas: %s", buf);
        fv.video = 0;
        for (int n = 0; n < 5; n++) vrview_step(&m, &fv);
        CHECK(m.kind == VRV_K_UI, "fin del vídeo: 2D");
        vrview_cine_describe(&m, &fv, buf, sizeof buf);
        CHECK(strstr(buf, "tipo=2d") != NULL, "registro 2d: %s", buf);
        CHECK(vrview_cine_describe(NULL, &fv, buf, sizeof buf) == 0 && vrview_cine_describe(&m, NULL, buf, sizeof buf) > 0 && vrview_cine_describe(&m, &fv, buf, 5) == 4, "describe cine robusto");
        /* el vídeo gana a todo salvo la pausa */
        VrViewIn pv; memset(&pv, 0, sizeof pv); pv.game = 1; pv.paused = 1; pv.mdec = 1;
        CHECK(vrview_kind_of(&pv) == VRV_K_UI, "pausa con vídeo = 2D");
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
    /* --- detección de fotograma de vídeo por subidas a VRAM --- */
    {
        long long sum = 0;
        sum = vrview_upload_area(0, 0, 320, 240, 0, 0, 0, 320, 240);
        CHECK(sum == 320 * 240 && vrview_uploads_cover(sum, 320, 240, 0), "rectángulo entero");
        sum = 0; for (int c = 0; c < 20; c++) sum += vrview_upload_area(0, 0, 320, 240, 0, c * 16, 0, 16, 240);
        CHECK(vrview_uploads_cover(sum, 320, 240, 0), "20 tiras de 16 px (FMV real)");
        sum = 0; for (int c = 0; c < 11; c++) sum += vrview_upload_area(0, 0, 320, 240, 0, c * 16, 0, 16, 240);
        CHECK(!vrview_uploads_cover(sum, 320, 240, 0), "11 de 20 tiras (55 %%): no");
        sum = 0; for (int c = 0; c < 12; c++) sum += vrview_upload_area(0, 0, 320, 240, 0, c * 16, 0, 16, 240);
        CHECK(vrview_uploads_cover(sum, 320, 240, 0), "12 de 20 tiras (60 %%): sí");
        sum = 0; for (int c = 0; c < 20; c++) sum += vrview_upload_area(0, 0, 320, 240, 0, 640 + c * 16, 0, 16, 240);
        CHECK(sum == 0 && !vrview_uploads_cover(sum, 320, 240, 0), "subidas fuera del área mostrada (texturas): no");
        sum = vrview_upload_area(0, 0, 320, 240, 0, 256, 0, 256, 256);
        CHECK(sum == 64 * 240 && !vrview_uploads_cover(sum, 320, 240, 0), "una textura que solo roza el área: no");
        sum = 0; for (int c = 0; c < 20; c++) sum += vrview_upload_area(0, 0, 320, 240, 1, c * 24, 0, 24, 240);
        CHECK(sum == 480 * 240 && vrview_uploads_cover(sum, 320, 240, 1), "24 bits: 20 tiras de 24 media-palabras");
        sum = 0; for (int c = 0; c < 12; c++) sum += vrview_upload_area(0, 0, 320, 240, 1, c * 16, 0, 16, 240);
        CHECK(!vrview_uploads_cover(sum, 320, 240, 1), "24 bits: 12 tiras de 16 media-palabras (40 %%) no llegan");
        sum = vrview_upload_area(0, 256, 384, 256, 0, 0, 256, 384, 256);
        CHECK(vrview_uploads_cover(sum, 384, 256, 0), "área mostrada en la mitad inferior de la VRAM");
        CHECK(vrview_upload_area(0, 0, 0, 0, 0, 0, 0, 320, 240) == 320 * 240, "área nominal por defecto 320x240");
    }
    {   /* el 3D del motor es siempre la pantalla estereoscópica: ENGINE tras la histéresis de entrada, sin atajo ni respaldo */
        VrView g; VrViewIn k; memset(&k, 0, sizeof k); k.scene3d = 1;
        vrview_reset(&g); vrview_step(&g, &k);
        CHECK(g.kind == VRV_K_UI, "la histéresis de entrada se mantiene (kind %d tras 1 fotograma)", g.kind);
        for (int n = 0; n < 20; n++) { vrview_step(&g, &k); CHECK(g.kind == VRV_K_UI || g.kind == VRV_K_ENGINE, "nunca otra clase"); }
        CHECK(g.kind == VRV_K_ENGINE, "ENGINE tras %d fotogramas", g.to_engine);
    }
    {   /* vrhost_keep_filter: ¿el recubrimiento (alarma roja, destellos, fundidos) se conserva como post-proceso por ojo en vez de ir al panel HUD? */
        /* (native, cine_cand, scene3d, has_3d, world_valid, n_scan, frame_on, n_strips) */
        CHECK(vrhost_keep_filter(0, 0, 0, 1, 1, 0, 0, 0) == 1, "gpu + mundo (has_3d y world_valid) = conserva el filtro");
        CHECK(vrhost_keep_filter(0, 1, 1, 0, 0, 0, 0, 0) == 1, "gpu + cinemática con 3D = conserva el filtro");
        CHECK(vrhost_keep_filter(0, 0, 0, 1, 0, 0, 0, 0) == 0 && vrhost_keep_filter(0, 0, 0, 0, 1, 0, 0, 0) == 0, "gpu sin mundo valido = al panel");
        CHECK(vrhost_keep_filter(0, 0, 0, 0, 0, 0, 0, 0) == 0, "gpu en menu = al panel");
        CHECK(vrhost_keep_filter(0, 0, 0, 0, 0, 0, 0, 2) == 1, "gpu + tiras de agua = conserva el filtro");
        CHECK(vrhost_keep_filter(1, 0, 0, 1, 1, 0, 0, 0) == 1, "nativo + mundo");
        CHECK(vrhost_keep_filter(1, 0, 0, 0, 0, 1, 0, 0) == 1 && vrhost_keep_filter(1, 0, 0, 0, 0, 0, 1, 0) == 1, "nativo: gafas y marco de mascara");
        CHECK(vrhost_keep_filter(1, 1, 1, 1, 1, 1, 1, 0) == 0, "nativo + cinemática con 3D = al panel");
        CHECK(vrhost_keep_filter(1, 1, 0, 1, 1, 0, 0, 0) == 1, "nativo + cinemática sin 3D sustancial: sigue la regla del mundo");
        CHECK(vrhost_keep_filter(1, 0, 0, 0, 0, 0, 0, 0) == 0, "nativo en menu = al panel");
    }
    {   /* vrhost_stereo2d_inside: ¿el 2D de la lista va dentro de las texturas de la pantalla 3D (y no en el panel aparte)? */
        /* (gpu, lista con 3D o base guardada, compuesto dentro del estéreo) */
        CHECK(vrhost_stereo2d_inside(1, 1, 1) == 1, "gpu + 3D/base + compuesto = dentro");
        CHECK(vrhost_stereo2d_inside(1, 1, 0) == 0, "gpu pero el estéreo no pudo componer esta lista = panel aparte (hp2)");
        CHECK(vrhost_stereo2d_inside(1, 0, 1) == 0, "gpu sin 3D ni base sobre la que componer = panel aparte");
        CHECK(vrhost_stereo2d_inside(0, 1, 1) == 0 && vrhost_stereo2d_inside(0, 0, 0) == 0, "nativo = panel aparte");
        CHECK(vrhost_stereo2d_inside(1, 0, 0) == 0, "gpu sin nada = panel aparte");
    }
    fprintf(stderr, fails ? "test_view: %d FALLOS\n" : "test_view: OK\n", fails);
    return fails ? 1 : 0;
}
