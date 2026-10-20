#pragma once
/* ns_types: CONTRATO de la escena nativa (renderer nativo). Qué se extrae en cada fotograma de juego de la RAM del motor y cómo se entrega al renderer.
 * C99 puro, sin dependencias de plataforma ni del runtime; el mismo fichero lo ven el extractor (hilo de emulación), el puente a Swift y las herramientas
 * de escritorio. Contexto y origen de cada campo: docs/escena_nativa.md (léelo primero). Versión 2 (ver NS_VERSION y la sección «Cambios» de la bitácora).
 *
 * Idea
 *   El motor mantiene en RAM una LISTA DE OBJETOS por canal (modelos con matriz de mundo; el escenario es una lista de objetos del mismo tipo) y una lista
 *   de PRIMITIVAS de efectos con coordenadas de mundo. Una vez por fotograma de juego, justo después de que el motor calcule todas las matrices de mundo,
 *   el extractor lee esa lista y produce una NsScene: instancias (qué mallas, dónde, con qué luz) + mallas (cacheables, se envían una vez) + luces +
 *   efectos + cámara del juego. El renderer la dibuja con la cámara VR a la frecuencia del visor. La lógica, la IA y el sonido siguen siendo del juego; el
 *   HUD y los menús 2D siguen saliendo de la lista de la GPU (vr_scene, clase 2/3).
 *
 * Unidades y ejes (iguales a los del puente y vr_scene)
 *   Todo va en UNIDADES DEL MOTOR (enteros del juego pasados a float donde se indica) y en el MUNDO DEL MOTOR: **Y ARRIBA, mano derecha** (verificado: el suelo
 *   está en y ~ 0, la cabeza de un personaje por encima de sus pies, la cámara por encima mirando hacia -Y, y la rotación vista->mundo tiene determinante +1),
 *   1 unidad = 1 mm nominal (NsScene.units_per_m = 1000). NO se convierte a metros ni a espacio del usuario en el extractor: NsScene.to_vr (3x4) lleva del
 *   mundo del motor al mundo VR con la MISMA regla que vr_scene:  v_vr = S * Mmap * (v - origen_juego),  S = 1/units_per_m,  Mmap = identidad (el mundo del
 *   motor ya es Y arriba y derecho; sólo sería un espejo en Z si det(R_ojo) < 0, opts.mirror_z), origen_juego = el origen que use vr_scene (suelo bajo el
 *   jugador o fijo). El renderer aplica to_vr * (world_pieza * v_local) y después la pose de la cabeza (vrcam_delta / usermap). NO hay inversión de Y.
 *   Ángulos del motor: 4096 = vuelta. Matrices del motor: rotación 3x3 s16 con 4096 = 1,0 (puede llevar escala, p. ej. una sombra aplanada; se pasa tal cual).
 *
 * Convenio de normales y luces (importante para sombrear)
 *   Las normales almacenadas de un modelo apuntan HACIA EL LADO NO VISIBLE de la cara (son «interiores»: coinciden con cross(B-A, D-A) en el orden de
 *   vértices del motor y el anverso es el de la prueba de orientación de §3.4 del documento). Las filas de luz son la dirección en que VIAJA la luz (una luz cenital
 * viaja hacia -Y). Por eso la intensidad es dot(dir_luz, normal_almacenada) sin cambio de signo: un suelo (normal almacenada hacia
 *   -Y) recibe la luz que cae. ns_shade_normal() implementa exactamente esa fórmula; NO invertir ningún signo.
 *
 * Entrega (triple búfer + almacén de mallas con liberación diferida) — PROTOCOLO OBLIGATORIO
 *   1. Las MALLAS son inmutables y viven en un NsMeshStore del productor organizado en ARENAS por época (mesh_epoch). Una NsScene sólo referencia mallas
 *      por id (u64) y por tanto es pequeña. id = hash de 64 bits del CONTENIDO GEOMÉTRICO (ns_mesh_content_hash: vértices, normales y caras con UV/tpage/clut,
 *      campo a campo, sin padding ni punteros). El color precalculado NO entra en el id: va aparte como juego de colores versionado (NsBaked, ns_store_get_baked).
 *   2. El hash se calcula UNA vez por (dirección de la definición, zone_gen) y se guarda en una tabla dirección -> id; los fotogramas siguientes sólo
 *      comprueban que la definición no cambió (zone_gen) y reutilizan el id. zone_gen sube cuando el motor libera/recarga definiciones (cambio de zona).
 *   3. LIBERACIÓN DIFERIDA CON DOS GENERACIONES: como mucho hay dos arenas vivas (época e y e-1). El productor abre la época e+1 (al vaciar por cambio de
 *      zona) sólo cuando ya se liberó la e-1, y la e-1 se libera SÓLO cuando el consumidor llamó a ns_store_ack(st, e): «no volveré a tocar mallas de época < e».
 *      Mientras el consumidor no reconozca, el productor sigue añadiendo a la época actual (no hay corte). Un puntero de ns_store_get() es válido hasta
 *      que el consumidor reconoce una época mayor que NsMesh.epoch.
 *   4. ORDEN DE PUBLICACIÓN: el productor (a) añade al almacén TODAS las mallas y juegos de colores que la escena referencia, (b) rellena el slot libre,
 *      (c) lo publica con ns_pub_publish() (ns_pub.h), que hace el intercambio con semántica release; el consumidor toma con ns_pub_acquire() (acquire).
 *      Así todo lo que el productor escribió antes (mallas incluidas) es visible para el consumidor al ver la escena.
 *   5. Cada slot lleva un CONTADOR DE GENERACIÓN (NsScene.slot_gen, creciente por slot): el consumidor puede comprobar antes y después de copiar que no cambió.
 *      La entrega es de TRIPLE búfer sin bloqueos: ni el productor espera al consumidor ni al revés; se pueden saltar escenas sin perder datos.
 *   - `seq` crece de 1 en 1 por escena publicada; `game_frame` es el fotograma del motor. Nada de punteros a la RAM del juego dentro de la escena.
 *
 * Capacidades mínimas y truncado
 *   Las capacidades de NsScene deben ser >= NS_MIN_INST / NS_MIN_PIECE / NS_MIN_LIGHT / NS_MIN_EFFECT (valen para una escena sin ningún recorte de visibilidad).
 *   Si algo no cabe: el escenario (NS_IF_STATIC) NUNCA se descarta —se emite primero—; después se emiten las instancias dinámicas por orden de la cola y se
 *   descarta por el final. `truncated` marca qué lista se llenó, `n_dropped_*` cuántos elementos se perdieron y NS_SV_MAP_INCOMPLETE avisa si ni el escenario cupo.
 *
 * Qué NO contiene (y por qué)
 *   - Texturas: salen de la VRAM, que el consumidor ya reconstruye por eventos con la lista de la GPU (vr_scene, VrBatch.ev_end). Aquí sólo tpage/clut/UV por cara.
 *   - Colores finales: se entrega la ENTRADA del sombreado (normales + luces + color precalculado) y el renderer evalúa ns_shade_*, así la luz responde a la
 *     posición real de cada pieza y no a la del último fotograma del motor. La fórmula está fijada y validada (docs §6).
 *   - Sombras: las dibuja el módulo de efectos (NsFxScene.shadow_objs). El extractor de mallas las OMITE (instancias marcadas NS_IF_SHADOW no se emiten, sólo
 *     se cuentan en n_shadow_omitted) para que no se dibujen dos veces.
 *   - Efectos de pantalla completa (agua, gas, fundido, gafas): los entrega el módulo de efectos en NsFxScene.screen. NsScene sólo lleva `fx_seq`, la secuencia de
 *     la NsFxScene que corresponde a esta escena (mismo game_frame), y NS_SV_FX si existe.
 */
