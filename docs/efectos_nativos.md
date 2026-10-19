# Efectos y primitivas que no son mallas (renderer nativo)

Complemento de `docs/escena_nativa.md` (§8 primitivas, §9 sombras). Aquí: qué efectos hay, en qué espacio viven, cómo se animan, cómo los extrae `vr/native/ns_fx.{c,h}` y cómo se decide qué de la lista de la GPU sigue saliendo por la GPU.
Marcas: [V] comprobado con instantáneas de RAM de partidas reales (14 ficheros de 2 MiB, 13 con paquetes de efectos; ninguno en el repositorio); [C] leído del código del motor y comprobado solo con pruebas sintéticas; [P] pendiente.

## 1. Cómo se produce un efecto

Todos los efectos «de mundo» (destellos, humo, burbujas, salpicaduras, ondas, chispas, sangre, trazadores, láseres, huellas, plasma, nieve, letreros, cintas...) son **primitivas** (`PRIM`, ver `escena_nativa.md` §8) que cuelgan del canal principal `0x800B2B7C` detrás de los objetos de malla. El actor del efecto reescribe cada fotograma `pos[]` (puntos de mundo), `rect` y las características de los paquetes; el motor proyecta `pos[]` y deja en `packs[reloj]` (doble búfer, `reloj = u32[0x800BCAB0] & 1`) el paquete de hardware ya relleno: posiciones de pantalla, uv, clut, tpage, color y código. **Leer la RAM da la animación gratis**: no hay que reproducir ninguna lógica de efecto.

Formatos de paquete (offsets desde el inicio del paquete; `voffset` = 8; `vstep` según clase):

| clases (`type & 31`) | paquete | notas |
| --- | --- | --- |
| 0-2 líneas planas F2/F3/F4 | `xy` @ 8 + 4v | color @4 |
| 3-5 líneas degradadas | `xy` @ 8 + 8v | color v0 @4, color v>0 @ 8 + 8(v-1) + 4 |
| 6-8 sprites (libre/8/16) | `xy`@8, `u,v`@12, `clut`@14, `w,h`(u16)@16 solo en la libre | **la página de textura NO está en el paquete** (la fija un cambio de modo de dibujo aparte) |
| 9-12 rectángulos (libre/1/8/16) | `xy`@8, `w,h`(u16)@12 solo en el libre | sin textura |
| 13-16 polígonos planos/degradados | `xy` @ 8 + 4v (F) / 8 + 8v (G) | color como líneas |
| 17-18 FT3/FT4 | `xy`@8+8v, `uv`@12+8v, `clut`@14, `tpage`@22 | color @4 |
| 19-20 GT3/GT4 | `xy`@8+12v, `uv`@12+12v, `clut`@14, `tpage`@26 | color v0 @4, v>0 @ 8 + 12(v-1) + 8 |
| 21-22 líneas texturizadas | disposición de FT4/GT4 con solo los vértices 0 y 1 dados | el motor rellena los 2 y 3 desplazando 1,5 px (ver §3) |
| 23 libre | `psize/verts/voffset/vstep` los fija el efecto | no se interpreta (`stats.n_free`) |

Los 16 bits bajos de la cabecera del paquete valen la **profundidad de vista `sz` del vértice 0**; `sz == 0` significa que el motor descartó el paquete en ese fotograma (detrás de la cámara del juego, fuera del rango, cara trasera en las de una cara). Código del paquete (byte @7): bit 1 semitransparente, bit 0 textura cruda, bit 2 texturizado (sprites). Modo de mezcla de los polígonos texturizados: bits 5-6 del `tpage`; **en los sin textura (planos, líneas, rectángulos) no está en el paquete**.

## 2. Espacios de coordenadas y tamaños

