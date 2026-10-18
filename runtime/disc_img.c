/* Lector de imágenes BIN/CUE + identificación de disco por contenido. Ver disc_img.h. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include "disc_img.h"

#define MAX_SEG 99
#define MAX_TRK 99
#define BLK 16          /* sectores por bloque de caché */
#define NCACHE 4

typedef struct {
    FILE *f;
    char path[1024];
    uint32_t base, nsec;    /* LBA inicial y sectores */
} Seg;

typedef struct { uint32_t start; int audio; } Trk;

typedef struct {
    int seg, valid;         /* valid = sectores leídos del bloque */
    uint32_t blk;
    uint8_t d[BLK * DISC_RAW];
} Cache;

struct DiscImg {
    Seg seg[MAX_SEG]; int nseg;
    Trk trk[MAX_TRK]; int ntrk;
    uint32_t total;
    Cache cache[NCACHE]; int rr;
};

/* ---------------- utilidades ---------------- */

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

static int has_ext(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext);
    return a >= b && !strcasecmp(name + a - b, ext);
}

static void dir_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%.1000s", path);
    char *s = strrchr(out, '/');
    if (s) *s = 0; else snprintf(out, n, ".");
    if (!out[0]) snprintf(out, n, "/");
}

/* Busca "name" en "dir" sin distinguir mayúsculas. 1 = encontrado (ruta completa en out). */
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(dp)))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(dp);
    return ok;
}

/* Resuelve el nombre de un FILE del .cue: tal cual, con '\' como '/', nombre base, sin mayúsculas. */
static int resolve(const char *cuedir, const char *name, char *out, size_t n) {
    char t[1024];
    if (strlen(cuedir) + strlen(name) + 2 > n) return 0;
    snprintf(out, n, "%s/%.1023s", cuedir, name);
    if (is_file(out)) return 1;
    if (name[0] == '/' && is_file(name)) { snprintf(out, n, "%s", name); return 1; }
    snprintf(t, sizeof t, "%s", name);
    for (char *p = t; *p; p++) if (*p == '\\') *p = '/';
    snprintf(out, n, "%s/%.1023s", cuedir, t);
    if (is_file(out)) return 1;
    const char *base = strrchr(t, '/'); base = base ? base + 1 : t;
    snprintf(out, n, "%s/%.1023s", cuedir, base);
    if (is_file(out)) return 1;
    return find_ci(cuedir, base, out, n);
}

/* ---------------- .cue ---------------- */

static int msf(const char *s, uint32_t *out) {
    unsigned m, sec, f;
    if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3) return -1;
    *out = (m * 60 + sec) * 75 + f;
    return 0;
}

static int add_seg(DiscImg *h, const char *path, int warn) {
    if (h->nseg == MAX_SEG) return -1;
    Seg *s = &h->seg[h->nseg];
    s->f = fopen(path, "rb");
    if (!s->f) { fprintf(stderr, "[disc] no se puede abrir %s\n", path); return -1; }
    snprintf(s->path, sizeof s->path, "%s", path);
    if (fseeko(s->f, 0, SEEK_END)) { fclose(s->f); s->f = NULL; return -1; }
    off_t sz = ftello(s->f);
    if (sz < 0) { fclose(s->f); s->f = NULL; return -1; }
    if (warn && sz % DISC_RAW) fprintf(stderr, "[disc] aviso: %s no es múltiplo de 2352 (se ignora la cola)\n", path);
    s->nsec = (uint32_t)(sz / DISC_RAW);
    s->base = h->total;
    h->total += s->nsec;
    h->nseg++;
    return 0;
}

