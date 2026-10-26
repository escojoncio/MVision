#pragma once
/* GPU original: rasterizador software de referencia (oráculo del renderer Metal/VR). */
#include <stddef.h>
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
/* Recorre una lista enlazada de paquetes (modo lista del DMA / gfx_list). */
void gpu_draw_list(uint32_t addr);

/* Área de VRAM mostrada (rango horizontal GP1(06h), vertical GP1(07h), modo GP1(08h)):
 *  x,y,w,h  rectángulo NOMINAL de la pantalla del modo (w = ancho del modo: 256/320/384/512/640; h = alto del rango vertical,
 *           x2 si entrelazado de alta resolución). Es el que usan la captura de listas, la escena VR y los renderers.
 *  aw       píxeles de ese rectángulo que el hardware muestra de verdad: ceil((x2-x1)/ciclos de reloj de punto por píxel). Puede
 *           ser menor que w (p. ej. el códec usa un modo de 384 con rango de 318): lo que cae fuera del rango NO se ve, es basura
 *           de la VRAM (el búfer vecino, texturas) y no debe presentarse.
 *  fw,pad_l marco completo en píxeles (ancho nominal del modo, ampliado si el rango se sale de él) y columna del marco donde
 *           empieza x: el hardware pinta x..x+aw en la columna pad_l y negro en el resto. Rango por defecto: pad_l = 0, fw = aw = w.
 *  mode,hx1,hx2,vy1,vy2  registros crudos (informativos). */
typedef struct {
    int x, y, w, h;
    int depth24, interlace, pal, enabled;
    int aw, fw, pad_l;
    int mode, hx1, hx2, vy1, vy2;
} GpuDisplay;
void gpu_get_display(GpuDisplay *d);
/* Imagen RGBA8 del marco completo (fw x h; usa aw, fw y pad_l) a partir de una VRAM de 1024x512x16 (NULL = la del oráculo): la lectura exacta del
 * hardware (15 bits con replicación de los bits altos, o 24 bits empaquetados), con el negro de los márgenes y con la pantalla
 * apagada en negro. dst necesita fw*h*4 bytes; devuelve el número de píxeles escritos (fw*h), 0 si no cabe (cap en píxeles). */
size_t gpu_present_rgba(const GpuDisplay *d, const uint16_t *vram, uint8_t *dst, size_t cap_px);

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
 * de display (GP1(05h), p.ej. gfx_put_disp tras frame_wait). Lo dibujado desde el cierre anterior hasta ese GP1(05h)
 * es exactamente el frame que acaba de pasar a mostrarse, así que la lista se cierra ahí. Si un juego no
 * alterna búferes, gpu_capture_vblank() (a llamar desde el manejador de VBlank del host) cierra la lista tras
 * GPU_CAP_VB_FALLBACK VBlanks seguidos sin alternancia. El cierre se difiere si ocurre en mitad de una subida
 * CPU->VRAM. GpuDrawList.disp se refresca durante la ráfaga de GP1 (modo/rango vertical se escriben
 * después de GP1(05h) en gfx_put_disp).
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
    uint8_t fvalid;          /* fx,fy,fz FIABLES para el modo plano (proyección sin saturar); implica vvalid y vflags==0 */
    uint8_t vvalid;          /* vx,vy,vz FIABLES (posición de vista), también en vértices saturados / detrás de la cámara */
    uint8_t vflags;          /* solo con vvalid (máscara de bits):
                              * GPU_VF_SAT    el SXY entero saturó/desbordó (fx,fy = proyección en double sin saturar, útil pero NO coherente con x,y);
                              * GPU_VF_BEHIND vz <= 0 (fx,fy = 0, no hay proyección; recortar contra el plano cercano);
                              * GPU_VF_AMBIG  baja confianza: había varios candidatos con el mismo SXY y la secuencia de la GTE no los
                              *              distingue (primitiva sin ancla, p.ej. todos los vértices saturados en la misma esquina);
                              * GPU_VF_DERIVED vértice NO transformado por la GTE: esquina de un sprite rectangular en pantalla construida
                              *              por el juego a partir de dos esquinas diagonales proyectadas; vx,vy,vz se reconstruyen a la
                              *              profundidad media de las dos esquinas (aproximación de cartel); fvalid=0. */
    float fx, fy, fz;
    float vx, vy, vz;        /* posición 3D en espacio de VISTA (R·V+TR de la GTE, unidades del juego; vz = fz) */
} GpuVtx;
enum { GPU_VF_SAT = 1, GPU_VF_BEHIND = 2, GPU_VF_AMBIG = 4, GPU_VF_DERIVED = 8 };

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
    uint8_t fbk;             /* POLY/RECT texturizado: 1 = REALIMENTACIÓN resuelta (ver abajo); fbt vale */
    uint16_t fbt;            /* con fbk: texel único muestreado (RGB555 + bit15 = semitransparente) tras CLUT, con la VRAM del oráculo justo antes de dibujar; 0 = transparente (no pinta nada) */
    float proj_h, proj_ox, proj_oy; /* POLY/LINE con algún vértice vvalid: H de la GTE y OFX/OFY en píxeles (0 si no). Con
                                     * ellos y vx,vy,vz se re-proyecta por ojo: x = proj_ox + proj_h*(vx -/+ e)/vz. */
} GpuOp;

