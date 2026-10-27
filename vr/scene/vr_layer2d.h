#pragma once
/* vr_layer2d: capa 2D (HUD, menús, textos, créditos, radio, fundidos) rasterizada con la aritmética EXACTA de la GPU original.
 *
 * Por qué existe. La capa 2D salía de los batches kind 2 de vr_scene dibujados por un shader (render/metal/vr_scene.metal) a 4x con filtro
 * bilineal, sin tramado, en 8 bits continuos, con rects y líneas convertidos a triángulos y los texels semitransparentes en dos pasadas por
 * batch (se pierde el orden entre primitivas). Y sobre un panel transparente de alfa premultiplicado no existe el modo B-F (los fundidos y
 * sombras sustractivos se perdían) ni el B+F real (aproximado). Todo eso hacía que textos, créditos y menús no coincidieran con el emulador.
 *
 * Qué hace. Reproduce EN ORDEN, a 1x y sobre una VRAM espejo (eventos de VRAM de las listas, como el renderer), las primitivas que vr_scene
 * mandó al panel (VrScene.op_dest == 2), con el mismo código que gpu.c: reglas de arista y relleno, interpolación entera, ventana de textura,
 * texels de 4/8/15 bits con CLUT, texel 0 transparente, bit 15 como interruptor de la semitransparencia, modulación (texel * color) >> 4,
 * tramado 4x4, canal de 5 bits, 4 modos de mezcla, máscara, recorte por primitiva y primitivas de realimentación ya resueltas (fbk/fbt).
 * El destino no es la VRAM sino una capa con transmitancia (lo que queda del mundo que hay debajo):
 *     composición = clamp( c + t * mundo - d )     (c = color acumulado en 5 bits, t = fracción de mundo que se ve 0..256, d = lo que se resta del mundo)
 * Con t = 0 (opaco) el píxel es el de la GPU original bit a bit. Sobre fondo negro (mundo = 0) la composición es la imagen de la GPU original
 * (pruebas: test_layer2d.c contra gpu.c). Mezcla: B/2+F/2 -> c=(c+F)>>1, t/=2; B+F -> c+=F; B-F -> c-=F si ya es opaco, si no d+=F; B+F/4 -> c+=F>>2.
 *
 * Salida: RGBA8 NO premultiplicado (como hudTexture) de la zona mostrada (disp.w x disp.h), filas de arriba abajo. Lo opaco es exacto (5 -> 8 bits como
 * la salida de vídeo); lo semitransparente se codifica como alfa premultiplicado equivalente (el panel se compone con "over"): el sustractivo sobre el
 * mundo se aproxima por oscurecimiento (1 - d); lo aditivo puro (alfa 0) como alfa = canal máximo. C99 puro, sin dependencias de plataforma. */
#include <stdint.h>
#include "gpu.h"
#include "vr_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t c[3];      /* color acumulado: 5 bits por canal con escala 1, 8 bits con escala > 1 */
    uint8_t m;         /* bit de máscara (bit 15) del último píxel escrito */
    uint16_t t;        /* transmitancia del mundo, 0 (opaco) .. 256 (sin cubrir) */
    uint16_t d[3];     /* resta pendiente sobre el mundo, en cuartos de nivel */
} VrLayerPx;

/* Suavizado del muestreo de texturas (y del vídeo ampliado): 0 original (vecino más próximo), 1 nítido (píxel art: corrección de diagonales tipo EPX/Scale2x
 * sobre el texel de VRAM), 2 suave (bilineal). Original (defecto) NO rasteriza a alta resolución: usa el rasterizado exacto de escala 1 (5 bits, tramado, el mismo
 * texel por píxel nativo que la GPU original) y replica cada píxel S x S en la salida RGBA (vecino más próximo a escala entera): texto idéntico en forma y color. Nítido y suave
 * rasterizan a la escala pedida y son experimentales (el reparto de UV por submuestra puede deformar glifos). vl2d_px devuelve la capa de RASTERIZADO (w*R x h*R, R = 1 en original). */
enum { VL_SMOOTH_ORIGINAL = 0, VL_SMOOTH_SHARP = 1, VL_SMOOTH_SOFT = 2 };

typedef struct VrLayer2D VrLayer2D;

/* Modo de vl2d_run (parámetro `full`): VL_PANEL = solo las primitivas que vr_scene mandó al panel (mundo VR / mundo debajo); VL_FULL = fotograma completo del emulador (pantalla de cine); VL_PANEL_VERIFIED = panel
 * encima de la pantalla 3D del motor (cinemáticas, créditos sobre una escena): igual que VL_PANEL pero se COMPARA con la imagen real del emulador (vram_end) y, donde difiere, se usa la del emulador
 * (ver verify_panel en vr_layer2d.c). Los valores 0 y 1 son los de siempre (`full` booleano). */