static int parse_cue(DiscImg *h, const char *cuepath) {
    FILE *f = fopen(cuepath, "rb");
    if (!f) { fprintf(stderr, "[disc] no se puede abrir %s\n", cuepath); return -1; }
    size_t cap = 1 << 20;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(f); return -1; }
    size_t len = fread(buf, 1, cap, f);
    fclose(f);
    buf[len] = 0;
    char *p = buf;
    if (len >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    char cuedir[1024]; dir_of(cuepath, cuedir, sizeof cuedir);
    int rc = 0, curtrk = -1;
    uint32_t segbase = 0;
    while (*p && !rc) {
        char *e = p; while (*e && *e != '\n' && *e != '\r') e++;
        char save = *e; *e = 0;
        char line[2048]; snprintf(line, sizeof line, "%s", p);
        *e = save; p = e; while (*p == '\n' || *p == '\r') p++;
        trim(line);
        char *kw = line, *rest = line;
        while (*rest && !isspace((unsigned char)*rest)) rest++;
        if (*rest) *rest++ = 0;
        trim(rest);
        if (!strcasecmp(kw, "FILE")) {
            char name[1024];
            if (strlen(rest) >= sizeof name) { rc = -1; break; }
            if (*rest == '"') {
                char *q = strchr(rest + 1, '"');
                size_t n = q ? (size_t)(q - rest - 1) : strlen(rest + 1);
                if (n >= sizeof name) n = sizeof name - 1;
                memcpy(name, rest + 1, n); name[n] = 0;
                rest = q ? q + 1 : rest + 1 + n;
            } else {  /* sin comillas: el último token es el tipo, el resto el nombre (puede tener espacios) */
                char *sp = strrchr(rest, ' '); char *tb = strrchr(rest, '\t');
                if (tb > sp) sp = tb;
                if (sp) { snprintf(name, sizeof name, "%.*s", (int)(sp - rest), rest); rest = sp + 1; }
                else { snprintf(name, sizeof name, "%.1023s", rest); rest = rest + strlen(rest); }
                trim(name);
            }
            trim(rest);
            if (*rest && strcasecmp(rest, "BINARY")) {
                fprintf(stderr, "[disc] tipo de fichero no soportado en el .cue: %s\n", rest);
                rc = -1; break;
            }
            char path[2048];
            if (!resolve(cuedir, name, path, sizeof path)) {
                fprintf(stderr, "[disc] %s: no encuentro %s\n", cuepath, name);
                rc = -1; break;
            }
            segbase = h->total;
            if (add_seg(h, path, 1)) rc = -1;
        } else if (!strcasecmp(kw, "TRACK")) {
            unsigned n; char mode[32] = "";
            if (sscanf(rest, "%u %31s", &n, mode) < 2 || h->ntrk == MAX_TRK) { rc = -1; break; }
            if (strcasecmp(mode, "AUDIO") && strcasecmp(mode, "MODE2/2352") && strcasecmp(mode, "MODE1/2352")) {
                fprintf(stderr, "[disc] modo de pista no soportado: %s (solo 2352)\n", mode);
                rc = -1; break;
            }
            curtrk = h->ntrk++;
            h->trk[curtrk].audio = !strcasecmp(mode, "AUDIO");
            h->trk[curtrk].start = segbase;
        } else if (!strcasecmp(kw, "INDEX")) {
            unsigned idx; char t[32]; uint32_t v;
            if (curtrk >= 0 && sscanf(rest, "%u %31s", &idx, t) == 2 && idx == 1 && !msf(t, &v))
                h->trk[curtrk].start = segbase + v;
        }
        /* PREGAP/POSTGAP/REM/CATALOG/FLAGS/PERFORMER/TITLE...: ignorados */
    }
    free(buf);
    if (!rc && (!h->nseg || !h->ntrk)) { fprintf(stderr, "[disc] %s: sin FILE/TRACK\n", cuepath); rc = -1; }
    return rc;
}

/* ---------------- API de imagen ---------------- */

void disc_img_close(DiscImg *h) {
    if (!h) return;
    for (int i = 0; i < h->nseg; i++) if (h->seg[i].f) fclose(h->seg[i].f);
    free(h);
}

DiscImg *disc_img_open(const char *path) {
    if (!path) return NULL;
    DiscImg *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    int rc;
    if (has_ext(path, ".cue")) rc = parse_cue(h, path);
    else {
        rc = add_seg(h, path, 0);
        if (!rc && h->seg[0].nsec == 0) rc = -1;
        if (!rc) {
            off_t sz = (off_t)h->seg[0].nsec * DISC_RAW;
            FILE *f = h->seg[0].f; fseeko(f, 0, SEEK_END);
            if (ftello(f) != sz) { fprintf(stderr, "[disc] %s: tamaño no múltiplo de 2352\n", path); rc = -1; }
            h->trk[0].start = 0; h->trk[0].audio = 0; h->ntrk = 1;
        }
    }
    if (rc) { disc_img_close(h); return NULL; }
    return h;
}