/* Realimentación: polígonos/sprites texturizados cuya textura (o CLUT) cae en el área de dibujo o de visualización (el frame buffer que
 * se está pintando) y que muestrean UN solo texel (tras CLUT) en todos sus píxeles (cubren <= 2 texels por eje). El consumidor que
 * recibe la VRAM solo por eventos NO ve lo dibujado por primitivas, así que ese texel sería obsoleto: la captura lo resuelve y lo deja
 * en GpuOp.fbk/fbt. El consumidor debe tratar la primitiva como NO texturizada, con color = color de vértice * fbt / 128 (como la GPU:
 * (texel5 * color) >> 4 por canal; TEXTURA CRUDA: el texel tal cual), semitransparente (modo abr) solo si GPU_F_SEMI && fbt bit 15, y
 * descartada si fbt == 0. El rasterizador de referencia (ref_raster) no usa estos campos: sigue leyendo la VRAM. */
typedef struct { uint32_t cand, baked, cleared, nonuniform; } GpuFbStats;   /* candidatas en frame buffer / resueltas con texel / resueltas transparentes / con >1 valor (sin resolver) */
extern GpuFbStats gpu_fb_stats;

enum { GPU_CLOSE_FLIP = 1, GPU_CLOSE_VBLANK = 2 };

typedef struct {
    uint32_t index;          /* nº de lista desde enable (consecutivo; saltos = listas perdidas) */
    uint32_t host_frame;     /* hle_frame_count() al cerrar */
    int reason;
    GpuDisplay disp;         /* área mostrada tras el cierre */
    GpuOp *ops; uint32_t nops, cap_ops;  /* privado: cap_ops */
    uint16_t *data; uint32_t ndata, cap_data;
    uint16_t *vram_init;     /* NULL salvo primera lista / resincronización: VRAM [512][1024] al INICIO del frame */
    uint16_t *vram_start;    /* NULL salvo gpu_capture_vram_start(1): VRAM REAL [512][1024] al INICIO de CADA lista (los eventos de la lista se aplican encima) */
    uint16_t *vram_end;      /* NULL salvo gpu_capture_vram_start(1): VRAM REAL al CERRAR la lista (incluye lo dibujado a VRAM durante ella: render a textura) */
    uint16_t *vram_after;    /* solo gpu_capture_enable(2): VRAM del oráculo al CERRAR (para verificar) */
} GpuDrawList;

/* 0 apaga y libera; 1 captura; 2 captura + copia de VRAM del oráculo al cerrar cada frame (verificación). */
void gpu_capture_enable(int mode);
/* Siguiente lista cerrada aún no entregada (FIFO), o NULL. Válida hasta la siguiente llamada. */
const GpuDrawList *gpu_capture_frame(void);
/* Con 1, cada lista trae vram_start (copia de 1 MB de la VRAM real al abrirla): consumidores que no pueden fiarse de reconstruir la VRAM solo con los eventos de la lista
 * (capa 2D: fuentes y CLUT subidas por DMA, copias o dibujos a VRAM). Vale para las listas que se abran a partir de la llamada. */
