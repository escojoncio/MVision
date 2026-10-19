/* vr_missile: seguimiento y guiado del misil guiado del juego (arma 3), sin reimplementar el proyectil.
 *
 * Qué hace el juego (edición europea, medido en el intérprete; direcciones en vr_fire_names.json):
 *  - Al soltar el ataque con ese arma crea UN actor de 808 bytes (la función de creación devuelve su dirección; vr_fire la
 *    anota con vrfire_last_missile) que vive unos 50-60 ticks (un tick = 2 VBlanks) hasta impactar o agotar la vida.
 *  - Estado del actor (offsets desde su dirección): posición s16[3] en +0x20 (unidades del motor, 1 = 1 mm), la posición del
 *    tick anterior en +0x108, guiñada (4096 = vuelta) en +0x6E, edad en ticks s16 en +0x118, bandera de "terminado" u8 en
 *    +0x112 (0 mientras vuela) y estado s32 en +0x11C (negativo al explotar). Su vida máxima es de 1000 ticks.
 *  - Guiado: la cruceta del mando fija la guiñada OBJETIVO absoluta del misil (ocho direcciones respecto a la mirada: arriba = recto,
 *    izquierda / derecha = +-90 grados, abajo = media vuelta, diagonales a 45); mientras se mantiene el misil gira hacia ella y al soltar sigue
 *    recto. No se halló control de cabeceo (la altura se mantiene). vrmis_pad convierte el eje de guiado en esa cruceta.
 *  - La cámara del juego durante el vuelo sigue al misil; con la cámara VR activa esa vista no se dibuja (la cabeza manda), así
 *    que la vista del misil se renderiza aparte desde la posición y dirección del actor (pantalla del lanzador).
 * El puntero del actor se recicla al terminar el misil: por eso se vigila la edad (si deja de avanzar, el misil terminó).
 * Todo se llama desde el hilo del emulador. No es reentrante.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VrMissile {
    int      alive;        /* el actor vuela (su edad avanza y no está marcado como terminado) */
    int      ended;        /* 0 vuela / sin misil, 1 explotó o impactó, 2 la edad dejó de avanzar (destruido / puntero reciclado) */
    uint32_t actor;        /* dirección del actor vigilado (0 = ninguno) */
    int32_t  pos[3];       /* unidades del motor */
    float    dir[3];       /* dirección de vuelo unitaria (mundo del motor), de la posición actual y la anterior */
    int      heading;      /* guiñada del juego (0..4095) */
    int      age;          /* edad en ticks */
    uint32_t serial;       /* nº de misil vigilado (sube con cada begin) */
} VrMissile;

void vrmis_reset(void);
/* Un misil nuevo (dirección devuelta por la creación). Sustituye al que hubiera. */
void vrmis_begin(uint32_t actor);
/* Una vez por VBlank tras el paso del juego. Devuelve 1 si hay un misil vigilado (vivo o recién terminado). */
int vrmis_poll(const uint8_t *ram, VrMissile *out);
void vrmis_get(VrMissile *out);

/* Guiado: ejes -1..1 (guiñada + = derecha) -> bits de la cruceta del mando del juego (arriba 0x10, derecha 0x20, abajo 0x40, izquierda
 * 0x80). pad_origin = vrcam_pad_origin() del VBlank (la cruceta se interpreta respecto a la mirada). Una decisión por tick (2 VBlanks).
 * Llamar una vez por VBlank. */
void vrmis_steer(float yaw, float pitch);
uint16_t vrmis_pad(int pad_origin);
/* 1 si el cabeceo tiene efecto en el juego (por ahora no: solo se reserva el contrato). */
int vrmis_pitch_supported(void);

#ifdef __cplusplus
}
#endif
