# Escena nativa: estructuras del motor de dibujo y puntos de extracción

Documento de arquitectura del renderer nativo. Sustituye la reconstrucción 3D a partir de la salida de la GPU (vértices ya proyectados con la cámara del juego: huecos, asociación adivinada, parpadeos) por la LECTURA DIRECTA de la escena del motor desde la RAM en cada fotograma de juego y su dibujo con la cámara VR. Los nombres de este documento son propios y descriptivos. Todas las direcciones son de la edición del usuario (ejecutable principal, segmento alto cacheado). Contrato en `vr/native/ns_types.h` (versión 2); publicación sin bloqueos en `vr/native/ns_pub.h`; volcados y cargador en `vr/native/ns_dump.{h,c}`.

Estado de verificación (marcado en cada sección): **[V]** comprobado leyendo instantáneas reales de RAM de partida; **[M]** deducido del desensamblado del ejecutable; **[P]** pendiente de comprobar con una captura nueva.

## 1. Resumen de la arquitectura del motor

El motor de dibujo mantiene **tres canales** (fondo, principal, superposición), cada uno con una lista de ordenación doble (búfer 0/1, `reloj_bufer`) y una **cola** de punteros en la que se registran dos clases de elementos:

- **Objetos compuestos** (en adelante `Objeto`): un modelo articulado (o un trozo de escenario) = cabecera + array de **piezas**; cada pieza apunta a un **modelo** de una **definición** cargada del disco. Los objetos se añaden por el principio de la cola, las primitivas por el final.
- **Primitivas de efectos** (`Efecto`): listas de polígonos, sprites o líneas con vértices en coordenadas de mundo (o pegados a la cámara) escritos por el código de efectos.

Cada fotograma de juego el motor ejecuta, para el canal principal, **siete etapas** (tabla de funciones editable en RAM) en este orden: pantalla (calcula las matrices) → visibilidad → transformación (perspectiva de vértices a paquetes) → sombreado (colores de vértice) → primitivas → subdivisión → ordenación (mete los paquetes en la lista de ordenación). La GPU recorre después esa lista.

Idea de la extracción: **enganchar al final de la etapa «pantalla»**. En ese instante todas las matrices de mundo de los objetos del fotograma están calculadas y todavía no se ha tocado nada más (los modelos, luces, rotaciones de articulaciones y la cámara ya fueron escritos por la lógica del juego). Se recorre la cola y se vuelca la escena en el formato del contrato. La lógica, IA y sonido siguen intactos; la lista de la GPU se sigue usando para HUD, menús y filtros de pantalla.

## 2. Convenciones y conversión al espacio VR

- **Mundo del motor: Y arriba, mano derecha [V].** Comprobado en instantáneas reales: el suelo está en la Y más baja de la escena, las articulaciones de la cabeza tienen mayor Y que las de los pies, la cámara está por encima del jugador mirando hacia -Y, y la rotación vista->mundo tiene determinante +1. 1 unidad = 1 mm nominal. La regla de `vr_scene` (espejo en Z sólo si det(R_ojo) < 0, `opts.mirror_z`) no se activa en el mundo del motor normal. No hay inversión de Y en ningún sitio.
- Matriz del motor (32 B): `s16 m[3][3]` en +0x00 (fila-mayor, 4096 = 1,0), 2 B de relleno, `s32 t[3]` en +0x14. Puede llevar escala (las sombras ponen un tamaño en la diagonal).
- Ángulos: 4096 = vuelta. Vectores cortos de 8 B: `s16 x,y,z` + relleno.
- Vista del motor: X derecha, **Y abajo**, Z delante (así la usan `VrCamRecord.M`, `vrcam_delta` y `vr_scene`). Es una base derecha vista desde el mundo Y arriba.
- **Factor vertical de proyección (sobreescaneo) [V medido]:** el motor multiplica la Y proyectada por **66/64** cuando usa su propia cámara (error de reproyección 0,85-1,3 px; con 58/64 salía 14-24 px). **Las matrices guardadas en RAM (`ojo`, `ojo_inv`) NO llevan ese factor aplicado**: son rotaciones con la escala de cámara (~4106 = 1,0) y la traslación sin tocar. El factor se aplica al proyectar (rutina de ajuste, 0x80017780, que `vr/camera` anula cuando la cámara VR está activa; entonces vale 1,0). `NsCamera.overscan_y` lo lleva.
- Del mundo del motor al mundo VR: `NsScene.to_vr = S · Mmap · (p − origen_juego)` con `S = 1/units_per_m`, la misma composición que `vr_scene`, y encima la pose de la cabeza (`vrcam_delta`/`usermap`). El renderer nativo NO usa la cámara del motor para proyectar: sólo la guarda (`NsCamera`) para convertir efectos de tamaño en píxeles y para validar contra el fotograma real.
- **Convenio de normales y luces [V]:** las normales almacenadas de un modelo coinciden con `cross(B−A, D−A)` en el orden de vértices del motor (verificado en todas las caras de un escenario completo) y como el anverso exige `dot(A_vista, cross(B−A, D−A)) > 0`, **apuntan hacia el lado no visible** (son «interiores»). Las filas de luz son la dirección en que **viaja** la luz (una luz cenital viaja hacia −Y). Por eso un suelo —cuya normal almacenada apunta a −Y— recibe la luz con el producto escalar sin cambiar signos. El contrato lo fija así y `ns_shade_normal` no invierte nada.

