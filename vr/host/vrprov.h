#pragma once
/* Procedencia de las primitivas de la GPU en vrhost.c (funciones puras, probadas en test_vrprov.c; runtime/prov.h).
 *  - vrprov_acc / vrprov_report: por cada id de procedencia, cuántas primitivas (GpuOp.prov) dejó en las listas del último segundo, su caja, los modos de
 *    semitransparencia (abr) que usó y la vista vigente; el registro [prov] saca las 16 con más primitivas.
 *  - vrprov_pick: volcado automático la primera vez que aparece cada actor de interés (scope.c, cinema.c, telop.c, motse.c en la lista de actores del juego)
 *    y la primera vez que la vista pasa a selector, pausa o códec (uno por motivo y sesión). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gpu.h"
#include "prov.h"
#include "vr_view.h"

enum { VP_V_WORLD, VP_V_SELECTOR, VP_V_PAUSE, VP_V_CODEC, VP_V_CINE, VP_V_OTHER, VP_V_N };
static inline const char *vrprov_view_name(int v) {
    static const char *nm[VP_V_N] = {"world", "selector", "pause", "codec", "cine", "other"};
    return v >= 0 && v < VP_V_N ? nm[v] : nm[VP_V_OTHER];
}
/* Vista de vr_view.h (kind VRV_K_*, reason VRV_R_*) a la etiqueta del registro: primero el motivo (ventana de armas, pausa, comunicador), luego el tipo. */
static inline int vrprov_view(int kind, int reason) {
    if (reason == VRV_R_SELECTOR || reason == VRV_R_MENU) return VP_V_SELECTOR;
    if (reason == VRV_R_PAUSE_WORLD || reason == VRV_R_PAUSED) return VP_V_PAUSE;
    if (reason == VRV_R_COMMS) return VP_V_CODEC;
    if (kind == VRV_K_ENGINE) return VP_V_CINE;
    if (kind == VRV_K_WORLD) return VP_V_WORLD;
    return VP_V_OTHER;
}

/* ------------------------------------------------------------------------------------------------ acumulador */
typedef struct { uint32_t ops; int x0, y0, x1, y1; uint8_t abr; ProvEntry e; } VrProvAcc1;   /* abr: máscara de modos usados por primitivas semitransparentes */
typedef struct { VrProvAcc1 a[PROV_MAX]; uint32_t lists, ops; } VrProvAcc;

static inline void vrprov_acc_box(VrProvAcc1 *a, int x, int y) {
    if (!a->ops || x < a->x0) a->x0 = x;
    if (!a->ops || y < a->y0) a->y0 = y;
    if (!a->ops || x > a->x1) a->x1 = x;
    if (!a->ops || y > a->y1) a->y1 = y;
}
static inline void vrprov_acc(VrProvAcc *A, const GpuDrawList *l) {
    if (!l) return;
    A->lists++;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        unsigned id = o->prov < PROV_MAX ? o->prov : 0;
        VrProvAcc1 *a = &A->a[id];
        if (id && l->prov && id < l->nprov && !a->e.id) a->e = l->prov[id];
        if (o->type == GPU_OP_POLY || o->type == GPU_OP_LINE) {
            int nv = o->type == GPU_OP_LINE ? 2 : o->nv > 4 ? 4 : o->nv;
            for (int k = 0; k < nv; k++) vrprov_acc_box(a, o->v[k].x, o->v[k].y);
        } else if (o->type == GPU_OP_RECT) {
            vrprov_acc_box(a, o->v[0].x, o->v[0].y);
            vrprov_acc_box(a, o->v[0].x + o->rw, o->v[0].y + o->rh);
        }
        if ((o->type == GPU_OP_POLY || o->type == GPU_OP_RECT) && (o->flags & GPU_F_SEMI)) a->abr |= (uint8_t)(1u << (o->abr & 3));
        a->ops++;
        A->ops++;
    }
}

