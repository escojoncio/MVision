#ifndef NS_PRESENT_H
#define NS_PRESENT_H
/* ns_present: qué se enseña del mundo en cada fotograma. El mundo 3D sale SIEMPRE del renderer nativo; la lista de la GPU no se dibuja en 3D jamás.
 * Si en un fotograma el nativo no está listo (escena inválida, mallas ausentes, renderer sin pipelines...), se REPITE el último fotograma nativo válido (reproyectado con
 * la cabeza actual: el mundo está fijo en el espacio VR) y, si dura demasiado, se funde suavemente a negro. Las antiguas condiciones de caída a la GPU (modo, coherencia
 * con la lista, escena vieja...) ya no deciden nada: solo quedan como bits de diagnóstico (`why`). C puro, probado en Linux (test_ns_present.c). */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NS_PR_CURRENT = 0, NS_PR_HOLD = 1, NS_PR_BLACK = 2 };   /* NO existe un modo «GPU» */
/* Bits de diagnóstico (solo registro). */
enum { NS_PRW_NO_RENDERER = 1, NS_PRW_NO_SCENE = 2, NS_PRW_UNFIT = 4, NS_PRW_MESHES = 8, NS_PRW_STALE = 16, NS_PRW_INCOHERENT = 32, NS_PRW_NO_PIPES = 64 };

typedef struct { uint64_t hold_ns; uint64_t fade_ns; uint64_t stale_ns; } NsPresentCfg;   /* repetir sin fundir, duración del fundido, antigüedad que solo se registra */
NsPresentCfg ns_present_cfg_default(void);   /* 9300 ms, 700 ms, 250 ms: el negro solo llega si NO hay ninguna escena dibujable durante 10 s */

typedef struct { int have_good; uint64_t last_good_ns; uint32_t held_frames, black_frames; uint64_t n_current, n_hold, n_black; } NsPresentState;
void ns_present_reset(NsPresentState *s);

typedef struct {
    int renderer_ok;      /* renderer nativo con pipelines y ranura */
    int have_scene;       /* el puente tiene una escena */
    int scene_fit;        /* la escena es apta (ns_scene_native_ok) */
    int meshes_ok;        /* todas sus mallas están subidas */
    int coherent;         /* red de coherencia con la lista (solo registro; 1 = no evaluada o coherente) */
    uint64_t scene_t_ns, now_ns;
} NsPresentIn;
typedef struct { int mode; float fade; uint32_t why; } NsPresentOut;   /* fade: 1 = imagen entera, 0 = negro */

/* Un paso por fotograma. Siempre devuelve CURRENT, HOLD o BLACK. */
int ns_present_step(NsPresentState *s, const NsPresentCfg *cfg, const NsPresentIn *in, NsPresentOut *out);

#ifdef __cplusplus
}
#endif
#endif
