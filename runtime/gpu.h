#pragma once
/* GPU PS1: rasterizador software de referencia (oráculo del renderer Metal/VR). */
#include <stdint.h>

#define GPU_VRAM_W 1024
#define GPU_VRAM_H 512
extern uint16_t gpu_vram[GPU_VRAM_H][GPU_VRAM_W];

void gpu_reset(void);
/* Puertos: GP0 = 0x1F801810 (escritura), GPUREAD = 0x1F801810 (lectura),
 * GP1 = 0x1F801814 (escritura), GPUSTAT = 0x1F801814 (lectura). */
void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
/* DMA canal 2 (0x1F8010A0 MADR, A4 BCR, A8 CHCR): ejecutar al escribir CHCR con bit 24. */
void gpu_dma2(uint32_t madr, uint32_t bcr, uint32_t chcr);
/* Recorre una lista enlazada de paquetes (modo lista del DMA / DrawOTag). */
void gpu_draw_list(uint32_t addr);

typedef struct {
    int x, y, w, h;      /* área de VRAM mostrada */
    int depth24, interlace, pal, enabled;
} GpuDisplay;
void gpu_get_display(GpuDisplay *d);

/* Contadores por frame (para depurar): primitivas recibidas por clase. */
typedef struct { uint32_t poly, line, rect, fill, copy, upload, download, env, lists; } GpuStats;
extern GpuStats gpu_stats;

/* Vuelca a PPM el área de display (full_vram = 0) o la VRAM completa (1). */
void gpu_dump_ppm(const char *path, int full_vram);

/* ====================== Captura de primitivas por frame ======================
 * Capa independiente del rasterizador: registra, en orden de llegada, cada primitiva y cada evento de
 * VRAM (uploads, copias, fills) tal y como los ejecuta el oráculo, con TODO el estado necesario para
 * reproducirlos fuera de gpu.c (render/ref_raster.c, shaders Metal). El rasterizador de gpu.c no se
 * modifica: la captura es un espectador. Apagada (gpu_capture_enable(0)) el coste es un `if` por comando.
 *
 * Cierre de frame (punto elegido): el juego dibuja en un búfer y lo muestra con un cambio de dirección
 * de display (GP1(05h), p.ej. PutDispEnv tras VSync). Lo dibujado desde el cierre anterior hasta ese GP1(05h)
 * es exactamente el frame que acaba de pasar a mostrarse, así que la lista se cierra ahí. Si un juego no
 * alterna búferes, gpu_capture_vblank() (a llamar desde el manejador de VBlank del host) cierra la lista tras
 * GPU_CAP_VB_FALLBACK VBlanks seguidos sin alternancia. El cierre se difiere si ocurre en mitad de una subida
 * CPU->VRAM. GpuDrawList.disp se refresca durante la ráfaga de GP1 (modo/rango vertical se escriben
 * después de GP1(05h) en PutDispEnv).
 *
 * Los eventos de VRAM van intercalados en el mismo orden que las primitivas: las texturas (y CLUTs) cambian
 * dentro del frame, el consumidor debe aplicarlos en secuencia. Las listas son consecutivas: aplicar TODAS
 * las listas, en orden, mantiene la VRAM del consumidor igual a la del oráculo. La primera lista tras
 * gpu_capture_enable (o tras perder listas por cola llena) trae vram_init: volcado completo de VRAM al
 * principio de ese frame. Cola llena: se descarta TODA la cola y la lista que se cerraba (nunca queda un hueco en medio).
 * MONOHILO: todo el estado es estático y sin sincronización; enable/frame/vblank y el emulador de GPU deben ir en el
 * mismo hilo (o con exclusión mutua externa).
 */
#define GPU_CAP_VB_FALLBACK 4
#define GPU_CAP_QDEPTH 6

enum { GPU_OP_POLY, GPU_OP_RECT, GPU_OP_LINE, GPU_OP_FILL, GPU_OP_VCOPY, GPU_OP_UPLOAD, GPU_OP_DOWNLOAD };

