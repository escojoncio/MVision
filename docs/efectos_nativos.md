# Efectos y primitivas que no son mallas (renderer nativo)

Complemento de `docs/escena_nativa.md` (primitivas y sombras). Aquí: qué efectos hay, en qué espacio viven, cómo los extrae `vr/native/ns_fx.{c,h}` y cómo se decide qué de la lista de la GPU puede cubrir el renderer nativo.
Marcas: [V] comprobado contra instantáneas locales de RAM (no están en el repositorio); [C] deducido del código y comprobado con pruebas sintéticas; [P] pendiente.

## 1. Cómo se produce un efecto

Los efectos de mundo (destellos, humo, burbujas, salpicaduras, ondas, chispas, trazadores, haces, marcas, partículas, letreros, cintas...) son **primitivas** que cuelgan del canal 3D principal detrás de los objetos de malla. El actor del efecto reescribe cada fotograma los puntos de mundo, el rectángulo y las características de los paquetes; el motor proyecta los puntos y deja en el búfer doble de paquetes (bit 0 del contador de fotogramas) el paquete de hardware ya relleno: posiciones de pantalla, uv, paleta, página de textura, color y código. **Leer la RAM da la animación gratis**: no hay que reproducir ninguna lógica de efecto.

Cada primitiva tiene una clase (`type & 31`, 0..23) que fija la forma del paquete: líneas planas o degradadas, líneas texturizadas, sprites (tamaño libre, 8 o 16), rectángulos, polígonos planos o degradados de 3/4 vértices con o sin textura, y una clase de formato libre cuyos parámetros fija el efecto (no se interpreta; sus paquetes se indexan como 2D). Detalles de disposición (desplazamientos de `xy`, `uv`, paleta, página y colores) en `ns_fx.c` (`dec_pack`).

Los 16 bits bajos de la cabecera del paquete valen la **profundidad de vista `sz` del vértice 0**; `sz == 0` significa que el motor no emitió el paquete en ese fotograma. El elemento se extrae igualmente con `NSFX_F_ENGINE_CULLED` y, si no hay profundidad fiable, `NSFX_F_LOW_CONF`: sus posiciones de mundo son válidas pero ni la reproyección ni la casación con la GPU lo son, así que ese elemento nunca se usa para decidir qué sale de la GPU. Los sprites no traen página de textura y los polígonos sin textura no traen modo de mezcla (`NSFX_F_TPAGE_UNKNOWN`, `NSFX_F_ABR_UNKNOWN`): `ns_fx_resolve_items` los completa desde la operación de la GPU que casó.

## 2. Espacios de coordenadas y tamaños

