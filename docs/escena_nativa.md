# Escena nativa: estructuras del motor de dibujo y puntos de extracción

Documento de arquitectura del renderer nativo (rama `render-nativo`). Sustituye la reconstrucción 3D a partir de la salida de la GPU (vértices ya proyectados con la cámara del juego: huecos, asociación adivinada, parpadeos) por la LECTURA DIRECTA de la escena del motor desde la RAM en cada fotograma de juego y su dibujo con la cámara VR. Los nombres de este documento son neutros; el mapa de consulta es la decompilación externa, de la que no se copia nada. Todas las direcciones son de la edición del usuario (ejecutable principal, KSEG0). Contrato en `vr/native/ns_types.h`; volcados y cargador en `vr/native/ns_dump.{h,c}`.

Estado de la verificación (marcado en cada sección): **[V]** comprobado leyendo volcados reales de RAM de partida (varias zonas, ver §12); **[M]** tomado del mapa (decomp) y confirmado en el desensamblado de la edición del usuario; **[P]** pendiente de comprobar con una captura nueva.

## 1. Resumen de la arquitectura del motor

El motor de dibujo mantiene **tres canales** (`canal[-1]` fondo, `canal[0]` principal, `canal[1]` superposición), cada uno con una tabla de ordenación (OT) doble (búfer 0/1, `reloj_bufer`) y una **cola** de punteros en la que se registran dos clases de elementos:

- **Objetos compuestos** (`OBJS`): un modelo articulado (o un trozo de escenario) = cabecera + array de **piezas** (`OBJ`); cada pieza apunta a un **modelo** (`MDL`) de una **definición** (`DEF`) cargada del disco. Los objetos se añaden por el principio de la cola, las primitivas por el final.
- **Primitivas de efectos** (`PRIM`): listas de polígonos/sprites/líneas con vértices en coordenadas de mundo (o pegados a la cámara) escritos por el código de efectos.

Cada fotograma de juego el motor ejecuta, para el canal principal, **siete etapas** (tabla de funciones editable en RAM) en este orden: pantalla (calcula las matrices) → visibilidad (recorte/`bound`) → transformación (RTPT de vértices a paquetes) → sombreado (colores de vértice con la GTE) → primitivas → subdivisión → ordenación (mete los paquetes en la OT). Después la GPU recorre la OT en el siguiente VBlank-flip.

Idea de la extracción: **enganchar al final de la etapa «pantalla»**. En ese instante todas las matrices de mundo (`obj->world`) del fotograma están calculadas y todavía no se ha tocado nada más (los modelos, luces, rotaciones de articulaciones y la cámara ya fueron escritos por la lógica del juego en su fase de actualización). Se recorre la cola y se vuelca la escena en el formato del contrato. La lógica, IA y sonido siguen intactos; la lista de la GPU se sigue usando para HUD/menús/filtros de pantalla.

## 2. Convenciones y conversión al espacio VR

- Mundo del motor: **Y arriba [V]** (la cámara cenital está por encima del jugador; las articulaciones de la cabeza tienen la Y mayor que las de los pies), mano derecha tras la regla de `vr_scene` (espejo en Z si det(R_ojo) < 0, `opts.mirror_z`). 1 unidad = 1 mm nominal (personaje 1650-1750).
- Matriz del motor (`MATRIX`, 32 B): `s16 m[3][3]` en +0x00 (fila-mayor, 4096 = 1,0; ~4106 por escala de cámara), 2 B de relleno, `s32 t[3]` en +0x14. Puede llevar escala (las sombras ponen un tamaño en la diagonal).
- Ángulos: 4096 = vuelta. Vectores cortos `SVECTOR` 8 B: `s16 x,y,z` + `pad`.
- Vista del motor (GTE): X derecha, **Y abajo**, Z delante (así la usan `VrCamRecord.M`, `vrcam_delta` y `vr_scene`). La proyección vertical del motor aplica el factor de sobreescaneo 58/64 a la fila Y de la matriz de vista y a su traslación (`ajuste_sobreescaneo`, 0x80017780, anulada por `vr/camera` cuando la cámara VR está activa).
- Del mundo del motor al mundo VR: `NsScene.to_vr` = la misma composición que `vr_scene` (§ «Espacio mundo VR y escala»: `S·Mmap·(p − origen_juego)`, con `S = 0,001`), y encima la pose de la cabeza (`vrcam_delta`/`usermap`). El renderer nativo NO usa la cámara del motor para proyectar: sólo la guarda (`NsCamera`) para convertir efectos de tamaño en píxeles y para validar contra el fotograma real.

## 3. Objetos: canales, cola, objeto compuesto, pieza, modelo

### 3.1 Canales y colas [V]