#include <stdint.h>
#include <stddef.h>

#define NS_VERSION 2u

/* Comprobación de tamaños en compilación (C99 y C11). */
#define NS_STATIC_ASSERT(name, cond) typedef char ns_sa_##name[(cond) ? 1 : -1]

/* Capacidades mínimas que debe tener una NsScene (ver «Capacidades mínimas y truncado»). */
enum { NS_MIN_INST = 256, NS_MIN_PIECE = 2048, NS_MIN_LIGHT = 256, NS_MIN_EFFECT = 4096 };

/* Tope de la intensidad de luz de una normal: el registro de 16 bits con signo satura en 0x7FFF sobre 4096 = 1,0. */
#define NS_INTENSITY_MAX (32767.0f / 4096.0f)

/* ---------------------------------------------------------------------------------------------------------------- transformaciones */
/* 3x4 fila-mayor: v' = R*v + t.  m[0..2] = fila 0 (r00 r01 r02), m[3] = tx, m[4..6] fila 1, m[7] = ty, m[8..10] fila 2, m[11] = tz.
 * Para matrices del motor (s16 con 4096 = 1) el productor ya divide por 4096; la traslación va en unidades del motor. */
typedef struct { float m[12]; } NsXf;
NS_STATIC_ASSERT(xf, sizeof(NsXf) == 48);