/* Una línea `[prov]` con las 16 entradas de más primitivas, y vacía el acumulador. Devuelve su longitud (0 = nada que contar; el acumulador se vacía igual). */
static inline int vrprov_report(VrProvAcc *A, int view, char *buf, size_t cap) {
    int n = 0;
    if (A->ops && cap) {
        n = snprintf(buf, cap, "[prov] vista=%s listas=%u ops=%u |", vrprov_view_name(view), A->lists, A->ops);
        for (int k = 0; k < 16; k++) {
            unsigned best = 0, bo = 0;
            for (unsigned id = 0; id < PROV_MAX; id++) if (A->a[id].ops > bo) { bo = A->a[id].ops; best = id; }
            if (!bo) break;
            const VrProvAcc1 *a = &A->a[best];
            char abr[8]; int na = 0;
            for (int m = 0; m < 4; m++) if (a->abr & (1u << m)) abr[na++] = (char)('0' + m);
            abr[na] = 0;
            if (n < (int)cap)
                n += snprintf(buf + n, cap - (size_t)n, " %s|%08x: ops=%u box=(%d,%d)-(%d,%d) abr=%s;", best ? (a->e.name[0] ? a->e.name : "?") : "-", (unsigned)a->e.fn, a->ops, a->x0, a->y0, a->x1, a->y1, na ? abr : "-");
            A->a[best].ops = 0;   /* ya listada: no se repite (se vacía el resto abajo) */
        }
        if (n >= (int)cap) n = (int)cap - 1;
    }
    memset(A, 0, sizeof *A);
    return n;
}

/* ------------------------------------------------------------------------------------------------ volcado automático */
#define VP_N_ACTORS 4
enum { VP_T_VIEW0 = VP_N_ACTORS };   /* códigos de vrprov_pick: 0..3 actor (VP_ACTORS), VP_T_VIEW0 + VP_V_* vista (selector, pausa, códec) */
typedef struct { uint8_t done[VP_N_ACTORS + VP_V_N]; } VrProvTrig;
static inline const char *vrprov_actor_name(int i) {
    static const char *nm[VP_N_ACTORS] = {"scope.c", "cinema.c", "telop.c", "motse.c"};
    return i >= 0 && i < VP_N_ACTORS ? nm[i] : "";
}
/* Índice (0..VP_N_ACTORS-1) del actor de interés cuyo nombre de depuración es `name` (coincide si el nombre lo contiene), o -1. */
static inline int vrprov_actor_idx(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < VP_N_ACTORS; i++) if (strstr(name, vrprov_actor_name(i))) return i;
    return -1;
}
/* Siguiente volcado pendiente (código) dado el conjunto de actores presentes (máscara por índice) y la vista actual; -1 = ninguno. No marca: ver vrprov_done. */
static inline int vrprov_pick(const VrProvTrig *t, unsigned actors_present, int view) {
    for (int i = 0; i < VP_N_ACTORS; i++) if ((actors_present >> i & 1u) && !t->done[i]) return i;
    if ((view == VP_V_SELECTOR || view == VP_V_PAUSE || view == VP_V_CODEC) && !t->done[VP_T_VIEW0 + view]) return VP_T_VIEW0 + view;
    return -1;
}
static inline void vrprov_done(VrProvTrig *t, int code) { if (code >= 0 && code < VP_N_ACTORS + VP_V_N) t->done[code] = 1; }
/* Nombre corto del motivo (carpeta y registro): scope, cinema, telop, motse, view_selector, view_pause, view_codec. */
static inline const char *vrprov_code_name(int code) {
    static const char *nm[VP_N_ACTORS] = {"scope", "cinema", "telop", "motse"};
    if (code >= 0 && code < VP_N_ACTORS) return nm[code];
    switch (code - VP_T_VIEW0) { case VP_V_SELECTOR: return "view_selector"; case VP_V_PAUSE: return "view_pause"; case VP_V_CODEC: return "view_codec"; default: return "none"; }
}