enum {
    GPU_F_GOURAUD = 1,   /* color por vértice (si no, v[0] para toda la primitiva) */
    GPU_F_TEXTURED = 2,  /* efectivo: ya descontado E1.11 (texturas deshabilitadas) */
    GPU_F_RAW = 4,       /* textura sin modular */
    GPU_F_SEMI = 8,      /* bit semitransparente del comando (texturizado: solo texels con bit15) */
    GPU_F_DITHER = 16,   /* EFECTIVO: E1.9 && (gouraud || (texturizado && !raw)); rects: 0 */
    GPU_F_MASK_SET = 32, /* E6.0: poner bit15 en lo escrito */
    GPU_F_MASK_CHK = 64, /* E6.1: no escribir sobre píxeles con bit15 */
    GPU_F_FLIPX = 128, GPU_F_FLIPY = 256, /* rects texturizados (E1.12/13) */
    GPU_F_QUAD = 512     /* polígono de 4 vértices = triángulos (0,1,2) y (1,2,3), en ese orden */
};

/* Vértice. x,y: enteros de dibujo con el offset E5 YA sumado (lo que rasteriza el oráculo).
 * Hueco subpíxel (ver gpu_capture_set_float_hook): si fvalid, fx,fy = posición de pantalla sin cuantizar
 * (mismo espacio que x,y, offset sumado) y fz = z de vista (SZ, >0) tal como los dejó el último RTPS/RTPT
 * cuyo SXY entero coincide con el que escribió el juego. Si no, ignorar (los enteros mandan). */
typedef struct {
    int16_t x, y;
    uint8_t r, g, b, u, v;
    uint8_t fvalid;
    float fx, fy, fz;
    float vx, vy, vz;        /* posición 3D en espacio de VISTA (R·V+TR de la GTE, unidades del juego; vz = fz) */
} GpuVtx;

typedef struct {
    uint8_t type, nv;
    uint16_t flags;
    uint8_t abr;             /* modo de semitransparencia: 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4 */
    uint8_t tp;              /* modo de color: 0 = 4 bits (CLUT), 1 = 8 bits (CLUT), 2/3 = 15 bits */
    uint8_t twmx, twmy, twox, twoy; /* ventana de textura E2 (máscara/offset en unidades de 8 px) */
    uint16_t tx, ty;         /* base de la página de textura en VRAM (halfwords, filas) */
    uint16_t clx, cly;       /* CLUT: x en halfwords (múltiplo de 16), y en filas */
    int16_t cx1, cy1, cx2, cy2; /* área de recorte vigente (E3/E4, inclusiva) */
    GpuVtx v[4];             /* POLY: nv=3/4; LINE: 2; RECT/FILL: v[0] (color; RECT: x,y esquina y u,v) */
    int16_t rx, ry, rw, rh;  /* RECT: tamaño; FILL/VCOPY/UPLOAD/DOWNLOAD: destino ya normalizado */
    int16_t sx, sy;          /* VCOPY: origen */
    uint16_t fill15;         /* FILL: color RGB555 */
    uint32_t data_off, data_n; /* UPLOAD: píxeles (uint16) en GpuDrawList.data, por filas */
    uint32_t seq;            /* orden global desde enable */
    int16_t dox, doy;        /* offset de dibujo E5 vigente (ya sumado en v[].x,y y fx,fy; restarlo da la coordenada cruda de la GTE) */
    float proj_h, proj_ox, proj_oy; /* POLY/LINE con algún vértice fvalid: H de la GTE y OFX/OFY en píxeles (0 si no). Con
                                     * ellos y vx,vy,vz se re-proyecta por ojo: x = proj_ox + proj_h*(vx -/+ e)/vz. */
} GpuOp;

enum { GPU_CLOSE_FLIP = 1, GPU_CLOSE_VBLANK = 2 };

