/* Modelo de color/iluminación fiel al motor original para el renderer nativo.
 *
 * Referencia en C (enteros, bit a bit con la unidad de geometría de referencia del runtime) + equivalente en coma flotante para shader.
 * Sin dependencias: tipos propios, C99. Descripción completa y fórmulas MSL en docs/iluminacion_nativa.md.
 *
 * Convenciones: 4096 = 1,0 en matrices y normales (4.12). Los colores de luz y el ambiente de las órdenes del
 * motor vienen en unidades 0..255 y la unidad de geometría los usa x16 ("unidades UG"); el color final de cada
 * vértice es un byte donde 128 = factor 1,0 al modular la textura (el resultado puede llegar a 255 = x1,99). */
#ifndef NS_LIGHT_H
#define NS_LIGHT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Conjunto de luz de un objeto (lo que ve la UG al calcular una normal) ----
 * lmat: fila i = dirección de la luz i en el espacio de la normal (x4096; las puntuales llegan con módulo variable).
 * cmat: columna j = color de la luz j (unidades UG: valor0..255 x16 = 4096 es el máximo "natural").
 * bg : ambiente en unidades UG (valor0..255 x16). */
typedef struct {
    int16_t lmat[3][3];
    int16_t cmat[3][3];
    int32_t bg[3];
} NsLight;

/* Luz global del nivel (órdenes del script) */
typedef struct {
    int16_t main_dir[3];   /* dirección principal normalizada (módulo 4096) */
    int16_t main_col[3];   /* color principal ya x16 (R,G,B) */
    int16_t ambient[3];    /* ambiente en unidades 0..255 (NO x16) */
} NsLightEnv;

/* Luz puntual del mapa / temporal (formato en disco) */
typedef struct {
    int16_t  pos[3];
    uint16_t r_range;      /* radio de caída */
    uint16_t e_range;      /* caja de influencia (|d| por eje <= e_range) */
    uint8_t  col[3];       /* R,G,B 0..255 (la UG usa x16) */
} NsPointLight;

typedef struct { const NsPointLight *l; int n; } NsLightList;

/* ---- Color de una normal (aritmética de la unidad de geometría, desplazamiento 12, sin negativos): exacto ----
 * n: normal en el MISMO espacio que las filas de lmat (4096 = 1,0). rgb: bytes 0..255. */
void ns_light_color(const NsLight *L, const int16_t n[3], uint8_t rgb[3]);
/* Igual, devolviendo el acumulador antes de >>4 y la saturación (acumuladores por canal). */
void ns_light_color_acc(const NsLight *L, const int16_t n[3], int32_t mac[3]);
/* Equivalente para shader (sin cuantizar). n unitaria float. Devuelve el FACTOR de modulación de textura
 * (1,0 = neutro, máx 255/128): tint = clamp(floor(256*(bg/4096 + cmat/4096 * clamp(lmat/4096 . n, 0, 7.999756)))/128, 0, 255/128).
 * El floor reproduce el byte del motor (error medio 0,008 byte, máximo 1 byte por redondeos de la cadena entera); sin él el error medio es 0,28 y el máximo ~1,1 byte. */
void ns_light_tint_f(const NsLight *L, const float n[3], float tint[3]);

/* ---- Cambio de espacio: lmat_local = lmat_mundo x R  (R = rotación del objeto/modelo 3x3 en 4.12). Replica
 * la multiplicación de matrices de la UG (cada elemento (suma>>12) saturado a 16 bits con signo). */
void ns_light_to_local(const int16_t lmat_world[3][3], const int16_t R[3][3], int16_t lmat_local[3][3]);
/* Posición mundo de un vértice local: p = T + (R.v >> 12) con acumulación exacta (como RT + lectura de MAC). */
void ns_light_world_pos(const int16_t R[3][3], const int32_t T[3], const int16_t v[3], int32_t out[3]);

/* ---- Luces puntuales ---- */
/* Raíz entera aproximada con tabla de 8 bits (réplica de la raíz del motor; NO es floor(sqrt); error relativo < 0,9 %). a<=0 -> 0 */
uint32_t ns_light_isqrt(int32_t a);
/* Vector de luz: d = pos_objeto - pos_luz (cada componente se trunca a 16 bits UNA vez, para distancia y numerador;
 * el motor solo trunca en la distancia, pero con la caja e_range <= 32767 es idéntico; la suma de cuadrados se satura), módulo 3072*(2r-dist)/r, dirección
 * luz->objeto. r_range==0 -> (0,0,0). */
void ns_light_point_vector(const int32_t d[3], int r_range, int16_t out[3]);
/* Elige hasta 2 luces: se recorren las listas en orden (temporales de la pasada anterior primero, luego las fijas
 * en el orden en que se registraron) y se toman las 2 primeras cuya caja contiene pos. Devuelve cuántas (0..2).
 * vec[k] = vector de luz, col[k] = color 0..255. */
int ns_light_pick(const NsLightList *lists, int nlists, const int32_t pos[3], int16_t vec[2][3], uint8_t col[2][3]);

/* Monta el NsLight en espacio MUNDO a partir del entorno y de las luces elegidas (filas/columnas sin luz = 0).
 * ambient_override: NULL -> env->ambient. */
void ns_light_assemble(const NsLightEnv *env, int nl, const int16_t vec[2][3], const uint8_t col[2][3],
                       const int16_t *ambient_override, NsLight *out);

/* Objeto dinámico (modelos animados): luz en la posición del objeto y rotación de cada pieza. */
void ns_light_object(const NsLightEnv *env, const NsLightList *lists, int nlists, const int32_t obj_pos[3],
                     const int16_t piece_rot[3][3], NsLight *out);
/* Vértice de mapa (preshade): luces evaluadas en la posición MUNDO de cada vértice. */
void ns_light_preshade_vertex(const NsLightEnv *env, const NsLightList *lists, int nlists, const int32_t vert_world[3],
                              const int16_t obj_rot[3][3], const int16_t normal[3], uint8_t rgb[3]);

/* ---- Niebla (actor opcional por zona). start/len en unidades de 256 de profundidad (SZ>>8), como los parámetros
 * del actor. Color lejano = negro. factor f en 0..4096 (4096 = todo niebla). ---- */
typedef struct { int start; int len; int len0; } NsFog;   /* len0 = longitud con la que se calculó la rampa */
void ns_fog_init(NsFog *f, int start, int len);
/* Cambio de inicio (el actor lo adapta con el zoom: start += (255-start)*(zoom-360)/3640). */
void ns_fog_set_start(NsFog *f, int start);
int  ns_fog_factor(const NsFog *f, int zi);     /* zi = SZ>>8, 0..255 */
int  ns_fog_culled(const NsFog *f, int zi);     /* el motor no ordena primitivas con zi >= start+len */
/* Mezcla de profundidad con color lejano 0 (mezcla de profundidad exacta): c*(1-f) */
void ns_fog_apply(int f, const uint8_t in[3], uint8_t out[3]);
/* Objeto dinámico con niebla: el motor escala la matriz de color y el ambiente por s=4096-f (a la profundidad de
 * la pieza raíz, screen.t[2]/256). ambient en unidades 0..255. */
void ns_fog_light_object(const NsLight *in, const int16_t ambient[3], int f, NsLight *out);

#ifdef __cplusplus
}
#endif
#endif
