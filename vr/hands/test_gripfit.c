/* test_gripfit: comprueba con el modelo real (STAGE.DIR; sin datos se omite) las poses de agarre por arma de vr_grip/vr_hand_pose (gid 0 SOCOM, 1 FAMAS, 9 PSG1, 4 Stinger, 3 Nikita) con la ruta real:
 * vrgrip_find -> zócalos + vrhandpose_blend (agarre 1, gatillo 0). Mide (avisos, ver ORDEN DEL USUARIO más abajo): ninguna parte de la mano penetra > 2 mm en la malla del arma (ni la mano derecha en la izquierda), la palma y cada dedo que agarra
 * (la mano que empuña: corazón, anular, meñique; la de apoyo: los cuatro) tienen al menos una falange a <= 3 mm de la malla del arma o de la otra mano, el pulgar también, y la yema del índice con el
 * gatillo suelto cae a <= max(12, max(d, dx) + 6, h + 8) mm del volcado del gatillo, con d = distancia del gatillo a la superficie del arma (gatillo dentro de una pieza maciza), dx = la misma
 * distancia hacia +X (el índice derecho llega por la cara derecha: en el Nikita la cara más cercana, a 28 mm, es la izquierda, y la derecha está a 31) y h = semiancho en X de la pieza
 * gatillo: la del FAMAS (cola pintada en las dos caras del bloque de empuñadura, wcut_tables.c) cruza el bloque de lado a lado (h = 19 mm) y el índice la toca en su cara lateral.
 * ORDEN DEL USUARIO (2026-11-12): las poses siguen sus dibujos aunque atraviesen el modelo; penetración, contactos y yema son AVISOS impresos, no fallos. Solo falla lo estructural: que
 * cada arma y granada tenga entrada en vr_grip con pose propia y que la pose dé 25 ángulos finitos (y que los .inc compilen).
 * Casos conocidos: FAMAS (el hueco del guardamanos solo está pintado, x -19..19, y -30..32, z -162..-72 mm, y ahí no cuenta la penetración; la mano va centrada en el mango macizo y lo atraviesa
 * ~17 mm), Stinger (gatillo del modelo en z = -250 mm, lejos del agarre que pidió el usuario: yema a ~105 mm).
 * Granadas (gid 2 frag, 7 aturdidora, 8 señuelo; grenade.c: grenade 0x3b88 y can_gren 0x7a64): el modelo del juego va en la mano con la relación de Snake: marco HELD
 * girado 180 grados sobre Z y mano derecha con Hm y dR de Snake, sin zócalo propio. El anfitrión (ns_wphide_carry_off) la aplica con vrgrip_object_offset (vr_grip_obj.inc), y se comprueba que da lo mismo; avisa si penetra > 2 mm o si la palma, un dedo o el pulgar quedan a > 3 mm del objeto.
 * Uso: test_gripfit [STAGE.DIR]. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wload.h"
#include "wcut.h"
#include "wcore.h"
#include "vr_hands.h"
#include "vr_grip.h"
#include "gripfit.h"
#include "w_throw.h"
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FALLO: " __VA_ARGS__); printf("\n"); } } while (0)
static int warns;
#define WARN(c, ...) do { if (!(c)) { warns++; printf("  AVISO: " __VA_ARGS__); printf("\n"); } } while (0)   /* orden del usuario: no es fallo */
static void check_angles(const char *nm, int pose) {   /* estructural: la pose da 25 ángulos finitos (vrhs_pose_ang los acota a sus límites anatómicos) */
    VrHandPoseIn in = {pose, 1.0f, 0.0f, 0.0f, 0.0f}; float a[VRHS_NANG]; vrhandpose_angles(&in, a);
    int ok = VRHS_NANG == 25 && pose >= 0 && pose < HP_COUNT; for (int i = 0; i < 25 && ok; i++) ok = isfinite(a[i]) && fabsf(a[i]) <= 180.0f;
    CHECK(ok, "%s: la pose %d no da 25 angulos validos", nm, pose);
}
static uint8_t *slurp(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long l=ftell(f);fseek(f,0,SEEK_SET);uint8_t*d=malloc(l);if(fread(d,1,l,f)!=(size_t)l){fclose(f);free(d);return NULL;}fclose(f);*n=l;return d;}
typedef struct{const char*name;uint16_t model;int gid;float Lp[3],Lq[4],dL[3],dR[3];}WI_t;
static const WI_t WI[]={
 {"SOCOM",0x36f6,0,{-0.05880f,-0.02242f,-0.04254f},{-0.18441f,0.65841f,0.72515f,0.08153f},{44.2f,66.5f,13.0f},{-47.7f,54.8f,15.4f}},
 {"FAMAS",0x4725,1,{-0.10622f,0.01185f,-0.23049f},{0.78183f,-0.61331f,-0.09187f,0.06449f},{46.8f,65.7f,13.6f},{-47.5f,55.1f,15.5f}},
 {"PSG1",0x2d3b,9,{-0.06649f,0.01792f,-0.11433f},{0.73084f,-0.57621f,-0.15323f,0.33223f},{46.2f,66.4f,12.5f},{-47.4f,60.7f,15.4f}},
 {"STINGER",0x57f8,4,{-0.16218f,0.01141f,-0.29264f},{-0.35963f,0.27085f,0.85976f,0.24106f},{29.4f,58.6f,14.6f},{-47.7f,54.5f,15.6f}},
 {"NIKITA",0x15a9,3,{-0.16218f,0.01141f,-0.29264f},{-0.35963f,0.27085f,0.85976f,0.24106f},{29.4f,58.6f,14.6f},{-47.7f,54.5f,15.6f}}};
