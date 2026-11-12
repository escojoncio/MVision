/* nsfill: relleno por la verdad del motor (ns_fill) sobre un volcado, con las medidas de verificación. NO ejecuta el juego.
 * Uso: nsfill DIR_VOLCADO [DIR_LISTA]   (el segundo, opcional, aporta list.bin si el primero no la trae)
 *   NSFILL_RAMCAM=1   usa siempre la cámara de la RAM (ignora el registro de la lista); por defecto ns_fill elige la que case mejor
 *   NSFILL_NOCALIB=1  sin la calibración de la cámara (desfase RAM/lista)
 *   NSFILL_REPS=N     repeticiones para medir el coste (def. 20)
 *   NSFILL_DETALLE=f  fichero con una línea por efecto de relleno (clase, textura, esquinas en mundo y error de reproyección)
 * Mide: (1) cobertura de la vista del juego, nativo y nativo+relleno, con el MISMO comparador independiente de ns_cover (ns_cover_core) sobre la escena nativa a la que se
 * añaden los triángulos rellenados; (2) error de posición de lo rellenado: reproyecta sus vértices del mundo a la pantalla con la cámara y lo compara con los x,y enteros de la
 * primitiva de la lista (píxeles del motor); (3) coste por fase. */
#include "../ns_cover.h"
#include "../ns_dump.h"
#include "../ns_fill.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void to_rtri(const NsEffect *e, NsRTri *out, NsAdaptSrc *src, uint32_t id, int *nout) {
    static const int T4[2][3] = {{0, 1, 2}, {1, 2, 3}};
    int nt = e->nv == 4 ? 2 : 1;
    for (int h = 0; h < nt; h++) {
        NsRTri *t = &out[*nout]; NsAdaptSrc *s = &src[*nout];
        memset(t, 0, sizeof *t); memset(s, 0, sizeof *s);
        for (int c = 0; c < 3; c++) {
            int k = e->nv == 4 ? T4[h][c] : c;
            memcpy(t->v[c].p, e->p[k], sizeof t->v[c].p);
            t->v[c].u = e->uv[k][0]; t->v[c].v = e->uv[k][1];
            t->v[c].r = e->rgb[k][0]; t->v[c].g = e->rgb[k][1]; t->v[c].b = e->rgb[k][2];
        }
        t->tpage = e->tpage; t->clut = e->clut; t->flags = (uint8_t)(((e->flags & NS_EF_TEXTURED) ? NS_TF_TEXTURED : 0) | ((e->flags & NS_EF_SEMI) ? NS_TF_SEMI : 0));
        s->key = 0xF1000000u + id; s->inst = 0x7FFF0000u + id; s->piece = 0; s->face = id; s->half = (uint32_t)h; s->mflags = NS_MF_TWOSIDED; s->vis = 1;
        (*nout)++;
    }
}

