/* Tarjeta de memoria sobre imagen cruda de 128 KB (.mcd: 1024 sectores de 128 B, 16 bloques de 8 KB).
 * Cubre las llamadas de BIOS de bajo nivel (_card_info/_card_load/_card_read/_card_write/_new_card)
 * y el sistema de ficheros "buXY:" (open/read/write/lseek/close/firstfile/nextfile/erase/format),
 * entregando los eventos de tarjeta (software 0xF0000011 y hardware 0xF4000001) que espera el juego. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "sys.h"

#define CARD_SIZE 0x20000
#define SECT 128
#define BLOCK 8192

typedef struct {
    int present, isnew, dirty;
    char path[512];
    uint8_t img[CARD_SIZE];
} Card;

static Card cards[2];
static int env_checked;

/* ---------------- formato ---------------- */

static void frame_sum(uint8_t *f) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
}

static void card_format_img(uint8_t *m) {
    memset(m, 0, CARD_SIZE);
    m[0] = 'M'; m[1] = 'C'; frame_sum(m);
    for (int b = 1; b < 16; b++) {
        uint8_t *f = m + b * SECT;
        f[0] = 0xA0;
        f[8] = 0xFF; f[9] = 0xFF;
        frame_sum(f);
    }
    for (int s = 16; s < 36; s++) {
        uint8_t *f = m + s * SECT;
        memset(f, 0xFF, 4);
        f[8] = 0xFF; f[9] = 0xFF;
        frame_sum(f);
    }
    memcpy(m + 63 * SECT, m, SECT);
}

static int formatted(const Card *k) { return k->img[0] == 'M' && k->img[1] == 'C'; }

static void flush(int port) {
    Card *k = &cards[port];
    if (!k->dirty || !k->path[0]) return;
    FILE *f = fopen(k->path, "wb");
    if (!f) { perror(k->path); return; }
    fwrite(k->img, 1, CARD_SIZE, f);
    fclose(f);
    k->dirty = 0;
}

int card_open(int port, const char *path) {
    if (port < 0 || port > 1) return -1;
    Card *k = &cards[port];
    flush(port);
    snprintf(k->path, sizeof k->path, "%s", path);
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    if (f) { n = fread(k->img, 1, CARD_SIZE, f); fclose(f); }
    if (n != CARD_SIZE) { card_format_img(k->img); k->dirty = 1; flush(port); }
    k->present = 1;
    k->isnew = 1;
    return 0;
}

void card_close(int port) {
    if (port < 0 || port > 1) return;
    flush(port);
    cards[port].present = 0;
}

static void check_env(void) {
    if (env_checked) return;
    env_checked = 1;
    const char *p = getenv("MV_CARD");
    if (p && !cards[0].present) card_open(0, p);
}

/* ---------------- eventos ---------------- */

static void card_event(Cpu *c, uint32_t spec) {
    bios_deliver_event(c, EV_CARD_SW, spec);
    bios_deliver_event(c, EV_CARD_HW, spec);
}

/* ---------------- directorio ---------------- */

static uint8_t *dir(Card *k, int b) { return k->img + b * SECT; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int find_file(Card *k, const char *name) {
    for (int b = 1; b < 16; b++) {
        uint8_t *d = dir(k, b);
        if (rd32(d) == 0x51 && !strncmp((const char *)d + 10, name, 20)) return b;
    }
    return 0;
}

/* Bloque n-ésimo de la cadena del fichero que empieza en b (0 si no existe). */
static int chain_block(Card *k, int b, int n) {
    while (n-- > 0 && b) {
        uint16_t nx = rd16(dir(k, b) + 8);
        b = nx == 0xFFFF ? 0 : nx + 1;
    }
    return b;
}

static int glob_match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') return 1;
        if (!*s) return 0;
        if (*p != '?' && *p != *s) return 0;
    }
    return *s == 0;
}