typedef struct {
    uint32_t index;          /* nº de lista desde enable (consecutivo; saltos = listas perdidas) */
    uint32_t host_frame;     /* hle_frame_count() al cerrar */
    int reason;
    GpuDisplay disp;         /* área mostrada tras el cierre */
    GpuOp *ops; uint32_t nops, cap_ops;  /* privado: cap_ops */
    uint16_t *data; uint32_t ndata, cap_data;
    uint16_t *vram_init;     /* NULL salvo primera lista / resincronización: VRAM [512][1024] al INICIO del frame */
    uint16_t *vram_after;    /* solo gpu_capture_enable(2): VRAM del oráculo al CERRAR (para verificar) */
} GpuDrawList;

/* 0 apaga y libera; 1 captura; 2 captura + copia de VRAM del oráculo al cerrar cada frame (verificación). */
void gpu_capture_enable(int mode);
/* Siguiente lista cerrada aún no entregada (FIFO), o NULL. Válida hasta la siguiente llamada. */
const GpuDrawList *gpu_capture_frame(void);
/* VBlank del host (cierre de reserva para juegos sin doble búfer). */
void gpu_capture_vblank(void);
/* Listas perdidas por cola llena (la siguiente entregada trae vram_init). */
uint32_t gpu_capture_dropped(void);

/* Enlace antiguo (solo float, sin vista) con la GTE; el actual es gpu_capture_hiprec / GpuHpHook (abajo). Dado el SXY entero que escribió el juego (sin offset, con signo), devuelve 1 y
 * rellena la posición float si el último RTPS/RTPT produjo exactamente ese SXY. La GTE (gte.c) mantendrá una
 * tabla pequeña (clave = par (sx,sy) saturado, valor = x,y float previos a saturar/redondear y SZ),
 * vaciada cada frame; si dos vértices comparten SXY gana el último. Este hook es el único acoplamiento. */
typedef int (*GpuFloatHook)(int sx, int sy, float *fx, float *fy, float *fz);
void gpu_capture_set_float_hook(GpuFloatHook h);

/* ====================== Geometría de alta precisión (sombra flotante de la GTE) ======================
 * gte.c mantiene, además del resultado entero exacto, una tabla por frame con la posición sin cuantizar de cada
 * vértice transformado (ver el bloque "Alta precisión" de gte.c). gpu_capture_hiprec(1) la enciende y engancha a
 * la captura: cada vértice de polígono/línea cuyo (sx,sy) entero coincida con uno de la tabla sale con
 * fvalid/fx/fy/fz/vx/vy/vz. Los rects y los polígonos 2D (HUD, texto) nunca coinciden con coherencia y salen con
 * fvalid=0. El cierre de frame de la captura vacía la tabla (gte_hp_frame). Con la alta precisión apagada
 * (por defecto) no cambia nada del comportamiento observable. */
typedef struct {
    float fx, fy, fz;      /* pantalla sin cuantizar (sin offset de dibujo) y z de vista */
    float vx, vy, vz;      /* espacio de vista */
    float h, ofx, ofy;     /* proyección vigente: x = ofx + h*vx/vz */
} GpuHpInfo;
typedef struct {
    uint32_t stored, evict, skipped, lookups, hits;
    uint32_t skip_sf, skip_sat, skip_ovf, skip_z; /* motivos de 'skipped' */
    uint32_t gen_stored, max_gen_stored;           /* almacenados en la generación en curso / máximo por generación */
} GteHpStats;
typedef int (*GpuHpHook)(int sx, int sy, GpuHpInfo *out);
void gpu_capture_set_hp_hook(GpuHpHook h);
void gpu_capture_hiprec(int on);
/* gte.c */
void gte_hp_enable(int on);
int gte_hp_enabled(void);
void gte_hp_frame(void);
int gte_hp_lookup(int sx, int sy, GpuHpInfo *out);
void gte_hp_stats(GteHpStats *s);