/* ---------------------------------------------------------------------------------------------------------------- malla */
typedef struct {
    int16_t x, y, z;     /* espacio local de la pieza, unidades del motor */
    uint8_t weld;        /* 0xFF = ninguno; si no: índice de vértice del modelo PADRE (NsPiece.parent) cuya posición TRANSFORMADA sustituye a ésta
                          * (costura entre piezas articuladas: el motor copia el vértice ya transformado del padre). El renderer calcula primero los
                          * vértices del padre y luego los soldados. Su color/normal: los de la propia cara (docs §3.5). */
    uint8_t pad;         /* 0 */
} NsVert;
NS_STATIC_ASSERT(vert, sizeof(NsVert) == 8);

typedef struct { int16_t x, y, z, pad; } NsNorm;   /* normal unitaria con 4096 = 1,0, espacio local; pad = 0 */
NS_STATIC_ASSERT(norm, sizeof(NsNorm) == 8);

enum {
    NS_FF_TRI = 1u,      /* triángulo: el motor repite el 3.er vértice (v[2] == v[3]); dibujar (A,B,C) */
    NS_FF_BAKED_OK = 2u  /* la cara tiene color precalculado válido en el NsBaked de la pieza */
};

/* Cara = cuadrilátero A,B,C,D en orden PERÍMETRO (el motor lo guarda así; el hardware lo dibuja en orden Z: triángulos (A,B,D) y (A,D,C) del vértice
 * 0,1,2,3 = A,B,D,C). Triángulos para el renderer: (A,B,D) y (B,C,D) (diagonal B-D, la del hardware; antes (A,B,C),(A,C,D) torcía los cuadriláteros alabeados). Anverso: la prueba de orientación de (A,B,D) en la proyección del motor es > 0, equivale a
 * dot(A_vista, cross(B-A, D-A)) > 0 con vista X derecha, Y abajo, Z delante (docs §3.4). Si el modelo es NS_MF_TWOSIDED se dibuja a doble cara. */
typedef struct {
    uint8_t v[4];        /* índices de vértice A,B,C,D (0..255). En modelos con vértices soldados el motor sólo usa 7 bits: el extractor ya los enmascaró */
    uint8_t n[4];        /* índices de normal A,B,C,D (0..255; igual: 7 bits si hay soldados) */
    uint8_t u[4], t[4];  /* texels (0..255) de A,B,C,D DENTRO de la página de textura del tpage (u = columna, t = fila), ya resueltos con el rectángulo de textura */
    uint16_t tpage;      /* palabra de página de textura: bits 0-3 x/64, 4 y/256, 5-6 modo de mezcla, 7-8 profundidad (0=4b,1=8b,2/3=15b) */
    uint16_t clut;       /* (clx/16) | (cly << 6) */
    uint16_t texid;      /* id de textura del motor (depuración; 0 = sin textura -> cara plana) */
    uint8_t flags;       /* NS_FF_* */
    uint8_t pad;         /* 0 */
} NsFace;
NS_STATIC_ASSERT(face, sizeof(NsFace) == 24);

enum {
    NS_MF_BLEND = 1u,    /* semitransparente (modo de mezcla en tpage de cada cara) */
    NS_MF_UNLIT = 2u,    /* sin sombreado: color neutro 128 (o color precalculado) */
    NS_MF_TWOSIDED = 4u, /* doble cara */
    NS_MF_WELDED = 8u,   /* tiene vértices soldados al padre */
    NS_MF_BAKED = 16u,   /* usa el NsBaked de la pieza (color precalculado por esquina, p. ej. el escenario) en vez de luz dinámica */
    NS_MF_BIAS_UP = 32u  /* depth_bias > 0: se dibuja por delante de otros de su misma profundidad (decals) */
};

typedef struct NsMesh {
    uint64_t id;         /* ns_mesh_content_hash() del contenido geométrico */
    uint32_t epoch;      /* época del almacén en que se creó (ver protocolo) */
    uint32_t flags;      /* NS_MF_* */
    uint32_t model_flags;/* palabra cruda del motor (depuración) */
    uint16_t n_verts, n_faces, n_norms, pad;
    int16_t depth_bias;  /* sesgo de orden de dibujo del motor (unidades de profundidad); 0 normal. Signo + = acercar */
    int16_t bbox[6];     /* min xyz, max xyz locales */
    int16_t pad2;
    const NsVert *verts;
    const NsNorm *norms;
    const NsFace *faces;
} NsMesh;