/* "buXY:resto" -> puerto (X) y nombre. */
static int parse_dev(Cpu *c, uint32_t a, char *name, size_t cap) {
    char s[64]; unsigned i = 0;
    while (i < sizeof s - 1) { s[i] = (char)mem_r8(c, a + i); if (!s[i]) break; i++; }
    s[i] = 0;
    if (strncmp(s, "bu", 2) || !s[2] || !s[3] || s[4] != ':') return -1;
    int port = s[2] - '0';
    if (port < 0 || port > 1) return -1;
    snprintf(name, cap, "%s", s + 5);
    return port;
}

/* ---------------- ficheros ---------------- */

#define MAX_FD 4
static struct { int used, port, head, async; uint32_t pos, size; } fds[MAX_FD];

static int f_open(Cpu *c, uint32_t pname, uint32_t flags) {
    char name[64];
    int port = parse_dev(c, pname, name, sizeof name);
    if (port < 0) return -1;
    Card *k = &cards[port];
    if (!k->present || !formatted(k)) return -1;
    int b = find_file(k, name);
    if (flags & 0x200) {               /* crear: bloques en los 16 bits altos */
        if (b) return -1;
        int nb = (int)(flags >> 16); if (nb < 1) nb = 1;
        int list[16], got = 0;
        for (int i = 1; i < 16 && got < nb; i++) if ((rd32(dir(k, i)) & 0xF0) == 0xA0) list[got++] = i;
        if (got < nb) return -1;
        for (int i = 0; i < nb; i++) {
            uint8_t *d = dir(k, list[i]);
            memset(d, 0, SECT);
            wr32(d, i == 0 ? 0x51 : i == nb - 1 ? 0x53 : 0x52);
            if (i == 0) { wr32(d + 4, (uint32_t)nb * BLOCK); snprintf((char *)d + 10, 21, "%s", name); }
            wr16(d + 8, i == nb - 1 ? 0xFFFF : (uint16_t)(list[i + 1] - 1));
            frame_sum(d);
            memset(k->img + list[i] * BLOCK, 0, BLOCK);
        }
        k->dirty = 1;
        flush(port);
        b = list[0];
    }
    if (!b) return -1;
    for (int i = 0; i < MAX_FD; i++) if (!fds[i].used) {
        fds[i].used = 1; fds[i].port = port; fds[i].head = b; fds[i].pos = 0;
        fds[i].async = (flags & 0x8000) != 0;
        fds[i].size = rd32(dir(k, b) + 4);
        return i + 2;                     /* 0/1 = consola */
    }
    return -1;
}

static int fd_ok(uint32_t fd) { return fd >= 2 && fd < 2 + MAX_FD && fds[fd - 2].used; }

static int f_rw(Cpu *c, uint32_t fd, uint32_t buf, uint32_t n, int wr) {
    if (!fd_ok(fd)) return -1;
    int i = (int)fd - 2;
    Card *k = &cards[fds[i].port];
    uint32_t done = 0;
    while (done < n && fds[i].pos < fds[i].size) {
        int b = chain_block(k, fds[i].head, (int)(fds[i].pos / BLOCK));
        if (!b) break;
        uint8_t *p = k->img + b * BLOCK + fds[i].pos % BLOCK;
        uint32_t chunk = SECT - (fds[i].pos % SECT);
        if (chunk > n - done) chunk = n - done;
        for (uint32_t j = 0; j < chunk; j++) {
            if (wr) p[j] = mem_r8(c, buf + done + j);
            else mem_w8(c, buf + done + j, p[j]);
        }
        done += chunk; fds[i].pos += chunk;
    }
    if (wr && done) { k->dirty = 1; flush(fds[i].port); }
    if (fds[i].async) {                   /* un evento de fin por sector transferido */
        for (uint32_t s = 0; s < (done + SECT - 1) / SECT; s++) card_event(c, EVS_IOE);
        if (done < n) card_event(c, EVS_ERROR);
    }
    return (int)done;
}

/* DIRENTRY BIOS: name[20], attr, size, next, head, system[4] (40 B). */
static struct { int port, next; char pat[24]; } ff;

