/* eyeshot: sonda de pruebas (ver eyeshot.c). No forma parte de la app. */
#pragma once
#include "vr_scene.h"
void eyeshot_begin(const VrScene *s);
void eyeshot_end(const VrScene *s);
int eyeshot_render(const VrScene *s, const float head[16], int W, int H, float hfov_deg, const char *ppm_path);
