/* wcut_tables: regiones de corte por modelo (ver wcut.h). SOLO parámetros: cajas orientadas, máscaras de
 * componente y rangos de UV; ni vértices ni texturas del juego. Marco: W_FRAME_HELD de wload CORREGIDO con
 * WCUT_FIX_UPRIGHT|WCUT_FIX_WINDING. Unidades: mm y grados (macros de wcut.h).
 *
 * Convenciones de las reglas (orden = orden de ejecución; cada una extrae del cuerpo restante salvo 'src'):
 *  - WCUT_WHOLE: el original ya tiene esa pieza como superficie propia (se toman sus triángulos enteros).
 *  - WCUT_CLIP: el original NO la tiene: se extrae el volumen de la caja (que debe CRUZAR la piel del cuerpo, si no
 *    no hay superficie que recortar) y se cierra con tapas; el hueco del cuerpo se rellena con 'cap_hole'.
 *  - Materiales de tapa: bloques 3x3 texels del propio atlas del modelo (D = el más oscuro, M = tono medio).
 *  - Ejes y recorridos (channel/axis/rotates) = los de las WPart del arma del registro (campo 'weapon' del modelo): una sola
 *    fuente de verdad; test_wcut comprueba la coincidencia con la tabla de piezas de cada WeaponDef.
 *  - AX(0, 100, 0) con translación = pieza "oculta": con el canal a 1 se desplaza 100 m en +Y, fuera de plano (así el
 *    arma esconde el supresor desmontado o el cartucho ausente sin tocar la malla).
 *  - Los nombres de pieza son los de mesh_names del arma (en inglés, igual que en w_guided.c). */
#include "wcut.h"
#include <string.h>

#define PV(x, y, z) {WCUT_MM(x), WCUT_MM(y), WCUT_MM(z)}
#define AX(x, y, z) {x, y, z}
#define MAT(x, y, w, h) WCUT_UVR(x, y, (x) + 3, (y) + 3, w, h)
#define BOXSEL(mode, cx, cy, cz, hx, hy, hz, rx, ry, rz) {1, WCUT_BOX(cx, cy, cz, hx, hy, hz, rx, ry, rz), mode, 0, 0, {0, 0, 0, 0}}
#define COMPSEL(mask) {0, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, WCUT_CLIP, mask, 0, {0, 0, 0, 0}}
#define CLIP(cx, cy, cz, hx, hy, hz) BOXSEL(WCUT_CLIP, cx, cy, cz, hx, hy, hz, 0, 0, 0)
#define WHOLE(cx, cy, cz, hx, hy, hz) BOXSEL(WCUT_WHOLE, cx, cy, cz, hx, hy, hz, 0, 0, 0)
#define CENTR(cx, cy, cz, hx, hy, hz) BOXSEL(WCUT_CENTROID, cx, cy, cz, hx, hy, hz, 0, 0, 0)
#define NOPIV {NAN, NAN, NAN}

#define NAMES(id, ...) static const char *const id[] = {__VA_ARGS__, NULL}
NAMES(N_PIST, "frame", "slide", "barrel", "supp", "trigger", "hammer", "mag", "round", "case");
NAMES(N_RIFLE, "body", "trigger", "selector", "mag", "charge", "bolt", "casing");
NAMES(N_SNIPER, "body", "stock", "trigger", "selector", "mag", "charge", "bolt", "lens", "casing");
NAMES(N_GUIDED, "tube", "grip", "trigger", "sight", "screen", "frame", "lid", "round");
NAMES(N_MISSILE, "round", "fins");
NAMES(N_AA, "body", "tube", "trigger", "battery", "antenna", "sight_rear", "bezel", "screen");
NAMES(N_THROW, "body", "fuze", "lever", "pin");
NAMES(N_C4, "block", "adh", "det", "ant");
NAMES(N_MINE, "body", "plate", "sight", "legl", "legr");