static uint32_t dir_scan(Cpu *c, uint32_t de) {
    Card *k = &cards[ff.port];
    if (!k->present || !formatted(k)) return 0;
    for (int b = ff.next; b < 16; b++) {
        uint8_t *d = dir(k, b);
        if (rd32(d) != 0x51) continue;
        char nm[21]; memcpy(nm, d + 10, 20); nm[20] = 0;
        if (!glob_match(ff.pat, nm)) continue;
        ff.next = b + 1;
        for (int j = 0; j < 20; j++) mem_w8(c, de + j, (uint8_t)nm[j]);
        mem_w32(c, de + 20, 0x50);
        mem_w32(c, de + 24, rd32(d + 4));
        mem_w32(c, de + 28, 0);
        mem_w32(c, de + 32, (uint32_t)b);
        mem_w32(c, de + 36, 0);
        return de;
    }
    return 0;
}

static uint32_t f_firstfile(Cpu *c, uint32_t pname, uint32_t de) {
    char name[64];
    int port = parse_dev(c, pname, name, sizeof name);
    if (port < 0) return 0;
    ff.port = port; ff.next = 1;
    snprintf(ff.pat, sizeof ff.pat, "%s", name[0] ? name : "*");
    return dir_scan(c, de);
}

static int f_erase(Cpu *c, uint32_t pname) {
    char name[64];
    int port = parse_dev(c, pname, name, sizeof name);
    if (port < 0) return 0;
    Card *k = &cards[port];
    int b = k->present ? find_file(k, name) : 0;
    if (!b) return 0;
    while (b) {
        uint8_t *d = dir(k, b);
        uint16_t nx = rd16(d + 8);
        d[0] = (uint8_t)(0xA0 | (d[0] & 0x0F));
        frame_sum(d);
        b = nx == 0xFFFF ? 0 : nx + 1;
    }
    k->dirty = 1; flush(port);
    return 1;
}

static int f_format(Cpu *c, uint32_t pname) {
    char name[64];
    int port = parse_dev(c, pname, name, sizeof name);
    if (port < 0 || !cards[port].present) return 0;
    card_format_img(cards[port].img);
    cards[port].dirty = 1; flush(port);
    return 1;
}

/* ---------------- bajo nivel ---------------- */

static int lport(uint32_t p) { return (int)((p >> 4) & 1); }

static void ll_info(Cpu *c, int port, int load) {
    Card *k = &cards[port];
    if (!k->present) { card_event(c, EVS_TIMEOUT); return; }
    if (load) card_event(c, formatted(k) ? EVS_IOE : EVS_NEWCARD);
    else card_event(c, k->isnew ? EVS_NEWCARD : EVS_IOE);
}

static int ll_sector(Cpu *c, int port, uint32_t sec, uint32_t buf, int wr) {
    Card *k = &cards[port];
    if (!k->present) { card_event(c, EVS_TIMEOUT); return 1; }
    if (sec >= CARD_SIZE / SECT) { card_event(c, EVS_ERROR); return 0; }
    /* Sondeo de escritura de la librería del juego: sector 0x3F con búfer NULL (dirección física 0, cualquier segmento). Solo se confirma con el
     * evento: copiar la RAM baja (RAM[0..127]) sobre el sector 63 destruiría la copia de la cabecera y la imagen dejaría de ser un formato válido.
     * Las escrituras a ese sector con un búfer real siguen copiándose. */
    if (wr && sec == 0x3F && (buf & 0x1FFFFFFFu) == 0) { card_event(c, EVS_IOE); return 1; }
    uint8_t *p = k->img + sec * SECT;
    for (int j = 0; j < SECT; j++) {
        if (wr) p[j] = mem_r8(c, buf + j);
        else mem_w8(c, buf + j, p[j]);
    }
    if (wr) { k->dirty = 1; flush(port); }
    card_event(c, EVS_IOE);
    return 1;
}