void gpu_capture_vram_start(int on);
/* VBlank del host (cierre de reserva para juegos sin doble búfer). */
void gpu_capture_vblank(void);
/* Listas perdidas por cola llena (la siguiente entregada trae vram_init). */
uint32_t gpu_capture_dropped(void);
/* Número de secuencia (GpuOp.seq) que tendrá la PRÓXIMA primitiva capturada: permite a un sustituto HLE delimitar qué
 * primitivas emitió una llamada (p.ej. un envío de lista al GPU): las que cumplan antes <= seq < después. */
uint32_t gpu_capture_seq(void);
/* GpuDrawList.index que tendrá la PRÓXIMA lista que se cierre (la que contendrá lo que el juego dibuje a partir de ahora): enlaza una escena capturada con su lista (NsScene.list_index = esto + 1). */
uint32_t gpu_capture_next_index(void);
/* Marcas en la lista de dibujo: un paquete GP0 con comando 0x04 (NOP en el hardware real y en el oráculo) y carga útil de
 * 24 bits `id` llama al gancho (si la captura está encendida) con el id y el número de secuencia que tendrá la PRÓXIMA primitiva
 * capturada. Permite a un sustituto HLE, que inserta paquetes de marca en la OT antes/después de lo que dibuja una función, delimitar
 * qué primitivas capturadas son suyas (rango de seq): el dibujo real ocurre más tarde, al recorrer la tabla de ordenación. Se llama en el hilo de emulación. */
typedef void (*GpuMarkHook)(uint32_t id, uint32_t seq);
void gpu_capture_set_mark_hook(GpuMarkHook h);

/* Enlace antiguo (solo float, sin vista) con la GTE; el actual es gpu_capture_hiprec / GpuHpHook (abajo). Dado el SXY entero que escribió el juego (sin offset, con signo), devuelve 1 y
 * rellena la posición float si el último RTPS/RTPT produjo exactamente ese SXY. La GTE (gte.c) mantendrá una
 * tabla pequeña (clave = par (sx,sy) saturado, valor = x,y float previos a saturar/redondear y SZ),
 * vaciada cada frame; si dos vértices comparten SXY gana el último. Este hook es el único acoplamiento. */
typedef int (*GpuFloatHook)(int sx, int sy, float *fx, float *fy, float *fz);
void gpu_capture_set_float_hook(GpuFloatHook h);

/* ====================== Geometría de alta precisión (sombra flotante de la GTE) ======================
 * gte.c mantiene, además del resultado entero exacto, una tabla por frame con la posición sin cuantizar de cada
 * vértice transformado (ver el bloque "Alta precisión" de gte.c). gpu_capture_hiprec(1) la enciende y engancha a
 * la captura: cada primitiva (polígono/línea) se asocia ENTERA con los vértices de la GTE por su (sx,sy) entero
 * (gte_hp_resolve; los saturados/detrás de la cámara se desambiguan por cercanía en la secuencia de salida de la GTE) y
 * cada vértice sale con:
 *   vvalid=1, vx,vy,vz (vista), vflags   si se encontró (también saturados / detrás de la cámara / derivados);
 *   fvalid=1, fx,fy,fz                   además, si la proyección del hardware es fiable (vflags sin SAT ni BEHIND).
 * CONTRATO PARA EL CONSUMIDOR 3D (escena VR):
 *   - polígono 3D = TODOS sus vértices vvalid (antes: todos fvalid). Los rects y los polígonos 2D (HUD, texto) salen con
 *     vvalid=0 (una coincidencia espuria de un vértice suelto es posible; exigir todos los vértices lo evita).
 *   - GPU_VF_BEHIND (vz <= 0): hay que recortar el polígono contra el plano cercano en la escena (vx,vy,vz sí valen).
 *   - GPU_VF_SAT: la posición de vista vale; fx,fy no se deben usar para dibujar en plano (los enteros x,y están recortados).
 *   - GPU_VF_AMBIG: baja confianza (ver gte.c); descartar el polígono si se prefiere no arriesgar posiciones erróneas.
 *   - GPU_VF_DERIVED: esquina de sprite sin GTE, vista aproximada a la profundidad media de las dos esquinas reales.
 *   - proj_h/proj_ox/proj_oy valen si algún vértice es vvalid. El cierre de frame de la captura vacía la tabla
 *     (gte_hp_frame). Con la alta precisión apagada (por defecto) no cambia nada del comportamiento observable. */