- **Polígonos y líneas con vértices**: mundo = `R·p/4096 + t` (raíz si la hay, si no `world`). Vista = `eye_inv·mundo`; la Y de vista se multiplica por el factor vertical de la tabla de ajustes (§6) antes de proyectar `sx = H·x/z`, `sy = H·y/z`. [V]
- **Pegado a la cámara** (bandera de primitiva): las posiciones ya están en espacio de vista. `NSFX_F_VIEWLOCKED`: se guarda `vpos` y su equivalente en mundo con la cámara actual. [C]
- **Carteles** (primitiva de rectángulo con polígono de 4 vértices): un punto por primitiva, proyectado; las esquinas se forman en la vista a la **profundidad del centro** con los desplazamientos del rectángulo. Es un cartel que mira a la cámara con tamaño de mundo constante [V]. `ns_fx` entrega `off[4]` (izq, arriba, der, abajo) en unidades de vista y `box_px[4]` (los mismos píxeles).
- **Orientación de carteles y cintas (una vez por fotograma, con la cabeza)**: `ns_fx_billboard_corners` recibe los ejes derecha/arriba de la **cabeza** en el espacio VR de salida y `ns_fx_ribbon_corners` la posición de la cabeza (centro de los ojos). La orientación se calcula una sola vez por fotograma para los dos ojos; así ambos ven la misma geometría y la paralaje estéreo es la correcta. Lo único que sí se evalúa por ojo es el descarte de caras de los polígonos de una cara (`ns_fx_item_faces`).
- **Sprites/rectángulos con tamaño en píxeles**: esquina sup-izq = proyección − desplazamiento del rectángulo, tamaño `w×h` píxeles. Se convierte a mundo a la profundidad original (`px·z/H`): `NSFX_F_PXSIZE`. Si la profundidad es ≤ 0 o menor que `px_z_min`, se usa la profundidad de referencia `px_z_ref` para que nunca quede invisible o infinito.
- **Líneas**: cintas con anchura en píxeles (`line_px`, `line_tex_px`) convertida a mundo con la profundidad de cada extremo (`width[2]`). Las polilíneas se parten en segmentos; las degeneradas (extremos iguales) no generan segmentos.
- **Una cara**: el motor dibuja el polígono si el área en pantalla de los vértices 0,1,2 es > 0. `NsFxItem.fn` es la normal ya multiplicada por el signo del determinante de la vista: **se dibuja si `dot(fn, p-ojo) > 0`**. [C]
- **Paquetes de formato libre ya proyectados**: se reconstruyen desde el paquete a la profundidad `sz` (plano de la cámara): rectángulo alineado con los ejes → cartel `PXSIZE`; si no, polígono plano en ese plano. Con polígonos de menos de 3 vértices se omiten.
- **Orden de dibujo**: `raise` se entrega tal cual (`NsFxItem.raise`, `NsEffect.depth_bias`).
- **Grupos**: con grupo distinto de 0 y sin bit en la máscara de mapa actual, la primitiva no se dibuja; las invisibles y las de ordenación 2D (capa 2D: menús/demos) tampoco se extraen.
- **Superficies de agua y otras superficies planas** son cuadriláteros de mundo; la animación (uv, escala) está en la RAM.

## 3. Catálogo por papel

El papel se deduce del **actor dueño** (el que guarda el puntero a la primitiva en su estructura de trabajo; se busca en las listas de actores de la RAM) mediante una tabla inyectable de hashes FNV-1a 32 del nombre del actor (`ns_fx_set_actor_table`; no hay nombres en claro en el repositorio y las pruebas instalan una tabla con nombres neutros). Si no hay dueño, por forma y mezcla. Es **informativo**: el dibujo no depende de él. Papeles: halo/destello aditivo, burbuja, humo, chispa, gota, salpicadura, onda sobre superficie, trazador, haz, marca plana, sombra sencilla, sombra de objeto, plasma, superficie animada, superficie de agua, partícula ambiental, cono de luz, estela, tira, partícula, sprite animado y sprite pegado a la cámara (`NSFX_ROLE_*`).

**Sombras de objeto**: no son primitivas sino un objeto de malla de una pieza (un cuadrado base) que posee un actor de sombra (puntero al objeto y bandera de habilitación en su estructura). El actor recalcula cada fotograma la matriz de la pieza (escalas en los ejes horizontales) y la del objeto (giro y traslación; la Y es la altura del suelo). `ns_fx` las extrae como cuadriláteros `NSFX_ROLE_SHADOW_OBJ` (semilado `shadow_half` de la tabla de ajustes por la escala de la pieza, `FROM_OBJ`) con el orden de vértices, uv, color, paleta y página del paquete real de la pieza (cada esquina se asigna a la que proyecta más cerca del `xy` del paquete). Si no cabe en `shadow_objs` (`NSFX_MAX_SHADOW_OBJS`) no se crea el elemento y el objeto sigue por el camino de mallas. Los objetos reclamados se listan en `NsFxScene.shadow_objs`: **el extractor de mallas debe omitirlos**.

## 4. Efectos de pantalla completa (post-procesos)

No son geometría de mundo: en el renderer nativo se hacen sobre la imagen ya compuesta de **cada ojo**. `NsFxScreen` da el estado leído de la RAM, y **solo ese estado** autoriza a clasificar una operación de la GPU como pantalla (§5).