static void cover_report(const char *tag, const NsCoverResult *r) {
    uint32_t cov = 0, tot = 0, pose = r->cause[NC_POSE][0] + r->cause[NC_POSE][1];
    for (int c = 0; c < NC_N; c++) { tot += r->cause[c][0] + r->cause[c][1]; }
    cov = r->cause[NC_MATCH][0] + r->cause[NC_MATCH][1] + r->cause[NC_SPLIT][0] + r->cause[NC_SPLIT][1] + r->cause[NC_SUBDIV][0] + r->cause[NC_SUBDIV][1];
    printf("[comprobador %s] tris=%u cubierto=%u (%.2f%%) pose=%u sin_pose=%.2f%% | sin cubrir: pose=%u otra_textura=%u parcial=%u ausente=%u sprite=%u | error_medio_match=%.2fpx desplazados>2px=%u/%u\n",
           tag, tot, cov, tot ? 100.0 * cov / tot : 100.0, pose, tot > pose ? 100.0 * cov / (tot - pose) : 100.0,
           pose, r->cause[NC_OTHERTEX][0] + r->cause[NC_OTHERTEX][1], r->cause[NC_PARTIAL][0] + r->cause[NC_PARTIAL][1], r->cause[NC_ABSENT][0] + r->cause[NC_ABSENT][1],
           r->cause[NC_BILLBOARD][0] + r->cause[NC_BILLBOARD][1], r->shift_mean_px, r->shift_bad, r->shift_n);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: nsfill DIR [DIR_LISTA]\n"); return 2; }
    NsDump *d = ns_dump_open(argv[1]);
    if (!d) { fprintf(stderr, "no se pudo abrir %s\n", argv[1]); return 1; }
    NsDump *dl = argc > 2 ? ns_dump_open(argv[2]) : d;
    if (!dl || !dl->has_list) { fprintf(stderr, "sin list.bin\n"); return 1; }
    float lm[9], le[3];
    int have_list_cam = !getenv("NSFILL_RAMCAM") && ns_dump_meta_floats(dl, "rec_M", lm, 9) == 9 && ns_dump_meta_floats(dl, "rec_eye", le, 3) == 3;
    NsFillRunOpts ro;
    memset(&ro, 0, sizeof ro);
    if (have_list_cam) { ro.list_M = lm; ro.list_eye = le; }
    ro.keep_native = 1;
    ro.list_vr = (int)ns_dump_meta_num(dl, "rec_active", 0) != 0;
    NsFillCfg cfg;
    ns_fill_cfg_default(&cfg); cfg.only_absent = 1;   /* como la app */
    if (getenv("NSFILL_POSE")) cfg.pose_fill = atoi(getenv("NSFILL_POSE"));
    if (getenv("NSFILL_NOCALIB")) cfg.calib = 0;   /* sin calibrar la cámara de la lista contra el nativo (desfase RAM/lista) */
    FILE *dbg = getenv("NSFILL_TRI") ? fopen(getenv("NSFILL_TRI"), "w") : NULL;
    cfg.dbg = dbg;
    uint32_t frame = (uint32_t)ns_dump_meta_num(dl, "game_frame", 1000);
    int reps = getenv("NSFILL_REPS") ? atoi(getenv("NSFILL_REPS")) : 20;
    NsFillSession *s = ns_fill_session_create();
    NsFillStats st;
    int cam_used = 0;
    int rc = ns_fill_session_run(s, d->ram, d->scratch, dl->list.ops, dl->list.nops, &ro, &cfg, frame, &st, &cam_used);
    if (rc) { fprintf(stderr, "fallo %d\n", rc); return 1; }
    cfg.dbg = NULL; if (dbg) fclose(dbg);
    char buf[4096];
    ns_fill_format(&st, frame, buf, sizeof buf);
    printf("camara=%s\n%s", cam_used ? "RAM" : "lista", buf);
    ns_fill_format_diag(&st, frame, buf, sizeof buf);
    printf("%s", buf);
    uint32_t nfx = 0;
    const NsEffect *fx = ns_fill_session_effects(s, &nfx);
    NsFillNative nat; const uint8_t *skip; NsFillCam cam;
    ns_fill_session_native(s, &nat, &skip, &cam);

    /* (2) error de posición: reproyección con la cámara usada frente a los x,y enteros de la lista */
    double A[9], Ai[9];
    ns_fill_cam_maps(&cam, A, Ai);
    double esum = 0, emax = 0; long en = 0, e1 = 0, e2 = 0, e3 = 0;
    FILE *det = getenv("NSFILL_DETALLE") ? fopen(getenv("NSFILL_DETALLE"), "w") : NULL;
    for (uint32_t i = 0; i < nfx; i++) {
        const NsEffect *e = &fx[i];
        uint32_t id; memcpy(&id, &e->box[0], sizeof id);   /* operación de origen (ns_fill la deja en box[0]; src_key es la identidad estable) */
        id &= 0xFFFFFFu;
        uint32_t seq = e->nv == 4 ? id : id >> 1;
        const GpuOp *o = NULL;
        for (uint32_t j = 0; j < dl->list.nops; j++) if ((dl->list.ops[j].seq & 0xFFFFFFu) == seq && dl->list.ops[j].type == GPU_OP_POLY) { o = &dl->list.ops[j]; break; }
        if (!o) continue;
        double worst = 0;
        for (int k = 0; k < e->nv; k++) {
            int vi = e->nv == 4 ? k : (int)e->idx + k;
            double dv[3] = {e->p[k][0] - cam.eye[0], e->p[k][1] - cam.eye[1], e->p[k][2] - cam.eye[2]}, v[3];
            for (int r = 0; r < 3; r++) v[r] = A[3 * r] * dv[0] + A[3 * r + 1] * dv[1] + A[3 * r + 2] * dv[2];
            if (v[2] < 8) continue;
            if (!o->v[vi].vvalid || o->v[vi].vflags) continue;   /* los enteros de un vértice saturado / derivado / reconstruido no son la proyección */
            double sx = o->dox + o->proj_ox + o->proj_h * v[0] / v[2], sy = o->doy + o->proj_oy + o->proj_h * v[1] / v[2];
            double ex = sx - o->v[vi].x, ey = sy - o->v[vi].y, er = sqrt(ex * ex + ey * ey);
            if (er > worst) worst = er;
            esum += er; en++; if (er > emax) emax = er; if (er <= 1.0) e1++; if (er <= 2.0) e2++; if (er <= 3.0) e3++;
        }
        if (det) fprintf(det, "FILL op=%u nv=%d flags=%04x tpage=%04x clut=%04x peor_error=%.2fpx p0=(%.0f,%.0f,%.0f)\n", (unsigned)(o - dl->list.ops), e->nv, e->flags, e->tpage, e->clut, worst, e->p[0][0], e->p[0][1], e->p[0][2]);
    }
    if (det) fclose(det);
    printf("[posicion relleno] vertices=%ld error_medio=%.3fpx max=%.2fpx dentro_de_1px=%.1f%% 2px=%.1f%% 3px=%.1f%%\n", en, en ? esum / en : 0, emax, en ? 100.0 * e1 / en : 100.0, en ? 100.0 * e2 / en : 100.0, en ? 100.0 * e3 / en : 100.0);

    /* (1) cobertura independiente: ns_cover_core sobre el nativo y sobre nativo + relleno (lista sin las operaciones de efectos) */
    if (!getenv("NSFILL_NOCOVER")) {
        uint32_t nops = dl->list.nops, nrest = 0;
        GpuOp *rest = (GpuOp *)malloc(sizeof(GpuOp) * (nops ? nops : 1));
        for (uint32_t i = 0; i < nops; i++) if (!(skip && skip[i])) rest[nrest++] = dl->list.ops[i];
        NsCoverCam cc;
        memset(&cc, 0, sizeof cc);
        for (int r = 0; r < 9; r++) cc.A[r] = (float)A[r];
        for (int r = 0; r < 3; r++) cc.t[r] = (float)(-(A[3 * r] * cam.eye[0] + A[3 * r + 1] * cam.eye[1] + A[3 * r + 2] * cam.eye[2]));
        NsCoverCfg ccfg;
        ns_cover_cfg_default(&ccfg);
        NsCoverResult r0, r1;
        ns_cover_core(nat.tris, nat.src, nat.n, &cc, rest, nrest, &ccfg, &r0);
        cover_report("solo nativo", &r0);
        size_t cap = nat.n + (size_t)nfx * 2 + 1;
        NsRTri *T = (NsRTri *)malloc(cap * sizeof *T); NsAdaptSrc *S = (NsAdaptSrc *)malloc(cap * sizeof *S);
        memcpy(T, nat.tris, nat.n * sizeof *T); memcpy(S, nat.src, nat.n * sizeof *S);
        int nn = (int)nat.n;
        for (uint32_t i = 0; i < nfx; i++) to_rtri(&fx[i], T, S, i, &nn);
        ns_cover_core(T, S, (size_t)nn, &cc, rest, nrest, &ccfg, &r1);
        cover_report("nativo+relleno", &r1);
        free(T); free(S); free(rest);
    }

    /* (3) coste: repeticiones del fotograma completo (la persistencia hace que sea el caso estable) */
    {
        double sum_t = 0, mn = 1e30, mx = 0, sum_i = 0, sum_m = 0, sum_p = 0, sum_e = 0, sum_a = 0, sum_f = 0;
        ro.keep_native = 0;
        for (int i = 0; i < reps; i++) {
            NsFillStats s2;
            ns_fill_session_run(s, d->ram, d->scratch, dl->list.ops, dl->list.nops, &ro, &cfg, frame + 1 + (uint32_t)i, &s2, NULL);
            sum_t += s2.ms_total; sum_i += s2.ms_index; sum_m += s2.ms_match; sum_p += s2.ms_persist; sum_e += s2.ms_extract; sum_a += s2.ms_adapt; sum_f += s2.ms_fx;
            if (s2.ms_total < mn) mn = s2.ms_total;
            if (s2.ms_total > mx) mx = s2.ms_total;
        }
        printf("[coste] %d fotogramas: total medio=%.2f ms (min %.2f, max %.2f) | de ello relleno puro: indice=%.2f casar=%.2f persistencia=%.2f | extraer=%.2f adaptar=%.2f efectos=%.2f\n",
               reps, sum_t / reps, mn, mx, sum_i / reps, sum_m / reps, sum_p / reps, sum_e / reps, sum_a / reps, sum_f / reps);
    }
    ns_fill_session_destroy(s);
    return 0;
}