/* ---------------------------------------------------------------- pistola con supresor (atlas 128x32) y sin él
 * 36f6 = 21b6 + tubo (componente 1). Cuerpo: losa cerrada de 36 triángulos con el dibujo lateral pintado en la textura. */
#define PW 128
#define PH 32
#define PD MAT(69, 12, PW, PH)
#define PM MAT(69, 20, PW, PH)
/* eje del cargador = el de w_pist = eje del mango medido en el modelo: la pared delantera del mango sube con pendiente
 * 0,327 (18,1 grados) y la trasera con 0,403 (22,0), así que el eje que las reparte (cizalla del cargador) es de 20 grados:
 * UY/UZ = (cos, -sin) de 20 grados */
#define MAG_UY 0.939693f
#define MAG_UZ -0.342020f
/* materiales del cargador (bloques 3x3 del propio atlas: el cuerpo es casi negro como el mango, gris claro para la vaina,
 * gris medio para la bala; el atlas de la pistola no tiene latón ni cobre). Cuerpo: bloque del relleno oscuro del propio
 * atlas, el mismo que pinta el mango original (luminancia 7/255 = percentil 5-10 de los texels del mango, muy por debajo
 * del tope de 28; el atlas es neutro ahi, sin tinte); test_wcut lo comprueba contra los texels del mango. */
#define MAG_BODY MAT(30, 10, PW, PH)
#define MAG_CASE MAT(50, 0, PW, PH)
#define MAG_BULLET MAT(37, 16, PW, PH)
#define PIST_SLIDE \
    {.name = "slide", .sel = CLIP(0, 68, -125, 30, 21.8f, 120), .cap_piece = PD, .cap_hole = PD, .pivot = PV(0, 0, 0), \
     .channel = WCH_SLIDE, .axis = AX(0, 0, 1), .note = "y >= 46,2 mm (junta de la textura); recorre +Z"}
/* Cañón generado (el original no lo tiene), todo proporcional a la corredera (ancho 22 mm = SLIDE_W; ver WCutTube):
 *  - pared de la corredera = 12 % del ancho (2,64 mm); ancho interior 16,72 mm; techo interior 69,36 mm (cara superior 72);
 *  - eje a y = 63 mm: el del asiento del supresor (centro de su sección trasera; w_pist PIST_BY); radio exterior = techo
 *    interior - eje - 2 holguras (0,3 mm) = 5,76 mm (diámetro 11,5 mm: el máximo que cabe bajo el techo), canal de la
 *    corredera de 6,06 mm;
 *  - ánima = 13 % del ancho (radio 2,86 mm), el calibre del cartucho del cargador (MAG_BORE_FRAC);
 *  - la boca asoma 12 % del ancho (2,64 mm) por delante de la corredera: ahí se enrosca el supresor, que cuelga del cañón;
 *  - recámara: capuchón rectangular sobre el cartucho del cargador (culote y largo del cartucho), con su rampa; la
 *    corredera gana una cavidad que cubre los 42 mm de recorrido y una ventana lateral derecha por la que se ve. */
#define SLIDE_W 22.0f
#define WALL_FRAC 0.12f
#define BORE_FRAC 0.13f
#define AXIS_Y 63.0f
#define PROT_MM (WALL_FRAC * SLIDE_W)
#define SLIDE_TRAVEL_MM 42.0f
#define BARREL_METAL MAT(68, 0, PW, PH)   /* bloque 3x3 uniforme del atlas, luminancia 16/255: negro algo mas claro que el cargador (7) */
#define BARREL_BORE MAT(0, 0, PW, PH)    /* luminancia 8/255 */
#define PIST_BARREL \
    {.name = "barrel", .pivot = PV(0, 0, 0), .cap_piece = BARREL_METAL, .cap_hole = MAG_BODY, \
     .tube = {.on = 1, .ref = "slide", .feed = "mag", .axis_y = WCUT_MM(AXIS_Y), .wall_frac = WALL_FRAC, .bore_frac = BORE_FRAC, \
              .prot_frac = WALL_FRAC, .gap = WCUT_MM(0.3f), .travel = WCUT_MM(SLIDE_TRAVEL_MM), .segs = 24, .mat_bore = BARREL_BORE}, \
     .note = "tubo con anima + capuchon de recamara generados en una cavidad de la corredera; fijo al marco (el supresor cuelga de el)"}