## 3. Objetos: canales, cola, objeto compuesto, pieza, modelo

### 3.1 Canales y colas [V]

| Elemento | Dirección | Detalle |
|---|---|---|
| Tabla de canales | `0x800B2990` | 3 × 0x1EC B: [0] fondo, [1] **principal**, [2] superposición |
| Cola del canal principal | `0x800B2110` | 256 punteros; objetos desde el índice 0 hacia arriba (`idx_objetos`), efectos desde el 255 hacia abajo (`idx_efectos`..`tam_cola`) |
| Cola del fondo | `0x800B20F0` | 8 punteros; vacía y SIN etapas (nunca se calculan sus matrices): se ignora |
| Superposición | — | sin cola |
| Reloj de búfer (`reloj_bufer`) | `0x800ACAB0` (u32, 0/1) | índice del búfer de paquetes del fotograma en construcción |
| Grupo activo | `0x800ACAF8` (u32, máscara) | los elementos con `grupo != 0` sólo se dibujan si `grupo & grupo_activo`; el escenario por zonas usa un bit por trozo de mapa |

Campos del canal (offsets): `+0x00 lista_ord[2]`, `+0x08 s16 tam_lista`, `+0x0A enlace`, `+0x0C doble_buf`, `+0x0E sucio`, **`+0x10 matriz ojo_inv`** (mundo→vista), `+0x30 matriz ojo` (vista→mundo; `t` = posición del ojo), **`+0x50 s16 pantalla`** (distancia de pantalla H, 320 normal), `+0x52 s16 tam_cola`, `+0x54 s16 idx_efectos`, `+0x56 s16 idx_objetos`, **`+0x58 ptr cola`**, `+0x5C rect_recorte`, `+0x64 rect_recorte_nuevo`, después tres entornos de dibujo dobles. Ambas matrices de cámara son **rotaciones sin el factor vertical de §2** (verificado: `ojo_inv` es la transpuesta de `ojo`).

El constructor de la cámara (0x80017564, ya envuelto por `vr/camera`) escribe `ojo`, `ojo_inv` y `pantalla`. Los constructores de objetos y de efectos añaden a la cola (0x80018458 y 0x80018558) y las bajas los quitan (0x800184C8 y 0x800185C8): la cola SIEMPRE es la lista completa y coherente.

### 3.2 Objeto compuesto, cabecera de 0x48 B [V]

| Off | Tipo | Campo |
|---|---|---|
| +0x00 | matriz | `world`: matriz de la raíz (la rellena la lógica; con `padre_mat` != 0 se copia de ahí en la etapa pantalla) |
| +0x20 | ptr | `padre_mat`: puntero a otra matriz (objetos pegados a otro; 0 normal) |
| +0x24 | ptr | `def`: definición |
| +0x28 | u32 | `marcas` (ver abajo) |
| +0x2C | u16 | `grupo` (0 = siempre visible) |
| +0x2E | s16 | `n_modelos` (nº de piezas dibujables) |
| +0x30 | s16 | `canal` (-1 fondo, 0 principal, 1 superposición) |
| +0x32 | s16 | `vis` (0 = descartado en el último pase de visibilidad; informativo, §10) |
| +0x34 | ptr | `luz`: **dos matrices consecutivas** (64 B) con las luces del objeto (§6); por defecto apunta a la global `0x8009E55C` |
| +0x38 | ptr | `ang_art`: array de vectores cortos con la rotación de cada articulación (Z-Y-X, 4096 = vuelta); 0 si no hay |
| +0x3C | ptr | `ajuste`: correcciones por articulación (0 normal) |
| +0x40 | ptr | `rot_cintura`: rotación alternativa de la raíz |
| +0x44 | ptr | `desplaz`: desplazamientos por pieza (objetos deslizantes, sin `ang_art`) |
| +0x48 | array | `piezas[]`, 0x5C B cada una (hay `def.n_total` entradas; las `n_modelos` primeras son dibujables, las demás son «extensiones») |

Bits de `marcas`: 0x001 M_TEXTURA (modelo con textura), 0x002 M_PRECOL (colores precalculados por esquina en `col_pre`), 0x004 M_SEMI (puede tener semitransparencia), 0x008 M_LUZ (se sombrea con las luces), 0x010 M_RECORTE_PIEZA, 0x020 M_RECORTE_CAJA, 0x040 M_RIGIDO (todas las piezas comparten la matriz de la raíz), 0x080 **M_OCULTO**, 0x100 M_AMB_PROPIO (ambiente propio `luz[0].t`), 0x200 M_TERMICA (admite la textura térmica por CLUT alterno), 0x400 M_REFLEJO.

