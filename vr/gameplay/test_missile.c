/* test_missile: seguimiento y guiado del misil guiado (vr_missile) con RAM sintética. Sin datos del juego.
 *   cc -I. test_missile.c vr_missile.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_missile.h"

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("FALLO: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

static uint8_t *ram;
#define ACTOR 0x801701B8u
static void w16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(ram + (a & 0x1FFFFF), &x, 2); }
static void w32(uint32_t a, int32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static void w8(uint32_t a, int v) { ram[a & 0x1FFFFF] = (uint8_t)v; }

/* un tick del juego: avanza la posición dx,dz y la edad */
static void tick(int *x, int *y, int *z, int dx, int dy, int dz, int *age) {
    w16(ACTOR + 0x108, *x); w16(ACTOR + 0x10A, *y); w16(ACTOR + 0x10C, *z);
    *x += dx; *y += dy; *z += dz; (*age)++;
    w16(ACTOR + 0x20, *x); w16(ACTOR + 0x22, *y); w16(ACTOR + 0x24, *z); w16(ACTOR + 0x118, *age);
}

int main(void) {
    ram = calloc(1, 0x200000);
    VrMissile m;
    vrmis_reset();
    CHECK(vrmis_poll(ram, &m) == 0 && !m.alive, "sin misil");
    vrmis_begin(0x00001000u);                          /* puntero fuera de RAM del juego */
    CHECK(vrmis_poll(ram, &m) == 0, "puntero invalido ignorado");
    vrmis_begin(ACTOR);
    CHECK(m.serial == 0 || vrmis_poll(ram, &m) == 1, "vigila el actor");
    vrmis_poll(ram, &m);
    CHECK(m.actor == ACTOR && m.serial == 2, "serial por begin");

    int x = -5312, y = 959, z = -1321, age = 0;
    w16(ACTOR + 0x6E, 1024);
    /* vuelo: 2 VBlanks por tick, +20 en X */
    int vb = 0;
    for (int t = 0; t < 30; t++) {
        tick(&x, &y, &z, 20, 0, 0, &age);
        for (int k = 0; k < 2; k++, vb++) vrmis_poll(ram, &m);
    }
    CHECK(m.alive && m.ended == 0, "vivo mientras la edad avanza");
    CHECK(m.pos[0] == x && m.pos[1] == y && m.pos[2] == z, "posicion del actor");
    CHECK(fabsf(m.dir[0] - 1.0f) < 1e-3f && fabsf(m.dir[1]) < 1e-3f, "direccion de vuelo desde el desplazamiento");
    CHECK(m.heading == 1024 && m.age == 30, "guinada y edad");
    CHECK(m.dy == 0 && m.done == 0 && m.state == 0, "diagnóstico: paso vertical, bandera y estado del actor (log [nikita])");
    tick(&x, &y, &z, 0, 250, 0, &age); vrmis_poll(ram, &m);
    CHECK(m.dy == 250 && m.alive, "paso vertical del último tick (pos.y - previa) = 250: el juego lo explota con >= 200");
    w32(ACTOR + 0x11C, -2); w8(ACTOR + 0x112, 1); vrmis_poll(ram, &m);
    CHECK(m.dy == 250 && m.done == 1 && m.state == -2 && m.ended == 1, "bandera y estado negativo en el informe de la explosión");
    w32(ACTOR + 0x11C, 0); w8(ACTOR + 0x112, 0); vrmis_begin(ACTOR); w16(ACTOR + 0x118, 0); age = 0; for (int k = 0; k < 14; k++) { tick(&x, &y, &z, 20, 0, 0, &age); vrmis_poll(ram, &m); }
    CHECK(m.alive, "(restablece el misil de prueba)");

    /* parado: deja de avanzar la edad -> terminado (2) tras 12 VBlanks */
    int n = 0;
    while (m.ended == 0 && n < 40) { vrmis_poll(ram, &m); n++; }
    CHECK(m.ended == 2 && !m.alive && n >= 10 && n <= 14, "edad parada = terminado");
    vrmis_poll(ram, &m);
    CHECK(m.ended == 2, "el final es definitivo hasta el siguiente begin");

    /* puntero reciclado por otro actor: la edad retrocede -> terminado (2) al instante */
    vrmis_begin(ACTOR); x = 0; y = 0; z = 0; age = 0;
    w8(ACTOR + 0x112, 0); w32(ACTOR + 0x11C, 0);
    for (int t = 0; t < 6; t++) { tick(&x, &y, &z, 0, 0, 30, &age); vrmis_poll(ram, &m); vrmis_poll(ram, &m); }
    CHECK(m.alive, "vivo antes del reciclado");
    w16(ACTOR + 0x118, 1);
    vrmis_poll(ram, &m);
    CHECK(m.ended == 2 && !m.alive, "edad que retrocede = puntero reciclado");

    /* explosion por bandera */
    vrmis_begin(ACTOR); x = 0; y = 0; z = 0; age = 0;
    for (int t = 0; t < 5; t++) { tick(&x, &y, &z, 0, 0, 30, &age); vrmis_poll(ram, &m); vrmis_poll(ram, &m); }
    CHECK(m.alive && fabsf(m.dir[2] - 1.0f) < 1e-3f, "vuelo hacia +Z");
    w8(ACTOR + 0x112, 1);
    vrmis_poll(ram, &m);
    CHECK(m.ended == 1 && !m.alive, "bandera de terminado = explosion");
    /* explosion por estado negativo */
    vrmis_begin(ACTOR); w8(ACTOR + 0x112, 0); age = 0; w32(ACTOR + 0x11C, 0);
    tick(&x, &y, &z, 0, 0, 30, &age); vrmis_poll(ram, &m);
    CHECK(m.alive, "otro misil vuela");
    w32(ACTOR + 0x11C, -2);
    vrmis_poll(ram, &m);
    CHECK(m.ended == 1, "estado negativo = explosion");

    /* sin movimiento aun: direccion por la guinada */
    vrmis_begin(ACTOR); w32(ACTOR + 0x11C, 0); w8(ACTOR + 0x112, 0);
    w16(ACTOR + 0x108, 100); w16(ACTOR + 0x10A, 100); w16(ACTOR + 0x10C, 100);
    w16(ACTOR + 0x20, 100); w16(ACTOR + 0x22, 100); w16(ACTOR + 0x24, 100); w16(ACTOR + 0x118, 1); w16(ACTOR + 0x6E, 2048);
    vrmis_poll(ram, &m);
    CHECK(fabsf(m.dir[2] + 1.0f) < 1e-3f, "guinada 2048 = -Z");

    /* guiado: el misil vuela hacia +X (1024); la cruceta se interpreta respecto al origen del mando (3072: arriba = 3072 + 2048 = 1024) */
    vrmis_begin(ACTOR); w32(ACTOR + 0x11C, 0); w8(ACTOR + 0x112, 0); x = 0; y = 0; z = 0; age = 5; w16(ACTOR + 0x118, age);
    for (int t = 0; t < 4; t++) { tick(&x, &y, &z, 30, 0, 0, &age); vrmis_poll(ram, &m); vrmis_poll(ram, &m); }
    CHECK(m.alive && fabsf(m.dir[0] - 1.0f) < 1e-3f, "vuela hacia +X");
    struct { float yaw; uint16_t bits; } cases[] = { { 1.0f, 0x20 }, { -1.0f, 0x80 }, { 0.5f, 0x30 }, { -0.5f, 0x90 }, { 0.0f, 0 }, { 0.05f, 0 }, { -0.05f, 0 } };
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        vrmis_steer(cases[c].yaw, 0.0f);
        int bad = 0, got = 0;
        for (int v = 0; v < 20; v++) {
            if (v % 2 == 0) tick(&x, &y, &z, 30, 0, 0, &age);
            vrmis_poll(ram, &m);
            uint16_t p = vrmis_pad(3072);
            if (p != cases[c].bits) bad++; else got++;
        }
        CHECK(!bad, "cruceta segun el eje (derecha 0x20, izquierda 0x80, diagonales a medio eje, nada con eje ~0)");
        CHECK(got == 20, "decision estable");
    }
    /* otro origen de mando: la cruceta cambia pero el rumbo pedido es el mismo; con origen 1024 (arriba = 3072) derecha = 3072-1024 = 2048 -> pedir 0 = abajo(+2048)? */
    vrmis_steer(1.0f, 0.0f);
    { uint16_t p = 0; for (int v = 0; v < 4; v++) { if (v % 2 == 0) tick(&x, &y, &z, 30, 0, 0, &age); vrmis_poll(ram, &m); p = vrmis_pad(1024); }
      /* origen 1024: up = 3072, right = 2048, down = 1024 (= recto), left = 0; se quiere 0 -> izquierda */
      CHECK(p == 0x80, "el origen del mando rota la cruceta"); }
    /* con rumbo igual a la cruceta pedida no pulsa: recto = abajo con origen 1024 */
    vrmis_steer(0.0f, 0.0f);
    { uint16_t p = 1; for (int v = 0; v < 4; v++) { if (v % 2 == 0) tick(&x, &y, &z, 30, 0, 0, &age); vrmis_poll(ram, &m); p = vrmis_pad(1024); } CHECK(p == 0, "sin eje no pulsa"); }
    /* los pulsos duran un tick (2 VBlanks) */
    vrmis_steer(1.0f, 0.0f);
    { int run = 0, maxrun = 0, last = 0;
      for (int v = 0; v < 30; v++) { if (v % 2 == 0) tick(&x, &y, &z, 30, 0, 0, &age); vrmis_poll(ram, &m); uint16_t p = vrmis_pad(3072); if (p == last) run++; else { run = 1; last = p; } if (run > maxrun) maxrun = run; }
      CHECK(maxrun >= 2, "la decision dura 2 VBlanks"); }
    /* el cabeceo no tiene efecto en el juego: nunca pulsa arriba/abajo solo por pedir cabeceo */
    vrmis_steer(0.0f, 1.0f);
    int vert = 0;
    for (int v = 0; v < 60; v++) { if (v % 2 == 0) tick(&x, &y, &z, 30, 0, 0, &age); vrmis_poll(ram, &m); if (vrmis_pad(3072)) vert++; }
    CHECK(vert == 0 && !vrmis_pitch_supported(), "cabeceo reservado");
    /* sin misil vivo no pulsa nada, aunque se pida */
    vrmis_steer(1.0f, 0.0f); w32(ACTOR + 0x11C, -1); vrmis_poll(ram, &m);
    int any = 0;
    for (int v = 0; v < 10; v++) any |= vrmis_pad(3072);
    CHECK(any == 0, "sin misil no hay pulsos");
    vrmis_steer(NAN, INFINITY);
    for (int v = 0; v < 10; v++) any |= vrmis_pad(3072);
    CHECK(any == 0, "NaN/inf no pulsan");

    free(ram);
    printf("test_missile: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