/* cargador: la pieza extraida es la franja de 7 mm del fondo del mango (piel original, a ras del fondo = la base visible con el
 * cargador insertado); sobre su cara superior se genera el cargador (ver WCutMag): cuerpo con la seccion del mango inset por
 * la pared, cizallado a lo largo del eje, hasta bajo la corredera, labios y bala arriba. Queda oculto dentro del mango. */
#define PIST_MAG \
    {.name = "mag", .sel = CLIP(0, -40.5f, -56.5f, 15, 11.5f, 30), .cap_piece = PM, .cap_hole = PD, \
     .pivot = PV(0, -29, -59), .channel = WCH_MAG, .axis = AX(0, -MAG_UY, -MAG_UZ), \
     .mag = {.on = 1, .slide = "slide", .bore_frac = BORE_FRAC, .dir = {0, MAG_UY, MAG_UZ}, .clear = WCUT_MM(0.8f), .wall_frac = 0.13f, \
             .lip_frac = 0.10f, .lip_keep = 0.75f, .rec_frac = 0.9f, .case_frac = 1.1f, .case_len = 1.9f, .bullet_len = 1.1f, \
             .mat_body = MAG_BODY, .mat_case = MAG_CASE, .mat_bullet = MAG_BULLET}, \
     .note = "base del fondo del mango + cargador completo generado (cuerpo, labios, ranura y cartucho)"}
#define PIST_TRIGGER \
    {.name = "trigger", .sel = CLIP(0, 6, -92, 3, 6, 6), .cap_piece = PM, .cap_hole = PD, .pivot = PV(0, 13, -92), \
     .channel = WCH_TRIGGER, .axis = AX(-0.35f, 0, 0), .rotates = 1, .note = "volumen que cruza la cara frontal de la empuñadura"}
/* cartucho (round) y vaina (case): copias pequeñas de la pared del prisma interior del cargador (mismo material). */
#define PIST_ROUND \
    {.name = "round", .src = "mag", .sel = CLIP(8.8f, -20, -61.4f, 2.5f, 5, 2.5f), .flags = WCUT_COPY, .cap_piece = PM, .cap_hole = PD, \
     .pivot = NOPIV, .channel = WCH_ROUND, .axis = AX(0, 100, 0), .note = "tramo 5x10x5 mm de la pared del prisma del cargador"}
#define PIST_CASE \
    {.name = "case", .src = "mag", .sel = CLIP(8.8f, -8, -62.5f, 2.5f, 4, 2.5f), .flags = WCUT_COPY, .cap_piece = PM, .cap_hole = PD, \
     .pivot = NOPIV, .note = "vaina: tramo 5x8x5 mm del prisma del cargador"}
static const WCutRule R_PIST[] = {
    /* supresor: componente conexa independiente; se cierra su extremo trasero abierto (CAP_OPEN). Se acopla al
     * extremo del cañón (rule 'barrel'): su asiento trasero es la boca del cañón, que asoma PROT_MM de la corredera. */
    {.name = "supp", .sel = COMPSEL(2), .flags = WCUT_CAP_OPEN, .cap_piece = PM, .cap_hole = PD, .pivot = PV(0, AXIS_Y, -217 - PROT_MM),
     .shift = {0, 0, -WCUT_MM(PROT_MM)}, .parent = "barrel",
     .channel = WCH_CUSTOM0, .axis = AX(0, 100, 0), .note = "tubo original (comp. 1) desplazado la parte de cañon que asoma; cuelga del cañon; pivote en su asiento trasero = boca"},
    PIST_SLIDE, PIST_MAG, PIST_BARREL, PIST_TRIGGER, PIST_ROUND, PIST_CASE,
};
static const WCutRule R_PIST_BARE[] = {  /* 21b6: igual sin la regla del supresor */
    PIST_SLIDE, PIST_MAG, PIST_BARREL, PIST_TRIGGER, PIST_ROUND, PIST_CASE,
};

