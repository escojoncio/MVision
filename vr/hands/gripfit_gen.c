/* Generador offline de poses de agarre por arma (NO se compila en los tests; ver gripfit.h). Necesita STAGE.DIR en /tmp/gamedata y rast.h (rasterizador por software, /tmp/w59; no va al repo).
 * MÉTODO: la FORMA de cada mano (25 ángulos de articulación) se diseña a mano por arma y mano (fichero de especificación, abajo la tabla DESIGN que generó vr_hand_grips.inc); la optimización solo
 * coloca y gira la palma (6 GDL: varios miles de arranques al azar + búsqueda por patrón, giro acotado a 20 grados de la pose medida de Snake) y pule unos grados por articulación (+-pj, índice y pulgar
 * +-1,5 pj) para quitar penetraciones (> 0,6 mm) y lograr contacto (palma, dedos que agarran y pulgar a <= 1 mm), con la yema del índice atraída al gatillo (límite: max(5, distancia del gatillo a la
 * superficie + 4) mm) y, si se da, el centro de agarre atraído a un punto propio del arma (mango de Stinger/Nikita, apoyo bajo el guardamanos). La mano izquierda se ajusta con la derecha ya posada como obstáculo.
 * Cada iteración se renderiza (render: conjunto + primeros planos por mano, malla del arma en alambre) y se revisa a ojo.
 * Uso: gripfit_gen fit ARMA|all SPEC | eval ARMA|all SPEC | spec SPEC | render DIR [ARMA] | emit vr_hand_grips.inc vr_grip_fit.inc | wview DIR | look ... | section WI v eje | shape OUT lado a0..a24 | axes
 * Entorno: NSTART (arranques al azar, 1500), TRANGE (mm, 40), RLIM (grados, 20), WCON (peso del contacto, 3), WTRIG (peso del gatillo, 1). Resultados en RES_DIR (res_<arma>.txt).
 * Línea de especificación: ARMA lado(1 derecha, -1 izquierda) tx ty tz rx ry rz (mm y grados, marco de la pose de Snake) + 25 ángulos (por dedo: metacarpo MCP PIP DIP abducción; pulgar: flexión
 * oposición separación proximal distal) + pj [x y z (m, marco del arma) peso].
 * DESIGN (especificación final; las formas de partida, antes del pulido, fueron: derecha de pistola/fusil con dedos "sujetos con los dedos" MCP 5-20 PIP 95 DIP 55-60 e índice 0/5-35/45-78/10-30;
 * apoyo de fusil con la palma bajo el guardamanos MCP 10 PIP 80 DIP 30 y el pulgar por el otro lado; izquierda de SOCOM en copa MCP 55 PIP 60 DIP 35 con el pulgar pegado al índice; Stinger/Nikita
 * palma contra el lateral de la caja con los dedos poco cerrados MCP 20 PIP 40 DIP 20):
 *   SOCOM 1   -6.4 -63.2 -30.5 -9.3 -8.1 -2.7 0.0 24.0 75.0 2.0 14.0 5.0 44.0 72.0 40.0 -2.0 5.0 50.0 73.0 40.0 1.0 8.0 51.0 72.0 40.0 -8.0 0.0 19.0 13.0 23.0 20.0   20  0.0000 -0.0050 -0.0880 2.00
 *   SOCOM -1   -10.4 -53.5 -36.5 26.5 48.4 58.8 3.0 48.5 84.0 39.0 0.0 3.0 43.5 85.0 40.0 0.0 3.0 37.5 86.0 40.0 0.0 3.0 36.5 84.0 40.0 0.0 29.0 37.0 9.0 8.0 10.0   15  -0.0080 -0.0520 -0.0850 1.00
 *   FAMAS 1   -7.3 -17.1 11.8 4.3 29.7 1.5 0.0 0.0 0.0 0.0 5.0 5.0 45.0 92.0 41.0 0.0 5.0 45.0 78.0 40.0 2.0 8.0 43.0 61.0 28.0 10.0 45.0 45.0 -5.0 60.0 54.0   20  0.0260 0.0150 -0.0320 1.00
 *   FAMAS -1   6.7 -122.4 -14.8 -9.0 25.8 28.7 3.0 69.0 40.0 20.0 0.0 3.0 73.0 40.0 20.0 0.0 3.0 70.0 40.0 20.0 0.0 3.0 68.0 39.0 20.0 0.0 8.0 20.0 40.0 10.0 5.0   20  0.0000 0.0620 -0.2400 0.50
 *   PSG1 1   -37.9 -12.7 -1.8 16.0 18.5 2.3 0.0 23.0 46.0 2.0 8.0 5.0 4.0 92.0 57.0 -2.0 5.0 22.0 92.0 54.0 -6.0 8.0 20.0 79.0 53.0 14.0 14.0 41.0 1.0 31.0 20.0   25  0.0000 -0.0220 -0.0400 0.50
 *   PSG1 -1   23.0 -66.6 6.8 11.2 4.4 9.1 3.0 8.0 80.0 30.0 -3.0 3.0 17.0 80.0 30.0 0.0 3.0 12.0 80.0 30.0 3.0 3.0 6.0 80.0 30.0 6.0 21.0 37.0 0.0 20.0 20.0   12  -0.0300 0.0260 -0.1800 0.50
 *   STINGER 1   -57.1 -106.7 -42.1 0.2 22.0 -9.0 0.0 77.0 11.0 0.0 -15.0 5.0 72.0 70.0 28.0 6.0 5.0 63.0 70.0 30.0 4.0 8.0 71.0 70.0 30.0 6.0 32.0 22.0 20.0 2.0 10.0   12  0.0200 -0.0300 -0.2150 0.50
 *   STINGER -1   -91.9 -49.1 -165.4 71.1 100.2 32.5 3.0 65.0 50.0 30.0 0.0 3.0 66.0 51.0 30.0 0.0 3.0 62.0 52.0 30.0 0.0 3.0 57.0 50.0 30.0 0.0 18.0 14.0 30.0 10.0 10.0   25  0.0000 -0.0450 -0.3150 0.50
 *   NIKITA 1   -32.1 25.8 -13.8 15.8 24.8 -11.3 0.0 22.0 18.0 8.0 -3.0 5.0 63.0 63.0 40.0 -15.0 5.0 56.0 79.0 40.0 0.0 8.0 31.0 60.0 41.0 6.0 34.0 26.0 3.0 18.0 14.0   12  0.0130 -0.0350 -0.0250 0.30
 *   NIKITA -1   -80.6 49.8 21.8 -28.1 -9.9 68.1 3.0 49.0 41.0 18.0 -4.0 3.0 41.0 51.0 30.0 3.0 3.0 31.0 56.0 30.0 4.0 3.0 26.0 46.0 30.0 3.0 20.0 24.0 0.0 10.0 10.0   12  -0.1000 -0.0200 -0.1900 0.50
 *   FRAG 1   0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 5.0 70.0 15.0 3.0 0.0 15.0 80.0 -7.5 5.0 5.0 0.0 80.0 -15.0 8.0 15.0 15.0 70.0 -15.0 0.0 0.0 -15.0 10.0 7.0   40
 *   STUN 1   0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 5.0 75.0 -7.5 3.0 0.0 10.0 80.0 -7.5 5.0 0.0 5.0 25.0 7.5 0.0 -5.0 10.0 50.0 15.0 15.0 30.0 15.0 45.0 31.5   40
 *   CHAFF 1   0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.0 5.0 75.0 -7.5 3.0 0.0 10.0 80.0 -7.5 5.0 0.0 5.0 25.0 7.5 0.0 -5.0 10.0 50.0 15.0 15.0 30.0 15.0 45.0 31.5   40
 * Ajuste 2026-11-12 contra los dibujos del usuario (manda su dibujo; la penetración no limita): SOCOM (índice estirado al gatillo, naranja centrada en el eje del mango, azul subida),
 * FAMAS (puño 40 mm hacia -X, índice recto), Stinger (naranja atrás y abajo del bloque), Nikita (rodea la esquina trasera del bloque), granadas FRAG/STUN/CHAFF (solo ángulos; la palma
 * queda donde la pone ns_wphide_carry_off con vr_grip_obj.inc). Herramientas nuevas: SIDE=0|1 (retoca una mano), NOPLACE + POLJ=a,b (solo pule esas articulaciones), FPTS/WFP (yemas a puntos 3D), HOLLOW (caja hueca),
 * modo sil con SILOCC (el arma tapa la mano), SILPTS/SILPW (puntos de color por dedo), SILJ (también ángulos), SILWR (muñeca al blanco). Ojo: el modo eval guarda RES redondeado a 1 decimal.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wload.h"
#include "wcut.h"
#include "wcore.h"
#include "vr_hands.h"
#include "vr_hand_skin.h"
#include "vr_hand_asset.h"
#include "gripfit.h"
#include "rast.h"
#ifndef RES_DIR
#define RES_DIR "/tmp/w63"
#endif
static uint8_t *slurp(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long l=ftell(f);fseek(f,0,SEEK_SET);uint8_t*d=malloc(l);if(fread(d,1,l,f)!=(size_t)l){fclose(f);free(d);return NULL;}fclose(f);*n=l;return d;}
#define DEG 0.017453292519943295f
typedef struct{const char*name;uint16_t model;int gid;float Lp[3],Lq[4],dL[3],dR[3];}WI_t;
/* pose de Snake (tabla_held de /tmp/w59): mano izquierda (posición y cuaternión en el marco del arma, desplazamiento de la muñeca) y desplazamiento de la derecha (orientación de apretón de manos Hm) */
static const WI_t WI[]={
 {"SOCOM",0x36f6,0,{-0.05880f,-0.02242f,-0.04254f},{-0.18441f,0.65841f,0.72515f,0.08153f},{44.2f,66.5f,13.0f},{-47.7f,54.8f,15.4f}},
 {"FAMAS",0x4725,1,{-0.10622f,0.01185f,-0.23049f},{0.78183f,-0.61331f,-0.09187f,0.06449f},{46.8f,65.7f,13.6f},{-47.5f,55.1f,15.5f}},
 {"PSG1",0x2d3b,9,{-0.06649f,0.01792f,-0.11433f},{0.73084f,-0.57621f,-0.15323f,0.33223f},{46.2f,66.4f,12.5f},{-47.4f,60.7f,15.4f}},
 {"STINGER",0x57f8,4,{-0.16218f,0.01141f,-0.29264f},{-0.35963f,0.27085f,0.85976f,0.24106f},{29.4f,58.6f,14.6f},{-47.7f,54.5f,15.6f}},
 {"NIKITA",0x15a9,3,{-0.16218f,0.01141f,-0.29264f},{-0.35963f,0.27085f,0.85976f,0.24106f},{29.4f,58.6f,14.6f},{-47.7f,54.5f,15.6f}},
 /* granadas (solo mano derecha, sin gatillo): el modelo del juego va en la mano por ns_wphide_carry_off (vr_grip_obj.inc) con la relación de Snake (marco HELD girado 180 grados sobre Z, mano con Hm y dR de Snake);
  * grenade.c: GRD_GRENADE -> grenade (0x3b88), GRD_STUN y GRD_CHAFF -> can_gren (0x7a64). La palma no se mueve (t = r = 0): solo se diseñan los ángulos. */
 {"FRAG",0x3b88,2,{0,0,0},{0,0,0,1},{0,0,0},{-47.7f,54.8f,15.4f}},
 {"STUN",0x7a64,7,{0,0,0},{0,0,0,1},{0,0,0},{-47.7f,54.8f,15.4f}},
 {"CHAFF",0x7a64,8,{0,0,0},{0,0,0,1},{0,0,0},{-47.7f,54.8f,15.4f}}};
