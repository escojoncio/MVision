#pragma once
/* Lector de CD virtual a partir de ficheros extraídos + HLE de la librería de CD. */
#include <stdint.h>
#include "cpu.h"

/* Monta un disco: "dir" es la raíz extraída (contiene SYSTEM.CNF y el directorio de datos).
 * Varias llamadas montan disco 1, 2... en orden; con "dir1;dir2" monta ambos de golpe.
 * Devuelve el número de discos montados (0 = error). El primero queda seleccionado. */
int cd_open(const char *dir);
/* Selecciona disco activo (0 = primero). Equivale a cambiar de disco con la tapa: instantáneo. */
int cd_select_disc(int n);
int cd_current_disc(void);
int cd_disc_count(void);
/* Entrega sectores pendientes de una lectura en curso (llamar por frame desde el host). */
void cd_pump(Cpu *c);

/* Acceso directo para herramientas/pruebas. lba = sector lógico (sin los 150 de pregap).
 * raw: 0 = 2048 B de datos, 1 = 2340 B (cabecera 4 + subcabecera 8 + 2328) como en modo 0x20. */
int cd_read_sector(uint32_t lba, uint8_t *out, int raw);
/* Busca un fichero por ruta ("DIR/FICHERO"); devuelve lba y tamaño. */
int cd_find(const char *path, uint32_t *lba, uint32_t *size);
