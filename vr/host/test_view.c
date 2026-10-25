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
        CHECK(vrview_kind_of(&k) == VRV_K_BACKUP, "sin partida y con 3D y sin medida = respaldo (imagen de la VRAM): el extractor no es de fiar");
        k.cover1 = 101;
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "sin partida y con 3D pero con cobertura medida buena = cinemática del motor");
        k.cover1 = 1;
        CHECK(vrview_kind_of(&k) == VRV_K_BACKUP, "con cobertura medida mala = respaldo");
        k.cover1 = 0;
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
        CHECK(vrview_kind_of(&k) == VRV_K_BACKUP, "con 3D no es vídeo (sin partida y sin medida: respaldo)");
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
        for (int q = 0; q <= VRV_K_BACKUP; q++) CHECK(vrview_kind_name(q)[0] != '?' && vrview_cine_type(q)[0] != '?', "nombre %d", q);
    }
    /* --- los tres casos del cine: fmv / motor / respaldo, con la intro medida (cobertura 30,2 26,4 26,2 27,8 14,9 0,1 0,6 0,1 0,1 1,4 0,2 %) --- */
    {
        char buf[700];
        /* (c) introducción sin partida: respaldo desde el primer fotograma y se queda, aunque llegue 1 sola medida buena */
        VrView m; vrview_reset(&m);
        VrViewIn in; memset(&in, 0, sizeof in); in.scene3d = 1;
        for (int n = 0; n < 3; n++) vrview_step(&m, &in);
        CHECK(m.kind == VRV_K_BACKUP && m.mode == VRV_CINEMA && (m.changed & VRV_CH_KIND), "intro: respaldo a los 3 fotogramas (clase %d)", m.kind);
        vrview_cine_describe(&m, &in, buf, sizeof buf);
        CHECK(strstr(buf, "[cine] tipo=respaldo motivo=") && strstr(buf, "sin partida"), "registro respaldo: %s", buf);
        static const int intro[] = { 30, 26, 26, 28, 15, 0, 1, 0, 0, 1, 0 };
        for (unsigned q = 0; q < sizeof intro / sizeof *intro; q++) {
            in.cover1 = (uint8_t)(intro[q] + 1); in.cover_new = 1;
            for (int n = 0; n < 50; n++) { vrview_step(&m, &in); in.cover_new = 0; CHECK(m.kind == VRV_K_BACKUP, "intro medida %d%%: sigue en respaldo (n=%d)", intro[q], n); }
        }
        CHECK(m.cov_state == 2, "intro: cobertura mala = no fiel");
        vrview_cine_describe(&m, &in, buf, sizeof buf);
        CHECK(strstr(buf, "tipo=respaldo") && strstr(buf, "cobertura baja") && strstr(buf, "cobertura=0%"), "registro respaldo medido: %s", buf);
        in.cover1 = 91; in.cover_new = 1; vrview_step(&m, &in); in.cover_new = 0;
        for (int n = 0; n < 40; n++) vrview_step(&m, &in);
        CHECK(m.kind == VRV_K_BACKUP, "una sola medida buena no basta");
        /* (b) escena del motor con partida: motor; una medida mala aislada (15,9 %) no la corta; dos seguidas sí */
        vrview_reset(&m);
        VrViewIn ci = world(); ci.in_control = 0; ci.cinematic = 1; ci.scene3d = 1;
        for (int n = 0; n < 5; n++) vrview_step(&m, &ci);
        CHECK(m.kind == VRV_K_ENGINE, "cinemática con partida y sin medida: motor");
        vrview_cine_describe(&m, &ci, buf, sizeof buf);
        CHECK(strstr(buf, "tipo=motor") != NULL, "registro motor: %s", buf);
        static const int cut[] = { 68, 69, 16, 85, 90, 92, 95 };   /* cinemática real: la medida de 16 % (corte de plano) no la corta */
        for (unsigned q = 0; q < sizeof cut / sizeof *cut; q++) {
            ci.cover1 = (uint8_t)(cut[q] + 1); ci.cover_new = 1;
            for (int n = 0; n < 50; n++) { vrview_step(&m, &ci); ci.cover_new = 0; CHECK(m.kind == VRV_K_ENGINE, "cinemática medida %d%%: sigue en motor", cut[q]); }
        }
        ci.cover1 = 5; ci.cover_new = 1; vrview_step(&m, &ci); ci.cover_new = 0; for (int n = 0; n < 40; n++) vrview_step(&m, &ci);
        CHECK(m.kind == VRV_K_ENGINE, "una medida mala aislada no basta");
        ci.cover1 = 4; ci.cover_new = 1; vrview_step(&m, &ci); ci.cover_new = 0; for (int n = 0; n < 40; n++) vrview_step(&m, &ci);
        CHECK(m.kind == VRV_K_BACKUP, "dos malas de las últimas 4: respaldo");
        /* muerte medida: 58,9 % del volcado (660 triángulos ausentes) cuenta como mala; 57,9 / 88,6 / 22,1 / 98,5 / 97,2 % del registro: la fiabilidad sigue la ventana */
        vrview_reset(&m);
        VrViewIn dd = world(); dd.in_control = 0; dd.dead = 1; dd.scene3d = 1;
        for (int n = 0; n < 5; n++) vrview_step(&m, &dd);
        CHECK(m.kind == VRV_K_ENGINE, "muerte con partida y sin medida: motor");
        static const int death[] = { 58, 59 };
        for (unsigned q = 0; q < 2; q++) { dd.cover1 = (uint8_t)(death[q] + 1); dd.cover_new = 1; vrview_step(&m, &dd); dd.cover_new = 0; for (int n = 0; n < 40; n++) vrview_step(&m, &dd); }
        CHECK(m.kind == VRV_K_BACKUP && m.reason == VRV_R_DEAD, "muerte con 58-59 %%: respaldo (imagen fiel de la VRAM)");
        static const int death2[] = { 89, 22, 99 };
        for (unsigned q = 0; q < 3; q++) { dd.cover1 = (uint8_t)(death2[q] + 1); dd.cover_new = 1; vrview_step(&m, &dd); dd.cover_new = 0; for (int n = 0; n < 40; n++) vrview_step(&m, &dd); }
        CHECK(m.kind == VRV_K_BACKUP, "alternar buenas y malas no devuelve al motor");
        for (int q = 0; q < 3; q++) { dd.cover1 = 98; dd.cover_new = 1; vrview_step(&m, &dd); dd.cover_new = 0; for (int n = 0; n < 40; n++) vrview_step(&m, &dd); }
        CHECK(m.kind == VRV_K_ENGINE, "3 buenas seguidas la recuperan");
        /* medidas en zona gris (62-80 %%) no cambian nada */
        vrview_reset(&m);
        for (int n = 0; n < 5; n++) vrview_step(&m, &ci);
        for (int q = 0; q < 6; q++) { ci.cover1 = 71; ci.cover_new = 1; vrview_step(&m, &ci); ci.cover_new = 0; for (int n = 0; n < 15; n++) vrview_step(&m, &ci); }
        CHECK(m.kind == VRV_K_ENGINE && m.cov_state == 0, "zona gris: sin cambios");
        /* sin 3D 1 s las medidas se olvidan: la siguiente escena parte del estado por defecto */
        VrViewIn nob = ci; nob.scene3d = 0; nob.cover1 = 0;
        for (int n = 0; n < 60; n++) vrview_step(&m, &nob);
        CHECK(m.cov_state == 0, "medidas olvidadas sin 3D");
        /* (d) INTRO EN 3D (cobertura de nativo + relleno global): la medida del registro de la intro (nativo solo 1,7 / 0,8 / 0,3 %%; con el relleno 98,8 / 99,4 / 98,3 %%) */
        {
            VrView iv; vrview_reset(&iv);
            VrViewIn ii; memset(&ii, 0, sizeof ii); ii.scene3d = 1;
            uint32_t seen = 0;
            for (int n = 0; n < 3; n++) vrview_step(&iv, &ii);
            CHECK(iv.kind == VRV_K_BACKUP, "intro sin medida: respaldo");
            /* el relleno mide en el fotograma 813: primera medida buena = fiable al instante (3 fotogramas de histéresis a motor) */
            CHECK(vrview_cover_feed(&ii, &seen, 816, 1, 813, 99, 1, 800, 2) == 1 && ii.cover1 == 100 && ii.cover_src == 2 && ii.cover_new == 1, "la medida con relleno gana a la del nativo solo (src %d)", ii.cover_src);
            for (int n = 0; n < 4; n++) { vrview_step(&iv, &ii); ii.cover_new = 0; }
            CHECK(iv.kind == VRV_K_ENGINE && iv.cov_state == 1, "intro con nativo+relleno 99 %%: motor (3D), clase %d estado %d", iv.kind, iv.cov_state);
            vrview_cine_describe(&iv, &ii, buf, sizeof buf);
            CHECK(strstr(buf, "tipo=motor") && strstr(buf, "relleno global") && strstr(buf, "cobertura=99%(nativo+relleno)"), "registro intro motor: %s", buf);
            /* las medidas siguientes (50 fotogramas) mantienen el motor; el nativo solo (34 %%, lo que veía el criterio anterior) ya no decide */
            static const int fillm[] = { 99, 98, 99, 98, 99 };
            uint32_t vb = 816 + 25;
            for (unsigned q = 0; q < sizeof fillm / sizeof *fillm; q++, vb += 25) {
                vrview_cover_feed(&ii, &seen, vb + 3, 1, vb, fillm[q], 1, vb - 5, 34);
                for (int n = 0; n < 30; n++) { vrview_step(&iv, &ii); ii.cover_new = 0; CHECK(iv.kind == VRV_K_ENGINE, "intro medida %d%%: sigue en 3D", fillm[q]); }
            }
            /* sin relleno (apagado o sin escena) vale el nativo solo, que en la intro es bajo: respaldo plano (la cobertura TOTAL sigue siendo baja) */
            VrView jv; vrview_reset(&jv); VrViewIn jj; memset(&jj, 0, sizeof jj); jj.scene3d = 1; uint32_t seen2 = 0; uint32_t b2 = 900;
            for (int q = 0; q < 3; q++, b2 += 25) { vrview_cover_feed(&jj, &seen2, b2 + 2, 0, 0, 0, 1, b2, 34 - q * 10); for (int n = 0; n < 30; n++) { vrview_step(&jv, &jj); jj.cover_new = 0; } }
            CHECK(jv.kind == VRV_K_BACKUP && jv.cov_state == 2 && jj.cover_src == 1, "sin relleno y nativo 34 %%: respaldo (clase %d)", jv.kind);
            /* con relleno pero cobertura total baja (escena que el relleno no puede reponer): respaldo */
            VrView kv; vrview_reset(&kv); VrViewIn kk; memset(&kk, 0, sizeof kk); kk.scene3d = 1; uint32_t seen3 = 0; uint32_t b3 = 900;
            for (int q = 0; q < 3; q++, b3 += 25) { vrview_cover_feed(&kk, &seen3, b3 + 2, 1, b3, 40, 1, b3, 30); for (int n = 0; n < 30; n++) { vrview_step(&kv, &kk); kk.cover_new = 0; } }
            CHECK(kv.kind == VRV_K_BACKUP && kv.cov_state == 2, "nativo+relleno 40 %%: respaldo (clase %d)", kv.kind);
            /* una primera medida MALA sola no cambia el estado (sin partida: sigue el respaldo; con partida: el extractor) */
            VrView lv; vrview_reset(&lv); VrViewIn ll = world(); ll.in_control = 0; ll.cinematic = 1; ll.scene3d = 1; ll.cover1 = 11; ll.cover_new = 1;
            vrview_step(&lv, &ll); ll.cover_new = 0; for (int n = 0; n < 10; n++) vrview_step(&lv, &ll);
            CHECK(lv.cov_state == 0 && lv.kind == VRV_K_ENGINE, "una primera medida mala no cambia nada con partida");
            /* vrview_cover_feed: espaciado, caducidad, retroceso del contador (carga de partida), punteros nulos */
            VrViewIn fi; memset(&fi, 0, sizeof fi); uint32_t sn = 0;
            CHECK(vrview_cover_feed(&fi, &sn, 1000, 1, 990, 97, 0, 0, 0) == 1 && fi.cover_new == 1 && sn == 990, "primera medida: nueva");
            CHECK(vrview_cover_feed(&fi, &sn, 1001, 1, 991, 97, 0, 0, 0) == 1 && fi.cover_new == 0 && sn == 990 && fi.cover1 == 98, "a menos de 25 fotogramas: vigente pero no nueva");
            CHECK(vrview_cover_feed(&fi, &sn, 1020, 1, 1015, 12, 0, 0, 0) == 1 && fi.cover_new == 1 && sn == 1015 && fi.cover1 == 13, "a 25 fotogramas: nueva");
            CHECK(vrview_cover_feed(&fi, &sn, 1200, 1, 1015, 97, 1, 1190, 50) == 1 && fi.cover_src == 1 && fi.cover1 == 51, "relleno caducado (> 150 fotogramas): se usa el nativo");
            CHECK(vrview_cover_feed(&fi, &sn, 1500, 1, 1015, 97, 1, 1190, 50) == 0, "todo caducado: sin medida");
            CHECK(vrview_cover_feed(&fi, &sn, 300, 1, 290, 90, 0, 0, 0) == 1 && fi.cover_new == 1 && sn == 290, "el contador retrocede (carga): medida nueva");
            CHECK(vrview_cover_feed(NULL, &sn, 1, 0, 0, 0, 0, 0, 0) == 0 && vrview_cover_feed(&fi, NULL, 1, 0, 0, 0, 0, 0, 0) == 0, "punteros nulos");
            /* M1: gana la medida MÁS RECIENTE de las dos fuentes (no siempre la del relleno); empate: el relleno */
            { VrViewIn ni; memset(&ni, 0, sizeof ni); uint32_t sx = 0;
              CHECK(vrview_cover_feed(&ni, &sx, 1000, 1, 990, 99, 1, 998, 30) == 1 && ni.cover_src == 1 && ni.cover1 == 31 && sx == 998, "M1: nativo mas reciente que el relleno: gana el nativo (src %d)", ni.cover_src);
              CHECK(vrview_cover_feed(&ni, &sx, 1030, 1, 1025, 99, 1, 1025, 30) == 1 && ni.cover_src == 2 && ni.cover1 == 100, "M1: empate: el relleno");
              CHECK(vrview_cover_feed(&ni, &sx, 1100, 1, 1040, 99, 1, 1090, 30) == 1 && ni.cover_src == 1, "M1: relleno vigente pero mas viejo que el nativo: el nativo"); }
            /* M1: la ventana de histéresis no mezcla fuentes: al cambiar de fuente se reinicia */
            { VrView wv; vrview_reset(&wv); VrViewIn wi; memset(&wi, 0, sizeof wi); wi.scene3d = 1;
              wi.cover_src = 2; wi.cover1 = 11; wi.cover_new = 1; vrview_step(&wv, &wi);
              wi.cover_new = 1; vrview_step(&wv, &wi);
              CHECK(__builtin_popcount(wv.cov_bad_mask) == 2 && wv.cov_src == 2, "M1: dos malas del relleno en la ventana (%d)", __builtin_popcount(wv.cov_bad_mask));
              wi.cover_src = 1; wi.cover1 = 11; wi.cover_new = 1; vrview_step(&wv, &wi);
              CHECK(__builtin_popcount(wv.cov_bad_mask) == 1 && wv.cov_src == 1, "M1: cambia la fuente: ventana nueva, solo cuenta la medida nueva (%d)", __builtin_popcount(wv.cov_bad_mask));
              wi.cover_src = 2; wi.cover1 = 100; wi.cover_new = 1; vrview_step(&wv, &wi);
              CHECK(wv.cov_bad_mask == 0 && __builtin_popcount(wv.cov_good_mask) == 1 && wv.cov_src == 2, "M1: de vuelta a la otra fuente: ventana nueva otra vez"); }
        }
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
    {   /* modo gpu (VrViewIn.gpu3d): el 3D de la lista de la GPU no depende de la cobertura nativa; nunca respaldo; a ENGINE al instante */
        VrView g;
        VrViewIn k;
        memset(&k, 0, sizeof k);
        k.scene3d = 1; k.gpu3d = 1;            /* sin partida, sin medida de cobertura (cover1 = 0): con el nativo sería BACKUP */
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "gpu: 3D del motor sin partida y sin cobertura = ENGINE, no respaldo");
        k.cover1 = 1; k.cover_src = 1;         /* cobertura medida 0 %: con el nativo, BACKUP */
        CHECK(vrview_kind_of(&k) == VRV_K_ENGINE, "gpu: cobertura nativa 0 %% no cuenta");
        k.gpu3d = 0;
        CHECK(vrview_kind_of(&k) == VRV_K_BACKUP, "nativo: la misma entrada sigue siendo respaldo");
        k.gpu3d = 1;
        vrview_reset(&g);
        vrview_step(&g, &k);
        CHECK(g.kind == VRV_K_ENGINE && (g.changed & VRV_CH_KIND), "gpu: ENGINE en el primer fotograma (sin histéresis de %d)", g.to_engine);
        vrview_reset(&g);
        k.gpu3d = 0; k.cover1 = 0; k.game = 1;
        vrview_step(&g, &k);
        CHECK(g.kind == VRV_K_UI, "nativo: la histéresis de entrada sigue (kind %d tras 1 fotograma)", g.kind);
        for (int i = 0; i < 20; i++) { memset(&k, 0, sizeof k); k.scene3d = 1; k.gpu3d = 1; vrview_step(&g, &k); CHECK(g.kind != VRV_K_BACKUP, "gpu: nunca respaldo"); }
        k.game = 1; k.in_control = 1; k.cam_active = 1; k.scene3d = 1;         /* mundo con 3D: sigue siendo mundo */
        CHECK(vrview_kind_of(&k) == VRV_K_WORLD, "gpu: el mundo no cambia");
        memset(&k, 0, sizeof k); k.scene3d = 1; k.gpu3d = 1; k.mdec = 1;       /* vídeo (MDEC) sin cambios */
        CHECK(vrview_kind_of(&k) == VRV_K_VIDEO, "gpu: el vídeo MDEC sigue siendo vídeo");
        { char buf[512]; vrview_reset(&g); memset(&k, 0, sizeof k); k.scene3d = 1; k.gpu3d = 1; vrview_step(&g, &k); vrview_cine_describe(&g, &k, buf, sizeof buf);
          CHECK(strstr(buf, "tipo=motor") && strstr(buf, "modo gpu"), "registro gpu: %s", buf); }
    }
    fprintf(stderr, fails ? "test_view: %d FALLOS\n" : "test_view: OK\n", fails);
    return fails ? 1 : 0;
}