/* Llamadas de BIOS relacionadas con tarjeta. Devuelve 1 si la ha atendido (v0 ya escrito). */
int card_bios(Cpu *c, uint32_t vec, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    check_env();
    static int trace = -1, ntr;
    if (trace < 0) trace = getenv("MV_CARD_LOG") != NULL;
    if (trace && ntr < 4000 && ((vec == 0xA0 && (fn == 0x70 || (fn >= 0xAB && fn <= 0xAF))) || (vec == 0xB0 && ((fn >= 0x32 && fn <= 0x36) || (fn >= 0x41 && fn <= 0x50)))))   /* diagnóstico: MV_CARD_LOG=1 */
        fprintf(stderr, "[card] %02X(%02X) a0=%08x a1=%08x a2=%08x\n", vec, fn, a0, a1, a2), ntr++;
    if (vec == 0xA0) {
        switch (fn) {
        case 0x70: c->r[2] = 1; return 1;                                /* _bu_init */
        case 0xAB: ll_info(c, lport(a0), 0); c->r[2] = 1; return 1;      /* _card_info */
        case 0xAC: ll_info(c, lport(a0), 1); c->r[2] = 1; return 1;      /* _card_load */
        case 0xAF: c->r[2] = 1; return 1;                                /* card_write_test */
        }
        return 0;
    }
    if (vec != 0xB0) return 0;
    switch (fn) {
    case 0x32: c->r[2] = (uint32_t)f_open(c, a0, a1); return 1;          /* open */
    case 0x33:                                                            /* lseek */
        if (!fd_ok(a0)) { c->r[2] = (uint32_t)-1; return 1; }
        fds[a0 - 2].pos = a2 == 1 ? fds[a0 - 2].pos + a1 : a1;
        c->r[2] = fds[a0 - 2].pos; return 1;
    case 0x34: c->r[2] = (uint32_t)f_rw(c, a0, a1, a2, 0); return 1;     /* read */
    case 0x35:                                                            /* write */
        if (a0 < 2) {                                                     /* consola */
            for (uint32_t j = 0; j < a2; j++) fputc(mem_r8(c, a1 + j), stderr);
            c->r[2] = a2; return 1;
        }
        c->r[2] = (uint32_t)f_rw(c, a0, a1, a2, 1); return 1;
    case 0x36:                                                            /* close */
        if (fd_ok(a0)) { fds[a0 - 2].used = 0; c->r[2] = a0; } else c->r[2] = (uint32_t)-1;
        return 1;
    case 0x41: c->r[2] = (uint32_t)f_format(c, a0); return 1;            /* format */
    case 0x42: c->r[2] = f_firstfile(c, a0, a1); return 1;               /* firstfile */
    case 0x43: c->r[2] = dir_scan(c, a0); return 1;                      /* nextfile */
    case 0x45: c->r[2] = (uint32_t)f_erase(c, a0); return 1;             /* erase */
    case 0x4A: case 0x4B: case 0x4C: c->r[2] = 1; return 1;              /* Init/Start/StopCARD */
    case 0x4E: c->r[2] = (uint32_t)ll_sector(c, lport(a0), a1, a2, 1); return 1; /* _card_write */
    case 0x4F: c->r[2] = (uint32_t)ll_sector(c, lport(a0), a1, a2, 0); return 1; /* _card_read */
    case 0x50: cards[lport(a0)].isnew = 0; c->r[2] = 1; return 1;       /* _new_card */
    }
    return 0;
}

/* HLE de librería: InitCARD/StartCARD/StopCARD tocan vectores del kernel; aquí no hacen falta. */
void hle_InitCARD(Cpu *c) { check_env(); c->r[2] = 1; }
void hle_StartCARD(Cpu *c) { c->r[2] = 1; }
void hle_StopCARD(Cpu *c) { c->r[2] = 1; }
/* Envoltorio de librería de firstfile (busca el dispositivo en la tabla del kernel). */
void hle_firstfile(Cpu *c) { check_env(); c->r[2] = f_firstfile(c, c->r[4], c->r[5]); }