### 3.3 Pieza, 0x5C B [V]

| Off | Campo |
|---|---|
| +0x00 | matriz `world` — **local→mundo de la pieza** (la calcula la etapa pantalla) |
| +0x20 | matriz `pantalla` — mundo→pantalla (la usa la etapa de transformación; no hace falta) |
| +0x40 | `modelo*` |
| +0x44 | `col_pre*` — colores precalculados (M_PRECOL): 4 por cara; el array es único por objeto y lo recorren las piezas en orden; el de la pieza 0 es el origen |
| +0x48 | `enlace*` — segunda malla que comparte matriz (cadena) |
| +0x4C | `s16 vis`: 0 descartada, 1 parcial, 2 visible |
| +0x50 | `s16 sesgo`: sesgo de orden (signo y magnitud derivados de las marcas del modelo) |
| +0x52 | `s16 n_paq` (= nº de caras) |
| +0x54 | `paquetes[2]` — paquetes por búfer (sólo existen si la pieza fue visible; **no se usan**: UV y colores se rederivan del modelo) |

Cómo se calcula `world` (etapa pantalla, 0x8001D324) [M]:
1. `W = obj.world` (o `*padre_mat`).
2. M_RIGIDO: todas las piezas `world = W`.
3. Con `ang_art`: para cada pieza `R = RotZYX(ang_art[i])` con traslación = (`modelo.tx,ty,tz`) y `world_i = world_padre · R` (el padre `modelo.padre` ya procesado; la raíz se compone con `W`); con `ajuste` se corrige después. Sin `ang_art` pero con `desplaz`: `world_i = padre_world · translate(desplaz[i])`.
4. La matriz `pantalla` de cada pieza se obtiene componiendo con `ojo_inv`.

**Resultado:** a la salida de la etapa pantalla, `world` de todas las piezas de todas las instancias de la cola es válido para ese fotograma (aunque `vis` no lo sea, porque la etapa de visibilidad aún no ha corrido: la pantalla corre sobre TODOS los objetos de la cola, visibles o no). **El extractor no necesita saber si el motor los iba a recortar.**

### 3.4 Modelo (0x58 B) y definición [V]

Definición: `s32 n_modelos` (+0), `s32 n_total` (+4), caja `lx,ly,lz,ux,uy,uz` (+8..+0x1C); modelos desde +0x20. `n_total` ≥ `n_modelos` (las extra son mallas de extensión/cadena).

Modelo:

| Off | Campo |
|---|---|
| +0x00 | `u32 marcas`: 0x2 MM_SEMI, 0x4 MM_SIN_LUZ, 0x100/0x200 sesgo (hacia delante / hacia atrás) con magnitud en bits 12-13: `sesgo = (4 − ((marcas>>12)&3)) × 250`, negativo si no está 0x100; 0x400 MM_DOBLE_CARA; **0x10000 MM_SOLDADO** (tiene vértices soldados); 0x1 siempre; 0x20000 (sin significado conocido [P]) |
| +0x04 | `n_caras` |
| +0x08..0x1C | caja local, `+0x20..0x28` `tx,ty,tz` (traslación de la articulación respecto al padre) |
| +0x2C | `padre` (índice de pieza, -1 raíz) |
| +0x30 | `enlace` (índice de la pieza que comparte matriz, -1) |
| +0x34 | `n_verts` |
| +0x38 | `verts*` (8 B: x,y,z s16 + relleno) |
| +0x3C | `ind_vert*` — **4 bytes por cara** |
| +0x40 | `n_norms` |
| +0x44 | `norms*` (8 B; normal unitaria 4096 = 1,0) |
| +0x48 | `ind_norm*` — 4 bytes por cara |
| +0x4C | `uv*` — 8 bytes por cara |
| +0x50 | `ids_tex*` — u16, 1 por cara |

Los punteros de las definiciones están ya relocalizados a RAM en tiempo de ejecución. Las definiciones son **inmutables tras la carga** salvo el cambio de zona (se liberan y reutilizan direcciones: por eso `zone_gen` invalida la tabla dirección → id del extractor y el id de malla es un hash del contenido). Un mismo definición la comparten todas las instancias de un tipo.

**Cara = cuadrilátero con 4 índices en orden perímetro A,B,C,D.** El hardware dibuja en orden Z: paquete (v0,v1,v2,v3) = (A,B,D,C); el motor carga `v0=byte0, v1=byte1, v2=byte3, v3=byte2` de `ind_vert`/`ind_norm` [V: coincide con el desensamblado y con los colores de los paquetes]. Los **triángulos** se codifican repitiendo el último vértice: `byte2 == byte3`; además algunas caras tienen otras repeticiones (degeneradas, descartables).