| Elemento | Dirección | Detalle |
|---|---|---|
| Tabla de canales | `0x800B2990` | 3 × 0x1EC B: [0] fondo, [1] **principal**, [2] superposición |
| Cola del canal principal | `0x800B2110` | 256 punteros; objetos desde el índice 0 hacia arriba (`objs_index`), primitivas desde el 255 hacia abajo (`prim_index`..`queue_size`) |
| Cola del fondo | `0x800B20F0` | 8 punteros; vacía en todos los volcados y SIN etapas (nunca se calculan sus matrices): se ignora |
| Superposición | — | sin cola (tamaño 0) |
| Reloj de búfer (`reloj_bufer`) | `0x800ACAB0` (u32, 0/1) | índice del búfer de paquetes del fotograma en construcción |
| Grupo activo (`grupo_actual`) | `0x800ACAF8` (u32, máscara) | los elementos con `group_id != 0` sólo se dibujan si `group_id & grupo_actual`; el escenario por zonas usa un bit por trozo de mapa |

Campos del canal (offsets): `+0x00 ot[2]`, `+0x08 s16 ot_size`, `+0x0A link`, `+0x0C dblbuf`, `+0x0E dirty`, **`+0x10 MATRIX eye_inv`** (mundo→vista), `+0x30 MATRIX eye` (vista→mundo; `t` = posición del ojo), **`+0x50 s16 screen`** (distancia de pantalla H, 320 normal), `+0x52 s16 queue_size`, `+0x54 s16 prim_index`, `+0x56 s16 objs_index`, **`+0x58 ptr queue`**, `+0x5C RECT clip_rect`, `+0x64 RECT new_clip_rect`, después tres `DR_ENV` dobles. `eye_inv` ya está ajustado por el sobreescaneo sólo en el momento de usarlo (se aplica a una copia en la etapa pantalla): al leerlo hay que aplicar 58/64 a la fila Y y a `t[1]` si se quiere reproducir la proyección del motor.

Registro: constructor de la cámara (0x80017564, ya envuelto por `vr/camera`) escribe `eye`, `eye_inv` y `screen`. El constructor de objetos y el de primitivas añaden a la cola (0x80018458 y 0x80018558) y las bajas las quitan (0x800184C8 y 0x800185C8): la cola SIEMPRE es la lista completa y coherente.

### 3.2 Objeto compuesto (`OBJS`), cabecera de 0x48 B [V]

| Off | Tipo | Campo |
|---|---|---|
| +0x00 | MATRIX | `world`: matriz de la raíz (la rellena `poner_matriz_actual` desde la GTE en la fase de lógica; con `root` != 0 se copia de ahí en la etapa pantalla) |
| +0x20 | ptr | `root`: puntero a otra matriz (objetos pegados a otro; 0 normal) |
| +0x24 | ptr | `def`: definición (`DEF`) |
| +0x28 | u32 | `flag` (ver abajo) |
| +0x2C | u16 | `group_id` (0 = siempre visible) |
| +0x2E | s16 | `n_models` (nº de piezas dibujables) |
| +0x30 | s16 | `chanl` (-1 fondo, 0 principal, 1 superp.) |
| +0x32 | s16 | `bound_mode` (0 = descartado en el último pase de visibilidad; informativo, §10) |
| +0x34 | ptr | `light`: **dos MATRIX consecutivas** (64 B) con las luces del objeto (§6); por defecto apunta a la global `0x8009E55C` |
| +0x38 | ptr | `rots`: array de `SVECTOR` con la rotación de cada articulación (Z-Y-X, 4096 = vuelta); 0 si no hay |
| +0x3C | ptr | `adjust`: correcciones Z/X/Y por articulación (0 normal) |
| +0x40 | ptr | `waist_rot`: rotación alternativa de la raíz |
| +0x44 | ptr | `movs`: desplazamientos por pieza (objetos deslizantes, sin `rots`) |
| +0x48 | array | `OBJ objs[]`, 0x5C B cada una (hay `def.n_x_models` entradas; las `n_models` primeras son dibujables, las demás son «extensiones») |

`flag` (bits): 0x001 TEXT (modelo con textura), 0x002 PAINT (colores precalculados por esquina en `rgbs`), 0x004 TRANS (puede tener semitransparencia), 0x008 SHADE (se sombrea con las luces), 0x010 BOUND (recorte por pieza), 0x020 GBOUND (recorte global por caja), 0x040 ONEPIECE (todas las piezas comparten la matriz de la raíz), 0x080 **INVISIBLE**, 0x100 AMBIENT (ambiente propio `light[0].t`), 0x200 IRTEXTURE (admite la textura térmica por CLUT alterno), 0x400 ENVMAP.