- **Agua**: el actor existe si hay una caja de agua (mín/máx por eje, en mundo; la cara superior es la superficie) y está activo mientras el ojo del motor está dentro de la caja. Efecto original: tinte multiplicativo por canal, ondulación horizontal por filas (`wave_rows`, `wave_period`, fase = contador del actor) y barras opacas arriba y abajo (`bar_rows`). En nativo: probar el ojo VR contra la caja → tinte en el fragmento (mundo) + ondulación de pantalla (post-proceso por ojo, solo con la cabeza dentro); las barras no tienen sentido en VR.
- **Modos de visión** (bits 2 y 3 de la palabra de estado global, `NsFxScreen.mode_a/mode_b`; el bit 1 es un destello de pantalla y el bit 0 auxiliar): un recubrimiento aditivo de pantalla completa y líneas de exploración finas. Las paletas son cambios de CLUT en la VRAM que el consumidor de VRAM ya reproduce. Post-proceso: tinte + exploración suave.
- **Anillo**: polígonos degradados semitransparentes en anillo entre un radio interior y `ring_outer_px`, con razón vertical/horizontal `aspect`; colores interior/exterior con intensidad y pulso (`NsFxGas`, recortados a rangos válidos: intensidad 0..1, aspecto 0..2, colores 0..1). En nativo: viñeta de color en el borde del campo de visión de cada ojo.
- **Fundido a negro/blanco**: recubrimiento de pantalla completa que va de 0 a 255 (o al revés). `NsFxFade`: blanco/negro, `amount` 0..1, sentido.
- Los destellos de pantalla y los filtros que realimentan el fotograma anterior se tratan como 2D salvo que el estado de la RAM los justifique.

## 5. Clasificación de la lista de la GPU y cobertura CONFIRMADA

`ns_fx_classify(ram, escena, lista, cls[], sub[], item[], stats)` etiqueta **cada** operación de la lista de dibujo (sin cambiar `GpuOp`). Las clases son solo **candidatas**, salvo 2D/EVENT/MAINT:

| clase | significado |
| --- | --- |
| `EVENT` | relleno/copia/subida/descarga de VRAM: se aplica a la VRAM; no dibuja |
| `2D` | HUD, menús, textos, radio, y todo lo que no se pueda justificar: **sigue saliendo de la GPU** |
| `MESH` | candidato: coincide exactamente con un paquete emitido de un objeto de malla |
| `MESH_GUESS` | candidato: polígono con posición de vista limpia (sin ambigüedad, saturación ni vértice detrás de la cámara) y sin paquete que coincida |
| `FX` | candidato: coincide exactamente con un elemento de `NsFxScene.items` (`item[i]` = índice) |
| `FX_PACK` | candidato: coincide con un paquete de una primitiva de efecto no extraída (invisible/otro mapa) y tiene posición de vista limpia |
| `SCREEN` + `sub` | candidato: efecto de pantalla justificado por el estado de la RAM |
| `MAINT` | reservado; no lo produce ns_fx |

**Contrato de cobertura confirmada.** El renderer indica con `confirmed[i]` qué operaciones ha dibujado realmente (malla/efecto/post-proceso equivalente), y `ns_fx_gpu_mask(cls, confirmed, n, gpu)` devuelve `gpu[i] = 1` para todo lo que debe seguir saliendo por la GPU: todo lo que no es candidato y todo candidato no confirmado (`confirmed == NULL` = nada confirmado). El peor caso aceptado es un doble dibujo cosmético; HUD, menús, radio y textos no deben desaparecer nunca. Ante la duda, 2D.

