#pragma once
/* Lector de imagen de disco BIN/CUE (MODE2/2352 y MODE1/2352, una o varias pistas/ficheros) y detección
 * del disco por contenido (ISO9660: SYSTEM.CNF + ejecutable de datos), sin cargar la imagen entera.
 *
 * Convenio de LBA: LBA 0 = primer sector del primer fichero de la imagen (en el CD real es el MSF 00:02:00);
 * es el mismo LBA lógico que usa ISO9660 (el PVD está en el 16) y que usa cd_read_sector() en cdrom.c.
 * Con varios ficheros BIN, el LBA continúa: base(fichero N) = suma de sectores de los anteriores.
 * Pendiente (opcional futuro): CHD, comando PREGAP/POSTGAP del .cue, pistas WAVE/MP3, .iso de 2048 B. */
#include <string.h>
#include <stdint.h>
#ifndef EXE_MAGIC
/* Firma de 8 bytes de la cabecera del formato de ejecutable (construida en hex; se valida con memcmp). */
#define EXE_MAGIC "\x50\x53\x2d\x58\x20\x45\x58\x45"
#endif

#define DISC_RAW 2352
#define DISC_USER 2048

#ifndef EXE_MAGIC
/* Firma de 8 bytes de la cabecera del formato de ejecutable (construida en hex; se valida con memcmp). */
#define EXE_MAGIC "\x50\x53\x2d\x58\x20\x45\x58\x45"
#endif

typedef struct DiscImg DiscImg;

/* Abre un .cue (cualquier mayúscula, CRLF/LF, BOM, nombres con espacios, rutas con '\' o relativas;
 * si el fichero no está tal cual se busca sin distinguir mayúsculas y por nombre base junto al .cue)
 * o un .bin suelto (tamaño % 2352 == 0). NULL si falla (mensaje en stderr). */
DiscImg *disc_img_open(const char *path);
void disc_img_close(DiscImg *h);

/* Sectores totales (todas las pistas). */
uint32_t disc_img_sectors(const DiscImg *h);
int disc_img_tracks(const DiscImg *h);
/* Pista n (0-based): LBA de inicio (INDEX 01) y si es de audio. -1 si no existe. */
int disc_img_track(const DiscImg *h, int n, uint32_t *start_lba, int *is_audio);
/* Ficheros BIN referenciados (para saber qué .bin ya pertenece a un .cue). */
int disc_img_nfiles(const DiscImg *h);
const char *disc_img_filepath(const DiscImg *h, int i);

/* Sector bruto de 2352 B tal cual está en la imagen (sync, cabecera MSF/modo, subcabecera XA, datos, EDC/ECC).
 * 0 = bien; -1 = fuera de rango o error de E/S (out queda a cero). */
int disc_img_read_raw(DiscImg *h, uint32_t lba, uint8_t out[DISC_RAW]);
/* Lecturas de sectores dentro de rango que fallaron tras reintentos y reapertura del fichero (0 = sano). Cada fallo
 * también se registra en stderr (los 8 primeros). */
uint32_t disc_img_io_errors(const DiscImg *h);
/* 2048 B de datos de usuario (forma 1 / modo 1); detecta el modo por el byte 15 de la cabecera. */
int disc_img_read_user(DiscImg *h, uint32_t lba, uint8_t out[DISC_USER]);

/* Busca un fichero por ruta ISO ("DIR/FICHERO" o "FICHERO", sin distinguir mayúsculas ni ";1"): LBA y tamaño. 0 = encontrado. */
int disc_img_find(DiscImg *h, const char *path, uint32_t *lba, uint32_t *size);

/* Identidad por contenido de un disco. */
typedef struct {
    char boot[32];      /* ejecutable de arranque de SYSTEM.CNF (BOOT=cdrom:\X;1 -> "X", tal cual está escrito) */
    char datadir[32];   /* primer directorio de la raíz ("" si no hay) */
    char dataexe[32];   /* primer fichero ejecutable del directorio de datos ("" si no hay) */
    int serial;         /* dígitos del nombre de arranque (BOOT_000.42 -> 42), 0 si no hay */
    int number;         /* nº de disco 1..n asignado por disc_scan_dir (0 = sin asignar) */
} DiscId;

/* Lee ISO9660 (PVD lba 16, raíz, SYSTEM.CNF). 0 = bien, -1 = no es un disco legible/sin BOOT. */
int disc_img_identify(DiscImg *h, DiscId *id);
/* Lo mismo sobre una carpeta extraída (SYSTEM.CNF en la raíz). */
int disc_dir_identify(const char *dir, DiscId *id);

typedef struct {
    char path[1024];    /* .cue, .bin o carpeta */
    int is_dir;         /* 1 = carpeta extraída */
    DiscId id;
} DiscEntry;

/* Busca discos en "dir" (y subcarpetas hasta 2 niveles): .cue, .bin sin .cue, carpetas con SYSTEM.CNF.
 * Descarta duplicados del mismo disco (misma identidad; gana la imagen sobre la carpeta), ordena por
 * serial/arranque (el disco 1 lleva el código más bajo) y asigna id.number = 1..n. Devuelve cuántos. */
int disc_scan_dir(const char *dir, DiscEntry out[], int max);

/* Copia el ejecutable de datos del disco (DiscId.datadir/dataexe) a "outfile". "path" = .cue/.bin o carpeta (is_dir).
 * 0 = bien; -1 = el disco no es legible, no tiene ejecutable de datos o falló la escritura. */
int disc_extract_dataexe(const char *path, int is_dir, const char *outfile);

/* Carga en memoria el archivo de stages (el fichero grande del directorio de datos cuyo comienzo es una tabla de entradas
 * nombre[8] + sector: u32 tamaño de tabla y entradas de 12 B) del disco "path" (.cue/.bin o carpeta extraída, is_dir). Abre su
 * PROPIO lector (no toca el del motor): se puede llamar desde otro hilo mientras el juego corre. Devuelve un búfer de malloc
 * (el llamador lo libera con free) y su tamaño en *len; NULL si el disco no es legible o no tiene ese fichero. */
uint8_t *disc_load_stage_archive(const char *path, int is_dir, size_t *len);

/* Solo el nombre final de una ruta: el registro no debe llevar rutas del dispositivo ni de la carpeta elegida. */
static inline const char *mv_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