enum { VL_PANEL = 0, VL_FULL = 1, VL_PANEL_VERIFIED = 2 };

VrLayer2D *vl2d_new(void);
void vl2d_free(VrLayer2D *L);
/* Olvida la VRAM espejo: hasta la siguiente lista con vram_start/vram_init no hay capa (vl2d_run devuelve 0 y rgba NULL). */
void vl2d_reset(VrLayer2D *L);
/* Escala interna (1..6; defecto 4) y suavizado (VL_SMOOTH_*; defecto nítido). Escala 1 = rasterizado exacto de 5 bits con tramado (idéntico a la GPU original bit a bit);
 * escala > 1 = muestras de 8 bits sin tramado, geometría a alta resolución con el mismo relleno (muestreo en el centro de cada submuestra). */
void vl2d_set_options(VrLayer2D *L, int scale, int smooth);

/* VRAM de partida de cada lista: l->vram_start (la REAL del emulador al abrirla, gpu_capture_vram_start(1)) si viene; si no, la de la primera lista/resincronización (vram_init) con los eventos
 * de las anteriores. Con vram_start la capa no depende de que todas las escrituras de VRAM (DMA, copias, dibujos a VRAM) generen eventos.
 * Procesa UNA lista (todas, en orden, aunque la escena falle: sin vram_start la VRAM espejo debe ver todos los eventos). sc == NULL, o sin sc->op_dest, solo aplica los
 * eventos (salvo con `full`: el fotograma completo dibuja todas las primitivas y no necesita la clasificación de la escena). Con escena dibuja las primitivas con op_dest == 2 en su orden original, intercaladas con los eventos de VRAM; con `full` != 0 dibuja TODAS las
 * primitivas sobre el área mostrada de la VRAM espejo ampliada (pantalla de cine: el fotograma entero a alta resolución, vídeo incluido; la salida es opaca).
 * `l` debe ser la lista con la que se ensambló sc. Devuelve 1 si hay capa nueva (vl2d_rgba / vl2d_px válidos hasta la siguiente llamada), 0 si no. */
int vl2d_run(VrLayer2D *L, const GpuDrawList *l, const VrScene *sc, int full);

/* Menú sobre la mano (solo panel): con `on` != 0 vl2d_run busca la ventana entre las primitivas del panel que NO estaban en la última lista dibujada con `on` == 0 (créditos y HUD que el juego
 * dibuja durante la partida no cuentan; clave por primitiva: tipo, banderas, vértices RELATIVOS a la zona mostrada (el doble búfer del juego alterna x = 0 / 320) o rect, página de textura, CLUT y u,v).
 * Sin rect conservado, las nuevas se agrupan en componentes conexos (hueco < 8 px entre cajas) y la ventana es el de mayor área que tenga marco o fondo opaco (alguna primitiva sin textura ni
 * GPU_F_SEMI); si ninguno lo tiene, el de mayor área de todos; el resto (créditos, texto) se ignora. Mientras no está asentado se reelige en cada lista (una barra animada
 * de la primera no deja el rect clavado) y crece por unión; al asentarse el rect se congela. Con `on` == 0 cada lista dibujada renueva esa lista base.
 * vl2d_menu_rect (tras vl2d_run con capa): rect UV [u0, v0, u1, v1] (0..1, de la capa mostrada) de la ventana nueva; 0 si no hay base, nunca hubo primitivas nuevas o su área pasa del 60 % del marco.
 * Histéresis: mientras el menú siga activo el último rect válido se conserva (una lista sin primitivas nuevas no lo borra) y, mientras no está asentado solo crece con la elección de cada lista si toca (< 8 px) el rect anterior; asentado queda fijo;
 * vl2d_set_menu(0) lo olvida. */
void vl2d_set_menu(VrLayer2D *L, int on);
/* vl2d_hide_credits: con 1 la capa no pinta el texto de los créditos de la intro (líneas de DOS rectángulos texturados semitransparentes idénticos seguidos, abr 2 y abr 1, firma que no usan la vida, el radar ni los menús); el selector de armas/objetos lo activa para que los créditos no vayan en el panel de la ventana. */
void vl2d_hide_credits(VrLayer2D *L, int on);
int vl2d_menu_rect(const VrLayer2D *L, float uv[4]);
/* vl2d_menu_settled: 1 cuando el rect lleva 8 listas consecutivas SIN crecer (máximo 60 listas de crecimiento por apertura): ya no cambia hasta cerrar el selector (la ventana se abre con animación; antes crece por unión).
 * Los consumidores del visor solo usan el rect asentado. Las primitivas de la franja superior (v < 0,35: vida/oxígeno y créditos) nunca entran en el rect. */