/* Color precalculado de una pieza (el escenario trae los de las luces fijas). Es del OBJETO, no del modelo, y puede recalcularse sin que cambie la geometría:
 * por eso va aparte del id de malla y versionado. ver >= 1; una versión nueva sustituye a la anterior (la vieja caduca con su época). */
typedef struct NsBaked {
    uint64_t mesh_id;
    uint32_t ver;
    uint32_t n_colors;   /* = 4 * n_faces */
    const uint32_t *rgba;/* RGBA8 (r | g<<8 | b<<16 | a<<24; 128 = neutro) en orden A,B,C,D de cada cara */
} NsBaked;

/* ---------------------------------------------------------------------------------------------------------------- instancias */
typedef struct {
    uint64_t mesh_id;    /* NsMesh.id */
    NsXf world;          /* local -> mundo del motor */
    uint32_t baked_ver;  /* versión del NsBaked de (mesh_id) que usa esta pieza; 0 = ninguno */
    int16_t parent;      /* índice de pieza (dentro de la misma instancia) cuyos vértices usan los soldados de esta malla; -1 */
    uint16_t flags;      /* NS_PF_* */
} NsPiece;
NS_STATIC_ASSERT(piece, sizeof(NsPiece) == 64);
enum { NS_PF_HIDDEN = 1u /* pieza oculta por el juego o por la capa VR (cabeza propia); no dibujar */ };

enum {
    NS_IF_LIT = 1u,      /* se sombrea con NsLight (si no: neutro/precalculado) */
    NS_IF_OWN_AMBIENT = 2u, /* ambiente propio (NsLight.amb) en vez del global */
    NS_IF_BLEND = 4u,    /* el objeto entero puede tener piezas semitransparentes (dibujar en el pase de transparentes) */
    NS_IF_STATIC = 8u,   /* escenario (objeto de mapa): no cambia entre fotogramas salvo cambio de zona; NUNCA se trunca */
    NS_IF_THERMAL_OK = 16u, /* admite textura térmica (CLUT alterno en VRAM; informativo) */
    NS_IF_RIGID = 32u,   /* todas las piezas comparten la misma matriz */
    NS_IF_ENVREFL = 64u, /* mapa de reflexión (raro; la normal se proyecta sobre la vista) */
    NS_IF_SHADOW = 128u  /* sombra proyectada: la dibuja el módulo de efectos; el extractor NO la emite (sólo aparece en pruebas/depuración) */
};

typedef struct {
    uint32_t key;        /* identidad estable entre fotogramas: dirección del objeto en la RAM del motor (cambia al recrearlo) */
    uint32_t first_piece;
    uint16_t n_pieces;
    uint16_t flags;      /* NS_IF_* */
    uint16_t zone_mask;  /* máscara de grupo de mapa (bits); 0 = siempre */
    int16_t layer;       /* canal del motor (0 = principal) */
    int16_t light;       /* índice en NsScene.lights, -1 si no */
    int16_t vis_state;   /* estado de visibilidad del último fotograma completo del motor (0 = lo descartó; informativo: el renderer decide por su cámara) */
    uint32_t pad;        /* 0 */
} NsInstance;
NS_STATIC_ASSERT(inst, sizeof(NsInstance) == 24);

/* ---------------------------------------------------------------------------------------------------------------- luces */
/* Conjunto de luces tal como las consume el sombreado del motor (docs §6), en MUNDO del motor:
 *   dir[l][0..2] (l=0..2): dirección en que VIAJA la luz l, s16 con 4096 = 1,0; la fila trae ya el factor de atenuación de las puntuales (módulo <= 3072).
 *   col[c][l]: color de la luz l en el canal c (s16; col/16 = aportación 0..255 con intensidad 1,0).
 *   amb[c]: color ambiente 0..255 (el motor lo multiplica por 16 al cargarlo).
 * Fórmula (docs §6): I_l = clamp(dot(dir[l]/4096, R*n), 0, NS_INTENSITY_MAX) ; c = clamp(amb[c] + sum_l col[c][l]/16 * I_l, 0, 255) ; la textura se modula con c/128. */