/* ---------------------------------------------------------------- fusil (atlas 128x64) */
#define RD MAT(26, 6, 128, 64)
#define RM MAT(37, 3, 128, 64)
static const WCutRule R_RIFLE[] = {
    /* cargador: caja propia en el original (triángulos enteros); se cierra su cara superior abierta. */
    {.name = "mag", .sel = WHOLE(0, -14, 64, 20, 52, 34), .flags = WCUT_CAP_OPEN, .cap_piece = RM, .cap_hole = RD, .pivot = PV(0, 36, 64),
     .channel = WCH_MAG, .axis = AX(0, -0.15f, 0), .note = "caja original bajo el cuerpo"},
    /* gatillo: prisma del bloque de empuñadura que cruza su cara inferior. */
    {.name = "trigger", .sel = CLIP(0, -26, -83, 3, 14, 7), .cap_piece = RM, .cap_hole = RD, .pivot = PV(0, -14, -83),
     .channel = WCH_TRIGGER, .axis = AX(-0.40f, 0, 0), .rotates = 1, .note = "volumen que cruza la base del bloque de empuñadura"},
    /* selector: botón en la pared lateral derecha. */
    {.name = "selector", .sel = CLIP(18, 60, -45, 4, 6, 8), .cap_piece = RM, .cap_hole = RD, .pivot = NOPIV,
     .channel = WCH_SAFETY, .axis = AX(1.1f, 0, 0), .rotates = 1, .note = "prisma de la pared lateral derecha"},
    /* palanca de montaje: prisma en la cara superior. */
    {.name = "charge", .sel = CLIP(0, 164, -80, 5, 8, 14), .cap_piece = RM, .cap_hole = RD, .pivot = NOPIV,
     .channel = WCH_CUSTOM0, .axis = AX(0, 0, 0.065f), .note = "prisma de la cara superior; recorre +Z"},
    /* cerrojo: placa en la ventana lateral (dibujada en la textura); al retroceder deja ver la tapa oscura. */
    {.name = "bolt", .sel = CLIP(18, 130, -200, 4, 14, 30), .cap_piece = RM, .cap_hole = RD, .pivot = NOPIV,
     .channel = WCH_BOLT, .axis = AX(0, 0, 0.05f), .note = "placa de la ventana de expulsión; recorre +Z"},
    {.name = "casing", .src = "mag", .sel = CLIP(0, -60, 64, 3, 6, 5), .flags = WCUT_COPY, .cap_piece = RM, .cap_hole = RD, .pivot = NOPIV,
     .note = "vaina: copia de un tramo de 6x12x10 mm del cargador"},
};