UV: 8 bytes por cara = (u,v) de A,B,C,D en 0..255 sobre el rectángulo de textura de la cara: `u_texel = u * (w+1) / 256 + off_x`, `v_texel = v * (h+1) / 256 + off_y` con `(off_x,off_y,w,h)` de la entrada de textura de `ids_tex[cara]` (§5). `tpage` y `clut` también salen de esa entrada. Una cara con id de textura desconocido usa la entrada vacía: plana.

Anverso: el motor calcula la prueba de orientación de (v0,v1,v2) = (A,B,D) con los vértices proyectados y sólo dibuja si > 0, salvo MM_DOBLE_CARA. Equivale a `dot(A_vista, cross(B−A, D−A)) > 0` en la vista del motor [M, y coherente con §2].

### 3.5 Vértices soldados [V]

En los personajes, las costuras entre articulaciones se arreglan reutilizando el vértice YA TRANSFORMADO de la pieza padre. Al cargar la definición, para cada vértice de una pieza hija que coincide con un vértice del padre (campo de relleno del vértice ≠ 0xFFFF antes de enlazar) se marca el índice de la cara con el bit 0x80 y se convierte el relleno en el desplazamiento de ese vértice dentro del paquete del padre: `rell = índice_orden[esquina]·12 + 52·cara_del_padre + 8` con `índice_orden = {0,1,3,2}` (52 = tamaño del paquete, 12 = paso entre vértices). Para reconstruir el índice del padre: `cara = (rell−8) / 52`, `slot = ((rell−8) % 52) / 12`, esquina `c` con `índice_orden[c] == slot`, vértice `= ind_vert_padre[cara·4 + c] & 0x7F`.

En el contrato esto es `NsVert.weld` = índice del vértice del padre (0xFF ninguno) y `NsPiece.parent`. El renderer calcula primero la posición transformada de los vértices del padre. Para el color: el motor copia el color del paquete del padre en esas esquinas; el sombreado nativo lo aproxima con la normal propia de la cara, aceptado como error visual pequeño [P: comparar].

**Anchura de índices:** SOLO en los modelos con MM_SOLDADO los índices de `ind_vert`/`ind_norm` usan 7 bits (el bit 0x80 es la marca); en el resto usan los 8 bits. Sin enmascarar donde toca, las normales salen basura [V]. El extractor entrega ya los índices limpios (`NsFace.v/n`, 0..255).

## 4. Escenario de la zona [V]

**El mapa son objetos del mismo tipo.** Cada zona define N trozos de mapa (hasta 16): cada uno carga su(s) modelo(s) como objeto con M_TEXTURA|M_PRECOL|M_SEMI|M_RECORTE_PIEZA|M_RIGIDO en el canal principal, lo pone en la matriz identidad, **precalcula los colores** con las luces fijas del trozo (§6.3), lo encola y le asigna `grupo = 1 << índice_de_trozo`. El motor activa/desactiva trozos cambiando el grupo activo, no los quita de la cola.

Para el renderer: un objeto de mapa = instancia `NS_IF_STATIC` (M_PRECOL + M_RIGIDO y sin M_LUZ), matriz identidad, malla con color precalculado (`NsBaked`: RGBA por esquina de cara, ya con luces fijas) y `zone_mask` para filtrar por grupo activo. Las mallas del mapa son grandes y estáticas: perfectas para caché (cambian sólo si el motor recalcula el precálculo; entonces sube la versión del `NsBaked`, no el id de la malla). **El escenario nunca se trunca** (contrato, «Capacidades mínimas»).

## 5. Texturas [V]

Tabla de texturas en `0x800B30E0`: 512 entradas de **12 B**: `u16 id`, `u8 usada`, `u8 col`, `u16 tpage` (+4), `u16 clut` (+6), `u8 off_x` (+8), `u8 off_y` (+9), `u8 w` (+10), `u8 h` (+11). Búsqueda por sondeo lineal desde `id % 512` hasta una entrada con id 0 (vacía) o coincidente. El `id` es un hash de 16 bits del nombre de la textura. Las páginas de textura y paletas están en VRAM; el renderer ya las reconstruye por eventos (no se entregan aquí). La textura térmica/nocturna es un cambio de paleta escrito en VRAM por el propio motor y entra por los mismos eventos; la marca M_TERMICA sólo señala qué objetos la admiten.

## 6. Luces y sombreado

### 6.1 Datos [V]

- **Luz principal global**: matriz de luz en `0x8009E55C` (dirección principal en la fila 0), matriz de color en `0x8009E57C` (columna 0 = color de la luz principal ×16). **Ambiente global** en `0x800AC530` (3 × s16, 0..255). Código de paquete en `0x800AC538` (derivado del ambiente).
- **Luz por objeto**: `luz` apunta a un par de matrices: `luz[0]` = tres direcciones de luz (filas, 4096 = 1,0; módulo ≤ 3072 en las puntuales por la atenuación) y, en `t[0..2]`, el ambiente propio si M_AMB_PROPIO; `luz[1]` = colores (columna `l` = color de la luz `l`, ×16).
  Los actores móviles rellenan su par cada fotograma: copian la luz principal + hasta **2 luces puntuales** (temporales del fotograma anterior y fijas de la zona) que alcanzan la posición (caja de alcance `e_alcance`): `fila_l = ((p − punto)·3072/|d|) · max(0, 2·r_alcance − |d|)/r_alcance`. El color de cada puntual es el de la luz ×16. Hay un caso térmico (visión térmica activa) con colores a 0 y ambiente fijo.