static const float FAMAS_HOLLOW[6] = {-0.019f, 0.019f, -0.030f, 0.032f, -0.162f, -0.072f};   /* hueco pintado del guardamanos del FAMAS (m, marco del arma) */
static const struct { const char *name; uint16_t model; int gid; } GREN[3] = {{"FRAG", 0x3b88, 2}, {"STUN", 0x7a64, 7}, {"CHAFF", 0x7a64, 8}};
/* ---- arma ---- */
static float TSURF, THX, DRIGHT; static WLoaded M_; static VrHandsFrame FR; static GfTris WT; static WPose GR,SP; static float TRIG[3]; static int HASTRIG;
static float (*TWp)[3]; static int NT;
static int load_weapon(const uint8_t*arch,size_t alen,int wi){
  WLoadOpts o;wload_default_opts(&o);o.resident="init";o.upscale=2;o.frame=W_FRAME_HELD;o.scale=0.001f;
  if(wload_model(arch,alen,"init",WI[wi].model,&o,&M_)){fprintf(stderr,"load\n");return -1;}
  WCutSet cs;memset(&cs,0,sizeof cs);const WCutModel*cm=wcut_find(WI[wi].model);if(!cm||wcut_run(&M_,cm,&cs)){fprintf(stderr,"wcut\n");return -1;}
  const WeaponDef*base=NULL;for(int i=0;i<w_registry_count;i++)if(w_registry[i]&&!strcmp(w_registry[i]->id,cm->weapon))base=w_registry[i];
  WeaponDef*def=malloc(sizeof *def);WeaponState*st=calloc(1,base->state_size);*def=*base;st->def=def;def->init(st);st->def=def;weapon_bind(st,cs.meshes,cs.nmeshes,cs.parts,cs.nparts);
  WPose g0;int dg=vrhands_derive_grip(st,&g0);if(dg==0)def->points[WP_GRIP]=g0;if(def->make_ready)def->make_ready(st,def->mag_capacity);
  VrHandsConfig hc;vrhands_default_config(&hc);hc.grip_mode=VRH_GRIP_SOCKET;VrHandsRig*rig=vrhands_create(&hc);vrhands_bind(rig,st,cs.atlas,cs.atlas_w,cs.atlas_h);
  GR=vrhands_grip(rig,st);SP=def->points[WP_SUPPORT];vrhands_set_socket(rig,wpose_inv(GR));
  VrHandsPose hp;memset(&hp,0,sizeof hp);hp.valid=1;hp.rot[3]=1;if(vrhands_build(rig,st,&hp,&FR)<=0){fprintf(stderr,"build\n");return -1;}
  NT=0;for(int m=0;m<FR.nmeshes;m++)NT+=FR.meshes[m].nidx/3;TWp=malloc(sizeof(float)*9*NT);int k=0;
  for(int m=0;m<FR.nmeshes;m++){const VrHandsMesh*Mm=&FR.meshes[m];for(uint32_t t=0;t+2<Mm->nidx;t+=3)for(int c3=0;c3<3;c3++){const float*q=Mm->verts[Mm->idx[t+c3]].pos,*a=Mm->model;
    TWp[k][0]=a[0]*q[0]+a[4]*q[1]+a[8]*q[2]+a[12];TWp[k][1]=a[1]*q[0]+a[5]*q[1]+a[9]*q[2]+a[13];TWp[k][2]=a[2]*q[0]+a[6]*q[1]+a[10]*q[2]+a[14];k++;}}
  gf_build(&WT,TWp,NT);gf_orient_outward(&WT);fprintf(stderr,"  flip %.0f\n",WT.flip);
  HASTRIG=0;for(int i=0;i<cs.nparts;i++)if(cs.parts[i].channel==WCH_TRIGGER&&cs.parts[i].mesh>=0){const WMesh*tm=&cs.meshes[cs.parts[i].mesh];double s[3]={0,0,0};
    for(uint32_t v=0;v<tm->nv;v++){for(int a=0;a<3;a++)s[a]+=tm->v[v].pos[a];}for(int a=0;a<3;a++)TRIG[a]=s[a]/tm->nv;TRIG[0]+=cs.parts[i].rest.p.x;TRIG[1]+=cs.parts[i].rest.p.y;TRIG[2]+=cs.parts[i].rest.p.z;HASTRIG=1;break;}
  /* el gatillo puede quedar dentro de una pieza maciza (guardamanos del FAMAS, caja del Stinger/Nikita): TSURF = distancia (mm) del gatillo a la superficie del arma sin contar la propia pieza gatillo (triángulos dentro de su caja) */
  TSURF=0;if(HASTRIG){float bl[3]={1e9f,1e9f,1e9f},bh[3]={-1e9f,-1e9f,-1e9f};for(int i=0;i<cs.nparts;i++)if(cs.parts[i].channel==WCH_TRIGGER&&cs.parts[i].mesh>=0){const WMesh*tm=&cs.meshes[cs.parts[i].mesh];const float o3[3]={cs.parts[i].rest.p.x,cs.parts[i].rest.p.y,cs.parts[i].rest.p.z};
      for(uint32_t v=0;v<tm->nv;v++){for(int a=0;a<3;a++){float q=tm->v[v].pos[a]+o3[a];if(q<bl[a])bl[a]=q;if(q>bh[a])bh[a]=q;}}break;}
    THX=0.5f*(bh[0]-bl[0])*1000.f;
    /* lado por el que llega el índice derecho: distancia del gatillo a la superficie del arma hacia +X (paso de 0,5 mm; se sale de la pieza gatillo antes de buscar) */
    DRIGHT=0;{float p[3]={TRIG[0],TRIG[1],TRIG[2]};for(int k=0;k<400;k++){p[0]=TRIG[0]+k*0.0005f;if(p[0]<=bh[0])continue;if(gf_sd(&WT,p,0.05f,1)>0){DRIGHT=(p[0]-TRIG[0])*1000.f;break;}}}
    float best=1e9f;for(int i=0;i<WT.n;i++){int in=1;for(int k=0;k<3&&in;k++)for(int a=0;a<3;a++)if(WT.t[i][k][a]<bl[a]-0.0005f||WT.t[i][k][a]>bh[a]+0.0005f){in=0;break;}if(in)continue;
      float q[3],d[3];gf_closest(TRIG,WT.t[i][0],WT.t[i][1],WT.t[i][2],q);gf_sub(TRIG,q,d);float dd=sqrtf(gf_dot(d,d));if(dd<best)best=dd;}TSURF=best*1000.f;}
  fprintf(stderr,"%s: grip(%.3f %.3f %.3f) supp(%.3f %.3f %.3f) tris %d trig %d (%.3f %.3f %.3f)\n",WI[wi].name,GR.p.x,GR.p.y,GR.p.z,SP.p.x,SP.p.y,SP.p.z,NT,HASTRIG,TRIG[0],TRIG[1],TRIG[2]);
  return 0;}