#define NW 5      /* armas (con malla de wcut y dos manos) */
#define NWI 8     /* + granadas */
static const float Hm[9]={-1,0,0, 0,0,1, 0,1,0};
static const float ANG_LO[25]={0,-15,0,0,-15, 0,-15,0,0,-15, 0,-15,0,0,-15, 0,-15,0,0,-15, 0,0,-25,0,0};
static const float ANG_HI[25]={10,90,110,80,15, 10,90,110,80,15, 10,90,110,80,15, 10,90,110,80,15, 45,45,25,60,70};
static void mv(const float*R,const float*v,float*o){for(int i=0;i<3;i++)o[i]=R[3*i]*v[0]+R[3*i+1]*v[1]+R[3*i+2]*v[2];}
static void mm3(const float*A,const float*B,float*C){float T[9];for(int i=0;i<3;i++)for(int j=0;j<3;j++)T[3*i+j]=A[3*i]*B[j]+A[3*i+1]*B[3+j]+A[3*i+2]*B[6+j];memcpy(C,T,36);}
static void q2m(const float*q,float*m){float x=q[0],y=q[1],z=q[2],w=q[3];float n=sqrtf(x*x+y*y+z*z+w*w);x/=n;y/=n;z/=n;w/=n;
 m[0]=1-2*(y*y+z*z);m[1]=2*(x*y-w*z);m[2]=2*(x*z+w*y);m[3]=2*(x*y+w*z);m[4]=1-2*(x*x+z*z);m[5]=2*(y*z-w*x);m[6]=2*(x*z-w*y);m[7]=2*(y*z+w*x);m[8]=1-2*(x*x+y*y);}
static void m2q(const float*m,float*q){float t=m[0]+m[4]+m[8];if(t>0){float s=sqrtf(t+1)*2;q[3]=s/4;q[0]=(m[7]-m[5])/s;q[1]=(m[2]-m[6])/s;q[2]=(m[3]-m[1])/s;}
 else if(m[0]>m[4]&&m[0]>m[8]){float s=sqrtf(1+m[0]-m[4]-m[8])*2;q[3]=(m[7]-m[5])/s;q[0]=s/4;q[1]=(m[1]+m[3])/s;q[2]=(m[2]+m[6])/s;}
 else if(m[4]>m[8]){float s=sqrtf(1+m[4]-m[0]-m[8])*2;q[3]=(m[2]-m[6])/s;q[0]=(m[1]+m[3])/s;q[1]=s/4;q[2]=(m[5]+m[7])/s;}
 else{float s=sqrtf(1+m[8]-m[0]-m[4])*2;q[3]=(m[3]-m[1])/s;q[0]=(m[2]+m[6])/s;q[1]=(m[5]+m[7])/s;q[2]=s/4;}}
static void rotvec(const float*r,float*R){float a=sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]);if(a<1e-9f){R[0]=R[4]=R[8]=1;R[1]=R[2]=R[3]=R[5]=R[6]=R[7]=0;return;}float x=r[0]/a,y=r[1]/a,z=r[2]/a,c=cosf(a),s=sinf(a),t=1-c;
 R[0]=t*x*x+c;R[1]=t*x*y-s*z;R[2]=t*x*z+s*y;R[3]=t*x*y+s*z;R[4]=t*y*y+c;R[5]=t*y*z-s*x;R[6]=t*x*z-s*y;R[7]=t*y*z+s*x;R[8]=t*z*z+c;}
/* ---- arma ---- */
static float TSURF;static GfTris TBASE,TFRAME;static WLoaded M_; static VrHandsFrame FR; static GfTris WT; static WPose GR,SP; static float TRIG[3]; static int HASTRIG;
static float (*TWp)[3]; static int NT;
static int OBJ=0;
static int load_object(const uint8_t*arch,size_t alen,int wi){   /* granada: malla del juego en el marco HELD girado 180 grados sobre Z (como wcut FIX_UPRIGHT) */
  WLoadOpts o;wload_default_opts(&o);o.resident="init";o.upscale=2;o.frame=W_FRAME_HELD;o.scale=0.001f;if(wload_model(arch,alen,"init",WI[wi].model,&o,&M_)){fprintf(stderr,"load\n");return -1;}
  NT=M_.ni/3;TWp=malloc(sizeof(float)*9*NT);for(int i=0;i<(int)M_.ni;i++){const WVertex*v=&M_.v[M_.idx[i]];TWp[i][0]=-v->pos[0];TWp[i][1]=-v->pos[1];TWp[i][2]=v->pos[2];}
  gf_build(&WT,TWp,NT);gf_orient_outward(&WT);HASTRIG=0;TSURF=0;memset(&TBASE,0,sizeof TBASE);memset(&TFRAME,0,sizeof TFRAME);memset(&GR,0,sizeof GR);GR.q.w=1;OBJ=1;return 0;}
static int load_weapon(const uint8_t*arch,size_t alen,int wi){
  if(wi>=NW)return load_object(arch,alen,wi);OBJ=0;
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
  gf_build(&WT,TWp,NT);gf_orient_outward(&WT);
  HASTRIG=0;for(int i=0;i<cs.nparts;i++)if(cs.parts[i].channel==WCH_TRIGGER&&cs.parts[i].mesh>=0){const WMesh*tm=&cs.meshes[cs.parts[i].mesh];double s[3]={0,0,0};
    for(uint32_t v=0;v<tm->nv;v++)for(int a=0;a<3;a++)s[a]+=tm->v[v].pos[a];for(int a=0;a<3;a++)TRIG[a]=s[a]/tm->nv;TRIG[0]+=cs.parts[i].rest.p.x;TRIG[1]+=cs.parts[i].rest.p.y;TRIG[2]+=cs.parts[i].rest.p.z;HASTRIG=1;break;}
  memset(&TBASE,0,sizeof TBASE);memset(&TFRAME,0,sizeof TFRAME);
  if(getenv("BASEY")||getenv("FRAMEY")){float yb=getenv("BASEY")?atof(getenv("BASEY"))*0.001f:-1e9f,yf=getenv("FRAMEY")?atof(getenv("FRAMEY"))*0.001f:1e9f;float (*tb)[3]=malloc(sizeof(float)*9*NT),(*tf)[3]=malloc(sizeof(float)*9*NT);int nb=0,nf=0;
    for(int t=0;t<NT;t++){int lo=1,hi=1;for(int k=0;k<3;k++){if(TWp[3*t+k][1]>yb)lo=0;if(TWp[3*t+k][1]<yf)hi=0;}if(lo){for(int k=0;k<3;k++)memcpy(tb[3*nb+k],TWp[3*t+k],12);nb++;}if(hi){for(int k=0;k<3;k++)memcpy(tf[3*nf+k],TWp[3*t+k],12);nf++;}}
    if(nb)gf_build(&TBASE,tb,nb);if(nf)gf_build(&TFRAME,tf,nf);free(tb);free(tf);}
  TSURF=HASTRIG?fabsf(gf_sd(&WT,TRIG,0.2f,1))*1000.f:0;fprintf(stderr,"%s: gatillo a %.1f mm de la superficie\n",WI[wi].name,TSURF);
  return 0;}
/* ---- mano ---- */
typedef struct{int side;float R[9],o[3],d[3],c[3];}Base;
static void make_base(int wi,int side,Base*B){memset(B,0,sizeof *B);B->side=side;vrhs_grip_center(side,B->c);
  if(side>0){memcpy(B->R,Hm,36);memcpy(B->d,WI[wi].dR,12);}
  else{q2m(WI[wi].Lq,B->R);memcpy(B->o,WI[wi].Lp,12);memcpy(B->d,WI[wi].dL,12);}}
/* x: t[3] mm (marco de la base), r[3] grados (giro alrededor del centro de agarre), ang[25] -> vértices en el marco del arma (m), pose (Rt, ot) y yema del índice */
static void hand_world(const Base*B,const float*x,float (*w)[3],float*Rt,float*ot,float*pad){
  float rv[3]={x[3]*DEG,x[4]*DEG,x[5]*DEG},Rr[9],Rtot[9];rotvec(rv,Rr);mm3(B->R,Rr,Rtot);
  float cc[3];mv(Rr,B->c,cc);float tl[3];for(int a=0;a<3;a++)tl[a]=B->c[a]-cc[a]+B->d[a]+x[a];float tw[3];mv(B->R,tl,tw);
  float xf[VRHS_J][12];vrhs_pose_ang(x+6,B->side,xf);static float pos[VRHA_NV][3];vrhs_skin(xf,B->side,pos,NULL);
  if(w)for(int i=0;i<VRHA_NV;i++){float v[3]={pos[i][0]*0.001f,pos[i][1]*0.001f,pos[i][2]*0.001f};float u[3];mv(Rr,v,u);float z[3];mv(B->R,u,z);for(int a=0;a<3;a++)w[i][a]=z[a]+tw[a]*0.001f+B->o[a];}
  if(Rt){memcpy(Rt,Rtot,36);for(int a=0;a<3;a++)ot[a]=tw[a]*0.001f+B->o[a];}
  if(pad){float pp[3];vrhs_index_pad((const float(*)[12])xf,pp);float u[3],z[3];for(int a=0;a<3;a++)u[a]=pp[a]*0.001f;mv(Rr,u,z);mv(B->R,z,u);for(int a=0;a<3;a++)pad[a]=u[a]+tw[a]*0.001f+B->o[a];}}