uint32_t disc_img_sectors(const DiscImg *h) { return h ? h->total : 0; }
int disc_img_tracks(const DiscImg *h) { return h ? h->ntrk : 0; }
int disc_img_track(const DiscImg *h, int n, uint32_t *start, int *audio) {
    if (!h || n < 0 || n >= h->ntrk) return -1;
    if (start) *start = h->trk[n].start;
    if (audio) *audio = h->trk[n].audio;
    return 0;
}
int disc_img_nfiles(const DiscImg *h) { return h ? h->nseg : 0; }
const char *disc_img_filepath(const DiscImg *h, int i) { return h && i >= 0 && i < h->nseg ? h->seg[i].path : NULL; }

static Cache *cache_get(DiscImg *h, int si, uint32_t blk) {
    for (int i = 0; i < NCACHE; i++)
        if (h->cache[i].valid && h->cache[i].seg == si && h->cache[i].blk == blk) return &h->cache[i];
    Cache *c = &h->cache[h->rr]; h->rr = (h->rr + 1) % NCACHE;
    Seg *s = &h->seg[si];
    c->valid = 0;
    uint32_t first = blk * BLK, n = s->nsec - first;
    if (n > BLK) n = BLK;
    if (fseeko(s->f, (off_t)first * DISC_RAW, SEEK_SET)) return NULL;
    size_t got = fread(c->d, DISC_RAW, n, s->f);
    if (!got) return NULL;
    c->seg = si; c->blk = blk; c->valid = (int)got;
    return c;
}

int disc_img_read_raw(DiscImg *h, uint32_t lba, uint8_t out[DISC_RAW]) {
    memset(out, 0, DISC_RAW);
    if (!h) return -1;
    for (int i = 0; i < h->nseg; i++) {
        Seg *s = &h->seg[i];
        if (lba < s->base || lba >= s->base + s->nsec) continue;
        uint32_t k = lba - s->base;
        Cache *c = cache_get(h, i, k / BLK);
        if (!c || (int)(k % BLK) >= c->valid) return -1;
        memcpy(out, c->d + (k % BLK) * DISC_RAW, DISC_RAW);
        return 0;
    }
    return -1;
}

int disc_img_read_user(DiscImg *h, uint32_t lba, uint8_t out[DISC_USER]) {
    uint8_t raw[DISC_RAW];
    if (disc_img_read_raw(h, lba, raw)) { memset(out, 0, DISC_USER); return -1; }
    memcpy(out, raw + (raw[15] == 1 ? 16 : 24), DISC_USER);
    return 0;
}