/* ---------------------------------------------------------------- francotirador (atlas 256x64) */
#define SD MAT(95, 22, 256, 64)
#define SM MAT(140, 18, 256, 64)
static const WCutRule R_SNIPER[] = {
    {.name = "lens", .sel = WHOLE(0, 130, 74, 25, 16, 2), .flags = WCUT_CAP_OPEN, .cap_piece = SD, .cap_hole = SD, .pivot = NOPIV,
     .channel = WCH_SCREEN, .note = "cara trasera de la mira (2 triángulos); su hueco se rellena oscuro"},
    {.name = "mag", .sel = CLIP(0, 24, -130, 12, 24, 14), .cap_piece = SM, .cap_hole = SD, .pivot = PV(0, 8, -130),
     .channel = WCH_MAG, .axis = AX(0, -0.16f, 0), .note = "pozo extraído del volumen del guardamanos (cruza su cara inferior)"},
    {.name = "trigger", .sel = CLIP(0, 0, -88, 3, 12, 6), .cap_piece = SM, .cap_hole = SD, .pivot = PV(0, 12, -88),
     .channel = WCH_TRIGGER, .axis = AX(-0.40f, 0, 0), .rotates = 1, .note = "cara frontal de la empuñadura"},
    {.name = "selector", .sel = CLIP(18, 70, -20, 4, 6, 8), .cap_piece = SM, .cap_hole = SD, .pivot = NOPIV,
     .channel = WCH_SAFETY, .axis = AX(0.9f, 0, 0), .rotates = 1},
    {.name = "bolt", .sel = CLIP(18, 85, 30, 5, 5, 18), .cap_piece = SM, .cap_hole = SD, .pivot = NOPIV,
     .channel = WCH_BOLT, .axis = AX(0, 0, 0.05f), .note = "mango del cerrojo en la pared derecha"},
    {.name = "charge", .sel = CLIP(0, 107, 150, 5, 6, 14), .cap_piece = SM, .cap_hole = SD, .pivot = NOPIV,
     .channel = WCH_CUSTOM0, .axis = AX(0, 0, 0.06f), .note = "prisma de la cara superior de la culata"},
    {.name = "casing", .src = "mag", .sel = CLIP(0, 8, -130, 3, 5, 5), .flags = WCUT_COPY, .cap_piece = SM, .cap_hole = SD, .pivot = NOPIV},
};

/* ---------------------------------------------------------------- lanzador guiado (atlas 128x128) */
#define GD MAT(108, 41, 128, 128)
#define GM MAT(11, 52, 128, 128)
static const WCutRule R_GUIDED[] = {
    {.name = "grip", .sel = WHOLE(0, -9, -102, 33, 56, 100), .flags = WCUT_CAP_OPEN, .cap_piece = GM, .cap_hole = GD, .pivot = NOPIV,
     .note = "cuña inferior original"},
    {.name = "sight", .sel = WHOLE(-20, 189, -362, 14, 44, 100), .flags = WCUT_CAP_OPEN, .cap_piece = GM, .cap_hole = GD, .pivot = NOPIV,
     .note = "soporte superior original"},
    {.name = "screen", .sel = WHOLE(-75, 171, -268, 50, 3, 190), .flags = WCUT_CAP_OPEN, .cap_piece = GM, .cap_hole = GD, .pivot = NOPIV,
     .channel = WCH_SCREEN, .note = "cara superior de la caja de electrónica (2 triángulos; la app debe fijar sus UV 0..1)"},
    {.name = "trigger", .src = "grip", .sel = CLIP(0, -30, -172, 4, 20, 13), .cap_piece = GM, .cap_hole = GD, .pivot = PV(0, -10, -160),
     .channel = WCH_TRIGGER, .axis = AX(-0.50f, 0, 0), .rotates = 1, .parent = "grip"},
    /* tapa trasera: losa de 28 mm del extremo posterior del tubo, con bisagra en el lado derecho. */
    {.name = "lid", .sel = CLIP(0, 97, 535, 80, 70, 14), .cap_piece = GM, .cap_hole = GD, .pivot = PV(58, 97, 543),
     .channel = WCH_LID, .axis = AX(0, 2.3f, 0), .rotates = 1, .note = "losa trasera; el misil (modelo aparte, pieza round del lanzador) entra por el hueco"},
};
/* misiles: cuerpo y aletas (componentes) */
#define MD MAT(38, 4, 128, 32)
static const WCutRule R_MISSILE_G[] = {
    {.name = "fins", .sel = COMPSEL(0x1E), .flags = WCUT_CAP_OPEN, .cap_piece = MD, .cap_hole = MD, .pivot = NOPIV, .note = "4 aletas = componentes 1..4"},
};
#define M2D MAT(16, 15, 64, 32)
static const WCutRule R_MISSILE_A[] = {
    {.name = "fins", .sel = COMPSEL(0x3C), .flags = WCUT_CAP_OPEN, .cap_piece = M2D, .cap_hole = M2D, .pivot = NOPIV, .note = "4 aletas = componentes 2..5"},
};

