/* Disco virtual + HLE de la librería de CD.
 *
 * El juego no usa CdSearchFile: su propio sistema de ficheros lee el descriptor primario ISO9660
 * (sector 16), la tabla de rutas y el directorio de datos, compara los nombres con su tabla interna
 * y guarda el LBA (+150) de cada fichero; el número de disco se deduce de qué ejecutable aparece en
 * ese directorio. Por eso aquí se sintetiza un ISO9660 mínimo pero coherente a partir de los ficheros
 * extraídos, en el orden del layout del volcado: cualquier LBA "base + offset" cae en el fichero bueno.
 *
 * Lecturas: el demonio de E/S del juego hace Setmode(0xA0: doble velocidad, sector 2340), Setloc,
 * ReadN y en el callback de "dato listo" saca 3 palabras de cabecera + hasta 512 palabras con
 * CdGetSector. Aquí ReadN entrega sectores de forma síncrona invocando ese callback hasta que el
 * juego pausa (fin de fichero) o se agota el cupo por llamada; el resto lo entrega cd_pump(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include "cpu.h"
#include "hle.h"
#include "cdrom.h"

#define V0 c->r[2]
#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]

#define SEC 2048
#define XA_SEC 2336
#define MAX_DISCS 2
#define MAX_FILES 16
#define FIRST_FILE_LBA 24
#define PREGAP 150

typedef struct {
    char name[16];      /* nombre ISO sin ";1" */
    int in_sub;         /* 1 = dentro del directorio de datos */
    int xa;             /* 1 = modo mixto (sectores de 2336 B en el fichero) */
    uint32_t lba, size, nsec;
    FILE *f;            /* NULL = relleno */
} VFile;

typedef struct {
    char root[1024];
    char subname[16];
    VFile files[MAX_FILES];
    int nfiles;
    uint32_t total;     /* sectores */
    uint8_t sys[8][SEC];/* sectores 16..23 sintetizados */
} Disc;

static Disc discs[MAX_DISCS];
static int ndiscs, cur;

/* ---------------- construcción del ISO ---------------- */

static void put16b(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 8; p[3] = v; }
static void put32b(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    p[4] = v >> 24; p[5] = v >> 16; p[6] = v >> 8; p[7] = v;
}
static void put16l(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32l(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put16m(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32m(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (24 - 8 * i); }

/* Registro de directorio ISO9660; devuelve su longitud. */
static int dirrec(uint8_t *p, const char *id, int idlen, uint32_t lba, uint32_t size, int isdir, int xa) {
    int len = 33 + idlen;
    if (len & 1) len++;
    int sys = 14; /* extensión XA */
    len += sys;
    memset(p, 0, len);
    p[0] = len;
    put32b(p + 2, lba);
    put32b(p + 10, size);
    p[18] = 99; p[19] = 1; p[20] = 1; /* fecha 1999-01-01 */
    p[25] = isdir ? 2 : 0;
    put16b(p + 28, 1);
    p[32] = idlen;
    memcpy(p + 33, id, idlen);
    uint8_t *x = p + len - sys;
    uint16_t attr = isdir ? 0x8D55 : xa ? 0x3D55 : 0x0D55;
    x[4] = attr >> 8; x[5] = attr; x[6] = 'X'; x[7] = 'A';
    return len;
}

static int add_file(Disc *d, const char *dirpath, const char *name, int in_sub, uint32_t *next) {
    if (d->nfiles == MAX_FILES) return -1;
    VFile *v = &d->files[d->nfiles];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->in_sub = in_sub;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dirpath, name);
    v->f = fopen(path, "rb");
    if (v->f) {
        fseek(v->f, 0, SEEK_END);
        long sz = ftell(v->f);
        v->xa = strstr(name, ".STR") && sz % XA_SEC == 0 && sz % SEC != 0;
        v->nsec = v->xa ? sz / XA_SEC : (uint32_t)((sz + SEC - 1) / SEC);
        v->size = v->xa ? v->nsec * SEC : (uint32_t)sz;
    } else {
        /* relleno de 3 MB al final del disco */
        v->nsec = 1536;
        v->size = v->nsec * SEC;
    }
    v->lba = *next;
    *next += v->nsec;
    d->nfiles++;
    return 0;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Lista de ficheros regulares de un directorio, ordenada (con orden fijo conocido delante). */
static int list_dir(const char *path, char out[][16], int max) {
    DIR *dp = opendir(path);
    if (!dp) return 0;
    char *names[64]; int n = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < 64) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 16) continue;
        char p[2048]; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        DIR *isd = opendir(p);
        if (isd) { closedir(isd); continue; }
        names[n++] = strdup(e->d_name);
    }
    closedir(dp);
    qsort(names, n, sizeof *names, cmp_name);
    int k = 0;
    for (int i = 0; i < n; i++) { if (k < max) snprintf(out[k++], 16, "%s", names[i]); free(names[i]); }
    return k;
}