int vl2d_menu_settled(const VrLayer2D *L);

/* Capa RGBA8 no premultiplicada de la última llamada con capa; w x h = (zona mostrada) * escala. NULL si no hay. */
const uint8_t *vl2d_rgba(const VrLayer2D *L, int *w, int *h);
/* Estado interno por píxel (pruebas): mismas dimensiones. NULL si no hay capa. */
const VrLayerPx *vl2d_px(const VrLayer2D *L, int *w, int *h);
/* VRAM espejo 1024x512 (pruebas). */
const uint16_t *vl2d_vram(const VrLayer2D *L);

/* Datos de la última ejecución para el log [capa2d]: marco (fw x h, aw visibles desde pad_l), escala de salida y de rasterizado, suavizado, si partió de vram_start / usó vram_end, y primitivas con «comprobar máscara». */
typedef struct {
    int fw, h, aw, pad, scale, raster, smooth, full, used_start, used_end, mask_prims, prims;
    int mirror_prims;      /* primitivas que solo actualizaron la VRAM espejo (destino fuera de la zona mostrada: render a textura, búfer alterno) */
    int fid_n, fid_bad;    /* pantalla completa a escala 1 con la VRAM real al cerrar (vram_end): píxeles visibles comparados y píxeles distintos de la imagen del emulador */
    int fid_mean_milli;    /* diferencia media por canal (niveles de 8 bits x 1000) de esa comparación */
    int fallback;          /* 1 = la capa no era fiel (fid_bad > 0, o en el panel verificado v_bad >= umbral / respaldo en curso) y se usó la imagen exacta del emulador (vram_end) */
    int verify_req;        /* se pidió VL_PANEL_VERIFIED */
    int verify;            /* 1 = el panel se verificó en esta lista (escala 1 original, con vram_end y clasificación) */
    int v_cov, v_bad;      /* píxeles que el panel toca / de ellos, los que no coinciden con el emulador al componerlos sobre el mundo (diferencia > 2 niveles de 5 bits) */
    int v_out;             /* píxeles que el panel NO toca y donde el mundo reproducido difiere del emulador (hueco del modelo, informativo) */
    int v_lost2d;          /* primitivas 2D texturadas sobre la pantalla que no fueron al panel ni al filtro (informativo: se pierden si el nativo no las dibuja) */
    int v_patched;         /* píxeles sustituidos (los malos; con respaldo, los tocados por el panel dilatados 1 px): opacos con el color del emulador o recuperados por resta */
    int v_sub, v_transp;   /* de ellos: recuperados por resta (emulador - mundo reproducido) / dejados transparentes por no ser fiable el mundo reproducido */
    int menu_new, menu_base;   /* ventana de menú sobre la mano: primitivas del panel NUEVAS respecto a la lista previa sin menú / primitivas de esa lista base (vl2d_set_menu) */
    double v_ms;           /* coste de la verificación (mundo reproducido + comparación), ms: el trabajador asíncrono lo descuenta de su medida de carga (la escala adaptativa no debe bajar por verificar) */
    int v_mean_milli;      /* diferencia media por canal (niveles de 8 bits x 1000) de los píxeles malos */
} VrL2Info;
void vl2d_info(const VrLayer2D *L, VrL2Info *out);

/* Medida de fidelidad (pruebas, herramientas de volcado): compara la capa RGBA (w x h, escala `scale`; el píxel nativo (x,y) se toma en (x*scale, y*scale)) con una imagen RGBA de referencia
 * rw x rh (el marco de la GPU o el frame.png de un volcado). mean = diferencia media absoluta por canal (0..255), pct = % de píxeles distintos, max = mayor diferencia de un canal. Devuelve 0 si
 * las dimensiones no encajan. */
typedef struct { double mean, pct; long n, bad; int max; } VrL2Diff;
int vl2d_compare(const uint8_t *rgba, int w, int h, int scale, const uint8_t *ref, int rw, int rh, VrL2Diff *out);

/* Composición de un píxel de la capa de ESCALA 1 sobre un mundo de 5 bits por canal (bg5) -> 5 bits (out5). */
void vl2d_compose5(const VrLayerPx *p, const uint8_t bg5[3], uint8_t out5[3]);
/* Codificación a RGBA8 no premultiplicado (la que usa vl2d_run). hq = 1 con color de 8 bits (escala > 1). */
void vl2d_encode(const VrLayerPx *p, int hq, uint8_t rgba[4]);

#ifdef __cplusplus
}
#endif