Valores observados [V]: escenario `0x57` (TEXT+PAINT+TRANS+BOUND+ONEPIECE); sombra y partes de una pieza `0x6D`/`0x16D`/`0x26D`/`0x36D`; personaje articulado con luz propia `0x32D`/`0x32D`.

### 3.3 Pieza (`OBJ`), 0x5C B [V]

| Off | Campo |
|---|---|
| +0x00 | `MATRIX world` — **matriz local→mundo de la pieza** (la calcula la etapa pantalla) |
| +0x20 | `MATRIX screen` — mundo→pantalla (la usa la etapa de transformación; no hace falta) |
| +0x40 | `MDL *model` |
| +0x44 | `CVECTOR *rgbs` — colores precalculados (PAINT): 4 por cara; el array es único por objeto compuesto y lo recorren las piezas en orden; `rgbs` de la pieza 0 es el origen |
| +0x48 | `OBJ *extend` — segunda malla que comparte matriz (cadena) |
| +0x4C | `s16 bound_mode`: 0 descartada, 1 parcial, 2 visible |
| +0x50 | `s16 raise`: sesgo de orden (signo y magnitud derivados del `flag` del modelo) |
| +0x52 | `s16 n_packs` (= nº de caras) |
| +0x54 | `POLY_GT4 *packs[2]` — paquetes por búfer (sólo existen si la pieza fue visible; **no se usan**: UV y colores se rederivan del modelo) |

Cómo se calcula `world` (etapa pantalla, 0x8001D324 → `pantalla_objetos`) [M]:
1. `W = objs->world` (o `*root`).
2. `ONEPIECE`: todas las piezas `world = W`.
3. Con `rots`: para cada pieza `R = RotZYX(rots[i])` con traslación = (`model.tx,ty,tz`) y `world_i = world_padre · R` (el padre `model.parent` ya procesado; la raíz se compone con `W`); con `adjust` se corrige después. Sin `rots` pero con `movs`: `world_i = padre_world · translate(movs[i])`.
4. La matriz `screen` de cada pieza se obtiene componiendo con `eye_inv`.

**Resultado:** a la salida de la etapa pantalla, `world` de todas las piezas de todas las instancias de la cola es válido para ese fotograma (aunque `bound_mode` no lo sea, porque la etapa de visibilidad aún no ha corrido: la pantalla corre sobre TODOS los objetos de la cola, visibles o no). **El extractor no necesita saber si el motor los iba a recortar** — con el parche `sin-recorte` (§54) casi todo se envía a dibujar de todos modos.

### 3.4 Modelo (`MDL`, 0x58 B) y definición (`DEF`) [V]

`DEF`: `s32 n_models` (+0), `s32 n_x_models` (+4), caja `lx,ly,lz,ux,uy,uz` (+8..+0x1C); `MDL models[]` desde +0x20. `n_x_models` ≥ `n_models` (las extra son mallas de extensión/cadena).

`MDL`:

| Off | Campo |
|---|---|
| +0x00 | `u32 flag`: 0x2 TRANS, 0x4 UNLIT, 0x100/0x200 «raise» (hacia delante / hacia atrás) con magnitud en bits 12-13: `raise = (4 − ((flag>>12)&3)) × 250`, negativo si no está 0x100; 0x400 BOTHFACE; **0x10000 INDIRECT** (tiene vértices welded); 0x1 siempre; 0x20000 (aparece en trozos de mapa, sin significado conocido [P]) |
| +0x04 | `n_faces` |
| +0x08..0x1C | caja local (`lx,ly,lz`,`ux,uy,uz`), `+0x20..0x28` `tx,ty,tz` (traslación de la articulación respecto al padre) |
| +0x2C | `parent` (índice de pieza, -1 raíz) |
| +0x30 | `extend` (índice de la pieza que comparte matriz, -1) |
| +0x34 | `n_verts` |
| +0x38 | `SVECTOR *verts` (8 B: x,y,z s16 + `pad`) |
| +0x3C | `u8 *vindices` — **4 bytes por cara** |
| +0x40 | `n_norms` |
| +0x44 | `SVECTOR *norms` (8 B; normal unitaria 4096 = 1,0) |
| +0x48 | `u8 *nindices` — 4 bytes por cara |
| +0x4C | `u8 *uvs` — 8 bytes por cara |
| +0x50 | `u16 *texids` — 1 por cara |

Los punteros de las definiciones están ya relocalizados a RAM en tiempo de ejecución (la carga suma la base). Las definiciones son **inmutables tras la carga** salvo el cambio de zona (se liberan y reutilizan direcciones: por eso el id de malla incluye hash del contenido). Un mismo `DEF` lo comparten todas las instancias de un tipo (todos los enemigos usan el mismo).