- **Luces fijas del mapa** (16 B: punto (vector corto), `r_alcance` u16, `e_alcance` u16, color RGB) en la lista de cada trozo y hasta 8 listas registradas; sólo se usan para el precálculo (§6.3) y para los actores.

### 6.2 Sombreado de objetos dinámicos (etapa 4) [V]

Para cada pieza visible de un objeto con M_LUZ, el coprocesador de geometría se carga con `L' = luz[0]·R_pieza` (matriz de luz efectiva = filas de luz × rotación de mundo de la pieza), `LC = luz[1]` y `fondo = ambiente·16` (el del objeto si M_AMB_PROPIO, si no el global), y para cada normal del modelo se ejecuta el cálculo de color por normal (sin modular por el color base del paquete: sólo se toma su byte de código):

```
I_l = clamp( (L'_l · n) >> 12 , 0 , 0x7FFF )            l = 0..2   (n s16, 4096 = 1)  → tope NS_INTENSITY_MAX = 0x7FFF/4096
c   = clamp( (fondo·4096 + Σ_l LC[canal][l]·I_l) >> 12 >> 4 , 0 , 255 )
```

El color de la esquina sale como `c` por canal y el hardware lo usa para MODULAR la textura (128 = neutro). **Validación:** reproduciendo esta fórmula contra los colores de los paquetes en RAM, coinciden exactamente en los objetos con luces de actor y en los personajes (con luz puntual de color distinto por canal), una vez enmascarados los índices a 7 bits en modelos soldados y descartadas las caras no sombreadas por el motor (cara no visible: color neutro intacto). Otras piezas del personaje difieren porque la instantánea se toma después de que el siguiente fotograma ya movió articulaciones y luces; es el comportamiento esperado de leer los paquetes del búfer anterior, no un fallo de la fórmula. **[P]** repetir con una captura tomada en el punto de enganche. Detalle y validación ampliada: `docs/iluminacion_nativa.md`.

`ns_shade_normal` en `ns_types.h` implementa exactamente esa fórmula en float, con el convenio de signos de §2. El renderer la evalúa por vértice o por píxel con la rotación REAL de la pieza en cada fotograma de visor (la luz «sigue» la pose).

Caras sin sombrear: MM_SIN_LUZ o sin M_LUZ ni M_PRECOL: color neutro 128 (textura sin modular). Con M_SEMI, el bit de semitransparencia del paquete se enciende (el modo de mezcla viene del `tpage` de la textura).

### 6.3 Sombreado estático del escenario (precálculo) [M]

M_PRECOL: los 4 colores por cara (orden perímetro A,B,C,D, copiados a los paquetes como `c0=col_pre[0], c1=col_pre[1], c2=col_pre[3], c3=col_pre[2]`) se calculan UNA vez al cargar/cambiar de grupo: para cada vértice se buscan las dos luces fijas del trozo más cercanas dentro de su `e_alcance` (atenuación igual que arriba), se carga el coprocesador con la luz principal + esas dos y se ejecuta el cálculo de color por normal con la normal de la esquina. El resultado se guarda en `piezas[0].col_pre` (16 B por cara). El extractor lee los colores YA calculados (no recalcula luces).

## 7. Niebla y atenuación por profundidad [M]

El motor **no usa atenuación por profundidad del hardware** en el canal principal. Sólo hay niebla en las zonas con el actor de niebla (el que sustituye las etapas del canal): reemplaza visibilidad, transformación, sombreado y ordenación por versiones propias (código de overlay, direcciones distintas por zona) y atenúa hacia **negro** con una rampa lineal: tabla de 256 entradas s16 `0..4096` indexada por `z/256`, con parámetros `inicio` y `largo` en unidades de 256 (leídos del guion de la zona) y un `inicio` modulado por el zoom de la cámara. Para modelos la atenuación es **por objeto** (z de la pieza 0, multiplica color de luz y ambiente por `1 − tabla[z/256]/4096`); para efectos es por vértice.

Detección en el extractor: la entrada 1/2/3/6 de la tabla de etapas (`0x8009E534 + 4·i`) deja de apuntar a la rutina original (0x80018C8C, 0x8001E840, 0x8001D76C, 0x8001DA18). `NsFog` lleva `start/len` en unidades del motor; el extractor los calcula del estado del actor (dirección dependiente de la overlay: **[P]**, hoy `fog.active = 0`).

## 8. Efectos [V]