static void hand_point(const Base*B,const float*x,const float*hp_mm,float*out){ /* punto del marco de la mano (mm) -> arma (m) */
  float rv[3]={x[3]*DEG,x[4]*DEG,x[5]*DEG},Rr[9];rotvec(rv,Rr);float cc[3];mv(Rr,B->c,cc);float tl[3];for(int a=0;a<3;a++)tl[a]=B->c[a]-cc[a]+B->d[a]+x[a];float tw[3];mv(B->R,tl,tw);
  float u[3],z[3];mv(Rr,hp_mm,u);mv(B->R,u,z);for(int a=0;a<3;a++)out[a]=(z[a]+tw[a])*0.001f+B->o[a];}
/* ---- especificación (forma diseñada) ---- */
typedef struct{float x[31];float tgt[3],tw;float pj;int has;}Spec;   /* x: t, r, 25 ángulos; tgt: punto (m, marco del arma) al que se atrae el centro de agarre con peso tw; pj: pulido máximo por articulación (grados) */
static Spec SPEC[NWI][2];
static int read_spec(const char*path){FILE*f=fopen(path,"r");if(!f)return -1;char line[2048];
  while(fgets(line,sizeof line,f)){if(line[0]=='#'||line[0]=='\n')continue;char nm[32];int side;int off;if(sscanf(line,"%31s %d%n",nm,&side,&off)<2)continue;
    int wi=-1;for(int i=0;i<NWI;i++)if(!strcmp(nm,WI[i].name))wi=i;if(wi<0)continue;Spec*S=&SPEC[wi][side>0];memset(S,0,sizeof *S);S->pj=8;
    char*p=line+off;for(int i=0;i<31;i++){S->x[i]=strtof(p,&p);}
    char*q;float v=strtof(p,&q);if(q!=p){S->pj=v;p=q;float t[4];int ok=1;for(int i=0;i<4;i++){t[i]=strtof(p,&q);if(q==p){ok=0;break;}p=q;}if(ok){memcpy(S->tgt,t,12);S->tw=t[3];}}
    S->has=1;}
  fclose(f);return 0;}
/* ---- coste ---- */
static int PART[VRHA_NV];static int NSTART=0;static float TRANGE=40;static float RLIM=20;static float RLIM2=25;static float TOFF[3];static float WBASE=0,WFRAME=0;static float PENW=40;static float WEB[3],WEBW;static float WCON=3;static float WTRIG=1;static int POLJ0=0,POLJ1=24;static float HOL[6];static int HASHOL=0;   /* HOLLOW x0,x1,y0,y1,z0,z1 (mm): caja hueca del arma (hueco del guardamanos pintado): dentro no cuenta la penetración */
static int NFP=0,FPF[8];static float FPP[8][3],WFP=1;   /* FPTS: dedo (1..4, pulgar 5) x y z (mm, marco del arma): la falange distal de la mano derecha se atrae a ese punto (peso WFP por (d/10 mm)^2) */
typedef struct{const GfTris*W,*H2;uint32_t need;int trig;const Spec*S;float r0[3],rlim;}Ctx;
typedef struct{float pen,g[7],trig,cost;}Rep;
static float rotnorm(const float*x){return sqrtf(x[3]*x[3]+x[4]*x[4]+x[5]*x[5]);}
static double eval(const Base*B,const Ctx*C,const float*x,Rep*rp){
  static float w[VRHA_NV][3];float pad[3];hand_world(B,x,w,NULL,NULL,pad);
  float mn[GF_NPART];for(int i=0;i<GF_NPART;i++)mn[i]=1e9f;double pen=0;float worst=0;
  for(int v=0;v<VRHA_NV;v++){float sd=gf_sd(C->W,w[v],0.03f,1)*1000.f;if(HASHOL&&w[v][0]*1000>HOL[0]&&w[v][0]*1000<HOL[1]&&w[v][1]*1000>HOL[2]&&w[v][1]*1000<HOL[3]&&w[v][2]*1000>HOL[4]&&w[v][2]*1000<HOL[5])sd=fmaxf(sd,0);if(C->H2){float s2=gf_sd(C->H2,w[v],0.03f,1)*1000.f;if(s2<sd)sd=s2;}
    if(sd<-0.6f)pen+=(-sd-0.6f)*(-sd-0.6f);if(-sd>worst)worst=-sd;if(sd<mn[PART[v]])mn[PART[v]]=sd;}
  float g[7];g[0]=mn[0];for(int f=0;f<4;f++)g[1+f]=fminf(mn[1+3*f],fminf(mn[2+3*f],mn[3+3*f]));g[5]=fminf(mn[13],mn[14]);g[6]=mn[3];
  double con=0;for(int i=0;i<7;i++)if(C->need>>i&1){float m=g[i]>25?25:g[i];if(m>1.0f)con+=(m-1.0f)*(m-1.0f);}
  double tr=0;float dm=0;if(C->trig){float d[3]={pad[0]-TRIG[0]-TOFF[0],pad[1]-TRIG[1]-TOFF[1],pad[2]-TRIG[2]-TOFF[2]};dm=sqrtf(gf_dot(d,d))*1000.f;{float lim=TOFF[0]||TOFF[1]||TOFF[2]?3.0f:fmaxf(5.0f,TSURF+4.0f);if(dm>lim)tr=WTRIG*(dm-lim)*(dm-lim);}}
  double pr=0;float dr[6]={0,0,0,x[3]-C->r0[0],x[4]-C->r0[1],x[5]-C->r0[2]};float rn=rotnorm(dr);if(rn>C->rlim)pr+=1000*(rn-C->rlim)*(rn-C->rlim);
  for(int i=6;i<31;i++){float d=x[i]-C->S->x[i];pr+=0.02*d*d;}
  if(C->S->tw>0){float cw[3];hand_point(B,x,B->c,cw);float d[3]={cw[0]-C->S->tgt[0],cw[1]-C->S->tgt[1],cw[2]-C->S->tgt[2]};pr+=C->S->tw*gf_dot(d,d)*1e6;}
  if(WEBW>0&&C->trig){float xf[VRHS_J][12];vrhs_pose_ang(x+6,B->side,xf);float hp[3];for(int a=0;a<3;a++)hp[a]=0.5f*(xf[2][4*a+3]+xf[6][4*a+3]);float wp[3];hand_point(B,x,hp,wp);
    float d[3]={wp[0]*1000-WEB[0],wp[1]*1000-WEB[1],wp[2]*1000-WEB[2]};pr+=WEBW*gf_dot(d,d);}
  if(B->side<0&&TBASE.n&&WBASE>0){float d=1e9f;for(int v=0;v<VRHA_NV;v++)if(PART[v]==0){float q=gf_sd(&TBASE,w[v],0.05f,0)*1000.f;if(q<d)d=q;}if(d>1)pr+=WBASE*(d-1)*(d-1);}
  if(B->side>0&&TFRAME.n&&WFRAME>0){float d=1e9f;for(int v=0;v<VRHA_NV;v++){float q=gf_sd(&TFRAME,w[v],0.05f,0)*1000.f;if(q<d)d=q;}if(d>1)pr+=WFRAME*(d-1)*(d-1);}
  if(B->side>0)for(int i=0;i<NFP;i++){int pt=FPF[i]>=5?14:3*FPF[i];double a[3]={0,0,0};int n=0;for(int v=0;v<VRHA_NV;v++)if(PART[v]==pt){for(int k=0;k<3;k++)a[k]+=w[v][k];n++;}
    double dd=0;for(int k=0;k<3;k++){double q=a[k]/n*1000-FPP[i][k];dd+=q*q;}pr+=WFP*dd/100;}
  double c=PENW*pen+WCON*con+tr+pr;
  if(rp){rp->pen=worst;memcpy(rp->g,g,sizeof g);rp->trig=dm;rp->cost=(float)c;}
  return c;}
static uint32_t rnd=12345;static float frand(void){rnd=rnd*1664525u+1013904223u;return (rnd>>8)/16777216.0f;}
static float gauss(void){float s=0;for(int i=0;i<4;i++)s+=frand();return (s-2)*1.7f;}
static double pattern(const Base*B,const Ctx*C,float*x){ /* búsqueda por patrón en los 6 GDL: paso 6 -> 0,25 */
  double cur=eval(B,C,x,NULL);for(float st=6;st>=0.25f;st*=0.5f){for(int rep=0;rep<60;rep++){double bo=cur;int bd=-1;float bs=0;
    for(int d=0;d<6;d++)for(int sg=-1;sg<=1;sg+=2){float y[31];memcpy(y,x,124);y[d]+=sg*st;double c=eval(B,C,y,NULL);if(c<bo){bo=c;bd=d;bs=sg*st;}}
    if(bd<0)break;x[bd]+=bs;cur=bo;}}
  return cur;}
