/* vr_fist: puño cerrado por deformación en tiempo de ejecución de la malla de mano del propio personaje (C99, matemática pura, sin datos del juego).
 *
 * La mano del personaje es UNA malla rígida de "manopla" (dedos juntos en un bloque, pulgar aparte) y el juego no trae variante de puño: se construye
 * doblando los dedos alrededor de la palma a partir de la propia malla, con un esqueleto simple de falanges definido por la geometría:
 *   - se detectan por conectividad de caras la mano principal (muñeca + palma + dedos) y las piezas sueltas (el pulgar);
 *   - el eje de los dedos es -Y local (de la palma a las puntas) y la palma mira a +X (mano derecha del personaje) o -X (izquierda); `side_sign` lo da;
 *   - los nudillos (articulación MCP) se toman de la propia malla (el último anillo de vértices antes de los dedos); el eje central de los dedos se ajusta por
 *     mínimos cuadrados (la manopla ya viene algo inclinada hacia la palma);
 *   - deformación de flexión ("bend") a lo largo de ese eje: la dirección de la punta gira con tres articulaciones suaves (MCP, PIP, DIP) hasta las
 *     direcciones absolutas de `head_deg`; cada vértice conserva su desplazamiento lateral respecto al eje (comprimido por el lado interior) y su z;
 *   - el pulgar es una cadena de 2 articulaciones (base con eje inclinado 45 grados respecto al de los dedos, y punta) que se cierra POR FUERA de los dedos doblados, apoyado sobre las falanges
 *     medias del índice y el corazón; los ángulos se resuelven para que la punta llegue ahí y el agarre los escala;
 *   - las normales giran con el vértice al que pertenecen (el que las usa en las caras); las UV y las caras no se tocan: mismo color y textura.
 * grip 0 devuelve la malla de entrada tal cual; 1 = puño cerrado. Sin estado (se puede llamar desde cualquier hilo). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRFIST_MAX_V 128   /* tope de vértices/normales de la malla de mano que se acepta */
#define VRFIST_MAX_F 128

typedef struct {
    int nv, nn, nf;
    const int16_t *v;        /* nv x 4 (x, y, z, relleno) */
    const int16_t *n;        /* nn x 4 (x, y, z, relleno), 4096 = 1 */
    const uint8_t *vidx;     /* nf x 4 índices de vértice por cara (bit 7 = marca: se ignora) */
    const uint8_t *nidx;     /* nf x 4 índices de normal por cara (bit 7 = marca: se ignora) */
} VrFistMesh;

typedef struct {
    float head_deg[3];       /* dirección absoluta de los dedos tras MCP, PIP y DIP con el puño cerrado (0 = a lo largo del eje, 90 = hacia la palma, 180 = hacia la muñeca) */
    float joint2, joint3;    /* posición de PIP y DIP a lo largo de los dedos (fracción de su longitud desde los nudillos) */
    float soft;              /* semiancho de cada articulación (fracción de la longitud de los dedos) */
    float inner_k, outer_k;  /* escala del desplazamiento lateral con el puño cerrado: lado de la palma / lado del dorso */
    float thumb_tilt_deg;    /* inclinación del eje de la articulación de la base del pulgar respecto al eje de flexión de los dedos (45 grados, hacia el lado de los dedos) */
    float thumb_joint;       /* posición de la articulación de la punta a lo largo del pulgar (fracción desde la base, 0,5) */
    float thumb_soft;        /* semiancho de la mezcla de la articulación de la punta (fracción de la longitud del pulgar, 0,12) */
    float thumb_base_deg, thumb_tip_deg;   /* tope de giro de la base (110) y de la punta (80) con el puño cerrado */
} VrFistParams;

void vrfist_params_default(VrFistParams *p);

/* Deforma la malla. side_sign: +1 la palma mira a +X local (mano derecha del personaje), -1 a -X (izquierda). grip 0..1.
 * out_v (nv x 4) y out_n (nn x 4) reciben la malla deformada (el relleno se copia). 0 = bien; < 0 = malla no válida o no reconocible
 * (out sin definir; el llamador no deforma). */
int vrfist_deform(const VrFistMesh *m, float side_sign, float grip, const VrFistParams *p, int16_t *out_v, int16_t *out_n);

#ifdef __cplusplus
}
#endif