/* ---------------------------------------------------------------- lanzador antiaéreo (atlas 256x64) */
#define AD MAT(30, 2, 256, 64)
#define AM MAT(65, 21, 256, 64)
static const WCutRule R_AA[] = {
    {.name = "antenna", .sel = WHOLE(66, 125, -174, 70, 100, 125), .flags = WCUT_CAP_OPEN, .cap_piece = AM, .cap_hole = AD, .pivot = PV(-2, 111, -174),
     .channel = WCH_LID, .axis = AX(1.5708f, 0, 0), .rotates = 1, .note = "rejilla (componente 0) + placa base"},
    {.name = "screen", .sel = CENTR(-70, 185, -54, 62, 20, 4), .flags = WCUT_CAP_OPEN, .cap_piece = AM, .cap_hole = AD, .pivot = NOPIV,
     .channel = WCH_SCREEN, .note = "cara trasera de la mira"},
    {.name = "sight_rear", .sel = WHOLE(-70, 170, -174, 62, 32, 125), .flags = WCUT_CAP_OPEN, .cap_piece = AM, .cap_hole = AD, .pivot = PV(-70, 142, -54),
     .channel = WCH_SAFETY, .axis = AX(-1.5708f, 0, 0), .rotates = 1},
    {.name = "tube", .sel = WHOLE(0, 87, 240, 50, 56, 700), .flags = WCUT_CAP_OPEN, .cap_piece = AM, .cap_hole = AD, .pivot = PV(0, 87, 240),
     .channel = WCH_MAG, .axis = AX(0, 0, 0.30f), .note = "tubo hexagonal completo; desliza +Z"},
    {.name = "battery", .sel = CLIP(0, -27, -120, 30, 49, 40), .cap_piece = AM, .cap_hole = AD, .pivot = NOPIV, .note = "volumen que cruza la base del grupo de empuñadura"},
    {.name = "trigger", .sel = CLIP(0, -45, -250, 4, 20, 10), .cap_piece = AM, .cap_hole = AD, .pivot = PV(0, -25, -250),
     .channel = WCH_TRIGGER, .axis = AX(-0.35f, 0, 0), .rotates = 1},
};

/* ---------------------------------------------------------------- granadas (atlas 64x64 y 64x32) */
#define TD MAT(19, 10, 64, 64)
#define TM MAT(8, 19, 64, 64)
static const WCutRule R_GRENADE[] = {
    {.name = "lever", .sel = WHOLE(-80, 57, -64, 30, 47, 16), .flags = WCUT_CAP_OPEN, .cap_piece = TM, .cap_hole = TD, .pivot = PV(-60, 100, -64),
     .channel = WCH_CUSTOM1, .axis = AX(0, 0, 1.35f), .rotates = 1, .note = "palanca = superficie propia (10 triángulos)"},
    {.name = "fuze", .sel = WHOLE(-38, 78.5f, -64, 31, 25, 16), .flags = WCUT_CAP_OPEN, .cap_piece = TM, .cap_hole = TD, .pivot = NOPIV,
     .note = "cuello (8 triángulos)"},
    /* anilla/pasador: copia de la punta de la palanca (mismo material). */
    {.name = "pin", .src = "lever", .sel = CLIP(-100, 25, -64, 12, 16, 20), .flags = WCUT_COPY, .cap_piece = TM, .cap_hole = TD, .pivot = NOPIV,
     .channel = WCH_CUSTOM0, .axis = AX(0, 0, 0.045f), .note = "copia de la punta de la palanca"},
};
#define KD MAT(28, 7, 64, 32)
#define KM MAT(14, 17, 64, 32)
static const WCutRule R_CANISTER[] = {
    {.name = "fuze", .sel = CLIP(0, 0, -38, 30, 30, 8), .cap_piece = KM, .cap_hole = KD, .pivot = NOPIV, .note = "losa del extremo -Z (tapa texturada)"},
    {.name = "lever", .sel = CLIP(0, 26, 0, 12, 6, 28), .cap_piece = KM, .cap_hole = KD, .pivot = PV(0, 26, -26),
     .channel = WCH_CUSTOM1, .axis = AX(0, 0, 1.35f), .rotates = 1, .note = "losa de la cara superior"},
    {.name = "pin", .src = "lever", .sel = CLIP(0, 26, -18, 5, 6, 5), .flags = WCUT_COPY, .cap_piece = KM, .cap_hole = KD, .pivot = NOPIV,
     .channel = WCH_CUSTOM0, .axis = AX(0, 0, 0.045f)},
};

