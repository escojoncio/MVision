#pragma once
/* ns_dump: volcado de un fotograma de juego para desarrollar el renderer nativo SIN ejecutar el juego.
 *
 * Dos mitades:
 *   - ESCRITOR (ns_dumpw.c): entra en la app. Lo dispara el usuario jugando en el visor (mantener los dos clics de stick ~1 s) y escribe un directorio
 *     en <carpeta del usuario>/Dumps/. No depende del runtime salvo gpu.h (el tipo GpuDrawList).
 *   - CARGADOR (ns_dump.c): SOLO herramientas y pruebas de escritorio (no está en la lista de app/stage.sh). Lee ese directorio y da acceso a la RAM
 *     con direcciones del motor para el extractor, el rasterizador de referencia y las pruebas.
 *
 * Formato de un volcado (directorio `d<NNNN>_f<fotograma>/`, todo little-endian, sin compresión salvo el PNG):
 *   ram.bin      2 097 152 B   RAM completa del motor (dirección 0x80000000 + offset). Instantánea tomada en el gancho de fin de fotograma, con la CPU parada.
 *   scratch.bin  1 024 B       memoria rápida de 1 KiB: restos de los cálculos del último trabajo (no hace falta para extraer la escena).
 *   vram.bin     1 048 576 B   VRAM 1024x512 de 16 bits (fila-mayor) en ese mismo instante: texturas, paletas y el fotograma mostrado.
 *   gte.bin      256 B         registros del coprocesador de geometría: 32 palabras de datos + 32 de control (en ese orden).
 *   list.bin     variable      la ÚLTIMA lista de dibujo de la GPU construida (puede faltar si no había ninguna) (primitivas capturadas con posición de vista): cabecera + GpuOp[] + píxeles de subidas:
 *                              char magic[4]="NSL1"; u32 sizeof(GpuOp); u32 nops; u32 ndata; u32 index; u32 host_frame; u32 reason; u32 sizeof(GpuDisplay);
 *                              GpuDisplay disp; GpuOp ops[nops]; u16 data[ndata]. Layout de la compilación del productor (se comprueba el tamaño). Puede faltar (entonces meta lleva list_omitted=1 si era demasiado grande).
 *   frame.png    PNG RGBA     el fotograma REAL mostrado (lo que muestra la salida de vídeo); referencia para comparar.
 *   meta.txt     texto        líneas `clave=valor`: format=NSD1, parts_failed=N, y la cabecera de contexto (fotograma, zona, jugador, banderas del puente, cámara del
 *                             motor, pose de la cabeza del usuario y registros de cámara VR) — ver ns_dump_meta().
 *
 * Claves de meta.txt (las que no aplican faltan): format, parts_failed (partes accesorias que no se pudieron escribir), dump_index, game_frame, vblank, list_index, list_omitted,
 *   area_id (número), game, in_control, posture, view_mode, player_x/y/z, eye_x/y/z (cámara del motor, mundo), cam_h (distancia de pantalla), cam_rot (9 enteros vista->mundo),
 *   rec_source (list = registro de cámara de la lista volcada, el que fijó la vista de sus polígonos; last = el último del hilo de emulación si no había lista),
 *   rec_active, rec_eye, rec_M, rec_screen, rec_yaw, rec_pitch, rec_theta, rec_units_per_m, rec_p_ref, rec_head (16 floats)  -> registro de cámara según rec_source;
 *   last_rec_* (mismas claves) -> el último registro del hilo de emulación, sólo si difiere del de la lista;
 *   head (16 floats, columnas, espacio del usuario), head_age.
 * El volcado NO lleva el ejecutable ni el disco; la RAM sí contiene datos del juego: por eso queda en la carpeta del usuario y nunca debe subirse a un repositorio.
 */
#include <stddef.h>
#include <stdint.h>
#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_DUMP_RAM_SIZE   0x200000u
#define NS_DUMP_VRAM_BYTES (1024u * 512u * 2u)

/* ------------------------------------------------------------------------------------------------ escritor (app) */
typedef struct {
    const uint8_t *ram;        /* NS_DUMP_RAM_SIZE */
    const uint8_t *scratch;    /* 1024 o NULL */
    const uint16_t *vram;      /* 1024*512 o NULL */
    const uint32_t *gte;       /* 64 palabras (datos 0..31, control 0..31) o NULL */
    const GpuDrawList *list;   /* o NULL */
    const uint8_t *rgba;       /* fotograma real RGBA8 fw*h o NULL */
    int fw, fh;
    const char *meta;          /* líneas `clave=valor\n` (se antepone format=NSD1) o NULL */
} NsDumpSrc;

/* Escribe el volcado de forma ATÓMICA: primero en `<dir>.tmp` (el padre debe existir) y al final `rename` a `dir`; el directorio final aparece completo o no aparece.
 * Devuelve 0 si todo fue bien, N > 0 si el volcado es válido pero fallaron N partes accesorias (también queda en meta: parts_failed), -1 si falló lo esencial (RAM,
 * la creación o el rename). No toca estado global: puede llamarse desde cualquier hilo con memoria estable. */
int ns_dump_write(const char *dir, const NsDumpSrc *src);

/* Codificador PNG RGBA8 sin compresión (bloques "stored"), sin dependencias. Devuelve 0 o -1. */
int ns_png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);

/* ------------------------------------------------------------------------------------------------ cargador (solo escritorio) */
typedef struct NsDump {
    uint8_t *ram;              /* 2 MiB */
    uint8_t scratch[1024];
    uint16_t *vram;            /* 1024*512, NULL si falta */
    uint32_t gte[64];
    int has_gte;
    GpuDrawList list;          /* ops/data propios; list.nops == 0 si falta list.bin */
    int has_list;
    char *meta;                /* texto crudo de meta.txt */
    char dir[512];
} NsDump;

NsDump *ns_dump_open(const char *dir);       /* NULL si falta ram.bin o es corrupto */
void ns_dump_close(NsDump *d);
const char *ns_dump_meta(const NsDump *d, const char *key, char *buf, size_t cap);   /* valor o NULL */
double ns_dump_meta_num(const NsDump *d, const char *key, double dflt);
int ns_dump_meta_floats(const NsDump *d, const char *key, float *out, int n);         /* nº leído */

/* Lectura de RAM con dirección del motor (segmentos de usuario, cacheado y no cacheado; 2 MiB con espejos); fuera de rango devuelve 0 y marca ns_dump_oob. */
uint32_t ns_r32(const NsDump *d, uint32_t addr);
uint16_t ns_r16(const NsDump *d, uint32_t addr);
int16_t  ns_s16(const NsDump *d, uint32_t addr);
int32_t  ns_s32(const NsDump *d, uint32_t addr);
uint8_t  ns_r8(const NsDump *d, uint32_t addr);
int ns_ptr_ok(uint32_t addr);                /* puntero RAM válido del motor: dentro de la RAM (o espejos) y fuera de los primeros 64 KiB (0 y enteros pequeños no son punteros) */
extern unsigned long ns_dump_oob;            /* lecturas fuera de rango acumuladas */

#ifdef __cplusplus
}
#endif
