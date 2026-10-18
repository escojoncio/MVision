/* Rasterizador software mínimo para verificar armas (z-buffer, Lambert, supersampling 2x2 = "MSAA x4"). */
#pragma once
#include "../../vr/weapons/wcore.h"

typedef struct { int w, h; uint8_t *rgb; } WImage;
typedef struct { WVec3 eye, target, up; float fov_y_deg; } WCam;

WImage wimg_new(int w, int h);
void wimg_free(WImage *im);
void wimg_blit(WImage *dst, const WImage *src, int x0, int y0);
int wimg_write_png(const WImage *im, const char *path);   /* PNG sin dependencias (deflate "stored") */
int wimg_write_ppm(const WImage *im, const char *path);

/* Cámara predefinida alrededor del arma: view = side|left|top|front|rear|three|three2|under. */
int wprev_cam(const WeaponState *s, WPose weapon_world, const char *view, float radius, WCam *out);
/* Dibuja piezas (jerarquía + canales) y objetos sostenidos (s->held). ss = factor de supersampling por eje. */
void wprev_render(const WeaponState *s, WPose weapon_world, const WCam *cam, WImage *img, int ss);