Estructura (0x54 B): `+0x00 matriz world`, `+0x20 matriz *padre_mat`, `+0x24 s32 tipo`, `+0x28 u16 grupo`, `+0x2A s16 n_prims`, `+0x2C s16 canal`, `+0x2E s16 sesgo`, `+0x30 s16 tam_paq`, `+0x32 s16 verts`, `+0x34 s16 desde`, `+0x36 s16 paso`, **`+0x38 pos*`** (vectores cortos), `+0x3C rect*`, `+0x40/0x44 paquetes[2]` (por búfer), `+0x48 s16 n_prims_total`, `+0x4C datos_usuario`, `+0x50 retrollamada`.

`tipo & 31` = clase (0..5 líneas planas/degradadas de 2-4 vértices, 6..8 sprites, 9..12 baldosas, **13..20 polígonos** planos/degradados/texturados de 3-4 vértices, 21/22 líneas texturizadas, 23 libre). Indicadores de `tipo`: `0x100` OCULTO, **`0x200` PE_EN_CAMARA** (posiciones ya en espacio de vista), **`0x400` PE_CARTEL** (billboard), `0x800` PE_SOLO_ORDEN (sólo ordena), `0x1000` PE_UNA_CARA, `0x2000` PE_PAQ_PROPIOS (los paquetes los rellena la retrollamada).

Paquete (formato del hardware, cuadrilátero texturizado de `tam_paq` 40): etiqueta(4) color+código(4) `x0y0`@8 `u0v0clut`@12 `x1y1`@16 `u1v1tpage`@20 `x2y2`@24 `u2v2`@28 `x3y3`@32 `u3v3`@36 (con color por vértice: 52 B). **Color, textura, tpage, clut y modo de mezcla los escribe el código del efecto en `paquetes[reloj_bufer]`**; el motor sólo rellena `x,y` y la profundidad de ordenación. Los vértices de mundo están en `pos[]`: vértice `j` del polígono `i` = `pos[i·verts + j]` (en el espacio de `world`/`padre_mat` salvo PE_EN_CAMARA).

- Polígonos y líneas sin PE_CARTEL: vértices de mundo `world·pos`. Casos típicos: marcas en el suelo (charcos, huellas), sombras proyectadas planas, cintas de luz (líneas texturizadas), brillos.
- `PE_CARTEL`: **billboard de tamaño constante en píxeles**: un punto de mundo por primitiva, proyectado; el cuadrilátero se forma alrededor en pantalla con `rect` = (x,y,w,h): esquinas `(−x,−y)` y `(w−x, h−y)` relativas al punto proyectado. El resultado depende de la distancia de pantalla `H` del motor: tamaño de mundo = `píxeles · z / H`. En el contrato: `NsEffect{kind=BILLBOARD, box, ref_h}`; el renderer elige si fija el tamaño de mundo o lo mantiene en píxeles angulares.
- `PE_UNA_CARA`: se descarta por orientación igual que los modelos.
- `PE_PAQ_PROPIOS`: llama a la retrollamada (efectos con geometría propia); el extractor no los interpreta: esos paquetes **siguen saliendo por la lista de la GPU** (clase 3D de `vr_scene`) y el extractor los marca para que el consumidor decida. **[P]** medir cuántos hay en partida.
- **Efectos de pantalla completa** (agua, gas, fundidos, gafas): no son geometría de la cola; los entrega el módulo de efectos (`NsFxScene.screen`). `NsScene.fx_seq`/`NS_SV_FX` enlazan ambas escenas por `game_frame`.

Orden de dibujo en el motor: por z media en listas de ordenación de 256 casillas por canal; el sesgo resta/suma profundidad. El renderer nativo usa z-buffer para lo opaco y ordena de atrás adelante lo semitransparente por profundidad de pieza/efecto; el sesgo se convierte en un pequeño desplazamiento de profundidad (decals y marcas por encima del suelo).

## 9. Sombras

No hay sistema de sombras dedicado. La sombra de los actores es un **objeto compuesto normal** (una sola pieza = un cuadrilátero plano semitransparente, M_RIGIDO + M_SEMI + M_LUZ) puesto bajo el actor: cada fotograma el actor recalcula una matriz con la escala adecuada (ancho y fondo en la diagonal, el resto identidad). **Decisión de contrato:** las sombras las dibuja el módulo de efectos (`NsFxScene.shadow_objs`); el extractor de mallas las **omite** (no las emite en `NsScene.inst`; sólo incrementa `n_shadow_omitted`) para que no se dibujen dos veces. Se reconocen por: una pieza, M_SEMI + M_RIGIDO, matriz de escala aplanada y posición en el suelo (el criterio exacto lo fija el extractor y debe coincidir con el de `shadow_objs`: ambos módulos usan la misma identidad `key` = dirección del objeto). Los reflejos de algunas zonas son objetos con M_REFLEJO o una segunda copia del modelo con matriz espejo; no hay trato especial.

## 10. Punto de enganche en el fotograma