/* ---------------- identificación ---------------- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Extrae el ejecutable de arranque de un SYSTEM.CNF (BOOT = cdrom:\NOMBRE;1). */
static int parse_boot(const char *txt, char *boot, size_t n) {
    const char *p = txt;
    while (*p) {
        const char *l = p;
        while (*l == ' ' || *l == '\t') l++;
        if (!strncasecmp(l, "BOOT", 4) && (l[4] == ' ' || l[4] == '=' || l[4] == '\t')) {
            const char *v = l, *le = l;
            while (*le && *le != '\n') le++;
            while (v < le && *v != '=') v++;
            if (v < le) {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                if (!strncasecmp(v, "cdrom:", 6)) v += 6;
                while (*v == '\\' || *v == '/') v++;
                size_t k = 0;
                while (*v && *v != ';' && *v != '\r' && *v != '\n' && *v != ' ' && k + 1 < n) boot[k++] = *v++;
                boot[k] = 0;
                return k ? 0 : -1;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

static int serial_of(const char *boot) {
    int v = 0, any = 0, nd = 0;
    for (; *boot && nd < 9; boot++) if (isdigit((unsigned char)*boot)) { v = v * 10 + *boot - '0'; any = 1; nd++; }
    return any ? v : 0;
}

static void id_clear(DiscId *id) { memset(id, 0, sizeof *id); }

/* Registro de directorio ISO: nombre sin ";1" (y sin '.' final). */
static void iso_name(const uint8_t *r, char *out, size_t n) {
    int l = r[32]; size_t k = 0;
    if (l > r[0] - 33) l = r[0] - 33;   /* el nombre no puede salirse del registro */
    for (int i = 0; i < l && k + 1 < n && r[33 + i] != ';'; i++) out[k++] = r[33 + i];
    if (k && out[k - 1] == '.') k--;
    out[k] = 0;
}

typedef struct { uint32_t lba, size; int dir; char name[32]; } IsoEnt;

/* Lista un directorio ISO (hasta max entradas, sin '.' ni '..'). */
#define ISO_MAX 4096    /* 64 sectores / 34 B mínimos por registro = 3855 */
static int iso_list(DiscImg *h, uint32_t lba, uint32_t size, IsoEnt *out, int max) {
    int n = 0;
    uint32_t nsec = (uint32_t)(((uint64_t)size + DISC_USER - 1) / DISC_USER);
    if (nsec > 64) nsec = 64;
    uint8_t sec[DISC_USER];
    for (uint32_t s = 0; s < nsec; s++) {
        if (disc_img_read_user(h, lba + s, sec)) break;
        uint32_t o = 0;
        while (o + 34 <= DISC_USER) {
            uint8_t len = sec[o];
            if (!len) break;
            if (o + len > DISC_USER || len < 34) break;
            const uint8_t *r = sec + o;
            o += len;
            if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;
            if (n < max) {
                IsoEnt *e = &out[n++];
                e->lba = rd32(r + 2); e->size = rd32(r + 10); e->dir = r[25] & 2;
                iso_name(r, e->name, sizeof e->name);
            }
        }
    }
    return n;
}

int disc_img_find(DiscImg *h, const char *path, uint32_t *lba, uint32_t *size) {
    uint8_t sec[DISC_USER];
    if (disc_img_read_user(h, 16, sec) || memcmp(sec + 1, "CD001", 5) || sec[0] != 1) return -1;
    uint32_t dl = rd32(sec + 156 + 2), ds = rd32(sec + 156 + 10);
    char comp[256];
    const char *p = path;
    IsoEnt *ents = malloc(ISO_MAX * sizeof *ents);
    if (!ents) return -1;
    int rc = -1;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        size_t k = 0;
        while (*p && *p != '/' && *p != '\\' && k + 1 < sizeof comp) comp[k++] = *p++;
        comp[k] = 0;
        if (!k) break;
        char *sc = strchr(comp, ';'); if (sc) *sc = 0;
        int n = iso_list(h, dl, ds, ents, ISO_MAX), hit = -1;
        for (int i = 0; i < n; i++) if (!strcasecmp(ents[i].name, comp)) { hit = i; break; }
        if (hit < 0) break;
        while (*p == '/' || *p == '\\') p++;
        if (!*p) { if (!ents[hit].dir) { *lba = ents[hit].lba; *size = ents[hit].size; rc = 0; } break; }
        if (!ents[hit].dir) break;
        dl = ents[hit].lba; ds = ents[hit].size;
    }
    free(ents);
    return rc;
}

static int identify_ents(DiscImg *h, uint32_t rlba, uint32_t rsize, IsoEnt *ents, IsoEnt *sub, DiscId *id);

int disc_img_identify(DiscImg *h, DiscId *id) {
    id_clear(id);
    uint8_t sec[DISC_USER];
    if (disc_img_read_user(h, 16, sec) || memcmp(sec + 1, "CD001", 5) || sec[0] != 1) return -1;
    uint32_t rlba = rd32(sec + 156 + 2), rsize = rd32(sec + 156 + 10);
    IsoEnt *ents = malloc(ISO_MAX * sizeof *ents), *sub = malloc(ISO_MAX * sizeof *sub);
    int rc = -1;
    if (ents && sub) rc = identify_ents(h, rlba, rsize, ents, sub, id);
    free(ents); free(sub);
    return rc;
}

static int identify_ents(DiscImg *h, uint32_t rlba, uint32_t rsize, IsoEnt *ents, IsoEnt *sub, DiscId *id) {
    int n = iso_list(h, rlba, rsize, ents, ISO_MAX);
    int got = 0;
    for (int i = 0; i < n; i++)
        if (!ents[i].dir && !strcasecmp(ents[i].name, "SYSTEM.CNF")) {
            char txt[DISC_USER + 1];
            if (disc_img_read_user(h, ents[i].lba, (uint8_t *)txt)) return -1;
            txt[DISC_USER] = 0;
            if (ents[i].size < DISC_USER) txt[ents[i].size] = 0;
            got = !parse_boot(txt, id->boot, sizeof id->boot);
            break;
        }
    if (!got) return -1;
    id->serial = serial_of(id->boot);
    for (int i = 0; i < n; i++)
        if (ents[i].dir) {
            snprintf(id->datadir, sizeof id->datadir, "%s", ents[i].name);
            int m = iso_list(h, ents[i].lba, ents[i].size, sub, ISO_MAX);
            for (int j = 0; j < m; j++) {
                uint8_t b[DISC_USER];
                if (sub[j].dir || sub[j].size < 2048) continue;
                if (!disc_img_read_user(h, sub[j].lba, b) && !memcmp(b, "PS-X EXE", 8)) {
                    snprintf(id->dataexe, sizeof id->dataexe, "%s", sub[j].name);
                    break;
                }
            }
            break;
        }
    return 0;
}

int disc_dir_identify(const char *dir, DiscId *id) {
    id_clear(id);
    char p[2048];
    if (!find_ci(dir, "SYSTEM.CNF", p, sizeof p)) return -1;
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    char txt[2048]; size_t n = fread(txt, 1, sizeof txt - 1, f); txt[n] = 0; fclose(f);
    if (parse_boot(txt, id->boot, sizeof id->boot)) return -1;
    id->serial = serial_of(id->boot);
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    struct dirent *e; char names[64][256]; int cnt = 0;
    while ((e = readdir(dp)) && cnt < 64) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) snprintf(names[cnt++], 256, "%s", e->d_name);
    }
    closedir(dp);
    if (!cnt) return 0;
    int best = 0;   /* orden estable: el primero alfabéticamente */
    for (int i = 1; i < cnt; i++) if (strcmp(names[i], names[best]) < 0) best = i;
    snprintf(id->datadir, sizeof id->datadir, "%.31s", names[best]);
    char sub[2048]; snprintf(sub, sizeof sub, "%s/%s", dir, names[best]);
    dp = opendir(sub);
    if (!dp) return 0;
    char exe[256] = "";
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", sub, e->d_name) >= (int)sizeof p) continue;
        FILE *g = fopen(p, "rb");
        if (!g) continue;
        char m[8];
        int ok = fread(m, 1, 8, g) == 8 && !memcmp(m, "PS-X EXE", 8);
        fclose(g);
        if (ok && (!exe[0] || strcmp(e->d_name, exe) < 0)) snprintf(exe, sizeof exe, "%s", e->d_name);
    }
    closedir(dp);
    snprintf(id->dataexe, sizeof id->dataexe, "%.31s", exe);
    return 0;
}