typedef struct {
    int16_t dir[3][3];
    int16_t col[3][3];
    int16_t amb[3];
    uint16_t flags;      /* bit 0: hay alguna luz distinta de la principal; bit 1: ambiente propio de la instancia */
} NsLight;
NS_STATIC_ASSERT(light, sizeof(NsLight) == 44);

/* Niebla / atenuación por profundidad. El motor NO usa atenuación por hardware: sólo hay niebla en las zonas con el actor de niebla, que sustituye
 * las etapas de canal y atenúa hacia NEGRO por profundidad de OBJETO (z de la pieza 0) y de primitiva. Rampa lineal en z de vista (unidades del motor):
 * f = clamp((z - start)/len, 0, 1); color = color*(1-f). `active` = 0 -> sin niebla. */
typedef struct {
    int32_t active;
    float start, len;    /* unidades del motor en profundidad de vista de la cámara del juego */
    float rgb[3];        /* color lejano 0..255 (siempre 0 en lo observado) */
} NsFog;
NS_STATIC_ASSERT(fog, sizeof(NsFog) == 24);

/* ---------------------------------------------------------------------------------------------------------------- efectos */
enum {
    NS_EK_QUAD = 0,      /* polígono/línea con vértices de mundo (p[]) */
    NS_EK_BILLBOARD = 1  /* sprite cuadrilátero de tamaño en PÍXELES de la proyección del motor alrededor de un punto de mundo */
};
enum {
    NS_EF_SEMI = 1u,     /* semitransparente (modo abr) */
    NS_EF_TEXTURED = 2u,
    NS_EF_GOURAUD = 4u,
    NS_EF_ONESIDE = 8u,  /* sólo anverso */
    NS_EF_LINE = 16u,    /* línea (dibujar como cinta de ancho `width_px`) */
    NS_EF_NOFOG = 64u,   /* el color ya lleva la niebla del motor (relleno por la lista de la GPU, ns_fill): ns_gpu_fx_build NO le aplica la de la escena */
    NS_EF_ON_CAMERA = 32u/* informativo: pegado a la cámara del motor; las posiciones ya vienen en el MUNDO del motor con la cámara de la escena (que en VR es la de la cabeza) */
};
typedef struct {
    uint8_t kind;        /* NS_EK_* */
    uint8_t nv;          /* 2 (línea), 3 o 4 vértices (orden de paquete: 0,1,2,3 = Z; cuadrilátero = triángulos (0,1,2) y (1,2,3)) */
    uint16_t flags;      /* NS_EF_* */
    int16_t depth_bias;  /* sesgo de orden */
    uint8_t abr;         /* modo de semitransparencia 0..3 */
    uint8_t sub;         /* segmento dentro del paquete (polilíneas): con src_key e idx identifica el elemento entre fotogramas (ver ns_fx_item_key) */
    uint16_t tpage, clut;
    uint16_t idx;        /* índice del paquete dentro de la primitiva */
    float p[4][3];       /* QUAD: vértices en mundo del motor (ya con la matriz del grupo aplicada); BILLBOARD: p[0] = centro */
    uint8_t rgb[4][3];   /* color por vértice 0..255 (128 = neutro con textura) */
    uint8_t uv[4][2];    /* texels en la página */
    float box[4];        /* BILLBOARD: caja en píxeles del motor respecto al centro proyectado: x0,y0 = (-rect.x,-rect.y), x1,y1 = (w-rect.x, h-rect.y) (y hacia abajo) */
    float ref_h;         /* BILLBOARD: distancia de pantalla H del motor con la que se midieron los píxeles; tamaño de mundo = px * z / ref_h con z = profundidad respecto a la cámara que se use */
    float width_px;      /* LINE: grosor aproximado en píxeles del motor */
    uint32_t src_key;    /* dirección de la primitiva del motor (identidad entre fotogramas) */
} NsEffect;
NS_STATIC_ASSERT(effect, sizeof(NsEffect) == 112);

/* ---------------------------------------------------------------------------------------------------------------- cámara y escena */
typedef struct {
    float eye[3];        /* posición del ojo, mundo del motor */
    float rot[9];        /* vista -> mundo, fila-mayor; columnas = X derecha, Y abajo, Z delante (como VrCamRecord.M). SIN escalar (tal como está en RAM) */
    float h;             /* distancia de pantalla H del motor (320 normal) */
    float overscan_y;    /* factor vertical de la proyección del motor con SU cámara: 66/64 (medido; error de reproyección 0,85-1,3 px frente a 14-24 px con 58/64).
                          * Las matrices de RAM NO lo llevan aplicado: el motor multiplica por él la Y proyectada. Con vr_active = 1 el motor no lo aplica y vale 1,0 */
    int32_t vr_active;   /* 1 si esta cámara es la que la capa VR le dio al motor (cabeza); 0 la propia del juego */
    float ofx, ofy;      /* centro de proyección en la pantalla del motor (registros de cálculo OFX/OFY, píxeles; 320x240 de referencia). 0 = desconocido: se toma el centro (160, 120). Lo fija el anfitrión (ctx) */
} NsCamera;
NS_STATIC_ASSERT(camera, sizeof(NsCamera) == 68);
#define NS_OVERSCAN_Y (66.0f / 64.0f)