**Cara = cuadrilátero con 4 índices en orden perímetro A,B,C,D.** El hardware dibuja en orden Z: paquete (v0,v1,v2,v3) = (A,B,D,C); el motor carga `v0=byte0, v1=byte1, v2=byte3, v3=byte2` de `vindices`/`nindices` [V: coincide con el desensamblado de la etapa de transformación y con los colores de los paquetes]. Los **triángulos** se codifican repitiendo el último vértice: `byte2 == byte3` (482 de 2986 caras en el escenario de la zona inicial [V]); además unas 40 caras tienen otras repeticiones (degeneradas, descartables).

UV: 8 bytes por cara = (u,v) de A,B,C,D en 0..255 sobre el rectángulo de textura de la cara: `u_texel = u * (w+1) / 256 + off_x`, `v_texel = v * (h+1) / 256 + off_y` con `(off_x,off_y,w,h)` de la entrada de textura de `texids[cara]` (§5). `tpage` y `clut` también salen de esa entrada. Una cara con id de textura 0 o desconocido (2 de 2986 [V]) usa la entrada vacía: plana.

Anverso: el motor calcula `nclip(v0,v1,v2)` = (A,B,D) con los vértices proyectados y sólo dibuja si > 0, salvo `BOTHFACE`. Equivale a `dot(A_vista, cross(B−A, D−A)) > 0` en la vista del motor (X derecha, Y abajo, Z delante) [M]. (Los paquetes llevan además el signo del área para ordenación de las dobles caras.)

### 3.5 Vértices «welded» (INDIRECT) [V]

En los personajes, las costuras entre articulaciones se arreglan reutilizando el vértice YA TRANSFORMADO de la pieza padre. Al cargar la definición (`enlazar_con_padre`), para cada vértice de una pieza hija que coincide con un vértice del padre (campo `pad` del `SVECTOR` ≠ 0xFFFF antes de enlazar) se marca el índice de la cara con el bit 0x80 y se convierte `pad` en el desplazamiento de ese vértice dentro del paquete del padre: `pad = índice_orden[esquina]·12 + 52·cara_del_padre + 8` con `índice_orden = {0,1,3,2}` (52 = tamaño de paquete GT4, 12 = paso entre vértices). Para reconstruir el índice de vértice del padre: `cara = (pad−8) / 52`, `slot = ((pad−8) % 52) / 12`, esquina `c` con `índice_orden[c] == slot`, vértice `= vindices_padre[cara·4 + c] & 0x7F`. 167 vértices en 53 modelos del volcado de la zona inicial [V].

En el contrato esto es `NsVert.weld` = índice del vértice del padre (0xFF ninguno) y `NsPiece.parent`. El renderer calcula primero la posición transformada de los vértices del padre. Para el color: el motor copia el color del paquete del padre en esas esquinas; el sombreado nativo lo aproxima con la normal propia de la cara (el padre y el hijo comparten posición y casi la misma normal), aceptado como error visual pequeño [P: comparar].

En los modelos INDIRECT los índices de `vindices`/`nindices` usan 7 bits (el bit 0x80 es la marca). Esto importa: sin enmascarar, las normales salen basura [V: así se descubrió].

## 4. Escenario de la zona [V]

**El mapa son objetos del mismo tipo.** Cada zona define N trozos de mapa (hasta 16): cada uno carga su(s) modelo(s) con `crear_objeto(def, TEXT|PAINT|TRANS|BOUND|ONEPIECE, canal 0)`, lo pone en la matriz identidad, **precalcula los colores** con las luces fijas del trozo (`preshade`, §6.2), lo encola y le asigna `group_id = 1 << índice_de_trozo`. El motor activa/desactiva trozos cambiando el `grupo_actual` (`añadir_mapa`/`quitar_mapa`), no los quita de la cola. Por tanto, en la zona inicial: la cola contiene `objs[0]` con 181 piezas y `objs[1]` con 23 piezas (`flag 0x57`, `group_id 1`), seguidos de los personajes y objetos.

Para el renderer: un objeto de mapa = instancia `NS_IF_STATIC` (flag con PAINT+ONEPIECE y sin SHADE), matriz identidad, malla con `paint` (colores RGBA por esquina de cara, ya con luces fijas), `group` para filtrar por `grupo_actual`. Las mallas del mapa son grandes y estáticas: perfectas para caché (cambian sólo si el motor recalcula el preshade).

Colisión/hazard del mapa (`hzd`) no se dibuja; si hace falta para el límite de la cabeza ya está `vr_headcol`.

## 5. Texturas [V]