static void place(const Base*B,const Ctx*C,float*x,int iters){
  double cur=eval(B,C,x,NULL);
  if(NSTART){float x0[31];memcpy(x0,x,124);float top[8][31];double tc[8];for(int i=0;i<8;i++)tc[i]=1e30;
    for(int k=0;k<NSTART;k++){float y[31];memcpy(y,x0,124);for(int d=0;d<3;d++)y[d]+=(frand()*2-1)*TRANGE;float dr[6]={0};for(int d=3;d<6;d++){dr[d]=(frand()*2-1)*C->rlim;y[d]=C->r0[d-3]+dr[d];}if(rotnorm(dr)>C->rlim)continue;
      double c=eval(B,C,y,NULL);int w=0;for(int i=1;i<8;i++)if(tc[i]>tc[w])w=i;if(c<tc[w]){tc[w]=c;memcpy(top[w],y,124);}}
    double best=pattern(B,C,x);for(int i=0;i<8;i++){if(tc[i]>1e29)continue;float y[31];memcpy(y,top[i],124);double cy=pattern(B,C,y);
      if(cy<best){best=cy;memcpy(x,y,124);}}
    cur=best;}
  cur=pattern(B,C,x);(void)iters;return;float st[6]={5,5,5,5,5,5};
  for(int s=0;s<3;s++){float k=s==0?1:(s==1?0.4f:0.15f);for(int it=0;it<iters;it++){float y[31];memcpy(y,x,124);int n=1+(int)(frand()*2);
    for(int j=0;j<n;j++){int d=(int)(frand()*6);if(d>5)d=5;y[d]+=gauss()*st[d]*k;}
    double c=eval(B,C,y,NULL);if(c<cur){cur=c;memcpy(x,y,124);}}}}
static void polish(const Base*B,const Ctx*C,float*x){
  double cur=eval(B,C,x,NULL);const float*d0=C->S->x+6;float pj=C->S->pj;
  for(int it=0;it<300;it++){double bo=cur;int bj=-1;float bd=0;
    for(int j=0;j<25;j++){if(j%5==0&&j<20)continue;if(j<POLJ0||j>POLJ1)continue;for(int dr=-1;dr<=1;dr+=2){float y[31];memcpy(y,x,124);y[6+j]+=dr;float lim=j>=20?pj*1.5f:(j<5&&C->trig?pj*1.5f:pj);
      if(y[6+j]<ANG_LO[j]||y[6+j]>ANG_HI[j]||fabsf(y[6+j]-d0[j])>lim)continue;double o=eval(B,C,y,NULL);if(o<bo-1e-4){bo=o;bj=j;bd=dr;}}}
    if(bj<0)break;x[6+bj]+=bd;cur=bo;}}
static uint32_t need_mask(int side){return side>0?(1u|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)):(1u|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5));}
static float RES[NWI][2][31];
static void save_res(int wi){char p[128];snprintf(p,sizeof p,RES_DIR "/res_%d.txt",wi);FILE*f=fopen(p,"w");for(int s=0;s<2;s++){for(int i=0;i<31;i++)fprintf(f,"%.4f ",RES[wi][s][i]);fprintf(f,"\n");}fclose(f);}
static int load_res(int wi){char p[128];snprintf(p,sizeof p,RES_DIR "/res_%d.txt",wi);FILE*f=fopen(p,"r");if(!f)return 0;for(int s=0;s<2;s++)for(int i=0;i<31;i++)if(fscanf(f,"%f",&RES[wi][s][i])!=1){fclose(f);return 0;}fclose(f);return 1;}
static void hand_tris(const Base*B,const float*x,GfTris*T){static float w[VRHA_NV][3];static float tri[3*VRHA_NT][3];hand_world(B,x,w,NULL,NULL,NULL);int n=gf_hand_tris(w,B->side,tri);gf_build(T,tri,n);}
static void pr_rep(const char*nm,const Rep*r,int trig){printf("  %s pen %.2f | palma %.1f ind %.1f cor %.1f ana %.1f men %.1f pul %.1f idxD %.1f%s",nm,r->pen,r->g[0],r->g[1],r->g[2],r->g[3],r->g[4],r->g[5],r->g[6],trig?"":"\n");if(trig)printf(" | yema %.1f\n",r->trig);}
static void fit(int wi,int mode){ /* mode 0: colocar + pulir; 1: solo evaluar la especificación */
  Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);
  for(int s=1;s>=0;s--){if(wi>=NW&&!s)continue;const Spec*S=&SPEC[wi][s];Ctx C={&WT,NULL,need_mask(s&&wi<NW?1:-1),s&&HASTRIG,S,{0,0,0},RLIM};if(!s||wi>=3){memcpy(C.r0,S->x+3,12);C.rlim=RLIM2;}GfTris HR;
    if(!s){hand_tris(&B[1],RES[wi][1],&HR);C.H2=&HR;}
    float x[31];memcpy(x,S->x,124);
    if(getenv("SIDE")&&atoi(getenv("SIDE"))!=s){if(!s)gf_free(&HR);continue;}   /* SIDE=0|1: retoque de una sola mano; la otra se queda como está en RES */
    if(getenv("SIDE"))memcpy(x,RES[wi][s],124);
    if(mode==0&&getenv("NOPLACE"))polish(&B[s],&C,x);   /* NOPLACE: solo pulido de articulaciones (POLJ), la palma no se mueve */
    else if(mode==0){NSTART=getenv("NSTART")?atoi(getenv("NSTART")):1500;place(&B[s],&C,x,250);NSTART=0;polish(&B[s],&C,x);place(&B[s],&C,x,120);polish(&B[s],&C,x);}
    memcpy(RES[wi][s],x,124);Rep r;eval(&B[s],&C,x,&r);
    printf("%s %s rot %.1f (%.1f %.1f %.1f) t(%.1f %.1f %.1f) cost %.1f\n",WI[wi].name,s?"R":"L",rotnorm(x),x[3],x[4],x[5],x[0],x[1],x[2],r.cost);pr_rep(s?"R":"L",&r,C.trig);
    {float o[3],p[3],z0[3]={0,0,0};hand_point(&B[s],x,z0,o);const float ax[3][3]={{s?100.f:-100.f,0,0},{0,-100,0},{0,0,100}};printf("   ejes:");for(int a=0;a<3;a++){hand_point(&B[s],x,ax[a],p);printf(" %s(%.2f %.2f %.2f)",a==0?"palma":a==1?"dedos":"pulgar",(p[0]-o[0])*10,(p[1]-o[1])*10,(p[2]-o[2])*10);}
     float cw[3];hand_point(&B[s],x,B[s].c,cw);printf(" c(%.0f %.0f %.0f)mm\n",cw[0]*1000,cw[1]*1000,cw[2]*1000);}
    printf("   ang:");for(int i=6;i<31;i++)printf(" %.0f",x[i]);printf("\n");
    if(!s)gf_free(&HR);}}
/* ---- ajuste por silueta (SIL=dir de máscaras h<v>_o/b/w.pgm 900x700; vistas como hito.sh) ---- */
static uint8_t SILT[2][4][IH][IW];static int SILN=0;static float SILC[3],SILD,SILDIR[4][3];static float SILW=1;static int SILOCC=0;
/* SILPTS: puntos de color del dibujo (vista, dedo 1..4 o pulgar 5, píxel): la falange distal de ese dedo se atrae a ese píxel (peso SILPW, escala 25 px) */
static int NPT=0,PTV[32],PTF[32];static float PTX[32],PTY[32],SILPW=1;
/* SILWR: la muñeca (origen de la mano) se atrae al centroide del blanco del dibujo (h<v>_w) en vez de sumar el blanco a la silueta naranja */
static float SILWR=0,WCX[4],WCY[4];static int WHAS[4];
static void draw_weapon(const Cam*c,const VrHandsFrame*fr);static int GRAYW;static int proj(const Cam*c,const float*p,int*x,int*y);
static int read_pgm(const char*p,uint8_t (*m)[IW]){FILE*f=fopen(p,"rb");if(!f)return -1;int w,h,mx;if(fscanf(f,"P5 %d %d %d",&w,&h,&mx)!=3||w!=IW||h!=IH){fclose(f);return -1;}fgetc(f);size_t r=fread(m,1,IW*IH,f);fclose(f);return r==(size_t)IW*IH?0:-1;}
typedef struct{double n,cx,cy,top,ang;}Mom;
static Mom moments(uint8_t (*m)[IW]){Mom r={0};double sx=0,sy=0,sxx=0,syy=0,sxy=0;int hist[IH]={0};for(int y=0;y<IH;y++)for(int x=0;x<IW;x++)if(m[y][x]){r.n++;sx+=x;sy+=y;sxx+=(double)x*x;syy+=(double)y*y;sxy+=(double)x*y;hist[y]++;}
  if(r.n<1)return r;r.cx=sx/r.n;r.cy=sy/r.n;double a=sxx/r.n-r.cx*r.cx,b=syy/r.n-r.cy*r.cy,c=sxy/r.n-r.cx*r.cy;r.ang=0.5*atan2(2*c,a-b);double acc=0;for(int y=0;y<IH;y++){acc+=hist[y];if(acc>=0.05*r.n){r.top=y;break;}}return r;}