/* ---------------------------------------------------------------- explosivo adhesivo y mina (atlas 64x32 / 64x128) */
static const WCutRule R_C4[] = {
    {.name = "ant", .sel = WHOLE(-41.5f, 120, -105.5f, 9, 55, 9), .flags = WCUT_CAP_OPEN, .cap_piece = KM, .cap_hole = KD, .pivot = PV(-41.5f, 70, -105.5f),
     .channel = WCH_CUSTOM1, .axis = AX(0, 0.03f, 0), .note = "antena = prisma propio (10 triángulos)"},
    {.name = "adh", .sel = CLIP(-42, -75, -106, 60, 8, 80), .cap_piece = KM, .cap_hole = KD, .pivot = NOPIV, .note = "losa inferior de 12 mm (cara adhesiva)"},
    {.name = "det", .sel = CLIP(-42, 20, -58, 16, 14, 6), .cap_piece = KM, .cap_hole = KD, .pivot = NOPIV, .note = "detonador: placa de la cara trasera"},
};
#define ND MAT(5, 70, 64, 128)
#define NM MAT(17, 8, 64, 128)
static const WCutRule R_MINE[] = {
    {.name = "legl", .sel = {1, WCUT_BOX(-130, 0, 0, 100, 400, 400, 0, 0, 0), WCUT_CLIP, 2, 0, {0, 0, 0, 0}}, .cap_piece = NM, .cap_hole = ND,
     .pivot = PV(-61.5f, -99, -207), .channel = WCH_CUSTOM0, .axis = AX(0, 0, 1.25f), .rotates = 1, .note = "pata izquierda = mitad de la componente de patas"},
    {.name = "legr", .sel = COMPSEL(2), .cap_piece = NM, .cap_hole = ND,
     .pivot = PV(0.5f, -99, -207), .channel = WCH_CUSTOM0, .axis = AX(0, 0, -1.25f), .rotates = 1, .note = "pata derecha = lo que queda de la componente 1 (patas) tras sacar la izquierda, incluido el relleno del hueco (hereda la componente)"},
    {.name = "plate", .sel = CLIP(24, 0, -207, 10, 120, 200), .cap_piece = NM, .cap_hole = ND, .pivot = NOPIV, .note = "placa frontal: losa de 20 mm de la cara +X"},
    {.name = "sight", .sel = CLIP(-18, 100, -207, 8, 6, 30), .cap_piece = NM, .cap_hole = ND, .pivot = NOPIV, .note = "alza: losa de la cara superior"},
};

/* Sellados (ver WCutSeal): contornos abiertos heredados del original que dejan el cuerpo sin cerrar. */
static const WCutSeal S_RIFLE[] = {
    {.piece = "body", .mat = RD, .note = "boca hueca del cañon (4 aristas): tapa oscura = ánima"},
};
static const WCutSeal S_SNIPER[] = {
    {.piece = "body", .mat = SD, .note = "boca hueca del cañon (4 aristas): tapa oscura = ánima"},
};
static const WCutSeal S_GUIDED[] = {
    {.piece = "tube", .mat = GD, .note = "hueco del vientre del tubo (6 aristas) que ocupa la cuña: parche oscuro"},
};