static float Hw[2][VRHA_NV][3];
static int PART[VRHA_NV];
static void hand_pts(const VrGripEntry *e, int side, int pose, WPose handle, WPose sock, float pad[3]) {
    VrHandPoseIn in = {pose, 1.0f, 0.0f, 0.0f, 0.0f};
    float xf[VRHS_J][12]; static float pos[VRHA_NV][3];
    vrhandpose_blend(side, &in, xf); vrhs_skin(xf, side, pos, NULL);
    WPose P = vrgrip_weapon_from_hand(handle, sock, side);
    for (int i = 0; i < VRHA_NV; i++) { WVec3 v = {pos[i][0] * VRHS_UNIT_M, pos[i][1] * VRHS_UNIT_M, pos[i][2] * VRHS_UNIT_M}; WVec3 w = wpose_apply(P, v); Hw[side > 0][i][0] = w.x; Hw[side > 0][i][1] = w.y; Hw[side > 0][i][2] = w.z; }
    if (pad) { float pp[3]; vrhandpose_index_tip((const float (*)[12])xf, pp); WVec3 v = {pp[0] * VRHS_UNIT_M, pp[1] * VRHS_UNIT_M, pp[2] * VRHS_UNIT_M}; WVec3 w = wpose_apply(P, v); pad[0] = w.x; pad[1] = w.y; pad[2] = w.z; }
    (void)e;
}
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : NULL;
    size_t alen = 0; uint8_t *arch = path ? slurp(path, &alen) : NULL;
    if (!arch) { printf("test_gripfit: sin datos del juego (%s): se omite\n", path ? path : "sin ruta"); return 0; }
    gf_parts(PART);
    static float tri[3 * VRHA_NT][3];
    for (int wi = 0; wi < 5; wi++) {
        if (load_weapon(arch, alen, wi)) { printf("  (%s: no se carga el modelo, se omite)\n", WI[wi].name); continue; }
        const VrGripEntry *e = vrgrip_find(WI[wi].gid);
        CHECK(e != NULL, "%s: sin entrada en vr_grip", WI[wi].name);
        if (!e) continue;
        check_angles(WI[wi].name, e->pose); if (e->support_pose >= 0) check_angles(WI[wi].name, e->support_pose);
        float pad[3];
        hand_pts(e, 1, e->pose, GR, e->grip, pad);
        hand_pts(e, -1, e->support_pose, SP, e->support, NULL);
        GfTris H[2];
        for (int s = 0; s < 2; s++) { int n = gf_hand_tris(Hw[s], s ? 1 : -1, tri); gf_build(&H[s], tri, n); }
        printf("%s (gid %d)\n", WI[wi].name, WI[wi].gid);
        for (int s = 1; s >= 0; s--) {
            float mins[GF_NPART], worst = 0; for (int i = 0; i < GF_NPART; i++) mins[i] = 1e9f;
            float mo[GF_NPART]; for (int i = 0; i < GF_NPART; i++) mo[i] = 1e9f;
            for (int v = 0; v < VRHA_NV; v++) {
                float sd = gf_sd(&WT, Hw[s][v], 0.05f, 1) * 1000.f, so = gf_sd(&H[1 - s], Hw[s][v], 0.05f, 1) * 1000.f;
                if (wi == 1) { const float *q = Hw[s][v], *b = FAMAS_HOLLOW; if (q[0] > b[0] && q[0] < b[1] && q[1] > b[2] && q[1] < b[3] && q[2] > b[4] && q[2] < b[5] && sd < 0) sd = 0; }
                float sa = sd < so ? sd : so;
                if (sd < mins[PART[v]]) mins[PART[v]] = sd;
                if (sa < mo[PART[v]]) mo[PART[v]] = sa;
                if (-sa > worst) worst = -sa;
            }
            float g[7]; g[0] = mo[0]; for (int f = 0; f < 4; f++) g[1 + f] = fminf(mo[1 + 3 * f], fminf(mo[2 + 3 * f], mo[3 + 3 * f])); g[5] = fminf(mo[13], mo[14]); g[6] = mo[3];
            printf("  %s: penetracion max %.2f mm; contacto (mm) palma %.1f indice %.1f corazon %.1f anular %.1f menique %.1f pulgar %.1f idx.distal %.1f\n", s ? "derecha" : "izquierda", worst, g[0], g[1], g[2], g[3], g[4], g[5], g[6]);
            WARN(worst <= 2.0f, "%s %s: penetracion %.2f mm > 2", WI[wi].name, s ? "derecha" : "izquierda", worst);
            WARN(g[0] <= 3.0f, "%s %s: palma a %.1f mm (> 3)", WI[wi].name, s ? "derecha" : "izquierda", g[0]);
            for (int f = s ? 1 : 0; f < 4; f++) WARN(g[1 + f] <= 3.0f, "%s %s: dedo %d a %.1f mm (> 3)", WI[wi].name, s ? "derecha" : "izquierda", f, g[1 + f]);
            WARN(g[5] <= 3.0f, "%s %s: pulgar a %.1f mm (> 3)", WI[wi].name, s ? "derecha" : "izquierda", g[5]);
            if (s) { float d[3] = {pad[0] - TRIG[0], pad[1] - TRIG[1], pad[2] - TRIG[2]}; float dm = sqrtf(gf_dot(d, d)) * 1000.f, lim = fmaxf(fmaxf(12.0f, fmaxf(TSURF, DRIGHT) + 6.0f), THX + 8.0f); printf("  yema del indice a %.1f mm del gatillo (limite %.1f)\n", dm, lim); WARN(!HASTRIG || dm <= lim, "%s: yema a %.1f mm del gatillo (> %.1f)", WI[wi].name, dm, lim); }
        }
        for (int s = 0; s < 2; s++) gf_free(&H[s]);
        wload_free(&M_); gf_free(&WT); free(TWp);
    }
    /* granadas: objeto del juego en la mano derecha con la relación de Snake (ver cabecera) */
    static const float Hm[9] = {-1, 0, 0, 0, 0, 1, 0, 1, 0}, dR[3] = {-47.7f, 54.8f, 15.4f};
    for (int gi = 0; gi < 3; gi++) {
        WLoadOpts o; wload_default_opts(&o); o.resident = "init"; o.upscale = 2; o.frame = W_FRAME_HELD; o.scale = 0.001f;
        if (wload_model(arch, alen, "init", GREN[gi].model, &o, &M_)) { printf("  (%s: no se carga el modelo, se omite)\n", GREN[gi].name); continue; }
        NT = M_.ni / 3; TWp = malloc(sizeof(float) * 9 * NT);
        for (int i = 0; i < (int)M_.ni; i++) { const WVertex *v = &M_.v[M_.idx[i]]; TWp[i][0] = -v->pos[0]; TWp[i][1] = -v->pos[1]; TWp[i][2] = v->pos[2]; }
        gf_build(&WT, TWp, NT); gf_orient_outward(&WT);
        const VrGripEntry *e = vrgrip_find(GREN[gi].gid);
        CHECK(e && e->pose >= HP_W_FIRST && !vrhandpose_has_trigger(e->pose), "%s: sin pose propia en vr_grip", GREN[gi].name);
        if (e) {
            check_angles(GREN[gi].name, e->pose);
            VrHandPoseIn in = {e->pose, 1.0f, 0.0f, 0.0f, 0.0f}; float xf[VRHS_J][12]; static float pos[VRHA_NV][3];
            vrhandpose_blend(1, &in, xf); vrhs_skin(xf, 1, pos, NULL);
            float mins[GF_NPART], worst = 0; for (int i = 0; i < GF_NPART; i++) mins[i] = 1e9f;
            for (int v = 0; v < VRHA_NV; v++) {
                float p[3] = {pos[v][0] + dR[0], pos[v][1] + dR[1], pos[v][2] + dR[2]}, q[3];
                for (int a = 0; a < 3; a++) q[a] = (Hm[3 * a] * p[0] + Hm[3 * a + 1] * p[1] + Hm[3 * a + 2] * p[2]) * 0.001f;
                float sd = gf_sd(&WT, q, 0.05f, 1) * 1000.f; if (sd < mins[PART[v]]) mins[PART[v]] = sd; if (-sd > worst) worst = -sd;
            }
            float g[6]; g[0] = mins[0]; for (int f = 0; f < 4; f++) g[1 + f] = fminf(mins[1 + 3 * f], fminf(mins[2 + 3 * f], mins[3 + 3 * f])); g[5] = fminf(mins[13], mins[14]);
            printf("%s (gid %d): penetracion max %.2f mm; contacto (mm) palma %.1f indice %.1f corazon %.1f anular %.1f menique %.1f pulgar %.1f\n", GREN[gi].name, GREN[gi].gid, worst, g[0], g[1], g[2], g[3], g[4], g[5]);
            WARN(worst <= 2.0f, "%s: penetracion %.2f mm > 2", GREN[gi].name, worst);
            for (int f = 0; f < 6; f++) WARN(g[f] <= 3.0f, "%s: parte %d a %.1f mm (> 3)", GREN[gi].name, f, g[f]);
            /* la colocación que usa el anfitrión (vrgrip_object_offset -> ns_wphide_carry_off) es esta misma relación: p_mano = R * e + t, e = unidades del motor del modelo; marco de la malla de arriba: (-x, -y, z) de HELD = (-e0, e2, e1) */
            float R[9], t[3];
            CHECK(vrgrip_object_offset(GREN[gi].gid, R, t) == 1, "%s: sin desplazamiento de objeto en vr_grip", GREN[gi].name);
            if (vrgrip_object_offset(GREN[gi].gid, R, t)) {
                float Ri[9]; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Ri[3 * r + c] = R[3 * c + r];   /* R es un giro: inversa = traspuesta */
                #define TO_MESH(pm, qm) do { float d_[3] = {(pm)[0] - t[0], (pm)[1] - t[1], (pm)[2] - t[2]}, e_[3]; for (int r_ = 0; r_ < 3; r_++) e_[r_] = Ri[3 * r_] * d_[0] + Ri[3 * r_ + 1] * d_[1] + Ri[3 * r_ + 2] * d_[2]; (qm)[0] = -e_[0] * 0.001f; (qm)[1] = e_[2] * 0.001f; (qm)[2] = e_[1] * 0.001f; } while (0)
                float mins2[GF_NPART], worst2 = 0; for (int i = 0; i < GF_NPART; i++) mins2[i] = 1e9f;
                for (int v = 0; v < VRHA_NV; v++) { float q2[3]; TO_MESH(pos[v], q2); float sd = gf_sd(&WT, q2, 0.05f, 1) * 1000.f; if (sd < mins2[PART[v]]) mins2[PART[v]] = sd; if (-sd > worst2) worst2 = -sd; }
                float dmax = fabsf(worst2 - worst); for (int i = 0; i < GF_NPART; i++) if (mins[i] < 1e8f && mins2[i] < 1e8f) dmax = fmaxf(dmax, fabsf(mins2[i] - mins[i]));
                CHECK(dmax < 0.05f, "%s: con vrgrip_object_offset la mano toca el objeto igual que en el ajuste (diferencia %.3f mm)", GREN[gi].name, dmax);
                /* el centro de agarre de la pose queda pegado al objeto, no delante ni a un lado de él: a menos de 15 mm de su superficie (con la relación del ajuste sale a 10,1 mm en la frag y 7,0 en el bote: la palma
                 * apoya en el borde del lado de la muñeca del objeto, no en su centro). El centro GEOMÉTRICO de la caja del modelo (104 x 80 x 250 mm el bote) queda a 61-71 mm del centro de agarre con la relación del
                 * ajuste: un criterio de < 10 mm contra él no lo cumple la pose diseñada, así que solo se informa. */
                float c[3], q3[3]; vrhs_grip_center(1, c); TO_MESH(c, q3);
                float sg = gf_sd(&WT, q3, 0.2f, 1) * 1000.f, lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                for (int i = 0; i < NT * 3; i++) for (int a = 0; a < 3; a++) { if (TWp[i][a] < lo[a]) lo[a] = TWp[i][a]; if (TWp[i][a] > hi[a]) hi[a] = TWp[i][a]; }
                const float cen[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
                const float dc = sqrtf(gf_dot((float[3]){q3[0] - cen[0], q3[1] - cen[1], q3[2] - cen[2]}, (float[3]){q3[0] - cen[0], q3[1] - cen[1], q3[2] - cen[2]})) * 1000.f;
                printf("%s: centro de agarre a %.1f mm de la superficie del objeto (dentro < 0) y a %.1f mm del centro de su caja\n", GREN[gi].name, sg, dc);
                CHECK(sg < 15.0f, "%s: el centro de agarre queda a %.1f mm de la superficie del objeto (>= 15)", GREN[gi].name, sg);
                #undef TO_MESH
            }
        }
        wload_free(&M_); gf_free(&WT); free(TWp);
    }
    /* claymore (id 5, placed_b, modelo 0x17a1 con las patas desplegadas): la mano derecha la coge por el medio (pose propia sin gatillo, zócalo FIT_CLAYMORE_GRIP respecto a WP_GRIP); avisa si penetra > 2 mm o si la
     * palma, un dedo o el pulgar quedan a > 3 mm. Estructural: con el giro de mano (turn) la mina queda girada R_y(+90) respecto al marco del arma del zócalo MAIN de antes (la cara picuda +X del modelo
     * hacia el -Z de entonces, el delante del mando), que es lo que compensa w_placed.c (MINE_FACE_Q) al colocar. */
    {
        WLoadOpts o; wload_default_opts(&o); o.resident = "init"; o.upscale = 2; o.frame = W_FRAME_HELD; o.scale = 0.001f;
        WCutSet cs; memset(&cs, 0, sizeof cs); const WCutModel *cm = wcut_find(0x17a1);
        if (wload_model(arch, alen, "init", 0x17a1, &o, &M_) || !cm || wcut_run(&M_, cm, &cs)) printf("  (CLAYMORE: no se carga el modelo, se omite)\n");
        else {
            const WeaponDef *base = NULL; for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, cm->weapon)) base = w_registry[i];
            WeaponDef *def = malloc(sizeof *def); WeaponState *st = calloc(1, base->state_size); *def = *base; st->def = def; def->init(st); st->def = def; weapon_bind(st, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
            if (def->make_ready) def->make_ready(st, def->mag_capacity);
            st->phase = WPL_PH_HELD; w5_hide(st, 0); st->channel[WCH_CUSTOM0] = 1.0f;
            VrHandsConfig hc; vrhands_default_config(&hc); hc.grip_mode = VRH_GRIP_SOCKET; VrHandsRig *rig = vrhands_create(&hc); vrhands_bind(rig, st, cs.atlas, cs.atlas_w, cs.atlas_h);
            GR = vrhands_grip(rig, st); vrhands_set_socket(rig, wpose_inv(GR));
            VrHandsPose hp; memset(&hp, 0, sizeof hp); hp.valid = 1; hp.rot[3] = 1;
            if (vrhands_build(rig, st, &hp, &FR) > 0) {
                NT = 0; for (int m = 0; m < FR.nmeshes; m++) NT += FR.meshes[m].nidx / 3; TWp = malloc(sizeof(float) * 9 * NT); int k = 0;
                for (int m = 0; m < FR.nmeshes; m++) { const VrHandsMesh *Mm = &FR.meshes[m]; for (uint32_t t = 0; t + 2 < Mm->nidx; t += 3) for (int c3 = 0; c3 < 3; c3++) { const float *q = Mm->verts[Mm->idx[t + c3]].pos, *a = Mm->model;
                    TWp[k][0] = a[0] * q[0] + a[4] * q[1] + a[8] * q[2] + a[12]; TWp[k][1] = a[1] * q[0] + a[5] * q[1] + a[9] * q[2] + a[13]; TWp[k][2] = a[2] * q[0] + a[6] * q[1] + a[10] * q[2] + a[14]; k++; } }
                gf_build(&WT, TWp, NT); gf_orient_outward(&WT);
                const VrGripEntry *e = vrgrip_find(5);
                CHECK(e && e->pose >= HP_W_FIRST && !vrhandpose_has_trigger(e->pose) && e->support_pose == -1, "CLAYMORE: sin pose propia en vr_grip");
                if (e) {
                    check_angles("CLAYMORE", e->pose);
                    hand_pts(e, 1, e->pose, GR, e->grip, NULL);
                    float mo[GF_NPART], worst = 0; for (int i = 0; i < GF_NPART; i++) mo[i] = 1e9f;
                    for (int v = 0; v < VRHA_NV; v++) { float sd = gf_sd(&WT, Hw[1][v], 0.05f, 1) * 1000.f; if (sd < mo[PART[v]]) mo[PART[v]] = sd; if (-sd > worst) worst = -sd; }
                    float g[6]; g[0] = mo[0]; for (int f = 0; f < 4; f++) g[1 + f] = fminf(mo[1 + 3 * f], fminf(mo[2 + 3 * f], mo[3 + 3 * f])); g[5] = fminf(mo[13], mo[14]);
                    printf("CLAYMORE (gid 5): penetracion max %.2f mm; contacto (mm) palma %.1f indice %.1f corazon %.1f anular %.1f menique %.1f pulgar %.1f\n", worst, g[0], g[1], g[2], g[3], g[4], g[5]);
                    WARN(worst <= 2.0f, "CLAYMORE: penetracion %.2f mm > 2", worst);
                    for (int f = 0; f < 6; f++) WARN(g[f] <= 3.0f, "CLAYMORE: parte %d a %.1f mm (> 3)", f, g[f]);
                    /* arma respecto al mando: turn * (giro de la mano en el modelo)^-1 = MAIN^-1 * R_y(+90) */
                    WPose hm = wpose_mul(GR, e->grip); WQuat hq = wquat_normalize(hm.q), hi = {-hq.x, -hq.y, -hq.z, hq.w};
                    WQuat wq = wquat_mul(e->turn, hi), mq = {0.0f, -0.5f, -0.86603f, 0.0f}, want = wquat_mul(mq, wquat_axis_angle(wv(0, 1, 0), 1.5707963f));
                    float d = fabsf(wq.x * want.x + wq.y * want.y + wq.z * want.z + wq.w * want.w);
                    CHECK(d > 0.9995f, "CLAYMORE: la mina no queda girada R_y(+90) respecto al mando (|dot| %.5f)", d);
                }
                gf_free(&WT); free(TWp);
            }
            wload_free(&M_);
        }
    }
    printf("%s test_gripfit: %d comprobaciones, %d fallos, %d avisos (penetracion/contacto/yema: orden del usuario)\n", fails ? "FALLA" : "OK", checks, fails, warns);
    return fails ? 1 : 0;
}
