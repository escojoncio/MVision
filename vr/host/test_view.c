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

    /* sin control (ascensor, transiciones, gateo mal leído): depende de la cámara VR y de que haya escena nativa APTA (no basta el puente) */
    i = world(); i.in_control = 0; i.scene3d = 1; i.native_ok = 1;
    CHECK(vrview_reason(&i) == VRV_R_WORLD && vrview_kind_of(&i) == VRV_K_WORLD, "ascensor: cámara VR activa y mundo nativo apto = mundo");
    i.cam_active = 0; CHECK(vrview_reason(&i) == VRV_R_LOST_CONTROL && vrview_kind_of(&i) == VRV_K_ENGINE, "sin control, sin cámara VR y con escena nativa apta y 3D = pantalla 3D");
    i.scene3d = 0; CHECK(vrview_kind_of(&i) == VRV_K_UI, "sin 3D = 2D");
    i = world(); i.in_control = 0; i.scene3d = 1; i.native_ok = 0;
    CHECK(vrview_reason(&i) == VRV_R_LOST_CONTROL && vrview_kind_of(&i) == VRV_K_UI, "carga con 3D basura (escena nativa no apta), con cámara VR: 2D, nunca pantalla 3D");
    i.cam_active = 0; CHECK(vrview_kind_of(&i) == VRV_K_UI, "carga con 3D basura sin cámara VR: 2D");
    i = world(); i.in_control = 1; i.scene3d = 1; i.native_ok = 1;
    CHECK(vrview_reason(&i) == VRV_R_WORLD && vrview_kind_of(&i) == VRV_K_WORLD, "gateo (control conservado): mundo");
    i = world(); i.dead = 1; i.in_control = 0; i.cam_active = 0; i.scene3d = 1; i.native_ok = 0;
    CHECK(vrview_reason(&i) == VRV_R_DEAD && vrview_kind_of(&i) == VRV_K_ENGINE, "muerte sin cámara VR: pantalla 3D con 3D aunque no haya escena nativa apta");
    i = world(); i.in_control = 0; i.menu_open = 1; i.scene3d = 1; i.native_ok = 1; CHECK(vrview_kind_of(&i) == VRV_K_WORLD, "ventana de armas con cámara VR sigue en el mundo");
    i.cam_active = 0; CHECK(vrview_kind_of(&i) == VRV_K_UI, "ventana de armas sin cámara VR con 3D detrás = 2D");
    i = world(); i.in_control = 0; i.comms = 1; i.scene3d = 1; i.native_ok = 1; CHECK(vrview_reason(&i) == VRV_R_COMMS && vrview_kind_of(&i) == VRV_K_UI, "radio gana al mundo nativo");

    /* caja (BRIDGE_PS_BOX): misma lógica que el gateo; la vista no depende de la postura: con cámara VR y mundo nativo apto es mundo, con o sin control */
    i = world(); i.in_control = 1; i.scene3d = 1; i.native_ok = 1; CHECK(vrview_reason(&i) == VRV_R_WORLD && vrview_kind_of(&i) == VRV_K_WORLD, "caja con control: mundo");
    i.in_control = 0; CHECK(vrview_reason(&i) == VRV_R_WORLD && vrview_kind_of(&i) == VRV_K_WORLD, "caja sin control (transicion al entrar/salir): mundo continuo");
    CHECK(vrhost_duct_ring(1, 1, 2) == 1 && vrhost_duct_ring(1, 0, 2) == 0 && vrhost_duct_ring(0, 1, 2) == 0 && vrhost_duct_ring(1, 1, 1) == 0, "anillo del conducto: solo con camara VR + 1a persona de gateo (INTRUDE) + anillo de resta");
    CHECK(vrhost_box_mask(1, 1, 1) == 1 && vrhost_box_mask(1, 0, 1) == 0 && vrhost_box_mask(1, 1, 0) == 0 && vrhost_box_mask(0, 1, 1) == 0, "mascara de la caja: solo nativo + en la caja + con mascara");

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
        e.game = 1; e.scene3d = 1; e.cinematic = 1;       /* con partida: el extractor es de fiar sin medidas (cinemática real, bandera de escena del motor) */
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
    {   /* vrhost_native_sfx: ¿pinta el render nativo el efecto de pantalla (bit NS_SFX_*) de la última escena publicada? (renderer listo, escena apta, banderas, edad en sincronismos, bit) */
        const uint32_t GAS = 2u, FADE = 4u;
        CHECK(vrhost_native_sfx(1, 1, FADE, 0, FADE) == 1 && vrhost_native_sfx(1, 1, GAS | FADE, VRHOST_SFX_MAX_AGE, GAS) == 1, "renderer listo + escena apta y reciente con el efecto = lo pinta el nativo");
        CHECK(vrhost_native_sfx(1, 1, GAS, 0, FADE) == 0 && vrhost_native_sfx(1, 1, 0, 0, FADE) == 0, "la escena no trae ese efecto (o no trae efectos): sigue en el panel");
        CHECK(vrhost_native_sfx(1, 0, FADE, 0, FADE) == 0, "escena no apta: el renderer repite la ultima valida con SUS efectos: el fundido de ahora sigue en el panel");
        CHECK(vrhost_native_sfx(1, 1, FADE, VRHOST_SFX_MAX_AGE + 1, FADE) == 0, "sin escenas nuevas (el renderer repite la ultima valida): sigue en el panel");
        CHECK(vrhost_native_sfx(0, 1, FADE, 0, FADE) == 0, "renderer nativo sin preparar: sigue en el panel");
    }
    {   /* vrhost_scene_sfx: banderas de efectos que prov_covered ve de la escena publicada; anillo del conducto recortado (duct_cut) = cubierto por el nativo (sus paquetes de gas_efct.c salen del panel) */
        const uint32_t GAS = 2u, FADE = 4u;
        CHECK(vrhost_scene_sfx(1, FADE, 0.5f, 1) == (FADE | GAS), "anillo del conducto recortado: el nativo lo 'cubre' (GAS) y respeta el resto");
        CHECK(vrhost_scene_sfx(1, FADE, 0.5f, 0) == FADE, "sin recorte (otra situacion): GAS no se inventa");
        CHECK(vrhost_scene_sfx(1, GAS | FADE, 0.5f, 0) == (GAS | FADE) && vrhost_scene_sfx(1, GAS, 0.0f, 0) == 0, "gas de verdad: tal cual; intensidad 0 = sin anillo");
        CHECK(vrhost_scene_sfx(1, 0, 0.0f, 1) == 0, "recorte sin intensidad de anillo: no cubre");
        CHECK(vrhost_scene_sfx(0, GAS | FADE, 0.5f, 1) == 0, "escena sin efectos (NS_SV_FX): 0");
    }
    {   /* vrhost_drop_world2d: ¿los polígonos 3D sin vista (el nativo los dibuja) se quitan de la capa 2D? (native, kind, reason) */
        CHECK(vrhost_drop_world2d(1, VRV_K_WORLD, VRV_R_WORLD) == 1, "nativo + mundo = quita el 3D sin resolver");
        CHECK(vrhost_drop_world2d(1, VRV_K_WORLD, VRV_R_SELECTOR) == 1, "nativo + selector de armas/objetos = quita el 3D sin resolver (cielo, estrellas): la ventana son rects 2D y el cielo sin vista no es un cuadrado en el HUD");
        CHECK(vrhost_drop_world2d(1, VRV_K_WORLD, VRV_R_PAUSE_WORLD) == 1, "nativo + pausa con la camara VR = quita el 3D sin resolver: el nativo sigue dibujando el mundo congelado (nat-diag dibujado=true en pausa, log 13-11) y esos poligonos (volcado d0001: cuña con textura) eran el cuadrado con imagen del panel");
        CHECK(vrhost_drop_world2d(1, VRV_K_ENGINE, VRV_R_CINEMATIC) == 1 && vrhost_drop_world2d(1, VRV_K_ENGINE, VRV_R_NO_GAME) == 1, "nativo + cinematica del motor (con o sin partida) = la capa 2D solo lleva 2D (el 3D lo dibuja el nativo)");
        CHECK(vrhost_drop_world2d(1, VRV_K_UI, VRV_R_NO_GAME) == 0 && vrhost_drop_world2d(1, VRV_K_VIDEO, VRV_R_NO_GAME) == 0, "nativo + menu / video = todo es 2D");
        CHECK(vrhost_drop_world2d(0, VRV_K_WORLD, VRV_R_WORLD) == 0 && vrhost_drop_world2d(0, VRV_K_ENGINE, VRV_R_CINEMATIC) == 0, "modo gpu: no");
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

    /* ---- pegado a la pared: cine con la pantalla 3D del motor, nunca «esperando cámara VR» ---- */
    {
        VrViewIn w = world(); w.cam_active = 0; w.wall = 1;
        CHECK(vrview_reason(&w) == VRV_R_WALL, "pared con control y cámara VR apagada a propósito");
        CHECK(vrview_kind_of(&w) == VRV_K_UI, "pared sin lista del motor todavía: la función pura da 2D (el paso la retiene)");
        w.scene3d = 1; CHECK(vrview_kind_of(&w) == VRV_K_ENGINE, "pared con 3D del motor: pantalla 3D");
        w.in_control = 0; CHECK(vrview_reason(&w) == VRV_R_LOST_CONTROL, "sin control no es vista de pared");
        w = world(); w.cam_active = 0; w.wall = 1; w.comms = 1; CHECK(vrview_reason(&w) == VRV_R_COMMS, "la radio gana a la pared");
        VrView vw; vrview_reset(&vw);
        VrViewIn a = world(); a.native_ok = 1; a.list3d = 1;
        for (int k = 0; k < 10; k++) vrview_step(&vw, &a);
        CHECK(vw.mode == VRV_WORLD && vw.kind == VRV_K_WORLD, "mundo antes de la pared");
        /* se pega: la cámara VR se apaga; las primeras listas aún salen con la cámara VR (scene3d = 0, list3d = 1): la vista ENTERA se retiene (con modo cine y clase mundo la app
         * enseñaría la pantalla plana con el fotograma de la PS1) */
        a.cam_active = 0; a.wall = 1;
        int saw_flat = 0;   /* lo que la app dibuja como pantalla plana: modo cine con una clase que no es la pantalla 3D */
        for (int k = 0; k < 8; k++) { vrview_step(&vw, &a); saw_flat |= vw.mode == VRV_CINEMA && vw.kind != VRV_K_ENGINE; CHECK(vw.reason == VRV_R_HOLD, "pared sin lista del motor: vista retenida, no transición"); }
        CHECK(vw.mode == VRV_WORLD && !saw_flat && vw.kind == VRV_K_WORLD, "pared: mundo retenido hasta la lista del motor (modo %d clase %d)", vw.mode, vw.kind);
        a.scene3d = 1;
        for (int k = 0; k < 6; k++) { vrview_step(&vw, &a); saw_flat |= vw.mode == VRV_CINEMA && vw.kind != VRV_K_ENGINE; }
        CHECK(vw.mode == VRV_CINEMA && vw.kind == VRV_K_ENGINE && vw.reason == VRV_R_WALL && !saw_flat, "pared: pantalla 3D en cuanto el motor dibuja con su cámara, sin pantalla plana");
        int saw_ui = 0;
        /* se despega: sin pared ni cámara VR todavía = transición (vista retenida), después el mundo */
        a.wall = 0; a.scene3d = 0;
        vrview_step(&vw, &a);
        CHECK(vw.reason == VRV_R_PENDING && vw.mode == VRV_CINEMA && vw.kind == VRV_K_ENGINE, "al despegarse: transición con la vista retenida");
        a.cam_active = 1;
        for (int k = 0; k < 4; k++) { vrview_step(&vw, &a); saw_ui |= vw.kind == VRV_K_UI; }
        CHECK(vw.mode == VRV_WORLD && vw.kind == VRV_K_WORLD && !saw_ui, "vuelta al mundo sin pasar por 2D");
    }
    /* ---- el motor no dibuja (carga / cambio de zona): vista congelada, nunca la pantalla plana con el fotograma viejo ---- */
    {
        VrView vf; vrview_reset(&vf);
        VrViewIn a = world(); a.native_ok = 1; a.list3d = 1;
        for (int k = 0; k < 10; k++) vrview_step(&vf, &a);
        CHECK(vf.mode == VRV_WORLD && vf.kind == VRV_K_WORLD, "mundo antes de la carga");
        VrViewIn l; memset(&l, 0, sizeof l); l.cam_active = 1; l.frozen = 1; l.list3d = 1;      /* sin partida, motor sin dibujar */
        for (int k = 0; k < 200; k++) { vrview_step(&vf, &l); CHECK(vf.mode == VRV_WORLD && vf.kind == VRV_K_WORLD && vf.reason == VRV_R_FROZEN, "carga: mundo congelado (k=%d modo %d clase %d)", k, vf.mode, vf.kind); }
        CHECK(vf.changed == 0, "congelado: sin cambios que registrar");
        /* zona nueva: el motor vuelve a dibujar con una escena (cámara del guion) */
        VrViewIn c = world(); c.in_control = 0; c.cam_active = 0; c.cinematic = 1; c.list3d = 1;   /* primeras listas: 3D sin cámara del motor reconocida */
        int saw_flat = 0;
        for (int k = 0; k < 6; k++) { vrview_step(&vf, &c); saw_flat |= vf.mode == VRV_CINEMA && vf.kind != VRV_K_ENGINE; }
        CHECK(vf.reason == VRV_R_HOLD && vf.mode == VRV_WORLD && vf.kind == VRV_K_WORLD, "zona nueva sin cámara del motor: vista retenida");
        c.scene3d = 1;
        for (int k = 0; k < 6; k++) { vrview_step(&vf, &c); saw_flat |= vf.mode == VRV_CINEMA && vf.kind != VRV_K_ENGINE; }
        CHECK(vf.mode == VRV_CINEMA && vf.kind == VRV_K_ENGINE && !saw_flat, "de la carga a la escena sin pasar por la pantalla plana");
        /* una pantalla 2D de verdad (sin 3D en la lista) sí pasa a 2D */
        VrViewIn m; memset(&m, 0, sizeof m);
        for (int k = 0; k < 12; k++) vrview_step(&vf, &m);
        CHECK(vf.kind == VRV_K_UI, "lista sin 3D: 2D");
        /* motor sin dibujar desde 2D (título -> carga): no hay vista nativa que congelar, clasificación normal (el motivo sigue siendo el de la pantalla: mismo formato de pantalla de texto) */
        m.frozen = 1;
        for (int k = 0; k < 20; k++) vrview_step(&vf, &m);
        CHECK(vf.kind == VRV_K_UI && vf.mode == VRV_CINEMA && vf.reason == VRV_R_NO_GAME, "carga desde un menú: se queda el menú con su motivo");
        /* un vídeo durante la carga no se congela */
        m.mdec = 1; vrview_step(&vf, &m); CHECK(vf.reason != VRV_R_FROZEN, "con vídeo no hay congelación");
        for (int k = 0; k < 5; k++) vrview_step(&vf, &m);
        CHECK(vf.kind == VRV_K_VIDEO, "vídeo durante la carga: pantalla de vídeo");
        /* tope de seguridad */
        VrView vt; vrview_reset(&vt); vt.frozen_max = 5;
        l.list3d = 0;
        for (int k = 0; k < 10; k++) vrview_step(&vt, &a);
        for (int k = 0; k < 5; k++) { vrview_step(&vt, &l); CHECK(vt.reason == VRV_R_FROZEN, "dentro del tope"); }
        vrview_step(&vt, &l); CHECK(vt.reason == VRV_R_NO_GAME, "pasado el tope: clasificación normal");
        vrview_step(&vt, &a); vrview_step(&vt, &l); CHECK(vt.reason == VRV_R_FROZEN, "el tope se rearma al volver a dibujar");
    }
    /* ---- arranque de una escena desde el mundo: las primeras listas salen aún con la cámara VR (scene3d = 0): la clase no pasa por 2D; la radio, la pausa y el juego detenido sí ---- */
    {
        VrView vs; vrview_reset(&vs);
        VrViewIn a = world(); a.native_ok = 1; a.list3d = 1;
        for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        VrViewIn c = world(); c.in_control = 0; c.cam_active = 0; c.cinematic = 1; c.list3d = 1;
        int saw_flat = 0;
        for (int k = 0; k < 8; k++) { vrview_step(&vs, &c); saw_flat |= vs.mode == VRV_CINEMA && vs.kind != VRV_K_ENGINE; }
        CHECK(vs.mode == VRV_WORLD && vs.kind == VRV_K_WORLD && vs.reason == VRV_R_HOLD && !saw_flat, "escena: vista retenida mientras la lista trae 3D sin cámara del motor");
        c.scene3d = 1; for (int k = 0; k < 6; k++) { vrview_step(&vs, &c); saw_flat |= vs.mode == VRV_CINEMA && vs.kind != VRV_K_ENGINE; }
        CHECK(vs.mode == VRV_CINEMA && vs.kind == VRV_K_ENGINE && !saw_flat, "escena: pantalla 3D sin pasar por la plana");
        /* desde la pantalla 3D también se retiene (corte de plano con listas sin cámara reconocida) y la retención está acotada: pasado hold_max, pantalla plana */
        c.scene3d = 0;
        for (int k = 0; k < vs.hold_max; k++) { vrview_step(&vs, &c); CHECK(vs.reason == VRV_R_HOLD && vs.mode == VRV_CINEMA && vs.kind == VRV_K_ENGINE, "retención desde la pantalla 3D (k=%d)", k); }
        for (int k = 0; k < 12; k++) vrview_step(&vs, &c);
        CHECK(vs.reason == VRV_R_CINEMATIC && vs.kind == VRV_K_UI, "retención agotada: clasificación normal");
        c.list3d = 0; vrview_step(&vs, &c); c.list3d = 1; c.scene3d = 1; for (int k = 0; k < 8; k++) vrview_step(&vs, &c);
        c.scene3d = 0; vrview_step(&vs, &c); CHECK(vs.reason == VRV_R_HOLD, "la retención se rearma cuando deja de hacer falta");
        /* listas con y sin 3D alternadas: el tope no se rearma con cada lista sin 3D (la retención termina) */
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        { VrViewIn alt = world(); alt.in_control = 0; alt.cam_active = 0; alt.cinematic = 1; int holds = 0;
          for (int k = 0; k < 600; k++) { alt.list3d = (uint8_t)(k & 1); vrview_step(&vs, &alt); holds += vs.reason == VRV_R_HOLD; }
          CHECK(holds == vs.hold_max && vs.kind == VRV_K_UI && vs.mode == VRV_CINEMA, "lista 3D alternada: retención acotada (%d fotogramas) y después 2D", holds); }
        /* una carga no gasta el plazo de espera de la cámara VR: tras ella sigue la transición (velo), no la pantalla plana */
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        { VrViewIn nc = world(); nc.cam_active = 0; nc.frozen = 1;
          for (int k = 0; k < 400; k++) { vrview_step(&vs, &nc); CHECK(vs.reason == VRV_R_FROZEN && vs.mode == VRV_WORLD, "carga con control y sin cámara VR: congelado (k=%d)", k); }
          nc.frozen = 0; vrview_step(&vs, &nc); vrview_step(&vs, &nc);
          CHECK(vs.reason == VRV_R_PENDING && vs.mode == VRV_WORLD && vs.kind == VRV_K_WORLD, "tras la carga: esperando la cámara VR, no cine (motivo %d)", vs.reason); }
        /* una ventana 2D del juego pedida se enseña aunque el motor no dibuje */
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        { VrViewIn pw = world(); pw.native_ok = 1; pw.paused = 1; pw.frozen = 1; vrview_step(&vs, &pw);
          CHECK(vs.reason == VRV_R_PAUSE_WORLD, "pausa sobre el mundo con el motor sin dibujar: se enseña la pausa");
          pw.cam_active = 0; pw.in_control = 0; for (int k = 0; k < 8; k++) vrview_step(&vs, &pw);
          CHECK(vs.reason == VRV_R_PAUSED && vs.kind == VRV_K_UI, "pausa sin cámara VR con el motor sin dibujar: 2D"); }
        /* mezcla de modo cine y clase mundo (histéresis separadas): es la pantalla plana para la app, no una vista nativa que retener */
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        { VrViewIn fz; memset(&fz, 0, sizeof fz); fz.frozen = 1; fz.list3d = 1; vs.mode = VRV_CINEMA; vs.kind = VRV_K_WORLD; vrview_step(&vs, &fz);
          CHECK(vs.reason != VRV_R_FROZEN && vs.reason != VRV_R_HOLD, "cine con clase mundo: sin retención (motivo %d)", vs.reason); }
        /* la muerte (menú de fin de partida) no se retiene */
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        VrViewIn dd = world(); dd.in_control = 0; dd.cam_active = 0; dd.dead = 1; dd.list3d = 1;
        for (int k = 0; k < 8; k++) vrview_step(&vs, &dd);
        CHECK(vs.kind == VRV_K_UI && vs.reason == VRV_R_DEAD, "muerte sin escena del motor: 2D, sin retención");
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        for (int k = 0; k < 8; k++) vrview_step(&vs, &c);
        c.scene3d = 1; for (int k = 0; k < 6; k++) vrview_step(&vs, &c);
        VrViewIn r = world(); r.in_control = 0; r.cam_active = 0; r.comms = 1; r.list3d = 1;
        for (int k = 0; k < 15; k++) vrview_step(&vs, &r);
        CHECK(vs.kind == VRV_K_UI, "radio: 2D aunque la lista traiga 3D");
        vrview_reset(&vs); for (int k = 0; k < 10; k++) vrview_step(&vs, &a);
        VrViewIn pz = world(); pz.in_control = 0; pz.cam_active = 0; pz.paused = 1; pz.list3d = 1;
        for (int k = 0; k < 6; k++) vrview_step(&vs, &pz);
        CHECK(vs.kind == VRV_K_UI, "pausa sin cámara VR: 2D");
    }
    /* ---- cámara de evento del juego con el mando retirado (panel del ascensor): cine con la pantalla 3D del motor, como la pared; secuencia medida en el visor (build 0d4635f, vb 28900-30594) ---- */
    {
        VrViewIn ev = world(); ev.cam_active = 0; ev.event = 1;
        CHECK(vrview_reason(&ev) == VRV_R_EVENT, "cámara de evento con control y cámara VR apagada a propósito");
        CHECK(vrview_kind_of(&ev) == VRV_K_UI, "evento sin lista del motor todavía: la función pura da 2D (el paso la retiene)");
        ev.scene3d = 1; CHECK(vrview_kind_of(&ev) == VRV_K_ENGINE, "evento con 3D del motor: pantalla 3D");
        ev.cam_active = 1; CHECK(vrview_reason(&ev) == VRV_R_EVENT, "evento con la cámara VR aún activa (se apaga en la siguiente construcción): ya es la vista del juego");
        ev.in_control = 0; CHECK(vrview_reason(&ev) == VRV_R_LOST_CONTROL, "sin control no es la vista de evento");
        ev = world(); ev.cam_active = 0; ev.event = 1; ev.comms = 1; CHECK(vrview_reason(&ev) == VRV_R_COMMS, "la radio gana al evento");
        ev = world(); ev.in_control = 0; ev.cam_active = 0; ev.event = 1; ev.cinematic = 1; CHECK(vrview_reason(&ev) == VRV_R_CINEMATIC, "la escena (llegada del ascensor) gana al evento");
        ev = world(); ev.in_control = 0; ev.event = 1; ev.menu_open = 1; CHECK(vrview_reason(&ev) == VRV_R_SELECTOR, "la ventana de armas/objetos gana al evento");
        CHECK(strcmp(vrview_reason_name(VRV_R_EVENT), "?") != 0, "el motivo tiene nombre");

        int flat = 0, pend = 0, changes = 0;   /* pantalla plana sin negro; «esperando cámara VR»; cambios de modo o clase */
#define EV(vv, inp, n) for (int k_ = 0; k_ < (n); k_++) { vrview_step(&(vv), &(inp)); flat |= (vv).mode == VRV_CINEMA && (vv).kind != VRV_K_ENGINE && !(vv).black; pend |= (vv).reason == VRV_R_PENDING; changes += ((vv).changed & (VRV_CH_MODE | VRV_CH_KIND)) != 0; }
        VrView ve; vrview_reset(&ve);
        VrViewIn a = world(); a.native_ok = 1; a.list3d = 1; a.fresh = 255;
        for (int k = 0; k < 10; k++) vrview_step(&ve, &a);
        CHECK(ve.mode == VRV_WORLD && ve.kind == VRV_K_WORLD, "mundo antes del panel");
        /* el jugador entra en la zona del panel: el motor pone su cámara de evento; la cámara VR se apaga y las primeras listas aún salen con ella (scene3d = 0, list3d = 1): vista retenida */
        a.cam_active = 0; a.event = 1;
        EV(ve, a, 6);
        CHECK(ve.reason == VRV_R_HOLD && ve.mode == VRV_WORLD && ve.kind == VRV_K_WORLD && !flat && !pend, "panel sin lista del motor: mundo retenido, ni pantalla plana ni transición (motivo %d)", ve.reason);
        a.scene3d = 1;
        EV(ve, a, 4);
        CHECK(ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && ve.reason == VRV_R_EVENT && !flat && !pend, "panel: pantalla 3D con la cámara del juego (modo %d clase %d motivo %d)", ve.mode, ve.kind, ve.reason);
        /* 25 s eligiendo planta (vb 28900-30145) y puerta + viaje (hasta 30282): la vista no cambia; una lista suelta sin 3D reconocido no la mueve */
        changes = 0;
        EV(ve, a, 600);
        a.scene3d = 0; EV(ve, a, 2); a.scene3d = 1;
        EV(ve, a, 780);
        CHECK(ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && ve.reason == VRV_R_EVENT && !changes && !flat && !pend && !ve.black, "panel y viaje: la vista se mantiene (cambios %d)", changes);
        /* carga de la planta elegida (183 sin dibujo; 2 «sin partida»; 53 sin dibujo con partida, los 2 primeros con control y la cámara de evento de la llegada): negro con la pantalla 3D retenida */
        VrViewIn l; memset(&l, 0, sizeof l); l.frozen = 1; l.list3d = 1;
        for (int k = 0; k < 9; k++) vrview_step(&ve, &l);
        CHECK(!ve.black, "viaje: una parada corta (9 VBlanks) no es carga");
        vrview_step(&ve, &l);
        CHECK(ve.black && ve.reason == VRV_R_FROZEN, "viaje: el negro empieza con la carga");
        EV(ve, l, 172);
        VrViewIn n; memset(&n, 0, sizeof n);
        EV(ve, n, 2);
        VrViewIn e2 = world(); e2.cam_active = 0; e2.frozen = 1; e2.event = 1;
        EV(ve, e2, 2);
        e2.in_control = 0; e2.event = 0; e2.cinematic = 1;
        EV(ve, e2, 51);
        CHECK(ve.black && !flat && !pend, "carga: en negro de principio a fin");
        /* llegada: escena del motor (cámara de evento de la cabina con el mando del guion), 5 fotogramas sin 3D y después con 3D y escenas nuevas; al recuperar el control, la cámara VR */
        VrViewIn c = world(); c.in_control = 0; c.cam_active = 0; c.cinematic = 1;
        EV(ve, c, 5);
        CHECK(ve.black, "llegada sin 3D todavía: negro");
        c.scene3d = c.list3d = 1; c.fresh = 3;
        EV(ve, c, 57);
        CHECK(!ve.black && ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && ve.reason == VRV_R_CINEMATIC && !flat, "llegada: escena en la pantalla 3D, sin negro");
        VrViewIn z = world(); z.cam_active = 0; z.scene3d = z.list3d = 1; z.fresh = 255;
        pend = 0; EV(ve, z, 8);
        CHECK(pend && ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE, "fin de la llegada: transición con la vista retenida");
        z.cam_active = 1; z.native_ok = 1; z.scene3d = 0;
        EV(ve, z, 4);
        CHECK(ve.mode == VRV_WORLD && ve.kind == VRV_K_WORLD && ve.reason == VRV_R_WORLD && !flat, "fin de la llegada: primera persona");

        /* salir del panel sin viajar (equis): el motor quita su cámara; transición con la vista retenida y vuelta al mundo sin pasar por la pantalla plana */
        vrview_reset(&ve); flat = pend = 0;
        a = world(); a.native_ok = 1; a.list3d = 1; a.fresh = 255;
        for (int k = 0; k < 10; k++) vrview_step(&ve, &a);
        a.cam_active = 0; a.event = 1; EV(ve, a, 6); a.scene3d = 1; EV(ve, a, 50);
        CHECK(ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && ve.reason == VRV_R_EVENT && !pend, "panel otra vez");
        a.event = 0;
        EV(ve, a, 8);
        CHECK(ve.reason == VRV_R_PENDING && ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && !flat, "al salir del panel: transición con la vista retenida");
        a.cam_active = 1; a.scene3d = 0;
        EV(ve, a, 4);
        CHECK(ve.mode == VRV_WORLD && ve.kind == VRV_K_WORLD && !flat, "vuelta al mundo sin pasar por 2D");
        /* pausa ante el panel: la cámara VR sigue apagada (vrcam_event_view se mantiene con cam_ctl); pausa en la pantalla plana y de vuelta a la pantalla 3D, sin primera persona ni transición */
        a = world(); a.cam_active = 0; a.event = 1; a.scene3d = a.list3d = 1; a.fresh = 255;
        EV(ve, a, 20);
        CHECK(ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && ve.reason == VRV_R_EVENT, "panel antes de la pausa");
        VrViewIn pz = a; pz.in_control = 0; pz.event = 0; pz.paused = 1;
        pend = 0; int saw_world = 0;
        for (int k = 0; k < 30; k++) { vrview_step(&ve, &pz); saw_world |= ve.mode == VRV_WORLD || ve.kind == VRV_K_WORLD; pend |= ve.reason == VRV_R_PENDING; }
        CHECK(ve.reason == VRV_R_PAUSED && ve.mode == VRV_CINEMA && ve.kind == VRV_K_UI && !saw_world && !pend, "pausa ante el panel: pantalla plana");
        for (int k = 0; k < 6; k++) { vrview_step(&ve, &a); saw_world |= ve.mode == VRV_WORLD || ve.kind == VRV_K_WORLD; pend |= ve.reason == VRV_R_PENDING; }
        CHECK(ve.reason == VRV_R_EVENT && ve.mode == VRV_CINEMA && ve.kind == VRV_K_ENGINE && !saw_world && !pend, "fin de la pausa: otra vez la pantalla 3D del panel");
        char buf[640];
        vrview_describe(&ve, &a, buf, sizeof buf);
        CHECK(strstr(buf, "evento=1"), "registro de la cámara de evento: %s", buf);
        /* a31 (vb 17361-17371): control devuelto con el mando aún retirado (PAD_OFF) y sin cámara de evento: la vista del juego en la pantalla 3D, con la causa en el registro */
        VrViewIn pd = world(); pd.cam_active = 0; pd.event = 1; pd.event_cause = 16; pd.scene3d = 1;
        CHECK(vrview_reason(&pd) == VRV_R_EVENT && vrview_kind_of(&pd) == VRV_K_ENGINE, "mando retirado con control: pantalla 3D del motor");
        VrView pv; vrview_reset(&pv); for (int k = 0; k < 8; k++) vrview_step(&pv, &pd);
        vrview_describe(&pv, &pd, buf, sizeof buf);
        CHECK(pv.mode == VRV_CINEMA && pv.kind == VRV_K_ENGINE && strstr(buf, "causa=16"), "registro con la causa: %s", buf);
#undef EV
    }
    /* ---- fundido a negro (VrView.black): desde que el motor desmonta / carga con una vista nativa hasta que hay una escena nueva completa; secuencias medidas en el visor (build 0d4635f) ---- */
#define RUN(vv, inp, n, cond, ...) for (int k_ = 0; k_ < (n); k_++) { vrview_step(&(vv), &(inp)); flat |= (vv).mode == VRV_CINEMA && (vv).kind != VRV_K_ENGINE && !(vv).black; CHECK(cond, __VA_ARGS__); }
    {
        int flat = 0;   /* se vio la pantalla plana sin negro entre dos vistas nativas */
        VrView vb; vrview_reset(&vb);
        VrViewIn w = world(); w.native_ok = 1; w.list3d = 1; w.fresh = 255;
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        CHECK(vb.mode == VRV_WORLD && !vb.black, "mundo: sin negro");
        /* cambio de zona desde el mundo (vb 10868-11084): 196 sin dibujo, 9 «sin partida», 9 sin dibujo con control y sin cámara VR, y el mundo */
        VrViewIn l; memset(&l, 0, sizeof l); l.cam_active = 1; l.frozen = 1; l.list3d = 1;
        for (int k = 0; k < 9; k++) vrview_step(&vb, &l);
        CHECK(!vb.black && vb.reason == VRV_R_FROZEN, "parada corta (9 VBlanks): sin negro");
        vrview_step(&vb, &l);
        CHECK(vb.black && (vb.changed & VRV_CH_BLACK) && vb.reason == VRV_R_FROZEN, "el negro empieza al décimo VBlank sin dibujo");
        RUN(vb, l, 185, vb.black && vb.mode == VRV_WORLD && vb.kind == VRV_K_WORLD, "carga: negro con la vista retenida (k=%d)", k_);
        CHECK(vb.black_run == 186, "fotogramas en negro: %d", vb.black_run);
        VrViewIn n; memset(&n, 0, sizeof n); n.cam_active = 1;
        RUN(vb, n, 9, vb.black, "zona cargada, sin partida todavía: sigue en negro (k=%d)", k_);
        VrViewIn p = world(); p.cam_active = 0; p.frozen = 1;
        RUN(vb, p, 9, vb.black, "segundo tramo sin dibujo, ya con la pantalla plana vigente: sigue en negro (k=%d)", k_);
        VrViewIn z = world(); z.native_ok = 1;
        RUN(vb, z, 2, vb.black, "mundo sin escenas nuevas: negro (k=%d)", k_);
        z.fresh = 1; RUN(vb, z, 2, vb.black, "una sola escena nueva: negro (k=%d)", k_);
        z.fresh = 2; RUN(vb, z, 1, vb.black, "dos escenas pero el motor lleva menos de black_settle fotogramas dibujando: negro (k=%d)", k_);
        z.native_ok = 0; RUN(vb, z, 4, vb.black, "escenas nuevas no aptas para la primera persona (el nativo repetiría la zona anterior): negro (k=%d)", k_);
        z.native_ok = 1; vrview_step(&vb, &z);
        CHECK(!vb.black && (vb.changed & VRV_CH_BLACK) && vb.reason == VRV_R_WORLD, "escena nueva completa: se levanta el negro (negro %d motivo %d)", vb.black, vb.reason);
        CHECK(!flat, "cambio de zona: la pantalla plana nunca se vio sin negro");

        /* ascensor (vb 30282-30594): mundo sin control con cámara VR; 183 sin dibujo; 2 «sin partida»; 53 sin dibujo con partida; escena sin 3D aún; escena 3D */
        vrview_reset(&vb); flat = 0;
        VrViewIn e = world(); e.in_control = 0; e.native_ok = 1; e.list3d = 1; e.fresh = 255;
        for (int k = 0; k < 10; k++) vrview_step(&vb, &e);
        CHECK(vb.mode == VRV_WORLD && vb.reason == VRV_R_WORLD && !vb.black, "ascensor: mundo");
        RUN(vb, l, 9, 1, "ascensor: primeros VBlanks de la carga (k=%d)", k_);
        RUN(vb, l, 174, vb.black, "ascensor: negro en la carga (k=%d)", k_);
        n.list3d = 0; RUN(vb, n, 2, vb.black, "ascensor: hueco sin partida en negro (k=%d)", k_);
        VrViewIn e2 = world(); e2.cam_active = 0; e2.frozen = 1;
        RUN(vb, e2, 53, vb.black, "ascensor: segundo tramo sin dibujo en negro (k=%d)", k_);
        VrViewIn c = world(); c.in_control = 0; c.cam_active = 0; c.cinematic = 1;
        RUN(vb, c, 5, vb.black, "ascensor: escena aún sin 3D, menos de black_2d fotogramas: negro (k=%d)", k_);
        c.scene3d = c.list3d = 1; c.fresh = 3;      /* la lista con la cámara del motor llega con las escenas nuevas ya publicadas: el negro espera a que la pantalla 3D sea la vigente */
        RUN(vb, c, 3, vb.black, "ascensor: escenas nuevas pero la pantalla 3D aún no es la vigente: negro (k=%d)", k_);
        vrview_step(&vb, &c);
        CHECK(!vb.black && vb.mode == VRV_CINEMA && vb.kind == VRV_K_ENGINE, "ascensor: la escena de llegada levanta el negro en la pantalla 3D (negro %d clase %d)", vb.black, vb.kind);
        CHECK(!flat, "ascensor: la pantalla plana nunca se vio sin negro");

        /* cadena de escenas (vb 14014-14049): pantalla 3D; 8 sin dibujo; 2 dibujando con las banderas de la lista vieja y una escena del desmontaje; 25 sin dibujo; escena nueva */
        vrview_reset(&vb); flat = 0;
        VrViewIn s; memset(&s, 0, sizeof s); s.scene3d = s.list3d = 1; s.fresh = 255;      /* escena del motor sin jugador */
        for (int k = 0; k < 10; k++) vrview_step(&vb, &s);
        CHECK(vb.mode == VRV_CINEMA && vb.kind == VRV_K_ENGINE && !vb.black, "escena del motor en la pantalla 3D");
        VrViewIn sf = s; sf.frozen = 1; sf.fresh = 0;
        RUN(vb, sf, 8, !vb.black && vb.kind == VRV_K_ENGINE, "entre escenas: parada corta de 8 VBlanks sin negro (k=%d)", k_);
        VrViewIn sg = s; sg.fresh = 2;
        RUN(vb, sg, 2, !vb.black, "hueco de 2 fotogramas en el desmontaje: la cuenta de parada se reinicia (k=%d)", k_);
        RUN(vb, sf, 9, !vb.black, "entre escenas: aún sin negro (k=%d)", k_);
        vrview_step(&vb, &sf);
        CHECK(vb.black && vb.kind == VRV_K_ENGINE, "entre escenas: negro con la pantalla 3D retenida tras 10 VBlanks de parada");
        RUN(vb, sf, 15, vb.black, "entre escenas: negro (k=%d)", k_);
        RUN(vb, sg, 2, vb.black, "hueco de 2 fotogramas con negro: no lo levanta aunque haya escenas (k=%d)", k_);
        sg.fresh = 0; RUN(vb, sg, 8, vb.black, "escena nueva aún sin capturar: negro (k=%d)", k_);
        sg.fresh = 2; vrview_step(&vb, &sg);
        CHECK(!vb.black && vb.kind == VRV_K_ENGINE && vb.mode == VRV_CINEMA, "escena siguiente: se levanta el negro");
        CHECK(!flat, "cadena de escenas: la pantalla plana nunca se vio sin negro");

        /* fin de cinemática -> juego sin carga (vb 20226-20496): sin negro de carga (la espera de la cámara VR ya oscurece en la app) */
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        VrViewIn ci = world(); ci.in_control = 0; ci.cam_active = 0; ci.cinematic = 1; ci.scene3d = ci.list3d = 1; ci.fresh = 255;
        for (int k = 0; k < 10; k++) vrview_step(&vb, &ci);
        CHECK(vb.mode == VRV_CINEMA && vb.kind == VRV_K_ENGINE && !vb.black, "cinemática en la pantalla 3D");
        VrViewIn pe = world(); pe.cam_active = 0; pe.scene3d = pe.list3d = 1; pe.fresh = 255;
        RUN(vb, pe, 8, !vb.black && vb.reason == VRV_R_PENDING, "fin de escena: transición sin negro de carga (k=%d)", k_);
        VrViewIn w2 = world(); w2.native_ok = 1; w2.scene3d = w2.list3d = 1; w2.fresh = 255;
        RUN(vb, w2, 6, !vb.black, "vuelta al mundo: sin negro de carga (k=%d)", k_);
        CHECK(vb.mode == VRV_WORLD && vb.kind == VRV_K_WORLD, "fin de escena: mundo");

        /* muerte -> continuar (vb 7375-8070): escena de muerte, fin de partida 2D, carga desde 2D, espera de la cámara y mundo: nunca negro de carga (no hay vista nativa que tapar) */
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        VrViewIn d = world(); d.in_control = 0; d.cam_active = 0; d.cinematic = 1; d.dead = 1; d.scene3d = d.list3d = 1; d.fresh = 255;
        RUN(vb, d, 20, !vb.black, "muerte: sin negro (k=%d)", k_);
        VrViewIn go = d; go.stopped = 1; go.scene3d = go.list3d = 0;
        RUN(vb, go, 20, !vb.black, "fin de partida: sin negro (k=%d)", k_);
        CHECK(vb.kind == VRV_K_UI, "fin de partida: 2D");
        VrViewIn gl; memset(&gl, 0, sizeof gl); gl.frozen = 1;
        RUN(vb, gl, 14, !vb.black && vb.kind == VRV_K_UI, "continuar: carga desde 2D, sin negro (k=%d)", k_);
        VrViewIn gp = world(); gp.cam_active = 0; gp.frozen = 1;
        RUN(vb, gp, 9, !vb.black, "continuar: espera de la cámara (k=%d)", k_);
        RUN(vb, w, 4, !vb.black, "continuar: mundo (k=%d)", k_);
        CHECK(vb.mode == VRV_WORLD && vb.kind == VRV_K_WORLD, "continuar: mundo");

        /* la pausa, el selector y la radio no funden, aunque el motor no dibuje; una radio durante el negro lo levanta */
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        VrViewIn pw = w; pw.paused = 1; RUN(vb, pw, 30, !vb.black, "pausa sobre el mundo: sin negro (k=%d)", k_);
        pw.frozen = 1; RUN(vb, pw, 30, !vb.black, "pausa con el motor sin dibujar: sin negro (k=%d)", k_);
        VrViewIn se = w; se.in_control = 0; se.menu_open = 1; se.frozen = 1; RUN(vb, se, 30, !vb.black, "selector: sin negro (k=%d)", k_);
        VrViewIn ra = world(); ra.in_control = 0; ra.cam_active = 0; ra.comms = 1; ra.stopped = 1; ra.frozen = 1; RUN(vb, ra, 30, !vb.black, "radio: sin negro (k=%d)", k_);
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        RUN(vb, l, 9, 1, "carga: parada corta (k=%d)", k_);
        RUN(vb, l, 20, vb.black, "carga (k=%d)", k_);
        { int stale = 0;
          for (int k = 0; k < 20; k++) { vrview_step(&vb, &ra); stale |= !vb.black && vb.kind != VRV_K_UI; }
          CHECK(!vb.black && vb.kind == VRV_K_UI && vb.mode == VRV_CINEMA && !stale, "radio durante el negro: se levanta cuando el códec ya es la pantalla vigente, nunca sobre la vista retenida"); }
        /* un vídeo durante el negro lo levanta */
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        RUN(vb, l, 9, 1, "carga: parada corta (k=%d)", k_);
        RUN(vb, l, 20, vb.black, "carga (k=%d)", k_);
        { VrViewIn mv = l; mv.mdec = 1; int stale = 0;
          for (int k = 0; k < 8; k++) { vrview_step(&vb, &mv); stale |= !vb.black && vb.kind != VRV_K_VIDEO; }
          CHECK(!vb.black && vb.kind == VRV_K_VIDEO && !stale, "vídeo durante el negro: se levanta con la pantalla de vídeo ya vigente"); }

        /* carga hacia una pantalla 2D (título): el negro se levanta tras black_2d fotogramas dibujando sin 3D; con 3D de PS1 sin cámara del motor aguanta hasta hold_max */
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        RUN(vb, l, 9, 1, "carga: parada corta (k=%d)", k_);
        RUN(vb, l, 50, vb.black, "carga (k=%d)", k_);
        VrViewIn t2; memset(&t2, 0, sizeof t2);
        RUN(vb, t2, vb.black_2d - 1, vb.black, "destino 2D: aún negro (k=%d)", k_);
        vrview_step(&vb, &t2); CHECK(!vb.black && vb.kind == VRV_K_UI && vb.mode == VRV_CINEMA, "destino 2D estable: se levanta el negro");
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        RUN(vb, l, 9, 1, "carga: parada corta (k=%d)", k_);
        RUN(vb, l, 50, vb.black, "carga (k=%d)", k_);
        t2.list3d = 1;
        flat = 0;
        RUN(vb, t2, vb.hold_max, vb.black, "zona cargada con 3D de PS1 sin cámara del motor: negro, nunca la pantalla plana (k=%d)", k_);
        { int stale = 0;
          for (int k = 0; k < 6; k++) { vrview_step(&vb, &t2); stale |= !vb.black && vb.kind != VRV_K_UI; }
          CHECK(!vb.black && vb.kind == VRV_K_UI && !stale, "acotado por hold_max; el negro se levanta con la pantalla 2D ya vigente, no sobre la vista retenida"); }
        CHECK(!flat, "3D de PS1 sin cámara del motor: en negro mientras dura la retención");

        /* topes: destino nativo sin escenas nuevas (black_nat_max) y motor que no vuelve a dibujar (black_max) */
        vrview_reset(&vb);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        RUN(vb, l, 9, 1, "carga: parada corta (k=%d)", k_);
        RUN(vb, l, 50, vb.black, "carga (k=%d)", k_);
        z.fresh = 0;
        RUN(vb, z, vb.black_nat_max - 1, vb.black, "mundo sin escenas nuevas: negro (k=%d)", k_);
        vrview_step(&vb, &z); CHECK(!vb.black, "mundo sin escenas nuevas: acotado por black_nat_max");
        vrview_reset(&vb); vb.frozen_max = 20; vb.black_max = 40;
        for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        RUN(vb, l, 9, 1, "motor sin dibujar: parada corta (k=%d)", k_);
        RUN(vb, l, 39, vb.black, "motor sin dibujar: negro (k=%d)", k_);
        vrview_step(&vb, &l); CHECK(!vb.black, "motor que no vuelve a dibujar: acotado por black_max");
        RUN(vb, l, 100, !vb.black, "pasado el tope no vuelve al negro hasta que el motor dibuje (k=%d)", k_);

        char buf[640];
        vrview_reset(&vb); for (int k = 0; k < 10; k++) vrview_step(&vb, &w);
        for (int k = 0; k < 10; k++) vrview_step(&vb, &l);
        vrview_describe(&vb, &l, buf, sizeof buf);
        CHECK(strstr(buf, "negro=1") && strstr(buf, " negro "), "registro del negro: %s", buf);
        CHECK(vrview_step(NULL, &l) == VRV_CINEMA, "NULL");
        vrview_step(&vb, NULL); CHECK(!vb.black, "entradas NULL: sin negro");
    }
    /* ---- clase «el motor lleva la cámara y el jugador no»: toda ella a la pantalla 3D, y la primera persona del jugador (conductos, gateo, libre) no ---- */
    {
        /* (1) Barrido de todas las combinaciones de hechos del motor. Con la cámara del motor propia (escena, cámara de evento con control, pared con control) y sin ventana 2D encima, el motivo es el
         * de esa vista y la clase la pantalla 3D si hay 3D del motor: nunca MUNDO, aunque la cámara VR siga encendida. Sin ningún hecho del motor y con control, siempre MUNDO (la cámara de
         * 1.ª persona del juego en conductos y gateo no se distingue: es gameplay). */
        int bad_engine = 0, bad_world = 0, n_engine = 0, n_world = 0;
        for (unsigned m = 0; m < 1u << 12; m++) {
            VrViewIn s; memset(&s, 0, sizeof s);
            s.game = 1; s.in_control = m & 1; s.cam_active = (m >> 1) & 1; s.native_ok = (m >> 2) & 1; s.cinematic = (m >> 3) & 1; s.event = (m >> 4) & 1; s.wall = (m >> 5) & 1;
            s.comms = (m >> 6) & 1; s.paused = (m >> 7) & 1; s.menu_open = (m >> 8) & 1; s.stopped = (m >> 9) & 1; s.dead = (m >> 10) & 1; s.scene3d = (m >> 11) & 1;
            const int overlay = s.comms || s.paused || s.menu_open || s.stopped || s.dead;
            const int engine_cam = s.cinematic || (s.in_control && (s.event || s.wall));
            if (s.event && !s.in_control) continue;      /* el anfitrión nunca la publica sin control (vrhost.c: vin.event = event_view && g.in_control) */
            if (s.wall && !s.in_control) continue;
            const int r = vrview_reason(&s), k = vrview_kind_of(&s);
            if (engine_cam && !overlay) {
                n_engine++;
                const int okr = r == VRV_R_CINEMATIC || (!s.cinematic && (r == VRV_R_WALL || r == VRV_R_EVENT));
                if (!okr || k == VRV_K_WORLD || (s.scene3d && k != VRV_K_ENGINE)) { if (!bad_engine) fprintf(stderr, "máscara 0x%x: motivo %d clase %d\n", m, r, k); bad_engine++; }
            } else if (!engine_cam && !overlay && s.in_control && s.cam_active) {
                n_world++;
                if (r != VRV_R_WORLD || k != VRV_K_WORLD) { if (!bad_world) fprintf(stderr, "máscara 0x%x: motivo %d clase %d\n", m, r, k); bad_world++; }
            }
        }
        CHECK(n_engine >= 64 && n_world >= 4, "el barrido cubre casos (%d motor, %d mundo)", n_engine, n_world);
        CHECK(!bad_engine, "cámara del motor propia sin ventana 2D: %d de %d combinaciones no van a la pantalla 3D", bad_engine, n_engine);
        CHECK(!bad_world, "primera persona con control y sin hechos del motor: %d de %d combinaciones no se quedan en el mundo", bad_world, n_world);

        /* (2) Las escenas del motor del registro del visor (build 0d4635f, 15 en total: {vb de inicio, VBlanks con la cámara VR aún encendida, duración}). Se juegan con las banderas medidas: sin
         * control, escena, y las primeras listas con la cámara VR (3D sin cámara del motor reconocida) y después con la del motor. Entre ellas, las del jugador tumbado en un conducto cuya
         * cámara es la de 1.ª persona del juego (vb 13954, 19951, 34745). Todas llegan a la pantalla 3D en 12 VBlanks como mucho y no vuelven al mundo mientras dura la escena. */
        static const struct { int vb0, linger, len; } SC[] = {
            {7375, 9, 384}, {12434, 9, 82}, {13954, 9, 60}, {19951, 9, 60}, {20219, 0, 267}, {28532, 9, 88}, {28672, 9, 88}, {30520, 0, 64},
            {30784, 9, 222}, {32540, 9, 180}, {32730, 0, 127}, {34745, 9, 70}, {42653, 9, 40}, {44692, 0, 29}, {45123, 9, 400}};
        int late = 0, back = 0, world_end = 0;
        for (unsigned q = 0; q < sizeof SC / sizeof SC[0]; q++) {
            VrView v; vrview_reset(&v);
            VrViewIn g = world(); g.native_ok = 1; g.list3d = 1; g.fresh = 255;
            for (int k = 0; k < 20; k++) vrview_step(&v, &g);
            VrViewIn sc = g; sc.in_control = 0; sc.cinematic = 1;
            int reached = -1;
            for (int k = 0; k < SC[q].len; k++) {
                if (k < SC[q].linger) { sc.cam_active = 1; sc.scene3d = 0; }
                else { sc.cam_active = 0; sc.scene3d = 1; }
                vrview_step(&v, &sc);
                if (v.mode == VRV_CINEMA && v.kind == VRV_K_ENGINE && reached < 0) reached = k;
                if (reached >= 0 && (v.mode != VRV_CINEMA || v.kind != VRV_K_ENGINE)) back++;
            }
            if (reached < 0 || reached > 12) { late++; fprintf(stderr, "escena de vb %d: pantalla 3D en %d VBlanks\n", SC[q].vb0, reached); }
            /* el motor devuelve el control: transición con la vista retenida y de vuelta a la primera persona */
            VrViewIn fin = g; fin.cam_active = 0; fin.scene3d = 1;
            for (int k = 0; k < 8; k++) vrview_step(&v, &fin);
            fin.cam_active = 1; fin.scene3d = 0;
            for (int k = 0; k < 6; k++) vrview_step(&v, &fin);
            world_end += v.mode == VRV_WORLD && v.kind == VRV_K_WORLD;
        }
        CHECK(!late, "%d escenas del registro no llegan a la pantalla 3D en 12 VBlanks", late);
        CHECK(!back, "%d fotogramas de escena vuelven al mundo o a la plana con la escena en curso", back);
        CHECK(world_end == (int)(sizeof SC / sizeof SC[0]), "al acabar la escena, primera persona (%d de %d)", world_end, (int)(sizeof SC / sizeof SC[0]));

        /* (3) Gameplay con la cámara de 1.ª persona del juego (conductos d0020-d0023, gateo): con control y sin hechos del motor se queda en el mundo 1500 VBlanks seguidos, también con la lista
         * del motor 3D y la cámara VR encendida o con un parpadeo de un fotograma sin ella. */
        VrView vg; vrview_reset(&vg);
        VrViewIn d = world(); d.native_ok = 1; d.list3d = 1; d.fresh = 255; d.scene3d = 1;
        int left = 0;
        for (int k = 0; k < 1500; k++) { d.cam_active = k % 400 != 399; vrview_step(&vg, &d); if (k > 10 && (vg.mode != VRV_WORLD || vg.kind != VRV_K_WORLD)) left++; }
        CHECK(!left, "conducto / gateo con control: %d fotogramas fuera del mundo", left);
    }
    /* ---- emulación sin límite de ritmo durante la carga (vrhost_load_boost): solo en la cuenta atrás de la petición de carga del motor, con la vista ya en negro y sin vídeo ---- */
    {
        const int32_t load = 0x7FFEFF7D;   /* contador del motor medido en plena carga */
        CHECK(vrhost_load_boost(1, VRHOST_BOOST_DELAY, load, 0) == 1, "carga en negro: sin límite de ritmo");
        CHECK(vrhost_load_boost(1, VRHOST_BOOST_DELAY - 1, load, 0) == 0, "antes de que el fundido termine: ritmo normal");
        CHECK(vrhost_load_boost(0, 500, load, 0) == 0, "sin negro (carga desde un menú): ritmo normal");
        CHECK(vrhost_load_boost(1, 500, 4, 0) == 0 && vrhost_load_boost(1, 500, 0, 0) == 0 && vrhost_load_boost(1, 500, -1, 0) == 0, "contador pequeño (el guion oculta unos fotogramas) o a cero: ritmo normal");
        CHECK(vrhost_load_boost(1, 500, load, 1) == 0, "con vídeo: ritmo normal");
        CHECK(vrhost_load_boost(1, VRHOST_BOOST_MAX, load, 0) == 0, "tope de duración");
    }
    return fails ? 1 : 0;
}