#define FIX (WCUT_FIX_UPRIGHT | WCUT_FIX_WINDING)
#define NR(a) ((int)(sizeof(a) / sizeof(*(a))))
/* src_tris y bounds (mm, marco corregido) identifican el modelo: wcut_run devuelve -4 si la tabla se aplica a otro. */
#define SEALS(a) .seals = a, .nseals = NR(a)
#define MODEL(ID, TAG, BODY, NAMES_, RULES, WEAPON, TRIS, BB, NOTE, ...) \
    {.id = ID, .tag = TAG, .fix = FIX, .body = BODY, .names = NAMES_, .rules = RULES, .nrules = NR(RULES), .note = NOTE, \
     .weapon = WEAPON, .src_tris = TRIS, .bounds = BB, ##__VA_ARGS__}
static const WCutModel MODELS[] = {
    MODEL(0x36f6, "pist_supp", "frame", N_PIST, R_PIST, "pist", 52, WCUT_BB(-13, -36, -371, 13, 75, -34), "pistola con supresor (el cuerpo desnudo es el mismo que 21b6)"),
    MODEL(0x21b6, "pist", "frame", N_PIST, R_PIST_BARE, "pist", 36, WCUT_BB(-11, -36, -217, 11, 72, -34), "pistola sin supresor"),
    MODEL(0x4725, "rifle", "body", N_RIFLE, R_RIFLE, "rifle", 70, WCUT_BB(-18, -64, -486, 18, 166, 251), "fusil bullpup; el cañón (componente 0) queda con el cuerpo", SEALS(S_RIFLE)),
    MODEL(0x2d3b, "sniper", "body", N_SNIPER, R_SNIPER, "sniper", 106, WCUT_BB(-22, -46, -782, 22, 162, 271), "rifle de cerrojo; mira y culata quedan en el cuerpo", SEALS(S_SNIPER)),
    MODEL(0x15a9, "guided", "tube", N_GUIDED, R_GUIDED, "guided", 81, WCUT_BB(-120, -63, -456, 60, 229, 543), "lanzador guiado; el misil es el modelo 76ab", SEALS(S_GUIDED)),
    MODEL(0x76ab, "guided_missile", "round", N_MISSILE, R_MISSILE_G, NULL, 66, WCUT_BB(-62, -63, -318, 62, 63, 405), "misil del lanzador guiado (= pieza 'round' del lanzador)"),
    MODEL(0x57f8, "aa", "body", N_AA, R_AA, "aa", 88, WCUT_BB(-130, -62, -447, 134, 229, 927), "lanzador antiaéreo; el misil es 9a90"),
    MODEL(0x9a90, "aa_missile", "round", N_MISSILE, R_MISSILE_A, NULL, 70, WCUT_BB(-109, -109, -108, 109, 109, 295), "misil antiaéreo"),
    MODEL(0x3b88, "throw_frag", "body", N_THROW, R_GRENADE, "throw_frag", 36, WCUT_BB(-108, -37, -109, 11, 102, -19), "granada (dudosa)"),
    MODEL(0x761a, "throw_can", "body", N_THROW, R_CANISTER, "throw_c", 44, WCUT_BB(-23, -27, -44, 23, 27, 39), "granada cilíndrica (dudosa)"),
    MODEL(0xf83d, "placed_c4", "block", N_C4, R_C4, "placed_a", 28, WCUT_BB(-92, -79, -156, 8, 170, -56), "explosivo adhesivo"),
    MODEL(0x17a1, "placed_mine", "body", N_MINE, R_MINE, "placed_b", 32, WCUT_BB(-80, -224, -370, 31, 100, -45), "mina direccional"),
};
const WCutModel *wcut_find(uint16_t id) {
    for (int i = 0; i < NR(MODELS); i++) if (MODELS[i].id == id) return &MODELS[i];
    return NULL;
}
int wcut_model_count(void) { return NR(MODELS); }
const WCutModel *wcut_model_at(int i) { return i >= 0 && i < NR(MODELS) ? &MODELS[i] : NULL; }
