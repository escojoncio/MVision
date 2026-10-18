#pragma once
/* API C para el host (app de ventana plana): hilo de emulación propio a ritmo real,
 * último frame RGBA8, audio de la SPU, mando y pausa. Modelado en headless.c.
 * Solo hay una instancia por proceso (el runtime usa estado global) y no se puede reiniciar. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tamaño máximo del área de display (VRAM 1024x512): un búfer RGBA8 de este tamaño siempre basta. */
enum { MV_HOST_MAX_W = 1024, MV_HOST_MAX_H = 512, MV_HOST_FB_BYTES = 1024 * 512 * 4 };

typedef enum {
    MV_HOST_IDLE = 0,    /* sin arrancar */
    MV_HOST_RUNNING,
    MV_HOST_PAUSED,
    MV_HOST_ERROR        /* error fatal del runtime; mv_host_error() da el texto */
} MvHostState;

/* Carga el ejecutable, monta los discos y arranca el hilo de emulación.
 *  exe      ruta del ejecutable principal (formato de consola).
 *  disc_dir lista separada por ';' de discos: directorio con los ficheros extraídos o imagen (.cue / .bin,
 *           ver disc_img.h). Puede ser NULL/"" (sin disco: error).
 *  card_dir directorio de la tarjeta de memoria (se crea "card0.mcd" si falta); NULL = sin tarjeta.
 * Devuelve 0 si arrancó; <0 si falló (texto en mv_host_error()). */
int mv_host_start(const char *exe, const char *disc_dir, const char *card_dir);

/* Fichero de firmas del SDK (hashes enmascarados, sin código del juego; ver hlebind.c). Solo se usa en el
 * runtime sin código generado (MV_NOGEN, intérprete): llamar antes de mv_host_start; si no, se toma MV_SIGS.
 * Sin fichero legible, mv_host_start devuelve -1. En el runtime recompilado se ignora. */
void mv_host_set_sigs(const char *path);

/* 1 si hay lector de imagen de disco enlazado (siempre; se conserva por compatibilidad). */
int mv_host_has_disc_img(void);

/* Busca discos en "dir" (.cue, .bin sueltos, carpetas con SYSTEM.CNF; imagen preferida sobre carpeta, orden de disco).
 * Escribe en "out" la lista de rutas separada por ';' (formato de mv_host_start). Devuelve cuántos discos (0 = ninguno). */
int mv_host_scan_discs(const char *dir, char *out, size_t cap);

/* Extrae el ejecutable de datos del disco "disc" (.cue/.bin o carpeta) al fichero "outfile". 0 = bien, <0 = error. */
int mv_host_extract_exe(const char *disc, const char *outfile);

MvHostState mv_host_state(void);
/* Último error (castellano, cadena estática o copia interna estable); "" si no hay. */
const char *mv_host_error(void);
/* Frames emulados desde el arranque. */
uint32_t mv_host_frame_count(void);

/* Copia el último frame publicado a dst (RGBA8, filas contiguas w*4 bytes) si es más nuevo que
 * last_seq. Devuelve su número de secuencia (>0) o 0 si no hay frame nuevo o dst_size no basta
 * (usar MV_HOST_FB_BYTES). Thread-safe (doble búfer interno + mutex). */
uint32_t mv_host_framebuffer(uint8_t *dst, size_t dst_size, int *w, int *h, uint32_t last_seq);

/* Extrae `frames` muestras estéreo intercaladas (L,R) int16 a 44100 Hz; rellena con ceros lo que
 * falte (pausa, arranque). Devuelve los frames reales de audio. Llamable desde el hilo de audio. */
int mv_host_audio_pull(int16_t *out_stereo, int frames);

/* Estado del mando 1. buttons activo-alto en orden PS1 (0 SELECT, 1 L3, 2 R3, 3 START, 4 ARRIBA,
 * 5 DERECHA, 6 ABAJO, 7 IZQUIERDA, 8 L2, 9 R2, 10 L1, 11 R1, 12 TRIANGULO, 13 CIRCULO, 14 CRUZ,
 * 15 CUADRADO); ejes 0..255 con centro 128. Se aplica en el siguiente VBlank. */
void mv_host_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
/* Vibración pedida por el juego en el último frame (motor pequeño 0/1, grande 0..255). */
void mv_host_rumble(uint8_t *small, uint8_t *large);

void mv_host_pause(void);
void mv_host_resume(void);
/* Sin limitador de ritmo (avance rápido). */
void mv_host_set_turbo(int on);

/* Pose de la cabeza (originFromDevice, ya recentrada) que publica el host VR cada frame de render.
 * m = 16 floats en columnas; t = segundos. Thread-safe. Lectura: mv_vr_get_head (host_vr.c). */
void mv_vr_set_head(const float m[16], double t);
/* Devuelve 1 si hay pose publicada (copiada en m; t puede ser NULL), 0 si aún no (m intacta). */
int mv_vr_get_head(float m[16], double *t);
/* Olvida la pose (al cerrar el espacio inmersivo): la cámara VR del juego se desactiva. */
void mv_vr_clear_head(void);
/* Giro del espacio de juego del usuario (giro por pasos / suave del mando): grados, + = a la IZQUIERDA. Se acumula y se
 * aplica en el siguiente VBlank. Thread-safe. */
void mv_vr_turn(float deg);
/* Pide recentrar la cámara VR del juego (la posición y mirada actuales de la cabeza pasan a ser el origen). Thread-safe. */
void mv_vr_recenter(void);

#ifdef __cplusplus
}
#endif