- **Polígonos y líneas con vértices** (`type` sin `0x400`): `pos[i·verts + j]` en el espacio de `root` si hay, si no de `world`: mundo = `R·p/4096 + t`. Vista = `eye_inv·mundo`, y **la Y de vista se multiplica por 66/64** (ver §6) antes de proyectar `sx = H·x/z`, `sy = H·y/z`. [V] error medio de reproyección 0,85-1,3 px en 13 volcados con paquetes (ver §7).
- **`ON_CAMERA` (`0x200`)**: `pos` ya está en espacio de vista (matriz de proyección nula; sin factor vertical): pegado a la cámara del juego. `ns_fx` lo marca `NSFX_F_VIEWLOCKED`, guarda `vpos` y su equivalente en mundo con la cámara actual. [C]
- **Billboard (`RECTANGLE`, `0x400`, polígono de 4 vértices)**: un punto por primitiva (`pos[j]`), proyectado; las esquinas se forman en la vista a la **profundidad del centro**: izquierda `-rect.x`, arriba `-rect.y`, derecha `rect.w-rect.x`, abajo `rect.h-rect.y` (unidades de vista = unidades de mundo, sin factor vertical: en pantalla `dy = H·by/z`). Por tanto es un cartel **orientado a la cámara y de tamaño de mundo constante** [V]; el orden de esquinas del paquete es sup-izq, sup-der, inf-izq, inf-der. `ns_fx` entrega `off[4]` = (izq, arriba, der, abajo) en unidades de vista y `ns_fx_billboard_corners` las orienta con los ejes derecha/arriba **del ojo VR** que se le pasen (un cartel por ojo).
- **Sprites/rectángulos con tamaño en píxeles** (`verts == 1`: clases 6-12, con o sin `RECTANGLE`): esquina sup-izq = proyección − (`rect.x`, `rect.y`) (0 si no hay `rect`), tamaño `w×h` píxeles de pantalla. Se convierte a mundo a la profundidad de vista original (`px·z/H`; vertical además `/66·64`): `NSFX_F_PXSIZE`. [V]
- **Líneas** (clases 0-5, 21, 22): cintas. Anchura = 1 px (1,5 px en las texturizadas, que el motor ensancha desplazando 1 px en Y o 2 en X según la pendiente) convertida a mundo con la profundidad de **cada extremo** (`width[2]`). `ns_fx_ribbon_corners` construye la tira que mira al ojo. [C]
- **`ONESIDE` (`0x1000`)**: el motor dibuja el polígono si el área en pantalla de los vértices 0,1,2 es > 0. Equivale a `det(vista)·dot(cross(p1-p0, p2-p0), p0-ojo) > 0` en mundo; `NsFxItem.fn` es la normal ya multiplicada por el signo, de modo que **se dibuja si `dot(fn, p-ojo) > 0`** (`ns_fx_item_faces`). Probado contra el criterio del motor con cámara girada [C].
- **`FREEPACKS` (`0x2000`)**: la geometría la fabrica una función del efecto. Se reconstruye desde el paquete a la profundidad `sz` (plano de la cámara del juego): rectángulo alineado con los ejes → billboard `PXSIZE`; si no, polígono plano en ese plano. Sin `sz` no se puede (se cuentan como descartados por el motor). [C] ([P] medir cuántos hay en partida: en los volcados disponibles no aparece ninguno).
- **Orden de dibujo**: `raise` (prim `+0x2E`) acerca la primitiva en la ordenación; se entrega tal cual (`NsFxItem.raise` / `NsEffect.raise`).
- **Grupos**: `group_id != 0 && !(group_id & máscara_de_mapa)` no se dibuja (`u32[0x800BCAF8]`); invisibles (`0x100`) y solo-ordenación (`0x800`) tampoco (estas últimas son capa 2D: menús/demos).

## 3. Catálogo por papel

El papel se deduce del **actor dueño** (el que guarda el puntero a la primitiva en su estructura de trabajo, buscado en las 9 listas de actores `0x800ADDA8`, 68 B cada una; nodo = cabecera del actor: `+8` función, `+20` puntero al nombre de fichero) con una tabla de hashes FNV-1a 32 de esos nombres (no hay nombres en claro en el repositorio); si no hay dueño, por forma y mezcla. Es **informativo** (para que el renderer elija mezcla/orden/animación propias); el dibujo no depende de él.