Tabla de texturas la tabla de texturas en `0x800B30E0`: 512 entradas de **12 B** (`TEX`): `u16 id`, `u8 used`, `u8 col`, `u16 tpage` (+4), `u16 clut` (+6), `u8 off_x` (+8), `u8 off_y` (+9), `u8 w` (+10), `u8 h` (+11). Búsqueda por sondeo lineal desde `id % 512` hasta una entrada con id 0 (vacía) o coincidente. El `id` es un hash de 16 bits del nombre de la textura. 2984 de 2986 caras resolvieron entrada [V]. Las páginas de textura y CLUT están en VRAM; el renderer ya las reconstruye por eventos (no se entregan aquí). La textura térmica/nocturna es un cambio de CLUT escrito en VRAM por el propio motor (`bound` → `actualizar_textura_térmica`) y entra por los mismos eventos; la bandera `IRTEXTURE` sólo señala qué objetos la admiten.

## 6. Luces y sombreado

### 6.1 Datos [V]

- **Luz principal global**: `MATRIX` de luz en `0x8009E55C` (`dir` principal en la fila 0; por defecto `(0, −4096, 0)`), `MATRIX` de color en `0x8009E57C` (columna 0 = color de la luz principal ×16; por defecto 560 = 35·16). **Ambiente global** en `0x800AC530` (3 × s16, 0..255; por defecto 51). Código de paquete en `0x800AC538` (derivado del ambiente).
- **Luz por objeto**: `objs->light` apunta a un par de matrices: `light[0]` = tres direcciones de luz (filas, 4096 = 1,0; módulo ≤ 3072 en las puntuales por la atenuación) y, en `t[0..2]`, el ambiente propio si `AMBIENT`; `light[1]` = colores (columna `l` = color de la luz `l`, ×16).
  Los actores móviles rellenan su par cada fotograma con `obtener_matriz_de_luz(pos, light)`: copia la luz principal + hasta **2 luces puntuales** (temporales del fotograma anterior y fijas de la zona) que alcanzan la posición (caja de alcance `e_range`): `fila_l = ((p − punto)·3072/|d|) · max(0, 2·r_range − |d|)/r_range`. El color de cada puntual es el de la luz (`LIT.color`) ×16. En la edición analizada: luz puntual de un actor (1093,−621,−1330), color (105,155,105)·16. Hay un caso térmico (visión térmica activa): colores a 0 y ambiente fijo (130,104,80).
- **Luces fijas del mapa** (`LIT`: `SVECTOR point; u16 r_range; u16 e_range; CVECTOR color`, 16 B) en la lista `lit` de cada trozo y hasta 8 listas registradas; sólo se usan para el preshade (§6.2) y para los actores vía `obtener_matriz_de_luz`.

### 6.2 Sombreado de objetos dinámicos (etapa 4) [V]

Para cada pieza visible de un objeto con `SHADE`: la GTE se carga con `L' = light[0]·R_pieza` (matriz de luz efectiva = filas de luz × rotación de mundo de la pieza), `LC = light[1]` y `BK = ambiente·16` (el del objeto si `AMBIENT`, si no el global), y para cada normal del modelo se ejecuta la instrucción **NCT** (normal, color):

```
I_l = clamp( (L'_l · n) >> 12 , 0 , 0x7FFF )            l = 0..2   (n s16, 4096 = 1)
c   = clamp( (BK·4096 + Σ_l LC[canal][l]·I_l) >> 12 >> 4 , 0 , 255 )     (NCS/NCT no multiplican por el color RGBC: sólo toman su byte de código)
```

El color de la esquina sale como `c` por canal y el hardware lo usa para MODULAR la textura (128 = neutro). **Validación:** reproduciendo esta fórmula en Python contra los colores `r0..r3` de los paquetes `POLY_GT4` de la RAM, **coinciden exactamente (0 de 36 y 0 de 32/220 caras distintas) en los objetos con luces de actor y en el personaje** (con luz puntual de color distinto por canal), una vez enmascarados los índices a 7 bits en modelos INDIRECT y descartadas las caras no sombreadas por el motor (cara no visible: color 0x80 intacto). Otras piezas del personaje difieren porque el volcado se toma después de que el siguiente fotograma ya movió articulaciones y luces; es el comportamiento esperado de leer los paquetes del búfer anterior, no un fallo de la fórmula. **[P]** repetir con volcado tomado en el punto de enganche.

`ns_shade_normal` en `ns_types.h` implementa exactamente esa fórmula en float. El renderer la evalúa por vértice o por píxel con la rotación REAL de la pieza en cada fotograma de visor (la luz «sigue» la pose).

Caras sin sombrear: `UNLIT` (modelo 0x4) o sin `SHADE` ni `PAINT`: color neutro `0x80` (textura sin modular). Con `TRANS`, el bit de semitransparencia del paquete se enciende (el modo de mezcla viene del `tpage` de la textura).

### 6.3 Sombreado estático del escenario (preshade) [M]