enum {
    NS_SV_GAME = 1u,     /* hay partida/escena 3D con jugador (puente válido) */
    NS_SV_LISTS_OK = 2u, /* las listas de instancias/primitivas se leyeron enteras (si no: truncadas; ver `truncated`) */
    NS_SV_MAP_OK = 4u,   /* hay objetos de escenario en la lista */
    NS_SV_MAP_INCOMPLETE = 8u, /* el escenario no cupo en las capacidades (no debería ocurrir con las mínimas) */
    NS_SV_FX = 16u,      /* existe la NsFxScene con secuencia fx_seq */
    NS_SV_STORE_FULL = 32u,/* el almacén de mallas llegó a su tope sin reconocimiento del consumidor: faltan instancias nuevas */
    NS_SV_TOVR_OK = 64u,   /* to_vr es válido (cámara VR activa, primera persona); lo fija el enganche (ns_hook), nunca el extractor. Sin él no se dibuja en el mundo del usuario */
    NS_SV_PLAYER_OK = 128u /* jugador enlazado, cabeza propia oculta e IK de los brazos copiada; lo fija el enganche. Sin él la escena no es apta (se pinta por GPU) */
};
enum { NS_TR_INST = 1u, NS_TR_PIECE = 2u, NS_TR_LIGHT = 4u, NS_TR_EFFECT = 8u };

/* ---------------------------------------------------------------------------------------------------------------- efectos de pantalla completa
 * Estado de agua, anillo (máscara de gas), fundido y modos de visión leído de la RAM (copia plana de NsFxScreen, ns_fx.h). El renderer los hace en nativo
 * (ns_screen.h): tinte y ondulación en el MUNDO (volumen de agua), recubrimientos en la pantalla del ojo. Mundo del motor para el volumen. */
enum { NS_SFX_WATER = 1u, NS_SFX_GAS = 2u, NS_SFX_FADE = 4u, NS_SFX_FLASH = 8u, NS_SFX_MODE_A = 16u, NS_SFX_MODE_B = 32u, NS_SFX_AUX = 64u };
typedef struct {
    uint32_t flags;      /* NS_SFX_*: efectos activos */
    uint32_t status;     /* palabra de estado global cruda */
    float water_min[3], water_max[3];   /* volumen de agua en el mundo del motor (la cara superior de Y es la superficie; Y arriba) */
    float water_tint[3]; /* multiplicador por canal */
    uint32_t water_phase;/* contador de fotogramas de la onda */
    int32_t water_wave_rows, water_wave_period, water_bar_rows;
    float gas_intensity, gas_inner_px, gas_outer_px, gas_aspect, gas_pulse;   /* anillo: radios horizontales en píxeles del motor, razón vertical/horizontal */
    float gas_in[3], gas_out[3];        /* color 0..1 del borde interior / exterior (con intensidad y pulso) */
    int32_t gas_abr;
    float fade_amount;   /* 0..1 */
    int32_t fade_white, fade_in;
} NsScreenFx;
NS_STATIC_ASSERT(screenfx, sizeof(NsScreenFx) == 120);