| papel | forma típica | mezcla | animación [C] salvo indicación |
| --- | --- | --- | --- |
| `glow` (halos de lámparas, destellos, LED, brasa, ojos) | billboard FT4, rect cuadrado | aditiva (abr 1) | tamaño/color por actor [V: 14-21 por escena] |
| `bubble` | billboard | semi | sube; tamaño en la vista |
| `smoke` | billboard | semi | crece y se desvanece (color) |
| `spark`, `blood`, `splash` | billboards / sprites px | semi | pos[] por partícula |
| `ripple` | **cuadriláteros de mundo** en el plano de agua (8 por primitiva, Y ≈ -1400 en la zona inicial) | semi | la escala de `world` crece [V: 16-64 por escena] |
| `tracer`, `laser`, `trail` | cintas (líneas texturizadas) o polilíneas | semi/aditiva | extremos por fotograma |
| `footprint`, `shadow` | cuadriláteros planos | resta/semi | fijos |
| `plasma`, `searchlight` (cono de luz), `surface` (letreros, paneles, cristales) | cuadriláteros/triángulos de mundo | semi | uv animados por el actor |
| `weather` (nieve, chaff) | sprites px | semi | pos[] por partícula |
| `water_surface` | cuadriláteros | semi | uv desplazados |

**Sombras de personaje [V]**: no son primitivas sino un **objeto de malla de una pieza** (un cuadrado de ±4000 en XZ) que posee un actor de sombra (una por personaje; el actor guarda el puntero al objeto en `+0x28` y su bandera de habilitación en `+0x90`). El actor recalcula cada fotograma la matriz de la pieza (`m[0][0] = ancho`, `m[2][2] = fondo`, resto 0; traslación 0) y la del objeto (giro en Y, traslación = XZ del personaje, **Y = altura del suelo**); mezcla de resta. `ns_fx` las extrae como cuadriláteros `NSFX_ROLE_SHADOW_CHAR` (esquinas = ±4000·escala/4096 en XZ, `FROM_OBJ`) con el orden de vértices, uv, color, clut y tpage del paquete real de la pieza (asigna cada esquina a la que proyecta más cerca del xy del paquete). Los objetos reclamados se listan en `NsFxScene.shadow_objs`: **el extractor de mallas debe omitirlos** o se dibujarían dos veces (si se prefiere dejarlas como instancias normales con matriz que escala, basta ignorar los elementos `FROM_OBJ`).

## 4. Efectos de pantalla completa (post-procesos)

No son geometría de mundo: en el renderer nativo hay que hacerlos sobre la imagen ya compuesta de **cada ojo**. `NsFxScreen` (de `ns_fx_extract`) da el estado leído de la RAM; las primitivas que los dibujan en la lista de la GPU se clasifican `NSFX_OP_SCREEN` (§5) para que no se dupliquen.