Orden en un fotograma de juego (hilo de emulación, en el pulso de refresco de render, cuenta de fotogramas `hle_frame_count`):

1. Fase de lógica: los actores se actualizan; cada uno escribe `ang_art`, su matriz de raíz, su `luz`, sus efectos (`pos`/`paquetes`), crea/destruye objetos.
2. Cámara: el constructor de cámara (0x80017564; envuelto por `vr/camera`) escribe `ojo`, `ojo_inv` y `pantalla` del canal principal.
3. **Fin de fotograma** (0x8001753C) llama a la ordenación de canales (0x80018304) → bucle de 7 etapas sobre el canal principal, función a función desde la tabla en `0x8009E534`:
   `0x8001D324` pantalla ← **ENGANCHE** · `0x80018C8C` visibilidad · `0x8001E840` transformación · `0x8001D76C` sombreado · `0x8001BA44` efectos · `0x8001A0C4` subdivisión · `0x8001DA18` ordenación.
   (Argumentos: `a0` = puntero al canal principal `0x800B2B7C`, `a1` = `reloj_bufer`.)
4. El inicio del siguiente fotograma (0x8001741C) manda a la GPU la lista del búfer contrario (0x80018110), borra y repone la memoria de paquetes.

**Enganche recomendado:** envolver la etapa de pantalla (0x8001D324) con el mecanismo `rt_patch(addr, fn)` + `rt_call_orig` que ya usa la IA: la envoltura llama a la original (calcula todas las `world`) y después ejecuta el extractor, que lee `a0` (canal), la cola, los objetos y los efectos. Se ejecuta UNA vez por fotograma de juego, dentro de la CPU emulada, así que lo que lee es coherente (no hay carrera con la lógica). Alternativa sin parche de código: leer en el gancho de fin de fotograma; las `world` siguen en RAM pero los `ang_art` de los actores y las luces ya pueden ser del fotograma siguiente (se observa en las instantáneas: pequeñas diferencias). Si el enganche es ese, descartar la escena cuando `reloj_bufer` no cambió desde la anterior.

El extractor ignora `vis` (el renderer decide por la cámara VR) y descarta sólo: M_OCULTO (0x080), `grupo != 0 && !(grupo & grupo_activo)`, piezas con `n_caras == 0`, las sombras (§9) y las ocultas por la capa VR (cabeza del jugador: `vis` de esa pieza puesto a 0 por `vrcam_hide_head`; el extractor la marca `NS_PF_HIDDEN`).

Dónde hay que mirar con cuidado [P]: objetos pegados a otros (`padre_mat`), objetos que cambian de definición en medio de la zona, listas truncadas si la captura coincide con el borrado de un objeto (leer con comprobación de punteros dentro de RAM, `ns_ptr_ok` que rechaza la página baja, y topes: máximo 256 entradas por cola, 24 articulaciones por objeto).

## 11. Algoritmo de extracción (referencia para `ns_extract`)

```
canal = 0x800B2B7C (principal); n = s16[+0x56]; q = ptr[+0x58]
cam   = canal.ojo (+0x30): ojo.t, rotación vista->mundo ; H = s16[+0x50]; overscan_y = 66/64 (o 1,0 con cámara VR)
luz global: 0x8009E55C / 0x8009E57C / ambiente 0x800AC530
orden de emisión: primero los objetos de escenario (M_PRECOL + M_RIGIDO, NS_IF_STATIC), después el resto por orden de cola
for i in 0 .. n-1:
    o = q[i]; si !ns_ptr_ok(o) continuar
    si marcas & M_OCULTO  o  (grupo != 0 && !(grupo & grupo_activo))  continuar
    si es sombra: n_shadow_omitted++; continuar
    crear NsInstance (key = o, flags desde `marcas`, light = índice de NsLight deduplicada por contenido)
    for k in 0 .. n_modelos-1:
        p = o + 0x48 + 0x5C*k ; m = ptr[p+0x40]
        malla = resolver(m)                 # id por (dirección, zone_gen): NO se rehashea cada fotograma
        baked = M_PRECOL ? resolver_colores(p, malla) : ninguno   # versión aparte
        pieza = {malla.id, baked_ver, world = p+0x00 (rot/4096, t), parent = modelo.padre, flags}
        enlace: la pieza `p.enlace` se emite como otra pieza con la misma matriz (mallas encadenadas)
efectos: for j in idx_efectos .. tam_cola-1:  → NsEffect (QUAD/BILLBOARD según tipo)
publicar: añadir TODAS las mallas/colores al almacén → rellenar el slot → ns_pub_publish (ver protocolo en ns_types.h)
```

`resolver()` decodifica el modelo una sola vez por (dirección, zone_gen): caras con índices enmascarados donde toca, UV resueltos con la tabla de texturas, normales. `zone_gen` sube cuando cambia el área o se recargan definiciones.

## 12. Protocolo de entrega (resumen; la especificación completa está en `ns_types.h`)

