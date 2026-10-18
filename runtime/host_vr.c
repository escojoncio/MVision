/* Pose de la cabeza publicada por el host VR (hilo de render) y leída por el runtime (hilo de emulación).
 * Matriz 4x4 en columnas (16 floats, formato simd/Metal), espacio de "origen recentrado" del host:
 * metros, Y hacia arriba, -Z = delante al recentrar. t = segundos (tiempo de presentación previsto). */
#include <pthread.h>
#include <string.h>
#include "host.h"

static pthread_mutex_t VR_MX = PTHREAD_MUTEX_INITIALIZER;
static float VR_HEAD[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static double VR_T = 0.0;
static int VR_VALID = 0;

void mv_vr_set_head(const float m[16], double t)
{
    if (!m) return;
    pthread_mutex_lock(&VR_MX);
    memcpy(VR_HEAD, m, sizeof VR_HEAD);
    VR_T = t;
    VR_VALID = 1;
    pthread_mutex_unlock(&VR_MX);
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
