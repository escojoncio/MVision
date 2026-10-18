/* Modo "sin código generado" (MV_NOGEN): sustituye a build/gen/dispatch.c.
 * No hay funciones recompiladas: toda la CPU pasa por el intérprete (interp.c), las funciones HLE del SDK se
 * enlazan en ejecución por firmas (hlebind.c) y el punto de entrada es el pc de la cabecera del EXE cargado.
 * Este fichero no contiene nada derivado del juego. */
#include "cpu.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
uint32_t rt_entry;   /* lo escribe el cargador del EXE (headless.c, host.c) */