/* ---------------- búsqueda ---------------- */

static int same_id(const DiscId *a, const DiscId *b) {
    return !strcasecmp(a->boot, b->boot) && !strcasecmp(a->dataexe, b->dataexe);
}

static int add_entry(DiscEntry *out, int *n, int max, const char *path, int dir, const DiscId *id) {
    if (strlen(path) >= sizeof out[0].path) return 0;   /* no cabe: no se trunca a una ruta falsa */
    for (int i = 0; i < *n; i++)
        if (same_id(&out[i].id, id)) {
            if (out[i].is_dir && !dir) { snprintf(out[i].path, sizeof out[i].path, "%s", path); out[i].is_dir = 0; }
            return 0;
        }
    if (*n >= max) return -1;
    DiscEntry *e = &out[(*n)++];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->is_dir = dir; e->id = *id;
    return 0;
}

static int cmp_entry(const void *pa, const void *pb) {
    const DiscEntry *a = pa, *b = pb;
    if (a->id.serial != b->id.serial) return a->id.serial < b->id.serial ? -1 : 1;
    int c = strcasecmp(a->id.boot, b->id.boot);
    if (c) return c;
    return strcasecmp(a->id.dataexe, b->id.dataexe);
}

static void scan(const char *dir, int depth, DiscEntry *out, int *n, int max) {
    DiscId id;
    if (disc_dir_identify(dir, &id) == 0) { add_entry(out, n, max, dir, 1, &id); return; }
    DIR *dp = opendir(dir);
    if (!dp) return;
    enum { MAXN = 512 };
    char (*names)[256] = malloc(MAXN * 256);
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && cnt < MAXN)
        if (e->d_name[0] != '.') snprintf(names[cnt++], 256, "%s", e->d_name);
    closedir(dp);
    /* .bin ya referenciados por algún .cue */
    char (*used)[1024] = malloc(MAXN * 1024);
    int nused = 0;
    for (int i = 0; i < cnt; i++) {
        char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (!has_ext(names[i], ".cue") || !is_file(p)) continue;
        DiscImg *h = disc_img_open(p);
        if (!h) continue;
        for (int k = 0; k < disc_img_nfiles(h) && nused < MAXN; k++)
            snprintf(used[nused++], 1024, "%s", disc_img_filepath(h, k));
        if (!disc_img_identify(h, &id)) add_entry(out, n, max, p, 0, &id);
        disc_img_close(h);
    }
    for (int i = 0; i < cnt; i++) {
        char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (is_dir(p)) { if (depth < 2) scan(p, depth + 1, out, n, max); continue; }
        if (!has_ext(names[i], ".bin") || !is_file(p)) continue;
        int skip = 0;
        for (int k = 0; k < nused; k++) {
            if (!strcasecmp(used[k], p)) { skip = 1; break; }
            const char *a = strrchr(used[k], '/'), *b = strrchr(p, '/');
            if (a && b && !strcasecmp(a, b)) { skip = 1; break; }
        }
        if (skip) continue;
        DiscImg *h = disc_img_open(p);
        if (!h) continue;
        if (!disc_img_identify(h, &id)) add_entry(out, n, max, p, 0, &id);
        disc_img_close(h);
    }
    free(names); free(used);
}