static int find_subdir(const char *root, char *out) {
    DIR *dp = opendir(root);
    if (!dp) return -1;
    struct dirent *e;
    int ok = -1;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char p[2048]; snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        DIR *s = opendir(p);
        if (s) { closedir(s); snprintf(out, 16, "%s", e->d_name); ok = 0; break; }
    }
    closedir(dp);
    return ok;
}

static int build_disc(Disc *d, const char *root) {
    memset(d, 0, sizeof *d);
    snprintf(d->root, sizeof d->root, "%s", root);
    if (find_subdir(root, d->subname)) { fprintf(stderr, "[cd] %s: sin directorio de datos\n", root); return -1; }
    uint32_t next = FIRST_FILE_LBA;
    /* Raíz: SYSTEM.CNF y ejecutable de arranque primero (orden del layout), resto alfabético. */
    char rootf[16][16]; int nr = list_dir(root, rootf, 16);
    const char *first = "SYSTEM.CNF";
    add_file(d, root, first, 0, &next);
    for (int i = 0; i < nr; i++) {
        if (!strcmp(rootf[i], first) || strstr(rootf[i], ".dat") || strstr(rootf[i], ".xml")) continue;
        add_file(d, root, rootf[i], 0, &next);
    }
    char sub[2048]; snprintf(sub, sizeof sub, "%s/%s", root, d->subname);
    char subf[16][16]; int ns = list_dir(sub, subf, 16);
    for (int i = 0; i < ns; i++) add_file(d, sub, subf[i], 1, &next);
    add_file(d, root, "FILLER.DAT", 0, &next); /* no extraído: relleno */
    d->total = next + 150;

    /* 16: descriptor primario. */
    uint8_t *pvd = d->sys[0];
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    memset(pvd + 8, ' ', 32); memcpy(pvd + 8, "PLAYSTATION", 11);
    memset(pvd + 40, ' ', 32); memcpy(pvd + 40, "VDISC", 5);
    put32b(pvd + 80, d->total);
    put16b(pvd + 120, 1); put16b(pvd + 124, 1); put16b(pvd + 128, SEC);
    uint32_t ptsize = 10 + 10 + ((strlen(d->subname) + 1) & ~1u);
    put32b(pvd + 132, ptsize);
    put32l(pvd + 140, 18); put32m(pvd + 148, 19);
    dirrec(pvd + 156, "\0", 1, 22, SEC, 1, 0);
    pvd[881] = 1;
    memcpy(pvd + 1024, "CD-XA001", 8);
    /* 17: terminador. */
    d->sys[1][0] = 255; memcpy(d->sys[1] + 1, "CD001", 5); d->sys[1][6] = 1;
    /* 18/19: tablas de rutas L y M. */
    for (int m = 0; m < 2; m++) {
        uint8_t *p = d->sys[2 + m];
        p[0] = 1; (m ? put32m : put32l)(p + 2, 22); (m ? put16m : put16l)(p + 6, 1);
        p += 10;
        int l = strlen(d->subname);
        p[0] = l; (m ? put32m : put32l)(p + 2, 23); (m ? put16m : put16l)(p + 6, 1);
        memcpy(p + 8, d->subname, l);
    }
    /* 22: raíz, 23: directorio de datos. */
    for (int k = 0; k < 2; k++) {
        uint8_t *p = d->sys[6 + k];
        int o = 0;
        o += dirrec(p + o, "\0", 1, 22 + k, SEC, 1, 0);
        o += dirrec(p + o, "\1", 1, 22, SEC, 1, 0);
        if (k == 0) {
            char id[20];
            for (int i = 0; i < d->nfiles; i++) {
                VFile *v = &d->files[i];
                if (v->in_sub) continue;
                if (!strcmp(v->name, "FILLER.DAT") && o) { /* se lista igual */ }
                int n = snprintf(id, sizeof id, "%s;1", v->name);
                o += dirrec(p + o, id, n, v->lba, v->size, 0, v->xa);
            }
            o += dirrec(p + o, d->subname, strlen(d->subname), 23, SEC, 1, 0);
        } else {
            char id[20];
            for (int i = 0; i < d->nfiles; i++) {
                VFile *v = &d->files[i];
                if (!v->in_sub) continue;
                int n = snprintf(id, sizeof id, "%s;1", v->name);
                o += dirrec(p + o, id, n, v->lba, v->size, 0, v->xa);
            }
        }
    }
    return 0;
}