`PAINT`: los 4 colores por cara (orden perímetro A,B,C,D, copiados a los paquetes como `r0=rgbs[0], r1=rgbs[1], r2=rgbs[3], r3=rgbs[2]`) se calculan UNA vez al cargar/cambiar de grupo: para cada vértice se buscan las dos luces fijas del trozo más cercanas dentro de su `e_range` (atenuación igual que arriba), se carga la GTE con la luz principal + esas dos y se ejecuta **NCS** con la normal de la esquina (la rotación de mundo del objeto compuesto). El resultado se guarda en `objs->objs[0].rgbs` (16 B por cara). El extractor lee los colores YA calculados (no recalcula luces).

## 7. Niebla y atenuación por profundidad [M]

El motor **no usa el depth-cue de la GTE** (no se carga color lejano) en el canal principal. Sólo hay niebla en las zonas con el actor de niebla (el que llama a `fijar_etapas_de_canal`): sustituye las etapas visibilidad, transformación, sombreado y ordenación por versiones propias (código de overlay, direcciones distintas por zona) y atenúa hacia **negro** (`ldfcdir(0,0,0)`, `dpcs`) con una rampa lineal: tabla de 256 entradas s16 `0..4096` indexada por `z/256`, con parámetros `start` y `len` en unidades de 256 (leídos del guion de la zona) y un `start` modulado por el zoom de la cámara. Para modelos la atenuación es **por objeto** (z de la pieza 0, multiplica color de luz y ambiente por `1 − tabla[z/256]/4096`); para primitivas es por vértice.

Detección en el extractor: la entrada 1/2/3/6 de la tabla de etapas (`0x8009E534 + 4·i`) deja de apuntar a la rutina original (0x80018C8C, 0x8001E840, 0x8001D76C, 0x8001DA18). `NsFog` lleva `start/len` en unidades del motor; el extractor los calcula del estado del actor (dirección dependiente de la overlay: **[P]**, hoy `fog.active = 0`).

## 8. Primitivas de efectos [V]

`PRIM` (0x54 B): `+0x00 MATRIX world`, `+0x20 MATRIX *root`, `+0x24 s32 type`, `+0x28 u16 group_id`, `+0x2A s16 prim_count`, `+0x2C s16 chanl`, `+0x2E s16 raise`, `+0x30 s16 psize`, `+0x32 s16 verts`, `+0x34 s16 voffset`, `+0x36 s16 vstep`, **`+0x38 SVECTOR *pos`**, `+0x3C RECT *rect`, `+0x40/0x44 void *packs[2]` (paquetes por búfer), `+0x48 s16 n_prims`, `+0x4C userdata`, `+0x50 callback`.

`type & 31` = clase (0..5 líneas F2/F3/F4/G2/G3/G4, 6..8 sprites, 9..12 tiles, **13..20 polígonos** F3/F4/G3/G4/FT3/FT4/GT3/GT4, 21/22 líneas texturizadas, 23 libre). Indicadores de `type`: `0x100` INVISIBLE, **`0x200` ON_CAMERA** (posiciones ya en espacio de vista), **`0x400` RECTANGLE** (billboard), `0x800` SORTONLY (sólo ordena), `0x1000` ONESIDE, `0x2000` FREEPACKS (los paquetes los rellena `callback`).

Paquete (formato del hardware): `psize` 40 para FT4: tag(4) rgb+code(4) `x0y0`@8 `u0v0clut`@12 `x1y1`@16 `u1v1tpage`@20 `x2y2`@24 `u2v2`@28 `x3y3`@32 `u3v3`@36 (GT4 52 B: colores por vértice @4/@16/@28/@40 y posiciones @8/@20/@32/@44). **Color, textura, tpage, clut y modo de mezcla los escribe el código del efecto en `packs[reloj_bufer]`**; el motor sólo rellena `x,y` y el z de ordenación. Los vértices de mundo están en `pos[]`: vértice `j` del polígono `i` = `pos[i·verts + j]` (en el espacio de `world`/`root` salvo ON_CAMERA).

- `QUAD` (sin RECTANGLE, tipos 0x12 etc.): vértices de mundo `world·pos`. 31+ prims por escena de combate/zona [V]: charcos y huellas (`type 0x12`, 8 por prim, a y ≈ −1400 bajo el suelo = plano de agua), sombras proyectadas planas, láseres (`type 0x116`, líneas FT2/GT2 texturizadas → cinta), brillos.
- `RECTANGLE` (`type 0x412/0x512`): **billboard de tamaño constante en píxeles**: un punto de mundo por primitiva, proyectado; el cuadrilátero se forma alrededor en pantalla con `rect` = (x,y,w,h): esquinas `(−x,−y)` y `(w−x, h−y)` relativas al punto proyectado (con `verts == 1` es un sprite cuyo `x,y` = proyección − (rect.x, rect.y)). El resultado depende de la distancia de pantalla `H` del motor: tamaño de mundo = `píxeles · z / H`. En el contrato: `NsEffect{kind=BILLBOARD, box, ref_h}`; el renderer elige si fija el tamaño de mundo (con la z de la cámara del juego al extraer) o lo mantiene en píxeles angulares.
- `ONE SIDE` (0x1000): se cull por `nclip` igual que los modelos.
- `FREEPACKS`: llama a `callback` (efectos con geometría propia); el extractor no los interpreta: esos paquetes **siguen saliendo por la lista de la GPU** (clase 3D de `vr_scene`) y el extractor los marca para que el consumidor decida. **[P]** medir cuántos hay en partida.

