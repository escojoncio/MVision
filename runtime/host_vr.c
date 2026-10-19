/* Pose de la cabeza publicada por el host VR (hilo de render) y leída por el runtime (hilo de emulación).
 * Matriz 4x4 en columnas (16 floats, formato simd/Metal), espacio de "origen recentrado" del host:
 * metros, Y hacia arriba, -Z = delante al recentrar. t = segundos (tiempo de presentación previsto). */
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "host.h"
#include "host_hooks.h"

static pthread_mutex_t VR_MX = PTHREAD_MUTEX_INITIALIZER;
static float VR_HEAD[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static double VR_T = 0.0;
static int VR_VALID = 0;
static struct timespec VR_STAMP;
static float VR_TURN;
static double VR_TURN_SUM;   /* suma de todos los giros pedidos (no se borra al tomarlos): lo lee el renderer para la corrección por fotograma (vr_smooth) */
static int VR_RECENTER;
static float VR_EYE_H;      /* altura de ojos de pie pedida (m); 0 = estimar al recentrar */
static int VR_EYE_H_SET;

void mv_vr_set_head(const float m[16], double t)
{
    if (!m) return;
    pthread_mutex_lock(&VR_MX);
    memcpy(VR_HEAD, m, sizeof VR_HEAD);
    VR_T = t;
    VR_VALID = 1;
    clock_gettime(CLOCK_MONOTONIC, &VR_STAMP);
    pthread_mutex_unlock(&VR_MX);
}

void mv_vr_clear_head(void)
{
    pthread_mutex_lock(&VR_MX);
    VR_VALID = 0;
    pthread_mutex_unlock(&VR_MX);
}

double mv_vr_head_age(void)
{
    struct timespec n;
    double a = -1.0;
    clock_gettime(CLOCK_MONOTONIC, &n);
    pthread_mutex_lock(&VR_MX);
    if (VR_VALID) a = (double)(n.tv_sec - VR_STAMP.tv_sec) + (double)(n.tv_nsec - VR_STAMP.tv_nsec) * 1e-9;
    pthread_mutex_unlock(&VR_MX);
    return a;
}

void mv_vr_turn(float deg)
{
    if (!isfinite(deg)) return;
    pthread_mutex_lock(&VR_MX);
    VR_TURN += deg;
    VR_TURN_SUM += (double)deg;
    pthread_mutex_unlock(&VR_MX);
}

float mv_vr_take_turn(void)
{
    pthread_mutex_lock(&VR_MX);
    float t = VR_TURN;
    VR_TURN = 0.0f;
    pthread_mutex_unlock(&VR_MX);
    return t;
}

void mv_vr_recenter(void)
{
    pthread_mutex_lock(&VR_MX);
    VR_RECENTER = 1;
    pthread_mutex_unlock(&VR_MX);
}

int mv_vr_take_recenter(void)
{
    pthread_mutex_lock(&VR_MX);
    int r = VR_RECENTER;
    VR_RECENTER = 0;
    pthread_mutex_unlock(&VR_MX);
    return r;
}

void mv_vr_set_user_eye_height(float m)
{
    if (!isfinite(m) || m < 0.0f || m > 3.0f) return;
    pthread_mutex_lock(&VR_MX);
    VR_EYE_H = m; VR_EYE_H_SET = 1;
    pthread_mutex_unlock(&VR_MX);
}

int mv_vr_take_user_eye_height(float *m)
{
    pthread_mutex_lock(&VR_MX);
    int r = VR_EYE_H_SET;
    if (r && m) *m = VR_EYE_H;
    VR_EYE_H_SET = 0;
    pthread_mutex_unlock(&VR_MX);
    return r;
}

/* Devuelve 1 si hay una pose publicada (y la copia), 0 si aún no (m queda intacta). t puede ser NULL. */
int mv_vr_get_head(float m[16], double *t)
{
    int ok;
    pthread_mutex_lock(&VR_MX);
    ok = VR_VALID;
    if (ok) {
        if (m) memcpy(m, VR_HEAD, sizeof VR_HEAD);
        if (t) *t = VR_T;
    }
    pthread_mutex_unlock(&VR_MX);
    return ok;
}

double mv_vr_turn_total(void)
{
    pthread_mutex_lock(&VR_MX);
    double t = VR_TURN_SUM;
    pthread_mutex_unlock(&VR_MX);
    return t;
}