Reglas de seguridad de la casación:
- La casación es **por contenido exacto** (vértices menos el offset de dibujo, tamaño de rectángulo, u,v, color, modo de color de la textura y semitransparencia) y solo contra los paquetes del **búfer vigente**. Los elementos llevan los datos de su paquete, así que con `ram == NULL` solo se casa con `sc->items` y no se produce `MESH`.
- Si casa cualquier entrada de ordenación 2D, **gana 2D**. Prioridad entre las demás: elemento > malla > paquete de efecto no extraído.
- Los elementos sin profundidad fiable (`ENGINE_CULLED`, `LOW_CONF`) no se indexan.
- Las primitivas de formato libre o de ordenación 2D que no se extraen se indexan como 2D. Índice con tabla de (primitiva, paquete) → elemento precalculada en la extracción, tope global de entradas (`idx_max`) y cadenas hash acotadas (`chain_max`): al desbordar, todo lo que no es evento sale como 2D (`idx_overflow`).
- Pantalla (`SCREEN`) solo con el estado que la justifica: `COVER` (recubrimiento semitransparente liso a pantalla completa) con fundido, destello o modo de visión activos; `SCAN` (líneas finas de ancho de pantalla) con modo de visión; `BARS` (barras opacas lisas en el borde) y `COPY` (relectura de una textura de 15 bits que sale del área de dibujo vigente) con agua activa; `RING` con anillo activo y **exactamente** `ring_segments` polígonos degradados semitransparentes con simetría radial. Cualquier otro recuento o reparto → 2D.

`ns_fx_resolve_items` copia de la operación casada lo que el paquete no trae. `NsFxClsStats` da recuentos y cuántos elementos fiables quedaron sin operación (`n_fx_items_missing`).

Orden de uso por fotograma: extraer → clasificar con la lista del mismo fotograma → dibujar nativo lo candidato, anotar qué se dibujó de verdad → `ns_fx_gpu_mask` → la GPU dibuja el resto (más los eventos de VRAM, siempre).

## 6. Tabla de ajustes (`NsFxTuning`)

Constantes del motor con descripción genérica; valores por defecto en `ns_fx_tuning_default`:

| campo | descripción | defecto |
| --- | --- | --- |
| `y_scale` | factor vertical de la proyección de posiciones (también `NsCamera.overscan_y` de `ns_types.h`) | 66/64 |
| `line_px`, `line_tex_px` | grosor de las líneas planas/texturizadas, en píxeles | 1, 1,5 |
| `shadow_half` | semilado del cuadrado base de los objetos de sombra, antes de escalar | 4000 |
| `px_z_min`, `px_z_ref` | profundidad mínima para convertir píxeles a mundo y profundidad de referencia si no se alcanza | 32, 1000 |
| `ring_outer_px`, `ring_segments` | radio horizontal exterior y número de polígonos del anillo | 230, 16 |
| `wave_rows`, `wave_period`, `bar_rows` | ondulación y barras del agua | 4, 32, 6 |
| `idx_max`, `chain_max` | topes del índice de coincidencias | 65536, 32 |

El factor vertical se midió reproyectando paquetes reales de polígonos de mundo y carteles con varias cámaras; con 66/64 el error medio queda en torno a 1 píxel (`ns_fx_dump --ysc` permite barrerlo).

## 7. Validación

- Instantáneas locales de RAM: `ns_fx_dump` reproyecta cada vértice de mundo y esquina de cartel con la cámara del canal y compara con los `xy` que escribió el motor (media ≈ 1 px); todos los paquetes emitidos por el motor de primitivas visibles del mapa tienen elemento. La selección de búfer (reloj o el que reproyecta mejor) acierta.
- Pruebas sintéticas (`make -f vr/native/fx.mk test` y `test-san`, sin datos del juego, con proyección de referencia independiente): polígonos, carteles, sprites en píxeles, haces, polilíneas, pegado a cámara, una cara frente al criterio del motor, filtros de grupo/invisible/ordenación, papeles por tabla inyectada, sombra con orden de vértices permutado, agua/anillo/fundido, clasificación; una **tabla de casos que deben seguir siendo 2D** (tiras de texto, separadores, fondos, recubrimientos sin fundido, anillos con recuento o reparto distinto, vértices ambiguos/saturados/detrás, relecturas fuera del área, ordenación 2D idéntica a un elemento); un **fuzzer estructurado** con la propiedad «sin efecto activo nada sale de 2D/EVENT salvo coincidencia exacta»; límites del gas; peor caso de rendimiento acotado; `ram == NULL`; máscara de cobertura confirmada; RAM aleatoria con ASan/UBSan.
- **[P]** Contrastar con lista de GPU + RAM del mismo fotograma (volcados de la app): ningún efecto visible sin elemento y ninguna operación de HUD en `FX`/`SCREEN`.