static int open_one(const char *dir) {
    if (ndiscs == MAX_DISCS) return -1;
    if (build_disc(&discs[ndiscs], dir)) return -1;
    Disc *d = &discs[ndiscs];
    fprintf(stderr, "[cd] disco %d: %s (%d ficheros, %u sectores)\n", ndiscs + 1, dir, d->nfiles, d->total);
    ndiscs++;
    return 0;
}

int cd_open(const char *dir) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *s = strtok(tmp, ";"); s; s = strtok(NULL, ";")) open_one(s);
    return ndiscs;
}

int cd_select_disc(int n) {
    if (n < 0 || n >= ndiscs) return -1;
    if (n != cur) fprintf(stderr, "[cd] cambio a disco %d\n", n + 1);
    cur = n;
    return 0;
}
int cd_current_disc(void) { return cur; }
int cd_disc_count(void) { return ndiscs; }

int cd_find(const char *path, uint32_t *lba, uint32_t *size) {
    if (!ndiscs) return -1;
    Disc *d = &discs[cur];
    const char *slash = strchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    for (int i = 0; i < d->nfiles; i++)
        if (!strcasecmp(d->files[i].name, name) && d->files[i].in_sub == (slash != NULL)) {
            *lba = d->files[i].lba; *size = d->files[i].size; return 0;
        }
    return -1;
}

static void bcd_msf(uint32_t abs, uint8_t *o) {
    uint32_t m = abs / 4500, s = abs / 75 % 60, f = abs % 75;
    o[0] = (m / 10) << 4 | m % 10; o[1] = (s / 10) << 4 | s % 10; o[2] = (f / 10) << 4 | f % 10;
}

int cd_read_sector(uint32_t lba, uint8_t *out, int raw) {
    uint8_t *data = raw ? out + 12 : out;
    memset(out, 0, raw ? 2340 : SEC);
    if (raw) { bcd_msf(lba + PREGAP, out); out[3] = 2; out[6] = out[10] = 8; }
    if (!ndiscs) return -1;
    Disc *d = &discs[cur];
    if (lba >= 16 && lba < 24) { memcpy(data, d->sys[lba - 16], SEC); if (raw && lba == 17) out[6] = out[10] = 0x89; return 0; }
    for (int i = 0; i < d->nfiles; i++) {
        VFile *v = &d->files[i];
        if (lba < v->lba || lba >= v->lba + v->nsec) continue;
        uint32_t k = lba - v->lba;
        if (!v->f) return 0;
        if (v->xa) {
            uint8_t b[XA_SEC];
            fseek(v->f, (long)k * XA_SEC, SEEK_SET);
            if (fread(b, 1, XA_SEC, v->f) != XA_SEC) return -1;
            if (raw) memcpy(out + 4, b, XA_SEC);
            else memcpy(out, b + 8, SEC);
        } else {
            fseek(v->f, (long)k * SEC, SEEK_SET);
            size_t n = fread(data, 1, SEC, v->f);
            (void)n;
            if (raw && k == v->nsec - 1) out[6] = out[10] = 0x89; /* fin de fichero */
        }
        return 0;
    }
    return 0; /* hueco: ceros */
}

/* ---------------- estado de la unidad ---------------- */

static uint32_t ready_cb, sync_cb;
static uint32_t pos;         /* LBA lógico de Setloc / lectura */
static int reading;          /* 1 = ReadN/ReadS activo */
static int in_delivery;
static uint8_t mode = 0x80;
static uint8_t secbuf[2340];
static uint32_t secoff;      /* bytes ya entregados por CdGetSector */
static uint8_t last_result[8];

/* Heurística de cambio automático: si el juego vuelve a escanear el directorio raíz/de datos
 * (lo hace al comprobar el disco tras un "DISK %d" equivocado) sin leer nada más, se conmuta. */
static int rescans;

static void invoke2(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!fn) return;
    Cpu saved = *c;
    c->r[4] = a0; c->r[5] = a1;
    c->r[29] = (c->r[29] - 0x400) & ~7u;
    rt_call(c, fn);
    *c = saved;
}

#define RESULT_ADDR 0x00000F00u /* resultado de 8 B en RAM baja libre para los callbacks */

static void stat_result(uint8_t *r) { memset(r, 0, 8); r[0] = 0x02 | (reading ? 0x20 : 0); }