typedef struct NsScene {
    uint32_t size;       /* sizeof(NsScene) de quien la rellenó: el consumidor rechaza la escena si no coincide con el suyo */
    uint32_t version;    /* NS_VERSION */
    uint32_t seq;        /* contador de escenas publicadas (sube 1 en 1) */
    uint32_t slot_gen;   /* generación del slot (sube cada vez que el productor reescribe este slot) */
    uint32_t game_frame; /* nº de fotograma del motor (hle_frame_count) */
    uint32_t vblank;     /* pulsos de refresco desde el arranque en el momento de la extracción */
    uint64_t t_ns;       /* reloj monótono (ns) al extraer */
    uint32_t mesh_epoch; /* época del almacén de mallas con la que se hizo la escena (el consumidor reconoce con ns_store_ack) */
    uint32_t zone_gen;   /* generación de definiciones del motor (sube al cambiar de zona/recargar); invalida la tabla dirección -> id del extractor */
    uint32_t valid;      /* NS_SV_* */
    uint32_t truncated;  /* NS_TR_*: listas que alcanzaron su capacidad */
    uint32_t n_dropped_inst, n_dropped_piece, n_dropped_effect;
    uint32_t n_shadow_omitted; /* sombras reconocidas y no emitidas (las dibuja el módulo de efectos) */
    uint32_t fx_seq;     /* secuencia de la NsFxScene de esta escena (mismo game_frame); sólo vale si NS_SV_FX */
    uint32_t clock;      /* reloj de búfer: búfer doble del motor (0/1) */
    uint32_t group_mask; /* grupos de mapa activos */
    uint16_t area_id; uint16_t pad0;
    float units_per_m;   /* 1000 */
    float to_vr[12];     /* mundo del motor -> mundo VR (3x4 fila-mayor, metros); misma regla que vr_scene (ver arriba) */
    NsCamera cam;
    NsFog fog;
    float bg_rgb[3];     /* color de fondo del canal de fondo 0..255 */
    NsLight global_light;/* luz global (el objeto por defecto del motor); lights[] la repite si alguna instancia la usa */
    NsScreenFx screen;   /* efectos de pantalla completa (agua, anillo, fundido, modos de visión); válido con NS_SV_FX */

    uint32_t n_inst, cap_inst;     NsInstance *inst;
    uint32_t n_piece, cap_piece;   NsPiece *piece;
    uint32_t n_light, cap_light;   NsLight *lights;
    uint32_t n_effect, cap_effect; NsEffect *effects;
} NsScene;

/* ---------------------------------------------------------------------------------------------------------------- almacén de mallas */
typedef struct NsMeshStore NsMeshStore;   /* opaco: lo implementa el extractor (productor). Ver «Entrega» arriba. */
/* Consumidor (cualquier hilo, sólo lectura): NULL si el id no está. Válido hasta que el propio consumidor reconozca una época > NsMesh.epoch. */
const NsMesh *ns_store_get(const NsMeshStore *st, uint64_t id);
/* Color precalculado (mesh_id, ver) o NULL si esa versión ya no existe. Misma validez que la malla. */
const NsBaked *ns_store_get_baked(const NsMeshStore *st, uint64_t mesh_id, uint32_t ver);
uint32_t ns_store_epoch(const NsMeshStore *st);        /* época actual del productor */
/* Consumidor: «no volveré a tocar mallas ni colores de época < epoch». Release; el productor libera la época anterior sólo tras verlo (acquire). */
void ns_store_ack(NsMeshStore *st, uint32_t epoch);

/* ---------------------------------------------------------------------------------------------------------------- utilidades (cabecera, sin dependencias) */
/* FNV-1a de 64 bits encadenable, SIEMPRE campo a campo (nunca sobre un struct entero: el padding no está definido). */
#define NS_HASH64_INIT 14695981039346656037ull
static inline uint64_t ns_hash64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static inline uint64_t ns_hash64_u8(uint64_t h, uint8_t v) { h ^= v; return h * 1099511628211ull; }
static inline uint64_t ns_hash64_u16(uint64_t h, uint16_t v) { return ns_hash64_u8(ns_hash64_u8(h, (uint8_t)v), (uint8_t)(v >> 8)); }
static inline uint64_t ns_hash64_u32(uint64_t h, uint32_t v) { return ns_hash64_u16(ns_hash64_u16(h, (uint16_t)v), (uint16_t)(v >> 16)); }