- **Arena de mallas con liberación diferida, dos generaciones:** como mucho dos épocas vivas; la anterior se libera sólo cuando el consumidor ejecuta `ns_store_ack(st, época_nueva)` (garantía: no volverá a tocar mallas de épocas menores). Mientras no lo haga, el productor sigue en la época actual.
- **Orden de publicación:** primero se añaden mallas y colores al almacén, luego se escribe el slot y se publica con `ns_pub_publish` (release); el consumidor toma con `ns_pub_acquire` (acquire). Hay prueba en `test_ns_dump.c`.
- **Contador de generación por slot** (`NsScene.slot_gen`) y campos `size`/`version` al principio de `NsScene` (el consumidor rechaza una escena con otro `size`).
- **Hash de malla:** 64 bits, campo a campo (sin padding), una vez por (dirección, `zone_gen`); el color precalculado va aparte, versionado (`NsBaked.ver`).
- **Capacidades mínimas** (`NS_MIN_*`): instancias ≥ 256, piezas ≥ 2048, luces ≥ 256, efectos ≥ 4096 (suficiente para una escena sin ningún recorte de visibilidad). Truncado: el escenario no se descarta nunca; lo dinámico se descarta por el final de la cola; `truncated`, `n_dropped_*` y `NS_SV_MAP_INCOMPLETE` lo señalan.

## 13. Qué se ha comprobado y con qué

Sin ejecutar el juego: se han usado instantáneas de RAM ya existentes de partidas reales (varias zonas) y el desensamblado del ejecutable del usuario.

- Direcciones y layout de canal, cola, objeto, pieza, modelo, definición, tabla de texturas y efectos leídos y coherentes en varias instantáneas de zonas y momentos distintos [V].
- El escenario es siempre un puñado de objetos al principio de la cola con muchas piezas; casi todas las caras resuelven textura; hay triángulos por repetición de vértice y caras con vértices soldados [V].
- Fórmula de luz validada contra colores de paquetes (§6.2) y luz principal/ambiente/código de paquete coherentes entre sí.
- Convenio de ejes y de normales (§2) verificado sobre un escenario completo: todas las normales almacenadas coinciden con `cross(B−A, D−A)`; los suelos están en la Y más baja con normal almacenada hacia −Y; la cámara está por encima mirando hacia −Y.
- Tabla de etapas localizada con los punteros de las siete rutinas y la rutina de pantalla desensamblada.

Pendiente [P]: captura tomada en el punto de enganche; color de esquinas soldadas; niebla (parámetros del actor); efectos con paquetes propios en partida; modelos con reflejo; efectos de zonas con rutas propias; escena durante cinemáticas del motor (otra cola/cámara); límite de la cola (256).

## 14. Volcados de desarrollo

Los volcados se generan JUGANDO: no se ejecuta el juego en el entorno de desarrollo. La app incluye la captura (`vr/native/ns_dump.h`, escritor en `ns_dumpw.c`, gesto en `app/Sources/DumpCapture.swift`):

- **Gesto:** mantener los **dos clics de stick pulsados ~1 s** durante la partida (en mando normal, L3 + R3). Mientras estén los dos pulsados la app **oculta ambos clics al juego y al arma** (no cambia el seguro/selector); se rearma al soltar los dos. Vibración corta al aceptar, larga al terminar con éxito, dos cortas si falló o si pasan 5 s sin resultado.
- **Hilos:** el hilo de emulación sólo COPIA (RAM, memoria rápida, VRAM, registros del coprocesador, última lista de la GPU, texto de meta) a búferes reservados una vez; un hilo trabajador convierte el fotograma a PNG y escribe a disco. Si no hay lista libre se vuelca sin lista. El hilo de render no toca disco ni `FolderAccess`: la carpeta se refresca desde el hilo principal y el resto va a una cola de fondo.
- **Destino:** `<carpeta del usuario>/Dumps/d<NNNN>_<fecha>/`. La escritura es atómica (`<dir>.tmp` y `rename` al final).
- **Contenido** (cargable con `ns_dump_open`): `ram.bin` (2 MiB), `scratch.bin` (1 KiB), `vram.bin` (1 MiB), `gte.bin` (64 palabras: datos y control del coprocesador), `list.bin` (última lista de la GPU), `frame.png` (fotograma real mostrado) y `meta.txt` (fotograma, área, jugador, cámara del motor, **registro de cámara VR de la lista volcada** (`rec_*`) más el último del hilo de emulación (`last_rec_*`) y pose de la cabeza; claves en `ns_dump.h`).
- **Cobertura pedida:** una decena de volcados en la zona de arranque (quieto, andando, junto al agua, con un enemigo a la vista) y unos cinco en otra zona.
- **Pruebas:** `make -f vr/native/dump.mk test` y `test-san` (escritor + cargador + PNG + casos hostiles + hash + triple búfer con datos sintéticos).
- La RAM contiene datos del juego: no se sube a ningún repositorio.