Orden de dibujo en el motor: por z media en OT de 256 casillas por canal; `raise`/`prim->raise` resta/suma profundidad. El renderer nativo usa z-buffer para lo opaco y ordena de atrás adelante lo semitransparente por profundidad de pieza/primitiva; `raise` se convierte en un pequeño desplazamiento de profundidad (decals y charcos por encima del suelo).

## 9. Sombras

No hay sistema de sombras dedicado. La sombra del jugador y de los actores es un **objeto compuesto normal** (`flag 0x16D`/`0x26D`: TEXT+TRANS+SHADE+ONEPIECE+…, una sola pieza = un cuadrilátero plano semitransparente) puesto bajo el actor: cada fotograma el actor recalcula una matriz con la escala adecuada (`world.m[0][0] = ancho`, `m[2][2] = fondo`, el resto identidad) y la pone en la pieza. Para el renderer son instancias estándar (con una matriz que escala). Los reflejos/espejos de algunas zonas son objetos con `ENVMAP` o una segunda copia del modelo con una matriz espejo; no hay trato especial.

## 10. Punto de enganche en el fotograma

Orden en un fotograma de juego (hilo de emulación, en el VBlank de render, cuenta de fotogramas de `hle_frame_count`):

1. Fase de lógica: los actores se actualizan; cada uno escribe `rots`, su matriz de raíz (`poner_matriz_actual`), su `light`, sus primitivas (`pos`/`packs`), crea/destruye objetos (colas).
2. Cámara: el constructor de cámara (0x80017564; envuelto por `vr/camera`) escribe `eye`, `eye_inv` y `screen` del canal principal.
3. **Fin de fotograma**: `fin_fotograma` (0x8001753C) llama a `ordenar_sistema_de_canales(reloj_bufer)` (0x80018304) → bucle de 7 etapas sobre el canal principal, función a función desde la tabla en `0x8009E534`:
   `0x8001D324` pantalla ← **ENGANCHE** · `0x80018C8C` visibilidad · `0x8001E840` transformación · `0x8001D76C` sombreado · `0x8001BA44` primitivas · `0x8001A0C4` subdivisión · `0x8001DA18` ordenación.
   (Argumentos: `a0` = puntero al canal principal `0x800B2B7C`, `a1` = `reloj_bufer`.)
4. `iniciar_fotograma` (0x8001741C) en el siguiente fotograma: manda a la GPU la OT del búfer contrario (`0x80018110`), borra y repone la memoria de paquetes.

**Enganche recomendado:** envolver la etapa de pantalla (0x8001D324) con el mecanismo `rt_patch(addr, fn)` + `rt_call_orig` que ya usa la IA: la envoltura llama a la original (calcula todas las `world`) y después ejecuta el extractor, que lee `a0` (canal), la cola, los objetos y las primitivas. Se ejecuta UNA vez por fotograma de juego (≈25-30 Hz, mismo ritmo que las listas de la GPU), dentro de la CPU emulada, así que lo que lee es coherente (no hay carrera con la lógica). Alternativa sin parche de código: leer en el gancho de VBlank (`frame_hook`) tras el fin de fotograma; las `world` siguen en RAM pero los `rots` de los actores y las luces ya pueden ser del fotograma siguiente (se observa en el volcado: pequeñas diferencias). Si el enganche es el VBlank, descartar la escena cuando `reloj_bufer` no cambió desde la anterior.

Con el parche `sin-recorte` (§54 de la bitácora) casi todos los objetos del entorno pasan la visibilidad; el extractor ignora `bound_mode` de todas formas (el renderer decide por la cámara VR) y descarta sólo: `INVISIBLE` (flag 0x080), `group_id != 0 && !(group_id & grupo_actual)`, piezas con `n_faces == 0` y las ocultas por la capa VR (cabeza del jugador: `bound_mode` de la pieza 6 puesta a 0 por `vrcam_hide_head`; el extractor marca esa pieza `NS_PF_HIDDEN`).