/* Hash del CONTENIDO GEOMÉTRICO de una malla (id). Campo a campo, sin padding, sin punteros, sin color precalculado, sin época. Excluye `id`. */
static inline uint64_t ns_mesh_content_hash(const NsMesh *m) {
    uint64_t h = NS_HASH64_INIT;
    h = ns_hash64_u32(h, m->flags);
    h = ns_hash64_u16(h, (uint16_t)m->depth_bias);
    h = ns_hash64_u16(h, m->n_verts); h = ns_hash64_u16(h, m->n_faces); h = ns_hash64_u16(h, m->n_norms);
    for (unsigned i = 0; i < m->n_verts; i++) {
        h = ns_hash64_u16(h, (uint16_t)m->verts[i].x); h = ns_hash64_u16(h, (uint16_t)m->verts[i].y); h = ns_hash64_u16(h, (uint16_t)m->verts[i].z);
        h = ns_hash64_u8(h, m->verts[i].weld);
    }
    for (unsigned i = 0; i < m->n_norms; i++) {
        h = ns_hash64_u16(h, (uint16_t)m->norms[i].x); h = ns_hash64_u16(h, (uint16_t)m->norms[i].y); h = ns_hash64_u16(h, (uint16_t)m->norms[i].z);
    }
    for (unsigned i = 0; i < m->n_faces; i++) {
        const NsFace *f = &m->faces[i];
        for (int k = 0; k < 4; k++) { h = ns_hash64_u8(h, f->v[k]); h = ns_hash64_u8(h, f->n[k]); h = ns_hash64_u8(h, f->u[k]); h = ns_hash64_u8(h, f->t[k]); }
        h = ns_hash64_u16(h, f->tpage); h = ns_hash64_u16(h, f->clut); h = ns_hash64_u16(h, f->texid); h = ns_hash64_u8(h, f->flags);
    }
    return h;
}

static inline float ns_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* Matriz del motor (s16[9] fila-mayor, 4096 = 1) + traslación s32[3] -> NsXf. */
static inline void ns_xf_from_engine(NsXf *o, const int16_t r[9], const int32_t t[3]) {
    for (int i = 0; i < 3; i++) {
        o->m[4 * i + 0] = r[3 * i + 0] * (1.0f / 4096.0f);
        o->m[4 * i + 1] = r[3 * i + 1] * (1.0f / 4096.0f);
        o->m[4 * i + 2] = r[3 * i + 2] * (1.0f / 4096.0f);
        o->m[4 * i + 3] = (float)t[i];
    }
}
static inline void ns_xf_apply(const NsXf *x, const float v[3], float o[3]) {
    o[0] = x->m[0] * v[0] + x->m[1] * v[1] + x->m[2] * v[2] + x->m[3];
    o[1] = x->m[4] * v[0] + x->m[5] * v[1] + x->m[6] * v[2] + x->m[7];
    o[2] = x->m[8] * v[0] + x->m[9] * v[1] + x->m[10] * v[2] + x->m[11];
}
/* Sólo rotación (para normales). */
static inline void ns_xf_rot(const NsXf *x, const float v[3], float o[3]) {
    o[0] = x->m[0] * v[0] + x->m[1] * v[1] + x->m[2] * v[2];
    o[1] = x->m[4] * v[0] + x->m[5] * v[1] + x->m[6] * v[2];
    o[2] = x->m[8] * v[0] + x->m[9] * v[1] + x->m[10] * v[2];
}

/* Sombreado por vértice-normal de una cara, EXACTO al del motor (docs §6, validado contra los paquetes de la GPU del motor):
 *   normal de mundo = R_pieza * n (sin normalizar: la matriz del motor ya es ~ortonormal; con escala ver docs),
 *   I_l = clamp(dot(dir_l, nw) / 4096, 0, NS_INTENSITY_MAX),   c = clamp(amb + sum col_l/16 * I_l, 0, 255).   Salida 0..255 por canal.
 * Convenio: n = normal ALMACENADA (interior) y dir_l = dirección en que viaja la luz; NO se invierte ningún signo. El hardware modula la textura con c/128.
 * `rw` = rotación 3x3 de mundo de la pieza (fila-mayor, ya dividida por 4096), n = normal s16/4096 en espacio local. */
static inline void ns_shade_normal(const NsLight *L, const float rw[9], const float n[3], float out_rgb[3]) {
    float nw[3] = { rw[0]*n[0] + rw[1]*n[1] + rw[2]*n[2], rw[3]*n[0] + rw[4]*n[1] + rw[5]*n[2], rw[6]*n[0] + rw[7]*n[1] + rw[8]*n[2] };
    float inten[3];
    for (int l = 0; l < 3; l++) {
        float d = (L->dir[l][0] * nw[0] + L->dir[l][1] * nw[1] + L->dir[l][2] * nw[2]) * (1.0f / 4096.0f);
        inten[l] = ns_clampf(d, 0.0f, NS_INTENSITY_MAX);
    }
    for (int c = 0; c < 3; c++) {
        float v = (float)L->amb[c] + (L->col[c][0] * inten[0] + L->col[c][1] * inten[1] + L->col[c][2] * inten[2]) * (1.0f / 16.0f);
        out_rgb[c] = ns_clampf(v, 0.0f, 255.0f);
    }
}