static void deliver(Cpu *c, int max) {
    if (in_delivery) return;
    in_delivery = 1;
    while (reading && max-- > 0) {
        cd_read_sector(pos, secbuf, 1);
        secoff = (mode & 0x20) ? 0 : 12;
        if (!(mode & 0x20) && (mode & 0x10)) secoff = 12; /* tamaño 2328: no usado */
        pos++;
        stat_result(last_result);
        for (int i = 0; i < 8; i++) mem_w8(c, RESULT_ADDR + i, last_result[i]);
        if (!ready_cb) continue;
        invoke2(c, ready_cb, 1, RESULT_ADDR);
    }
    in_delivery = 0;
}

void cd_pump(Cpu *c) { deliver(c, 16); }

static uint32_t msf_to_lba(uint32_t p) {
    uint8_t m = p, s = p >> 8, f = p >> 16;
    uint32_t abs = ((m >> 4) * 10 + (m & 15)) * 4500 + ((s >> 4) * 10 + (s & 15)) * 75 + (f >> 4) * 10 + (f & 15);
    return abs >= PREGAP ? abs - PREGAP : 0;
}

static int control(Cpu *c, uint32_t com, uint32_t param, uint32_t result) {
    uint8_t r[8];
    com &= 0xFF;
    switch (com) {
    case 0x02: /* Setloc */
        if (param) { uint32_t p = mem_r32(c, param) & 0xFFFFFF; pos = msf_to_lba(p); }
        break;
    case 0x06: case 0x1B: /* ReadN / ReadS */
        if (param) { uint32_t p = mem_r32(c, param) & 0xFFFFFF; pos = msf_to_lba(p); }
        if (pos == 22 || pos == 23) {
            if (++rescans > 3 && ndiscs > 1) { cd_select_disc(cur ^ 1); rescans = 0; }
        } else if (pos > 23) rescans = 0;
        reading = 1;
        break;
    case 0x09: case 0x08: case 0x0A: /* Pause / Stop / Init */
        reading = 0;
        break;
    case 0x0E: /* Setmode */
        if (param) mode = mem_r8(c, param);
        break;
    default: break; /* Nop, Mute, Demute, Setfilter, Play, Seek...: sin efecto */
    }
    stat_result(r);
    if (com == 0x13) { r[1] = 1; r[2] = 1; }               /* GetTN */
    if (com == 0x14) { bcd_msf(discs[cur].total, r + 1); }/* GetTD (aprox.) */
    if (com == 0x10 || com == 0x11) bcd_msf(pos + PREGAP, r);
    if (result) for (int i = 0; i < 8; i++) mem_w8(c, result + i, r[i]);
    /* Un comando completa al momento: CdSyncCallback(CdlComplete). */
    if (sync_cb && !in_delivery) {
        for (int i = 0; i < 8; i++) mem_w8(c, RESULT_ADDR + i, r[i]);
        invoke2(c, sync_cb, 2, RESULT_ADDR);
    }
    return 1;
}

/* ---------------- HLE ---------------- */

void hle_CdInit(Cpu *c) { reading = 0; mode = 0x80; V0 = 1; }
void hle_CdReset(Cpu *c) { reading = 0; V0 = 1; }
void hle_CdFlush(Cpu *c) { (void)c; }
void hle_CdMix(Cpu *c) { V0 = 1; }
/* En este EXE los símbolos identificados como CdSync/CdReady son los registradores de callback
 * (guardan a0 y devuelven el anterior). */
void hle_CdSync(Cpu *c) { uint32_t o = sync_cb; sync_cb = A0; V0 = o; }
void hle_CdReady(Cpu *c) { uint32_t o = ready_cb; ready_cb = A0; V0 = o; }

void hle_CdControl(Cpu *c) {
    uint32_t com = A0 & 0xFF;
    V0 = control(c, com, A1, A2);
    if ((com == 0x06 || com == 0x1B) && !in_delivery) deliver(c, 4096);
}
void hle_CdControlB(Cpu *c) { hle_CdControl(c); }
void hle_CdControlF(Cpu *c) { V0 = control(c, A0, A1, 0); if (((A0 & 0xFF) == 6 || (A0 & 0xFF) == 0x1B) && !in_delivery) deliver(c, 4096); }

/* CdGetSector(buf, nwords): copia del sector actual (formato 2340 si modo 0x20). */
void hle_CdGetSector(Cpu *c) {
    uint32_t dst = A0, n = A1 * 4;
    uint32_t lim = (mode & 0x20) ? 2340 : 2048 + 12;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = secoff < lim ? secbuf[secoff] : 0;
        secoff++;
        mem_w8(c, dst + i, b);
    }
    V0 = 1;
}