- **Bajo el agua** [V] (actor de vista submarina): existe si hay una caja de agua (mín/máx s16 por eje, en mundo; la cara superior es la superficie) y está **activo mientras el ojo del juego (`eye.t` del canal) está dentro de la caja** (bandera de reserva de paquetes). Efecto original: tinte multiplicativo por canal (`color/128`, por defecto 100/128/120), **ondulación horizontal** por filas (desplazamiento seno de ±4 filas con periodo de 32 pasos de 2 filas, fase = contador de fotogramas que lleva el actor) y **barras negras de 6 filas** arriba y abajo. En el nativo: probar el ojo VR (no el del juego) contra la caja → tinte + ondulación suave (o refracción) en el fragmento; las barras no tienen sentido en VR. `NsFxWater` da caja, tinte, fase, amplitud, periodo y barras. Las tiras de copia de pantalla que el motor emite para ondular se clasifican `SCREEN/COPY` (no dibujarlas).
- **Gafas de visión nocturna / térmicas** (bits 2/3 de `u32[0x800AC570]`; aturdimiento bit 1, chaff bit 0): las gafas se dibujan con (a) un recubrimiento aditivo de pantalla completa con color y (b) líneas de exploración finas [C]; las **paletas** (nocturna verde, térmica) son **cambios de CLUT en la VRAM** que el consumidor de VRAM ya reproduce, así que las texturas del renderer nativo salen teñidas sin hacer nada. Post-proceso: tinte + exploración suave; cualquier marco/viñeta en VR mejor omitirlo o hacerlo en 2D delante de cada ojo. `NsFxScreen.nvg/thermal` = bits crudos. Con térmicas el motor no dibuja el anillo de gas.
- **Anillo de gas** [C]: 16 polígonos degradados semitransparentes (modo `abr` configurable) en anillo entre un radio interior (parámetro, píxeles) y 230 px, con razón vertical/horizontal 3205/4096; el color interior/exterior viene de dos colores multiplicados por `intensidad + sen(fotograma·f54)·intensidad·f50/4096` (pulso). `NsFxGas`: activo, intensidad 0-1 (rampa de entrada/salida), radios, colores ya con pulso, `abr`. En nativo: viñeta de color en el borde del campo de visión de cada ojo.
- **Fundido a negro/blanco** [C]: un recubrimiento de pantalla completa (rectángulo semitransparente resta/suma) cuyo color pasa de 0 a 255 (o al revés) en N fotogramas. `NsFxFade`: blanco/negro, `amount` 0-1 (0 = imagen normal), si va de entrada. En nativo: multiplicar/sumar sobre la imagen de cada ojo (para fundidos de zona/cinemática; en VR conviene un fundido más corto).
- **Destellos de daño, aturdimiento, alerta, enfoque, cine, máscara de escaneo**: recubrimientos o líneas planas en la lista de la GPU. Se reconocen por forma (`SCREEN/COVER`, `SCAN`, `BARS`, `RING`, `GHOST`) y el estado que los desencadena está en el campo de banderas global (`u32[0x800AC570]`: aturdido, daño, chaff...). El post-proceso nativo lee esa palabra (`NsFxScreen.game_status`) y aplica un destello equivalente (p. ej. flash rojo/blanco corto), no una copia de la geometría 2D.
- Filtros que **realimentan** el fotograma anterior (sprites fantasma, `fbk` en `GpuOp`): `SCREEN/GHOST`; se descartan en nativo.

## 5. Clasificación de la lista de la GPU

`ns_fx_classify(ram, escena, lista, cls[], sub[], item[], stats)` etiqueta **cada** operación de la lista de dibujo (sin cambiar `GpuOp`):

| clase | significado | qué hace el consumidor |
| --- | --- | --- |
| `EVENT` | relleno/copia/subida/descarga de VRAM | aplicar a la VRAM; no dibuja |
| `2D` | HUD, menús, textos, radio, retratos, radar, paquetes de ordenación 2D, y todo lo que no se reconoce | **sigue saliendo de la GPU** |
| `MESH` | coincide con un paquete emitido de un objeto de malla (ambos búferes) | cubierto por el renderer de mallas; no dibujar |
| `MESH_GUESS` | polígono con posición de vista conocida y sin paquete que coincida (subdividido por el motor, etc.) | cubierto por el renderer de mallas |
| `FX` | coincide con un paquete de un elemento de `NsFxScene.items` (`item[i]` = índice) | cubierto por ns_fx |
| `FX_PACK` | paquete de una primitiva de efecto que no se extrae (invisible, otro mapa) o sprite de vértices derivados sin primitiva | no dibujar |
| `SCREEN` + `sub` | `COPY` relee el búfer de pantalla (tiras de agua, enfoque); `COVER` recubrimiento semitransparente a pantalla completa (fundido, destello, tinte); `BARS` barras opacas; `SCAN` líneas finas de ancho de pantalla; `RING` ≥ 8 polígonos degradados semitransparentes alrededor del centro; `GHOST` realimentación | post-proceso del §4; no dibujar |
| `MAINT` | realimentación transparente (no pinta) | ignorar |