static uint8_t HM[IH][IW];
static void hand_mask(int view,int side,const float (*w)[3]){Cam c;float eye[3],dn[3]={SILDIR[view][0],SILDIR[view][1],SILDIR[view][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=SILC[k]+dn[k]*SILD;cam_look(&c,eye,SILC,40);clear_img();if(SILOCC){GRAYW=1;draw_weapon(&c,&FR);}   /* SILOCC: el arma tapa la mano, como en el dibujo */
  static const uint8_t col[3]={255,0,1};for(int pc=0;pc<VRHA_NP;pc++){const VrHaPiece*P=&vrha_piece[pc];for(int t=0;t<P->nt;t++){const uint8_t*ix=vrha_tri[P->t0+t];float T[3][3];for(int k=0;k<3;k++)memcpy(T[k],w[P->v0+ix[k]],12);draw_tri(&c,T,NULL,col,NULL,0,0);}}
  for(int y=0;y<IH;y++)for(int x=0;x<IW;x++)HM[y][x]=img[y][x][1]<20;(void)side;}
static double sil_cost(const Base*B,const float*x,int side,int verbose){static float w[VRHA_NV][3];hand_world(B,x,w,NULL,NULL,NULL);double c=0;
  for(int v=0;v<SILN;v++){hand_mask(v,side,w);Mom a=moments(HM),b=moments(SILT[side>0?0:1][v]);if(b.n<50)continue;if(a.n<1){c+=100;continue;}
    double dc=hypot(a.cx-b.cx,a.cy-b.cy),dt=a.top-b.top,da=a.ang-b.ang;while(da>M_PI/2)da-=M_PI;while(da<-M_PI/2)da+=M_PI;
    c+=(dc/40)*(dc/40)+(dt/30)*(dt/30)+0.5*(da/0.35)*(da/0.35);
    if(verbose)printf("   vista %d %s: centroide a %.0f px, borde superior %+.0f px, eje %+.0f grados (n %.0f / %.0f px)\n",v,side>0?"naranja":"azul",dc,dt,da*57.3,a.n,b.n);}
  if(side>0&&SILWR>0)for(int v=0;v<SILN;v++)if(WHAS[v]){Cam cm;float eye[3],dn[3]={SILDIR[v][0],SILDIR[v][1],SILDIR[v][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=SILC[k]+dn[k]*SILD;cam_look(&cm,eye,SILC,40);
    float o[3],z0[3]={0,0,0};hand_point(B,x,z0,o);int px,py;if(!proj(&cm,o,&px,&py))continue;double dd=hypot(px-WCX[v],py-WCY[v]);c+=SILWR*(dd/40)*(dd/40);if(verbose)printf("   vista %d muñeca: a %.0f px del blanco\n",v,dd);}
  if(side>0)for(int i=0;i<NPT;i++){int f=PTF[i],pt=f>=5?14:3*f;double sx=0,sy=0;int n=0;Cam cm;float eye[3],dn[3]={SILDIR[PTV[i]][0],SILDIR[PTV[i]][1],SILDIR[PTV[i]][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=SILC[k]+dn[k]*SILD;cam_look(&cm,eye,SILC,40);
    for(int v=0;v<VRHA_NV;v++)if(PART[v]==pt){float d[3];sub3(w[v],cm.e,d);float z=dot3(d,cm.f);sx+=IW*0.5f+dot3(d,cm.r)/z*cm.fpx;sy+=IH*0.5f-dot3(d,cm.u)/z*cm.fpx;n++;}
    double dx=sx/n-PTX[i],dy=sy/n-PTY[i],dd=hypot(dx,dy);c+=SILPW*(dd/25)*(dd/25);if(verbose)printf("   vista %d dedo %d: a %.0f px del punto del dibujo\n",PTV[i],f,dd);}
  return c;}
/* ---- render ---- */
static int GRAYW=0;
static void draw_weapon(const Cam*c,const VrHandsFrame*fr){
  if(OBJ){static const uint8_t gray[3]={150,152,162};for(int t=0;t<NT;t++)draw_tri(c,TWp+3*t,NULL,gray,NULL,0,0);return;}
  for(int m=0;m<fr->nmeshes;m++){const VrHandsMesh*Mq=&fr->meshes[m];const uint8_t*tex=NULL;int tw=0,th=0;
    if(!GRAYW)for(int t=0;t<fr->ntex;t++)if(fr->tex[t].id==Mq->tex_id&&Mq->tex_id){tex=fr->tex[t].rgba;tw=fr->tex[t].w;th=fr->tex[t].h;}
    static const uint8_t gray[3]={120,122,130};float (*wp)[3]=malloc(sizeof(float)*3*Mq->nverts);
    for(uint32_t i=0;i<Mq->nverts;i++){const float*p=Mq->verts[i].pos;const float*a=Mq->model;
      wp[i][0]=a[0]*p[0]+a[4]*p[1]+a[8]*p[2]+a[12];wp[i][1]=a[1]*p[0]+a[5]*p[1]+a[9]*p[2]+a[13];wp[i][2]=a[2]*p[0]+a[6]*p[1]+a[10]*p[2]+a[14];}
    for(uint32_t t=0;t+2<Mq->nidx;t+=3){float T[3][3],UV[3][2];for(int k=0;k<3;k++){uint16_t ix=Mq->idx[t+k];memcpy(T[k],wp[ix],12);UV[k][0]=Mq->verts[ix].uv[0];UV[k][1]=Mq->verts[ix].uv[1];}
      draw_tri(c,T,UV,gray,tex,tw,th);}
    free(wp);}}
static float Hw[2][VRHA_NV][3];
/* colores por dedo: palma, índice, corazón, anular, meñique, pulgar (izquierda, derecha) */
static const uint8_t FC[2][6][3]={{{110,160,240},{40,90,220},{90,200,230},{150,110,240},{60,200,150},{20,40,140}},{{235,150,90},{230,40,40},{240,220,60},{200,110,40},{240,130,200},{130,60,20}}};
static void line2(int x0,int y0,int x1,int y1){int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,e=dx+dy;for(int n=0;n<4000;n++){if(x0>=0&&x0<IW&&y0>=0&&y0<IH){img[y0][x0][0]=img[y0][x0][1]=img[y0][x0][2]=20;}if(x0==x1&&y0==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;}}}
static int proj(const Cam*c,const float*p,int*x,int*y){float d[3];sub3(p,c->e,d);float z=dot3(d,c->f);if(z<0.01f)return 0;*x=(int)(IW*0.5f+dot3(d,c->r)/z*c->fpx);*y=(int)(IH*0.5f-dot3(d,c->u)/z*c->fpx);return 1;}
static void wire(const Cam*c){for(int i=0;i<WT.n;i++)for(int k=0;k<3;k++){int x0,y0,x1,y1;if(proj(c,WT.t[i][k],&x0,&y0)&&proj(c,WT.t[i][(k+1)%3],&x1,&y1))line2(x0,y0,x1,y1);}}
static void render_weapon(int wi,const char*od){
  Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);
  static const uint8_t cR[3]={235,150,90},cL[3]={110,160,240},arm[3]={90,110,95},cy[3]={240,240,0};
  float cen[2][3],org[2][3],up[2][3];
  for(int s=0;s<2;s++){float Rt[9],ot[3];hand_world(&B[s],RES[wi][s],Hw[s],Rt,ot,NULL);memcpy(org[s],ot,12);float yy[3]={0,1,0},u[3];mv(Rt,yy,u);memcpy(up[s],u,12);
    double a[3]={0,0,0};for(int v=0;v<VRHA_NV;v++)for(int k=0;k<3;k++)a[k]+=Hw[s][v][k];for(int k=0;k<3;k++)cen[s][k]=a[k]/VRHA_NV;}
  float mid[3];for(int k=0;k<3;k++)mid[k]=wi>=NW?cen[1][k]:0.5f*(cen[0][k]+cen[1][k]);
  /* 0 conjunto; 1..3 derecha arriba/lado derecho/abajo; 4..6 izquierda arriba/lado izquierdo/abajo; 7 derecha lado izquierdo; 8 izquierda lado derecho */
  static const float dirs[9][3]={{-0.8f,0.5f,0.7f},{0.15f,1,0.15f},{1,0.15f,0.1f},{0.15f,-1,0.15f},{-0.15f,1,0.15f},{-1,0.15f,0.1f},{-0.15f,-1,0.15f},{-1,0.15f,0.1f},{1,0.15f,0.1f}};
  for(int view=0;view<9;view++){Cam c;float tgt[3],eye[3];float dist=0.95f;GRAYW=view>0;
    if(view==0)memcpy(tgt,mid,12);else{int hs=(view>=4&&view<=6)||view==8?0:1;memcpy(tgt,cen[hs],12);dist=0.30f;}
    float dn[3]={dirs[view][0],dirs[view][1],dirs[view][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=tgt[k]+dn[k]*dist;
    cam_look(&c,eye,tgt,40);clear_img();draw_weapon(&c,&FR);if(HASTRIG&&view>0)draw_sph(&c,TRIG,0.003f,cy);
    for(int s=0;s<2;s++){if(wi>=NW&&!s)continue;float e[3];for(int k=0;k<3;k++)e[k]=org[s][k]+up[s][k]*0.28f;draw_cyl(&c,org[s],e,0.034f,0.042f,arm);
      for(int pc=0;pc<VRHA_NP;pc++){const VrHaPiece*P=&vrha_piece[pc];for(int t=0;t<P->nt;t++){const uint8_t*ix=vrha_tri[P->t0+t];float T[3][3];for(int k=0;k<3;k++)memcpy(T[k],Hw[s][P->v0+ix[k]],12);
        int pt=PART[P->v0+ix[0]];int f=pt==0?0:(pt>=13?5:1+(pt-1)/3);const uint8_t*col=view?FC[s][f]:(s?cR:cL);draw_tri(&c,T,NULL,col,NULL,0,0);}}}
    if(view)wire(&c);
    char p[256];snprintf(p,sizeof p,"%s/g_%s_%d.ppm",od,WI[wi].name,view);save_ppm(p);}}
static void wview(int wi,const char*od){
  static const uint8_t cr[3]={255,0,0},cg[3]={0,200,0},cb[3]={0,0,255},cy[3]={230,230,0};
  float lo[3]={1e9,1e9,1e9},hi[3]={-1e9,-1e9,-1e9};for(int v=0;v<NT*3;v++)for(int k=0;k<3;k++){if(TWp[v][k]<lo[k])lo[k]=TWp[v][k];if(TWp[v][k]>hi[k])hi[k]=TWp[v][k];}
  printf("  bbox x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f mm\n",lo[0]*1000,hi[0]*1000,lo[1]*1000,hi[1]*1000,lo[2]*1000,hi[2]*1000);
  float c0[3];for(int k=0;k<3;k++)c0[k]=0.5f*(lo[k]+hi[k]);float ext=fmaxf(hi[2]-lo[2],hi[1]-lo[1]);
  static const float dirs[4][3]={{1,0.05f,0.0f},{-1,0.05f,0.0f},{0.3f,-1,0.05f},{0.3f,1,0.05f}};
  for(int view=0;view<4;view++){Cam c;float eye[3];float dn[3]={dirs[view][0],dirs[view][1],dirs[view][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=c0[k]+dn[k]*ext*1.6f;GRAYW=1;
    cam_look(&c,eye,c0,40);clear_img();draw_weapon(&c,&FR);float g[3]={GR.p.x,GR.p.y,GR.p.z},sp[3]={SP.p.x,SP.p.y,SP.p.z},o[3]={0,0,0};
    draw_sph(&c,o,0.004f,cr);draw_sph(&c,g,0.004f,cg);draw_sph(&c,sp,0.004f,cb);if(HASTRIG)draw_sph(&c,TRIG,0.004f,cy);
    for(int s=0;s<2;s++)if(SPEC[wi][s].tw>0)draw_sph(&c,SPEC[wi][s].tgt,0.004f,s?cr:cb);
    char p[256];snprintf(p,sizeof p,"%s/w_%s_%d.ppm",od,WI[wi].name,view);save_ppm(p);}
  static const float d2[3][3]={{1,0.02f,0.0f},{0.02f,-1,0.3f},{0.02f,0.3f,-1}};
  for(int view=0;view<3;view++){Cam c;float eye[3],c1[3]={TRIG[0],TRIG[1]-0.02f,TRIG[2]+0.03f};float dn[3]={d2[view][0],d2[view][1],d2[view][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=c1[k]+dn[k]*0.22f;
    cam_look(&c,eye,c1,40);clear_img();draw_weapon(&c,&FR);wire(&c);if(HASTRIG)draw_sph(&c,TRIG,0.003f,cy);float g[3]={GR.p.x,GR.p.y,GR.p.z};draw_sph(&c,g,0.003f,cg);
    for(int s=0;s<2;s++)if(SPEC[wi][s].tw>0)draw_sph(&c,SPEC[wi][s].tgt,0.003f,s?cr:cb);
    char p[256];snprintf(p,sizeof p,"%s/w_%s_%d.ppm",od,WI[wi].name,4+view);save_ppm(p);}}
/* corte de la malla del arma por el plano y = Y (marco del arma): segmentos en x/z, como texto */
static void section(float Y,int ax){int u=(ax+1)%3,v=(ax+2)%3;const char*nm="xyz";for(int i=0;i<WT.n;i++){float (*t)[3]=WT.t[i];float pts[2][3];int n=0;for(int k=0;k<3;k++){const float*a=t[k],*b=t[(k+1)%3];if((a[ax]-Y)*(b[ax]-Y)<0){float w=(Y-a[ax])/(b[ax]-a[ax]);if(n<2){for(int q=0;q<3;q++)pts[n][q]=a[q]+w*(b[q]-a[q]);n++;}}}
  if(n==2)printf("%c %.0f..%.0f %c %.0f..%.0f | ",nm[u],pts[0][u]*1000,pts[1][u]*1000,nm[v],pts[0][v]*1000,pts[1][v]*1000);}printf("\n");}
/* ---- emitir ---- */
static WPose mirror_p(WPose p){p.p.x=-p.p.x;p.q.y=-p.q.y;p.q.z=-p.q.z;return p;}
static WPose pose_of(const float*R,const float*o){float q[4];m2q(R,q);WPose p;p.p.x=o[0];p.p.y=o[1];p.p.z=o[2];p.q.x=q[0];p.q.y=q[1];p.q.z=q[2];p.q.w=q[3];p.q=wquat_normalize(p.q);return p;}
static void pr_pose(FILE*f,WPose p){fprintf(f,"{{%.5ff, %.5ff, %.5ff}, {%.5ff, %.5ff, %.5ff, %.5ff}}",p.p.x,p.p.y,p.p.z,p.q.x,p.q.y,p.q.z,p.q.w);}
static FILE *FG,*FS;
static void emit_weapon(int wi){
  static const char*up[NWI]={"SOCOM","FAMAS","PSG1","STINGER","NIKITA","FRAG","STUN","CHAFF"};
  if(wi>=NW){const float*x=RES[wi][1];fprintf(FS,"#define FIT_%s_POSE HP_W_%s_R\n",up[wi],up[wi]);fprintf(FG,"    [HP_W_%s_R - HP_W_FIRST] = {{",up[wi]);   /* granada: sin gatillo ni zócalo propio */
    for(int i=0;i<25;i++)fprintf(FG,"%.1ff%s",x[6+i],i<24?", ":"");fprintf(FG,"}, 0, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},\n");return;}
  Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);float Rt[9],ot[3],pad[3];
  hand_world(&B[1],RES[wi][1],NULL,Rt,ot,pad);WPose grip=wpose_mul(wpose_inv(GR),pose_of(Rt,ot));
  WPose pp;pp.p.x=pad[0];pp.p.y=pad[1];pp.p.z=pad[2];pp.q.x=pp.q.y=pp.q.z=0;pp.q.w=1;WPose tg=wpose_mul(wpose_inv(GR),pp);
  hand_world(&B[0],RES[wi][0],NULL,Rt,ot,NULL);WPose sup=mirror_p(wpose_mul(wpose_inv(SP),pose_of(Rt,ot)));
  fprintf(FS,"#define FIT_%s_GRIP ",up[wi]);pr_pose(FS,grip);fprintf(FS,"\n#define FIT_%s_SUP ",up[wi]);pr_pose(FS,sup);
  fprintf(FS,"\n#define FIT_%s_TRIG {%.5ff, %.5ff, %.5ff}\n#define FIT_%s_POSE HP_W_%s_R\n#define FIT_%s_SUPPOSE HP_W_%s_L\n",up[wi],tg.p.x,tg.p.y,tg.p.z,up[wi],up[wi],up[wi],up[wi]);
  if(wi>=3){const float*x=RES[wi][1];float rv[3]={x[3]*DEG,x[4]*DEG,x[5]*DEG},a=sqrtf(rv[0]*rv[0]+rv[1]*rv[1]+rv[2]*rv[2]),k=a>1e-9f?sinf(0.5f*a)/a:0.5f;   /* giro de la mano respecto a la pose de Snake: va en la mano, no en el arma (vr_grip.h turn) */
    fprintf(FS,"#define FIT_%s_TURN {%.5ff, %.5ff, %.5ff, %.5ff}\n",up[wi],rv[0]*k,rv[1]*k,rv[2]*k,cosf(0.5f*a));}
  for(int s=1;s>=0;s--){const float*x=RES[wi][s];fprintf(FG,"    [HP_W_%s_%s - HP_W_FIRST] = {{",up[wi],s?"R":"L");
    for(int i=0;i<25;i++)fprintf(FG,"%.1ff%s",x[6+i],i<24?", ":"");
    fprintf(FG,"}, %d, {",s);float pl_[5];memcpy(pl_,x+6,20);if(s){pl_[1]+=12;pl_[2]+=18;pl_[3]+=10;}for(int i=0;i<5;i++){if(pl_[i]>ANG_HI[i])pl_[i]=ANG_HI[i];fprintf(FG,"%.1ff%s",s?pl_[i]:0.0f,i<4?", ":"");}fprintf(FG,"}},\n");}}
int main(int argc,char**argv){
  if(getenv("RLIM"))RLIM=atof(getenv("RLIM"));if(getenv("RLIM2"))RLIM2=atof(getenv("RLIM2"));if(getenv("WBASE"))WBASE=atof(getenv("WBASE"));if(getenv("WFRAME"))WFRAME=atof(getenv("WFRAME"));if(getenv("PENW"))PENW=atof(getenv("PENW"));if(getenv("WEB"))sscanf(getenv("WEB"),"%f,%f,%f,%f",WEB,WEB+1,WEB+2,&WEBW);if(getenv("TOFF"))sscanf(getenv("TOFF"),"%f,%f,%f",TOFF,TOFF+1,TOFF+2);for(int k=0;k<3;k++)TOFF[k]*=0.001f;if(getenv("WCON"))WCON=atof(getenv("WCON"));if(getenv("WTRIG"))WTRIG=atof(getenv("WTRIG"));if(getenv("TRANGE"))TRANGE=atof(getenv("TRANGE"));if(getenv("POLJ"))sscanf(getenv("POLJ"),"%d,%d",&POLJ0,&POLJ1);if(getenv("WFP"))WFP=atof(getenv("WFP"));if(getenv("HOLLOW"))HASHOL=sscanf(getenv("HOLLOW"),"%f,%f,%f,%f,%f,%f",HOL,HOL+1,HOL+2,HOL+3,HOL+4,HOL+5)==6;
  if(getenv("FPTS")){FILE*pf=fopen(getenv("FPTS"),"r");if(pf){while(NFP<8&&fscanf(pf,"%d %f %f %f",&FPF[NFP],&FPP[NFP][0],&FPP[NFP][1],&FPP[NFP][2])==4)NFP++;fclose(pf);}}size_t alen;uint8_t*arch=slurp("/tmp/gamedata/STAGE.DIR",&alen);if(!arch)return 1;gf_parts(PART);
  const char*mode=argc>1?argv[1]:"report";
  if(!strcmp(mode,"fit")||!strcmp(mode,"eval")){const char*w=argv[2];if(read_spec(argv[3]))return 1;
    for(int wi=0;wi<NWI;wi++){if(strcmp(w,"all")&&strcmp(w,WI[wi].name))continue;if(!SPEC[wi][1].has||(wi<NW&&!SPEC[wi][0].has))continue;if(getenv("SIDE")&&!load_res(wi))continue;if(load_weapon(arch,alen,wi))continue;
      rnd=777+wi;fit(wi,strcmp(mode,"fit")?1:0);save_res(wi);wload_free(&M_);gf_free(&WT);free(TWp);}return 0;}
  if(!strcmp(mode,"render")){for(int wi=0;wi<NWI;wi++){if(argc>3&&strcmp(argv[3],WI[wi].name))continue;if(!load_res(wi))continue;if(load_weapon(arch,alen,wi))continue;render_weapon(wi,argv[2]);wload_free(&M_);gf_free(&WT);free(TWp);}return 0;}
  if(!strcmp(mode,"wview")){if(argc>3)read_spec(argv[3]);for(int wi=0;wi<5;wi++){if(load_weapon(arch,alen,wi))continue;
      printf("%s GR(%.1f %.1f %.1f) SP(%.1f %.1f %.1f) TRIG %d (%.1f %.1f %.1f) mm\n",WI[wi].name,GR.p.x*1000,GR.p.y*1000,GR.p.z*1000,SP.p.x*1000,SP.p.y*1000,SP.p.z*1000,HASTRIG,TRIG[0]*1000,TRIG[1]*1000,TRIG[2]*1000);
      wview(wi,argv[2]);wload_free(&M_);gf_free(&WT);free(TWp);}return 0;}
  if(!strcmp(mode,"shape")){ /* shape OUT side a0..a24 : la mano sola en su marco, 3 vistas (palma +X, pulgar +Z, dorso) */
    int side=atoi(argv[3]);float a[25];for(int i=0;i<25;i++)a[i]=strtof(argv[4+i],NULL);float xf[VRHS_J][12];vrhs_pose_ang(a,side,xf);static float pos[VRHA_NV][3];vrhs_skin(xf,side,pos,NULL);
    for(int v=0;v<VRHA_NV;v++)for(int k=0;k<3;k++)Hw[0][v][k]=pos[v][k]*0.001f;float cen[3]={0,-0.08f,0};
    static const float dirs[3][3]={{1,0.05f,0.1f},{0.1f,0.05f,1},{-1,0.05f,0.1f}};
    for(int view=0;view<3;view++){Cam c;float eye[3],dn[3]={dirs[view][0]*side,dirs[view][1],dirs[view][2]};nrm3(dn);for(int k=0;k<3;k++)eye[k]=cen[k]+dn[k]*0.45f;cam_look(&c,eye,cen,40);clear_img();
      for(int pc=0;pc<VRHA_NP;pc++){const VrHaPiece*P=&vrha_piece[pc];for(int t=0;t<P->nt;t++){const uint8_t*ix=vrha_tri[P->t0+t];float T[3][3];for(int k=0;k<3;k++)memcpy(T[k],Hw[0][P->v0+ix[k]],12);
        int pt=PART[P->v0+ix[0]];int f=pt==0?0:(pt>=13?5:1+(pt-1)/3);draw_tri(&c,T,NULL,FC[side>0][f],NULL,0,0);}}
      char p[256];snprintf(p,sizeof p,"%s_%d.ppm",argv[2],view);save_ppm(p);}
    return 0;}
  if(!strcmp(mode,"spec")){read_spec(argv[2]);for(int wi=0;wi<NWI;wi++){if(!load_res(wi))continue;for(int s=1;s>=0;s--){const Spec*S=&SPEC[wi][s];if(!S->has)continue;printf("%s %d  ",WI[wi].name,s?1:-1);
      for(int i=0;i<31;i++)printf(" %.1f",RES[wi][s][i]);printf("   %.0f",S->pj);if(S->tw>0)printf("  %.4f %.4f %.4f %.2f",S->tgt[0],S->tgt[1],S->tgt[2],S->tw);printf("\n");}}return 0;}
  if(!strcmp(mode,"axes")){for(int wi=0;wi<5;wi++)for(int sd=-1;sd<=1;sd+=2){Base B;make_base(wi,sd,&B);float x[31]={0},o[3],p[3];float z0[3]={0,0,0};hand_point(&B,x,z0,o);
      const float ax[3][3]={{(float)sd,0,0},{0,-1,0},{0,0,1}};const char*nm[3]={"palma","dedos","pulgar"};printf("%s %+d ",WI[wi].name,sd);
      for(int a=0;a<3;a++){float q[3]={ax[a][0]*100,ax[a][1]*100,ax[a][2]*100};hand_point(&B,x,q,p);printf(" %s(%.2f %.2f %.2f)",nm[a],(p[0]-o[0])*10,(p[1]-o[1])*10,(p[2]-o[2])*10);}printf("\n");}return 0;}
  if(!strcmp(mode,"look")){ /* look WI cx cy cz(mm) dist(m) dx dy dz OUT [res]: vista libre del arma (y de las manos si res) */
    int wi=atoi(argv[2]);load_weapon(arch,alen,wi);float c0[3]={strtof(argv[3],0)*0.001f,strtof(argv[4],0)*0.001f,strtof(argv[5],0)*0.001f},dist=strtof(argv[6],0),dn[3]={strtof(argv[7],0),strtof(argv[8],0),strtof(argv[9],0)};nrm3(dn);
    Cam c;float eye[3];for(int k=0;k<3;k++)eye[k]=c0[k]+dn[k]*dist;cam_look(&c,eye,c0,40);clear_img();GRAYW=getenv("TEX")?0:1;draw_weapon(&c,&FR);
    if(argc>11&&load_res(wi)){Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);for(int s=0;s<2;s++){hand_world(&B[s],RES[wi][s],Hw[s],NULL,NULL,NULL);
      for(int pc=0;pc<VRHA_NP;pc++){const VrHaPiece*P=&vrha_piece[pc];for(int t=0;t<P->nt;t++){const uint8_t*ix=vrha_tri[P->t0+t];float T[3][3];for(int k=0;k<3;k++)memcpy(T[k],Hw[s][P->v0+ix[k]],12);
        int pt=PART[P->v0+ix[0]];int f=pt==0?0:(pt>=13?5:1+(pt-1)/3);draw_tri(&c,T,NULL,FC[s][f],NULL,0,0);}}}}
    if(!getenv("TEX"))wire(&c);static const uint8_t cg[3]={0,200,0},cy[3]={230,230,0};float g[3]={GR.p.x,GR.p.y,GR.p.z};draw_sph(&c,g,0.004f,cg);if(HASTRIG)draw_sph(&c,TRIG,0.004f,cy);save_ppm(argv[10]);return 0;}
  if(!strcmp(mode,"rv")){ /* rv WI lado px py pz(mm) nx ny nz fx fy fz: t y r (spec) que ponen el centro de agarre en P, la palma mirando a n y los dedos hacia f (marco del arma) */
    int wi=atoi(argv[2]),sd=atoi(argv[3]);float P[3],n[3],f[3];for(int k=0;k<3;k++){P[k]=strtof(argv[4+k],0);n[k]=strtof(argv[7+k],0);f[k]=strtof(argv[10+k],0);}
    nrm3(n);float d=dot3(f,n);for(int k=0;k<3;k++)f[k]-=d*n[k];nrm3(f);
    float ex[3]={sd*n[0],sd*n[1],sd*n[2]},ey[3]={-f[0],-f[1],-f[2]},ez[3];ez[0]=ex[1]*ey[2]-ex[2]*ey[1];ez[1]=ex[2]*ey[0]-ex[0]*ey[2];ez[2]=ex[0]*ey[1]-ex[1]*ey[0];
    float Rt[9];for(int r=0;r<3;r++){Rt[3*r]=ex[r];Rt[3*r+1]=ey[r];Rt[3*r+2]=ez[r];}
    Base B;make_base(wi,sd,&B);float BT[9];for(int r=0;r<3;r++)for(int c=0;c<3;c++)BT[3*r+c]=B.R[3*c+r];float Rr[9];mm3(BT,Rt,Rr);
    float q[4];m2q(Rr,q);if(q[3]<0)for(int k=0;k<4;k++)q[k]=-q[k];float sn=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]),a=2*atan2f(sn,q[3]);float rv[3];for(int k=0;k<3;k++)rv[k]=sn>1e-9f?q[k]/sn*a/DEG:0;
    float pp[3]={P[0]-B.o[0]*1000,P[1]-B.o[1]*1000,P[2]-B.o[2]*1000},t[3];mv(BT,pp,t);for(int k=0;k<3;k++)t[k]-=B.c[k]+B.d[k];
    printf("%.1f %.1f %.1f %.1f %.1f %.1f\n",t[0],t[1],t[2],rv[0],rv[1],rv[2]);return 0;}
  if(!strcmp(mode,"texscan")){ /* texscan WI xmin: puntos de las caras con x > xmin (mm) y luminancia de textura: rejilla de 2 mm (y, z) -> caracter */
    int wi=atoi(argv[2]);float xmin=strtof(argv[3],0)*0.001f;load_weapon(arch,alen,wi);static int lum[200][400];for(int i=0;i<200;i++)for(int j=0;j<400;j++)lum[i][j]=-1;
    for(int m=0;m<FR.nmeshes;m++){const VrHandsMesh*Mq=&FR.meshes[m];const uint8_t*tex=NULL;int tw=0,th=0;for(int t=0;t<FR.ntex;t++)if(FR.tex[t].id==Mq->tex_id&&Mq->tex_id){tex=FR.tex[t].rgba;tw=FR.tex[t].w;th=FR.tex[t].h;}if(!tex)continue;
      for(uint32_t t=0;t+2<Mq->nidx;t+=3){float P[3][3],UV[3][2];for(int k=0;k<3;k++){uint16_t ix=Mq->idx[t+k];const float*q=Mq->verts[ix].pos,*a=Mq->model;for(int c=0;c<3;c++)P[k][c]=a[c]*q[0]+a[4+c]*q[1]+a[8+c]*q[2]+a[12+c];UV[k][0]=Mq->verts[ix].uv[0];UV[k][1]=Mq->verts[ix].uv[1];}
        if(P[0][0]<xmin||P[1][0]<xmin||P[2][0]<xmin)continue;
        for(int a=0;a<=160;a++)for(int b=0;a+b<=160;b++){float u=a/160.f,v=b/160.f,w=1-u-v;float X[3],T[2];for(int c=0;c<3;c++)X[c]=w*P[0][c]+u*P[1][c]+v*P[2][c];for(int c=0;c<2;c++)T[c]=w*UV[0][c]+u*UV[1][c]+v*UV[2][c];
          int tx=(int)(T[0]*tw),ty=(int)(T[1]*th);if(tx<0)tx=0;if(ty<0)ty=0;if(tx>=tw)tx=tw-1;if(ty>=th)ty=th-1;const uint8_t*px=tex+4*(ty*tw+tx);int L=(px[0]+px[1]+px[2])/3;
          int iy=(int)((X[1]*1000+100)/2),iz=(int)((X[2]*1000+400)/2);if(iy>=0&&iy<200&&iz>=0&&iz<400)lum[iy][iz]=L;}}}
    for(int iy=199;iy>=0;iy--){int any=0;for(int iz=0;iz<400;iz++)if(lum[iy][iz]>=0)any=1;if(!any)continue;printf("%4d ",iy*2-100);for(int iz=120;iz<240;iz++){int L=lum[iy][iz];putchar(L<0?' ':L<10?'#':L<25?'+':L<45?'-':'.');}printf("\n");}
    printf("     z: columna 0 = %d mm, 2 mm por columna (izquierda = -z, boca)\n",120*2-400);return 0;}
  if(!strcmp(mode,"sil")){ /* sil ARMA SPEC MASKDIR cx cy cz dist "dx dy dz"x4: mano derecha por silueta (+ física), izquierda reajustada con la derecha como obstáculo */
    int wi=-1;for(int i=0;i<5;i++)if(!strcmp(argv[2],WI[i].name))wi=i;if(wi<0||read_spec(argv[3])||!load_res(wi)||load_weapon(arch,alen,wi))return 1;
    for(int k=0;k<3;k++)SILC[k]=strtof(argv[5+k],0)*0.001f;SILD=strtof(argv[8],0);SILN=0;for(int v=0;v<4&&9+v<argc;v++){sscanf(argv[9+v],"%f %f %f",&SILDIR[v][0],&SILDIR[v][1],&SILDIR[v][2]);SILN++;}
    char p[512];for(int v=0;v<SILN;v++){uint8_t (*o)[IW]=SILT[0][v],(*b)[IW]=SILT[1][v];static uint8_t tw_[IH][IW];snprintf(p,sizeof p,"%s/h%d_o.pgm",argv[4],v);if(read_pgm(p,o))return 1;snprintf(p,sizeof p,"%s/h%d_w.pgm",argv[4],v);WHAS[v]=0;if(!read_pgm(p,tw_)){if(getenv("SILWR")){double sx=0,sy=0,n=0;for(int y=0;y<IH;y++)for(int x=0;x<IW;x++)if(tw_[y][x]){sx+=x;sy+=y;n++;}if(n>200){WHAS[v]=1;WCX[v]=sx/n;WCY[v]=sy/n;}}else for(int y=0;y<IH;y++)for(int x=0;x<IW;x++)o[y][x]|=tw_[y][x];}snprintf(p,sizeof p,"%s/h%d_b.pgm",argv[4],v);if(read_pgm(p,b))return 1;}
    if(getenv("SILW"))SILW=atof(getenv("SILW"));SILOCC=getenv("SILOCC")!=NULL;if(getenv("SILPW"))SILPW=atof(getenv("SILPW"));if(getenv("SILWR"))SILWR=atof(getenv("SILWR"));
    if(getenv("SILPTS")){FILE*pf=fopen(getenv("SILPTS"),"r");if(pf){while(NPT<32&&fscanf(pf,"%d %d %f %f",&PTV[NPT],&PTF[NPT],&PTX[NPT],&PTY[NPT])==4)NPT++;fclose(pf);}}
    int nd=getenv("SILJ")?31:6;   /* SILJ: también los 25 ángulos (paso de 8 a 0,5 grados, límites ANG_LO/ANG_HI) */
    Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);
    for(int s=1;s>=0;s--){if(getenv("SIDE")&&atoi(getenv("SIDE"))!=s)continue;const Spec*S=&SPEC[wi][s];Ctx C={&WT,NULL,need_mask(s?1:-1),s&&HASTRIG,S,{0,0,0},RLIM};if(!s||wi>=3){memcpy(C.r0,S->x+3,12);C.rlim=RLIM2;}GfTris HR;if(!s){hand_tris(&B[1],RES[wi][1],&HR);C.H2=&HR;}
      float*x=RES[wi][s];printf("%s antes:\n",s?"derecha":"izquierda");double sc=sil_cost(&B[s],x,s?1:-1,1);Rep r;double ph=eval(&B[s],&C,x,&r);pr_rep(s?"R":"L",&r,C.trig);
      double cur=SILW*sc+ph;for(int round=0;round<3;round++){for(float st=8;st>=0.5f;st*=0.5f){for(int rep=0;rep<40;rep++){double bo=cur;int bd=-1;float bs=0;
        for(int d=0;d<nd;d++)for(int sg=-1;sg<=1;sg+=2){if(d>=6&&((d-6)%5==0&&d<26))continue;float y[31];memcpy(y,x,124);y[d]+=sg*st;if(d>=6&&(y[d]<ANG_LO[d-6]||y[d]>ANG_HI[d-6]))continue;double c=SILW*sil_cost(&B[s],y,s?1:-1,0)+eval(&B[s],&C,y,NULL);if(c<bo){bo=c;bd=d;bs=sg*st;}}
        if(bd<0)break;x[bd]+=bs;cur=bo;}}polish(&B[s],&C,x);cur=SILW*sil_cost(&B[s],x,s?1:-1,0)+eval(&B[s],&C,x,NULL);}
      printf("%s despues:\n",s?"derecha":"izquierda");sil_cost(&B[s],x,s?1:-1,1);eval(&B[s],&C,x,&r);pr_rep(s?"R":"L",&r,C.trig);if(!s)gf_free(&HR);}
    save_res(wi);return 0;}
  if(!strcmp(mode,"ext")){ /* ext WI: extensión de cada mano en el marco del arma (mm) */
    int wi=atoi(argv[2]);if(!load_res(wi)||load_weapon(arch,alen,wi))return 1;Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);
    for(int s=1;s>=0;s--){static float w[VRHA_NV][3];hand_world(&B[s],RES[wi][s],w,NULL,NULL,NULL);float lo[3]={1e9,1e9,1e9},hi[3]={-1e9,-1e9,-1e9};for(int v=0;v<VRHA_NV;v++)for(int k=0;k<3;k++){if(w[v][k]<lo[k])lo[k]=w[v][k];if(w[v][k]>hi[k])hi[k]=w[v][k];}
      float xf[VRHS_J][12];vrhs_pose_ang(RES[wi][s]+6,B[s].side,xf);float hp[3];for(int a=0;a<3;a++)hp[a]=0.5f*(xf[2][4*a+3]+xf[6][4*a+3]);float wp[3];hand_point(&B[s],RES[wi][s],hp,wp);
      printf("%s x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f  membrana (%.0f %.0f %.0f)\n",s?"D":"I",lo[0]*1000,hi[0]*1000,lo[1]*1000,hi[1]*1000,lo[2]*1000,hi[2]*1000,wp[0]*1000,wp[1]*1000,wp[2]*1000);}return 0;}
  if(!strcmp(mode,"meas")){ /* meas WI ybase(mm) yframe(mm) cx cy cz dist "dir"x3: contacto base del mango-palma azul, mano naranja-armazón y píxel de la yema por vista */
    int wi=atoi(argv[2]);float yb=strtof(argv[3],0)*0.001f,yfr=strtof(argv[4],0)*0.001f;if(!load_res(wi)||load_weapon(arch,alen,wi))return 1;Base B[2];make_base(wi,1,&B[1]);make_base(wi,-1,&B[0]);
    static float w[2][VRHA_NV][3];float pad[3];for(int s=0;s<2;s++)hand_world(&B[s],RES[wi][s],w[s],NULL,NULL,s?pad:NULL);
    float (*tb)[3]=malloc(sizeof(float)*9*NT),(*tf)[3]=malloc(sizeof(float)*9*NT);int nb=0,nf=0;
    for(int t=0;t<NT;t++){int lo=1,hi=1;for(int k=0;k<3;k++){if(TWp[3*t+k][1]>yb)lo=0;if(TWp[3*t+k][1]<yfr)hi=0;}if(lo){for(int k=0;k<3;k++)memcpy(tb[3*nb+k],TWp[3*t+k],12);nb++;}if(hi){for(int k=0;k<3;k++)memcpy(tf[3*nf+k],TWp[3*t+k],12);nf++;}}
    GfTris TB,TF;gf_build(&TB,tb,nb);gf_build(&TF,tf,nf);float db=1e9,df=1e9;
    for(int v=0;v<VRHA_NV;v++){if(PART[v]==0){float d=gf_sd(&TB,w[0][v],0.1f,0)*1000;if(d<db)db=d;}float d2=gf_sd(&TF,w[1][v],0.1f,0)*1000;if(d2<df)df=d2;}
    printf("base del mango (y < %.0f mm, %d triangulos) - palma azul: %.1f mm\\nmano naranja - armazon (y > %.0f mm, %d triangulos): %.1f mm\\n",yb*1000,nb,db,yfr*1000,nf,df);
    float C0[3]={strtof(argv[5],0)*0.001f,strtof(argv[6],0)*0.001f,strtof(argv[7],0)*0.001f},D=strtof(argv[8],0);
    for(int v=0;9+v<argc;v++){float dn[3];sscanf(argv[9+v],"%f %f %f",dn,dn+1,dn+2);nrm3(dn);Cam c;float eye[3];for(int k=0;k<3;k++)eye[k]=C0[k]+dn[k]*D;cam_look(&c,eye,C0,40);int px,py;if(proj(&c,pad,&px,&py))printf("vista %d: yema en (%d, %d)\\n",v,px,py);}
    printf("yema (mm) %.1f %.1f %.1f, gatillo %.1f %.1f %.1f\\n",pad[0]*1000,pad[1]*1000,pad[2]*1000,TRIG[0]*1000,TRIG[1]*1000,TRIG[2]*1000);return 0;}
  if(!strcmp(mode,"section")){int wi=atoi(argv[2]);load_weapon(arch,alen,wi);section(strtof(argv[3],NULL)*0.001f,argc>4?atoi(argv[4]):1);return 0;}
  if(!strcmp(mode,"emit")){FG=fopen(argv[2],"w");FS=fopen(argv[3],"w");
    fprintf(FG,"/* GENERADO por vr/hands/gripfit_gen.c: ángulos de articulación (grados, orden de vrhs_pose_ang) por arma y mano. La forma de cada mano se diseñó a mano por arma (tabla DESIGN de gripfit_gen.c);\n"
               " * el generador solo colocó la palma (6 GDL, giro <= 20 grados respecto a la pose de Snake) y pulió unos grados por articulación contra la malla real del arma (STAGE.DIR). Comprobado por test_gripfit. */\n"
               "static const WeaponPoseDef WTAB[HP_COUNT - HP_W_FIRST] = {\n");
    fprintf(FS,"/* GENERADO por vr/hands/gripfit_gen.c (ver vr_hand_grips.inc): zócalos por arma (mano que empuña -> marco de empuñadura vrhands_grip; mano de apoyo, reflejada -> punto WP_SUPPORT) y yema del índice con el gatillo suelto. */\n");
    for(int wi=0;wi<NWI;wi++){if(!load_res(wi))continue;if(load_weapon(arch,alen,wi))continue;emit_weapon(wi);wload_free(&M_);gf_free(&WT);free(TWp);}
    fprintf(FG,"};\n");fclose(FG);fclose(FS);return 0;}
  return 0;}
