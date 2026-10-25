/* Mezcla semitransparente del original (4 modos) por canal en el espacio GAMMA, el mismo en que el original mezcla. Válido en C (prueba vr/scene/test_blend.c) y en MSL (vr_fs_scene).
 * B = destino, F = fragmento, ambos en [0,1]. Modo 0: 0,5B + 0,5F; 1: B + F; 2: B - F; 3: B + 0,25F; resultado recortado a 0..1. */
#pragma once
#ifdef __METAL_VERSION__
#define VR_BLEND_FN inline
#else
#define VR_BLEND_FN static inline
#endif

VR_BLEND_FN float vr_blend1(int mode, float b, float f) {
    float r = mode == 0 ? 0.5f * b + 0.5f * f : mode == 1 ? b + f : mode == 2 ? b - f : b + 0.25f * f;
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}