Dónde hay que mirar con cuidado [P]: objetos pegados a otros (`root`), objetos que cambian de `def` en medio de la zona, listas truncadas si el volcado coincide con el borrado de un objeto (leer con comprobación de punteros dentro de RAM y topes: máximo 256 entradas por cola, 181 piezas por objeto, 24 articulaciones).

## 11. Algoritmo de extracción (referencia para `ns_extract`)

```
canal = 0x800B2B7C (principal); n = s16[+0x56]; q = ptr[+0x58]
cam   = canal.eye (+0x30): eye.t, rotación vista->mundo ; H = s16[+0x50]
luz global: 0x8009E55C / 0x8009E57C / ambiente 0x800AC530
for i in 0 .. n-1:
    o = q[i]; si !puntero_ram(o) continuar
    si flag & INVISIBLE  o  (gid != 0 && !(gid & grupo_actual))  continuar
    crear NsInstance (key = o, flags desde `flag`, light = índice de NsLight deduplicada por contenido)
    for k in 0 .. n_models-1:
        p = o + 0x48 + 0x5C*k ; m = ptr[p+0x40]
        malla = resolver(m, rgbs si PAINT)     # cacheada por (m, hash del contenido)
        pieza = {malla.id, world = p+0x00 (rot/4096, t), parent = model.parent, flags}
        extend: la pieza `p.extend` se emite como otra pieza con la misma matriz (modelos con malla encadenada)
prims: for j in prim_index .. queue_size-1:  → NsEffect (QUAD/BILLBOARD según type)
```

`resolver()` decodifica el modelo una sola vez por (dirección, hash): caras con índices enmascarados a 7 bits, UV resueltos con la tabla de texturas, normales, paint.

## 12. Qué se ha comprobado y con qué

Sin ejecutar el juego: se han usado las **instantáneas de RAM ya existentes** de partidas reales (zona inicial y otras; 70+ ficheros de 2 MiB de otras tareas) y el desensamblado del ejecutable del usuario.

- Direcciones y layout de canal, cola (`objs_index` 19-28 objetos, 4-121 primitivas por escena), `OBJS`, `OBJ`, `MDL`, `DEF`, `TEX`, `PRIM` leídos y coherentes en ≥ 6 volcados de zonas/momentos distintos [V].
- Escenario = objetos 0 y 1 de la cola con 181 y 23 piezas [V]; 295 modelos / 2986 caras en la zona inicial; 2984 con textura resuelta; 182 caras con vértices welded; 482 triángulos por repetición.
- Fórmula de luz validada contra colores de paquetes (§6.2) y luz principal por defecto/ambiente/código de paquete coherentes entre sí (`0x3C666666` ⇔ ambiente 51).
- Tabla de etapas localizada con los punteros de las siete rutinas y la rutina de pantalla desensamblada (copia `eye_inv` al scratchpad, recorre la cola).

Pendiente [P]: volcado tomado en el punto de enganche; color de esquinas welded; niebla (parámetros del actor); `FREEPACKS` en partida; modelos con `ENVMAP`; efectos de zonas con rutas propias (la zona con niebla); escena durante cinemáticas del motor (otra cola/cámara); límite de la cola (256).

## 13. Volcados de desarrollo

Los volcados pedidos por la tarea se generan JUGANDO: no se ejecuta el juego en el entorno de desarrollo. La app incluye la captura (`vr/native/ns_dump.h`, escritor en `ns_dumpw.c`, gesto en `app/Sources/DumpCapture.swift`):

- **Gesto:** mantener los **dos clics de stick pulsados ~1 s** durante la partida (en mando normal, L3 + R3). Vibración corta al aceptar y larga al terminar (dos cortas si falló). Se rearma al soltar.
- **Destino:** `<carpeta del usuario>/Dumps/d<NNNN>_<fecha>/` (en `Documents/Dumps` si no hay carpeta).
- **Contenido** (cargable con `ns_dump_open`): `ram.bin` (2 MiB), `scratch.bin` (1 KiB), `vram.bin` (1 MiB), `gte.bin` (64 palabras), `list.bin` (última lista de la GPU capturada), `frame.png` (fotograma real mostrado), `meta.txt` (fotograma, zona, jugador, cámara del motor, registro de cámara VR, pose de la cabeza).
- **Cobertura pedida:** 10 volcados en la zona inicial (quieto, andando, junto al agua, con un enemigo a la vista) y 5 en otra zona.
- **Pruebas:** `make -f vr/native/dump.mk test` (escritor + cargador + PNG con datos sintéticos).
- La RAM contiene datos del juego: no se sube a ningún repositorio.

Mientras tanto, las instantáneas de RAM ya existentes (sin VRAM ni lista) sirven para desarrollar el extractor; se guardan fuera del repositorio.