## 8. Contrato con el resto del renderer

- `ns_fx_export.{h,c}`: `NsFxScene.items` → `NsEffect[]` (`NS_EK_QUAD` con 2/3/4 vértices; `NS_EK_BILLBOARD` con `box` en píxeles y `ref_h`; líneas con `NS_EF_LINE` y `width_px`; pegado a cámara con posiciones de vista; `depth_bias` = `raise`). Los efectos de pantalla completa se leen de `NsFxScene.screen`.
- Todo se lee de la RAM con comprobación de punteros; capacidad por defecto 4096 elementos.
- Conversión mundo → VR: `ns_fx_to_vr` aplica la misma regla que `vr_scene` (escala 0,001, origen, espejo Z si el determinante de la vista es negativo, mapa de usuario opcional).

## 9. Efectos de pantalla en el renderer nativo (sin post-proceso de la lista de la GPU)

`NsFxScreen` → `NsScreenFx` (`NsScene.screen`) → `ns_screen_build` (`vr/native/ns_screen.{h,c}`) produce, por fotograma, **parámetros del mundo** y una **lista ordenada de recubrimientos**:

- **Agua**: volumen en el mundo del motor. El tinte multiplicativo lo aplica el sombreador de fragmentos por posición del mundo (`ew` y `fromvr` en el sombreador), de modo que está anclado al mundo y no gira con la cabeza; si la cabeza está dentro del volumen se aplica a todo el mundo. La **onda** es una distorsión de la IMAGEN FINAL del ojo (como el PS1: desplazamiento horizontal por fila de pantalla, animado con la fase del actor), no de las texturas: solo con la cabeza dentro, `RGRenderer.planWave`/`encodePost(wave:)` resuelven la escena nativa en una textura intermedia del tamaño del drawable (una vez, se recrea si cambia) y un pase por ojo (`vr_fs_wave`, con capas o sin ellas) la copia al drawable con `dx = amp * (ancho/1888) * sin(freq * fila + fase)` px; la fila es la del motor desde el centro óptico (`(y - cy)/fy * H`), `amp = wave_rows * wave_gain`, `freq = 2π/(wave_period * wave_space)` rad por fila (`ns_screen_wave_dx`/`ns_screen_wave_row`, referencia de CPU `ns_screen_wave_rgb8`, prueba `test_ns_wave`). Va antes de los paneles 2D del pase final; no toca la profundidad. El radar y el arma del pase de escena también se ondulan. Las barras del original son recubrimientos opcionales (`bars_gain`, 0,85 por defecto).
- **Recubrimientos** (un triángulo de pantalla completa por recubrimiento, sin profundidad, tras todo el mundo; las tangentes salen de la proyección de cada ojo, así que no dependen de la resolución): barras, tinte del modo de visión A/B más líneas de exploración finas, anillo de gas, destello (con suavizado temporal `flash_slew`) y fundido a negro/blanco. Orden fijo: barras, modo, anillo, destello, fundido.
- **Niebla en los efectos**: cada vértice de efecto lleva `1 - ns_gpu_fog_f(z)`, la misma fórmula que las mallas.
- **Una sola pasada semitransparente** de atrás adelante: `ns_gpu_semi_merge` intercala los triángulos semitransparentes de las mallas con los efectos semitransparentes (profundidad de la vista de ordenación).
- **Formato libre (kind 23) y FREEPACKS**: `ns_fx.c` los decodifica (`dec_free`) y los clasifica por posición en pantalla; el diseño de los campos se dedujo del código y no se pudo contrastar con un volcado que los contenga.
- **Presentación** (`ns_present.{h,c}`): `CURRENT` (escena vigente), `HOLD` (se repite el último fotograma nativo válido con la cabeza actual, 9300 ms) y `BLACK` (fundido suave a negro, 700 ms; solo si no hay escena dibujable durante 10 s). No existe un modo que dibuje el 3D de la lista de la GPU.
- **Cinemáticas del motor** (`ns_cinema.{h,c}`): se pinta la escena nativa desde la cámara del juego (A, t, H, desplazamiento OFX/OFY, sobredimensión de la vista 66/64, espejo si el determinante es negativo) con dos cámaras paralelas de ventana desplazada, en la textura de la pantalla 3D; la capa 2D va encima.
- Pruebas: `make -f vr/native/fx.mk test test-san` (`test_ns_fx`, `test_ns_gpu_fx`, `test_ns_screen`, `test_ns_present`, `test_no_gpu3d`). `test_no_gpu3d` lee los fuentes Swift/Metal/C y falla si reaparece un camino de dibujo 3D por la lista de la GPU o el modo «gpu».