typedef struct {
    float fx, fy, fz;      /* pantalla sin cuantizar (sin offset de dibujo) y z de vista */
    float vx, vy, vz;      /* espacio de vista */
    float h, ofx, ofy;     /* proyección vigente: x = ofx + h*vx/vz */
    uint32_t seq;          /* orden de salida de la GTE (creciente, un número por vértice RTPS/RTPT) */
    uint8_t flags;         /* 0 normal; GPU_VF_SAT: fx,fy sin saturar (no coherentes con el SXY entero); GPU_VF_BEHIND: vz<=0 */
} GpuHpInfo;
typedef struct {
    uint32_t stored, evict, skipped, lookups, hits;
    uint32_t skip_sf, skip_sat, skip_ovf, skip_z; /* motivos de 'skipped' */
    uint32_t gen_stored, max_gen_stored;           /* almacenados en la generación en curso / máximo por generación */
    /* Desde la asociación por secuencia: skip_sat/skip_ovf/skip_z YA NO descartan; cuentan vértices registrados con flag
     * (SXY saturado o IR1/IR2/SZ saturados, división desbordada, detrás de la cámara). 'skipped' solo cuenta sf=0. */
    uint32_t flagged, ring_evict;                  /* vértices con flag registrados / sobrescritos por antigüedad en el anillo */
    uint32_t res_prims, res_amb, res_pivot;        /* gte_hp_resolve: primitivas, vértices con >1 candidato, primitivas sin ancla */
    uint32_t ev_verts, ev_ok, ev_amb, ev_amb_ok;   /* autoevaluación (gte_hp_eval): vértices evaluados / acertados / ambiguos / ambiguos acertados */
    uint32_t ev_hi, ev_hi_ok, ev_anc, ev_anc_ok;   /* ... de ellos: de confianza alta (sin AMBIG) / en primitivas con alguna ancla única */
    uint32_t res_addr, res_val;                    /* vértices de primitivas resueltos por DIRECCIÓN de memoria (hps.h) / por valor de SXY */
} GteHpStats;
typedef int (*GpuHpHook)(int sx, int sy, GpuHpInfo *out);
/* Asociación por primitiva (la que usa gpu_capture_hiprec): n SXY crudos (sin offset de dibujo) -> out[i], ok[i]=1 si se resolvió.
 * addr[i]/word[i] (addr NULL o addr[i]==0 = desconocida): dirección de memoria y palabra de la que la GPU leyó el vértice i; la sombra por
 * dirección del intérprete (hps.h) lo resuelve sin ambigüedad y, si no, se cae a la asociación por valor. */
typedef int (*GpuHpResolve)(int n, const int *sx, const int *sy, const uint32_t *addr, const uint32_t *word, GpuHpInfo *out, uint8_t *ok);
void gpu_capture_set_hp_hook(GpuHpHook h);
void gpu_capture_hiprec(int on);
/* gte.c */
void gte_hp_enable(int on);
int gte_hp_enabled(void);
void gte_hp_frame(void);
int gte_hp_lookup(int sx, int sy, GpuHpInfo *out);   /* solo vértices normales (flags==0), el más reciente */
int gte_hp_resolve(int n, const int *sx, const int *sy, GpuHpInfo *out, uint8_t *ok); /* ver gte.c; devuelve nº resueltos */
int gte_hp_resolve_a(int n, const int *sx, const int *sy, const uint32_t *addr, const uint32_t *word, GpuHpInfo *out, uint8_t *ok); /* idem, con la dirección de cada palabra de vértice */
int gte_hp_resolve_addr(uint32_t addr, uint32_t word, GpuHpInfo *out); /* una palabra, SOLO por dirección (RECT/sprites; sin asociación por valor): 1 si vale */
void gte_hp_eval(int on, int ex, int ey); /* autoevaluación de la heurística (solo medición; ver gte.c) */
void gte_hp_stats(GteHpStats *s);