int disc_scan_dir(const char *dir, DiscEntry out[], int max) {
    int n = 0;
    scan(dir, 0, out, &n, max);
    qsort(out, n, sizeof *out, cmp_entry);
    for (int i = 0; i < n; i++) out[i].id.number = i + 1;
    return n;
}

/* ---------------- extracción del ejecutable de datos ---------------- */

int disc_extract_dataexe(const char *path, int is_dir, const char *outfile) {
    DiscId id;
    FILE *o = NULL;
    int rc = -1;
    if (is_dir) {
        if (disc_dir_identify(path, &id) || !id.datadir[0] || !id.dataexe[0]) return -1;
        char src[2048];
        if (snprintf(src, sizeof src, "%s/%s/%s", path, id.datadir, id.dataexe) >= (int)sizeof src) return -1;
        FILE *f = fopen(src, "rb");
        if (!f) return -1;
        o = fopen(outfile, "wb");
        if (!o) { fclose(f); return -1; }
        uint8_t b[16384];
        size_t n;
        rc = 0;
        while ((n = fread(b, 1, sizeof b, f)) > 0)
            if (fwrite(b, 1, n, o) != n) { rc = -1; break; }
        fclose(f);
    } else {
        DiscImg *h = disc_img_open(path);
        if (!h) return -1;
        uint32_t lba = 0, size = 0;
        char rel[80];
        if (disc_img_identify(h, &id) || !id.datadir[0] || !id.dataexe[0]) { disc_img_close(h); return -1; }
        snprintf(rel, sizeof rel, "%s/%s", id.datadir, id.dataexe);
        if (disc_img_find(h, rel, &lba, &size) || !size) { disc_img_close(h); return -1; }
        o = fopen(outfile, "wb");
        if (!o) { disc_img_close(h); return -1; }
        rc = 0;
        for (uint32_t done = 0, s = 0; done < size; s++) {
            uint8_t b[DISC_USER];
            if (disc_img_read_user(h, lba + s, b)) { rc = -1; break; }
            uint32_t n = size - done < DISC_USER ? size - done : DISC_USER;
            if (fwrite(b, 1, n, o) != n) { rc = -1; break; }
            done += n;
        }
        disc_img_close(h);
    }
    if (fclose(o)) rc = -1;
    if (rc) remove(outfile);
    return rc;
}