La coincidencia es por **contenido**: se indexan los paquetes del motor de los **dos** búferes y se comparan vértices (x,y menos el offset de dibujo), tamaño de rectángulo y u,v. Las operaciones desconocidas caen en `2D` (lo seguro: nunca desaparece HUD). `NsFxClsStats` da recuentos y cuántos elementos emitidos por el motor quedaron sin operación (`n_fx_items_missing`; ≠ 0 → la lista es de otro fotograma o el motor recortó por pantalla). `ns_fx_resolve_items` copia de la operación casada lo que el paquete no trae (página de textura de sprites, modo de mezcla de los sin textura).

Orden de uso recomendado por fotograma: extraer (`ns_fx_extract`) → clasificar con la lista del mismo fotograma → dibujar nativo lo `MESH/MESH_GUESS/FX/FX_PACK`, hacer post-proceso de `SCREEN` y dejar a la GPU `2D` (+ eventos de VRAM siempre).

## 6. Hallazgo: el factor vertical del motor es 66/64, no 58/64

La proyección del motor escala la Y de vista por una constante (el código la escribe como 58/64). Medido sobre paquetes reales de polígonos de mundo y billboards en varias cámaras (H = 160 y 320), **el factor que reproduce los `xy` del motor es 66/64 = 1,03125**: error medio 0,85-1,3 px, frente a 4,2-6,2 px con 1,0 y 14-24 px con 58/64 (barrido en `ns_fx_dump --ysc`). Es una constante configurable (`NsFxOpts.y_scale`, `NsFxCam.y_scale`); `NsCamera.overscan_y` de `ns_types.h` debería llevar este valor (hoy documentado como 58/64) para convertir bien los billboards de tamaño en píxeles.

## 7. Validación

- 14 instantáneas de RAM reales (sin lista de GPU ni VRAM): `ns_fx_dump` reproyecta cada vértice de mundo y esquina de billboard con la cámara del canal y compara con los `xy` que escribió el motor: media 0,85-1,3 px (máx. 10-20 px en efectos que se movieron entre la lógica y el volcado), y todos los paquetes emitidos por el motor (`sz != 0`) de primitivas visibles del mapa tienen elemento (`cobertura: ... sin elemento=0` en los 14). La selección de búfer (`reloj` o el que reproyecta mejor) acierta en todos.
- Pruebas sintéticas (`make -f vr/native/fx.mk test` y `test-san`): 169 comprobaciones con RAM construida a mano y proyección de referencia independiente (polígonos, billboards, sprites px, láseres, polilíneas, `ON_CAMERA`, `ONESIDE` frente al criterio del motor con cámara girada, filtros de grupo/invisible/solo-orden, papeles por actor, sombra con orden de vértices permutado, agua/gas/fundido, clasificación completa incluido el anillo, mapas mundo→VR con espejo y mapa de usuario, RAM aleatoria, desbordamiento de capacidad) sin ASan/UBSan.
- **[P]** Falta contrastar con **lista de GPU + RAM del mismo fotograma** (los volcados de la app, `NSD1`): comprobar que ningún efecto visible queda sin elemento y ninguna operación de HUD cae en `FX/SCREEN`. El volcado de pantalla bajo el agua con ojo dentro de la caja también está pendiente (los volcados disponibles tienen la caja pero el ojo fuera).

## 8. Contrato con el resto del renderer

- `ns_fx_export.{h,c}`: `NsFxScene.items` → `NsEffect[]` (`NS_EK_QUAD` con 2/3/4 vértices; `NS_EK_BILLBOARD` con `box` en píxeles del motor y `ref_h`; líneas con `NS_EF_LINE` y `width_px`; `ON_CAMERA` con posiciones de vista). Los efectos de pantalla completa no caben en `NsScene` y se leen de `NsFxScene.screen`.
- Extracción en el mismo gancho que las mallas (tras la etapa de pantalla). Todo se lee de la RAM con comprobación de punteros; capacidad por defecto 4096 elementos.
- Conversión mundo → VR: `ns_fx_to_vr` aplica la misma regla que `vr_scene` (escala 0,001, origen, espejo Z si `det < 0` de la rotación de vista, y mapa de usuario opcional). Los billboards/cintas se construyen en el espacio VR con los ejes de la cabeza de cada ojo.
