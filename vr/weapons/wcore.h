/* Utilidades del núcleo de armas (matemática y helpers de interacción).
 * Complementa weapon.h sin modificar el contrato. */
#pragma once
#include "weapon.h"
#include <math.h>

/* ---- vectores ---- */
static inline WVec3 wv(float x, float y, float z) { WVec3 r = {x, y, z}; return r; }
static inline WVec3 wv_add(WVec3 a, WVec3 b) { return wv(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline WVec3 wv_sub(WVec3 a, WVec3 b) { return wv(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline WVec3 wv_scale(WVec3 a, float s) { return wv(a.x * s, a.y * s, a.z * s); }
static inline float wv_dot(WVec3 a, WVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline WVec3 wv_cross(WVec3 a, WVec3 b) {
    return wv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float wv_len(WVec3 a) { return sqrtf(wv_dot(a, a)); }
static inline WVec3 wv_norm(WVec3 a) {
    float l = wv_len(a); return l > 1e-9f ? wv_scale(a, 1.0f / l) : wv(0, 0, 0);
}
static inline WVec3 wv_lerp(WVec3 a, WVec3 b, float t) { return wv_add(a, wv_scale(wv_sub(b, a), t)); }

/* ---- cuaterniones / poses (las básicas de weapon.h: wpose_mul, wpose_apply, wquat_axis_angle) ---- */
WQuat wquat_identity(void);
WQuat wquat_mul(WQuat a, WQuat b);
WQuat wquat_inv(WQuat q);
WQuat wquat_normalize(WQuat q);
WQuat wquat_slerp(WQuat a, WQuat b, float t);
WVec3 wquat_rotate(WQuat q, WVec3 v);
WVec3 wquat_to_rotvec(WQuat q);          /* eje*ángulo (ángulo en [-pi,pi]) */
WQuat wquat_from_rotvec(WVec3 r);
WPose wpose_identity(void);
WPose wpose_inv(WPose a);
WPose wpose_lerp(WPose a, WPose b, float t);

/* ---- helpers de arma ---- */
/* Pose mundo del punto de interacción p (usa wcore_weapon_world). */
WPose wcore_point_world(const WeaponState *s, const WInput *in, WPoint p);
/* Índice de pieza por nombre (-1 si no existe); mira las piezas enlazadas (s->parts) si hay binding. */
int wcore_part_find(const WeaponState *s, const char *name);
/* Reinicio común de init (ver WeaponDef.init): pone a cero state_size bytes conservando el binding. */
void wcore_state_reset(WeaponState *s, const WeaponDef *d);
/* dt seguro: 0 si es NaN o <= 0, y como mucho maxdt. Los update salen sin hacer nada si da 0. */
static inline float wcore_dt(float dt, float maxdt) {
    if (!(dt > 0.0f)) return 0.0f;
    return dt > maxdt ? maxdt : dt;
}
/* Ruido determinista en [-1,1]; avanza *seed. */
float wcore_rand(uint32_t *seed);
/* Acerca v a target como mucho max_step. */
static inline float wcore_approach(float v, float target, float max_step) {
    if (v < target) { v += max_step; return v > target ? target : v; }
    v -= max_step; return v < target ? target : v;
}
static inline int wcore_edge_down(uint32_t prev, uint32_t cur, uint32_t mask) { return (cur & mask) && !(prev & mask); }
static inline float wcore_clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