- **Escena válida no apta para primera persona** (vehículos, ascensores, zonas especiales): se coloca con la cámara del juego y se enseña en la pantalla 3D; vuelve al mundo al ser apta (histéresis). La ventana de la pantalla 3D usa la H real y OFX/OFY de la escena (`ns_cinema_window`).
- **Bajo el agua**: sin barras (BARS desactivado por defecto: dejaba un círculo claro con el resto casi negro y translúcido); con la cabeza sumergida el sombreador descarta por detrás las caras de una cara de mallas semitransparentes (NSG_CULL_SEMI: la superficie del agua vista desde abajo, que el motor no dibuja); la onda de pantalla no depende de las texturas (no abre agujeros). **Caja**: NSG_OV_FRAME (máscara 160x128 de vrhost, solo con BRIDGE_PS_BOX, anclada a la cabeza).
- **Recubrimientos**: RING y BARS son viñetas circulares; SCAN está anclado al mundo (elevación del rayo). Ninguno gira con la cabeza.

## Comodidad VR de los efectos cerca de la cabeza (ns_gpu_fx_build_ex) — ELIMINADA: `comfort` y `oncam_drop` ya no existen (solo queda el descarte a < 15 cm de la cabeza, `drop_inside_m`); el texto siguiente es histórico
- Tope angular por distancia del centro a la cabeza: 0 a <= `comfort_dmin_m` (0,12 m: se descarta, "nace dentro de la cabeza"), X (20 grados) a 0,3 m, 90 grados a 1 m, sin tope a mas de 1 m. Un efecto que lo supera se REDUCE alrededor de su centro (no se mueve). Los papeles SURFACE y WATER_SURFACE (entorno) quedan fuera.
- Pegados a la camara del motor (`NS_EF_ON_CAMERA` o papel VIEW_OVERLAY): no tienen emisor en el mundo; con el centro a menos de `oncam_drop_m` (1,5 m) se descartan. Descartar cuenta como confirmado (la clave entra en `keys`: la ruta de la GPU no los redibuja).
- Los recubrimientos intencionados de pantalla (fundido, gas, agua, modos de vision) NO pasan por aqui: son `NsScreenFx`.
- Registro (1 s): `fx-cerca(<0.5m): ...` dentro de la linea `nat-diag` (acumulador `NsGpuFxNearAcc`): fotogramas, con cercanos, en movimiento (>0,3 m/s o >30 grados/s), maximos de cercanos / pegados / pantalla, reducidos, descartados, carteles con profundidad de referencia y, por efecto, papel, forma, ON_CAMERA, distancia, tamano, angulo antes->despues, accion y origen (primitiva/paquete).

## Tamano en mundo (sin profundidad de referencia)
- Un cartel/sprite de tamano en pixeles mide `px * z / H` con la z de vista REAL del vertice del motor y se exporta con `NS_EF_WORLD_SIZE` (semiextensiones en unidades del motor, independientes de cualquier profundidad). El tamano se aprende por clave (`ns_fx_item_key`) al emitirse; un elemento que el motor no emite usa el aprendido, y sin el no se exporta (`NSFX_F_NOSIZE`). Antes los no emitidos salian con la profundidad de referencia 1000 (hasta 2000 m de lado).
- La comodidad (tope angular / descartes de pegados a la camara) queda como opcion en `ns_gpu_fx_build_ex`; la app solo usa `drop_inside_m = 0,05`.
