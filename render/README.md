# render/ — renderer de alta resolución (base)

Estado: capa de captura en `runtime/gpu.c`, rasterizador de **referencia** en C (`ref_raster.c`), shaders Metal
**sin compilar** (`metal/`), herramientas de verificación. El rasterizador software de `gpu.c` sigue siendo el
**oráculo**: nada de lo de aquí lo sustituye ni lo modifica (la captura es un espectador).

```
runtime/gpu.h,gpu.c        captura de primitivas/eventos -> GpuDrawList
render/ref_raster.{h,c}    modelo exacto: consume GpuDrawList, dibuja a escala N con supermuestreo KxK
render/metal/rg_types.h   estructuras compartidas host <-> shaders
render/metal/rg_gpu.metal vertex/fragment/compute que portan ref_raster.c
render/capture_test.c      arranca el juego, captura, compara con el oráculo, vuelca PNG
render/ref_selftest.c      prueba aleatoria (GP0 al azar) del modelo contra el oráculo
render/build.sh            compila ambos a build-render/ (enlaza el runtime como el objetivo headless)
```

## 1. Captura (`GpuDrawList`)

`gpu_capture_enable(1)` activa; `(2)` además guarda la VRAM del oráculo al cerrar cada frame (verificación);
`(0)` apaga y libera. Apagada, cada punto de enganche es un `if (cap.on)`. `gpu_capture_frame()` devuelve la
siguiente lista cerrada (FIFO de 6, válida hasta la siguiente llamada).

Cada `GpuOp` (152 B) lleva todo el estado necesario para reproducirlo sin mirar el estado de la GPU: vértices con
el offset E5 ya sumado, color/UV por vértice, CLUT, página de textura (`tx,ty`), modo de color (`tp` 0/1/2),
`abr`, ventana de textura E2, área de recorte E3/E4, bits de máscara E6 y banderas ya **efectivas**
(`TEXTURED` descontado E1.11; `DITHER` = E1.9 && (gouraud || texturizado no raw); volteo de rects).
Tipos: `POLY` (3/4 vértices; un cuádrilátero son los triángulos (0,1,2) y (1,2,3) **en ese orden**, que importa para la
regla de aristas y la interpolación), `RECT`, `LINE` (cada tramo de polilínea es una op), `FILL`, `VCOPY`,
`UPLOAD` (con los píxeles en `GpuDrawList.data`), `DOWNLOAD` (marcador, sin efecto en el consumidor).
Los eventos de VRAM van **intercalados en orden** con las primitivas (las texturas y CLUT cambian dentro del frame).

**Cierre de frame.** El punto elegido es el cambio de dirección de display `GP1(05h)` (lo que hace `gfx_put_disp`
tras `frame_wait`): lo dibujado desde el cierre anterior es exactamente el frame que acaba de pasar a mostrarse.
Reserva: `gpu_capture_vblank()` (a llamar desde el VBlank del host) cierra tras 4 VBlanks sin cambio de display
(juegos de un solo búfer). El cierre se difiere si cae en mitad de una subida CPU->VRAM; `disp` se refresca en la
ráfaga de GP1 (en `gfx_put_disp` modo y rango vertical se escriben después de `GP1(05h)`).
**Pendiente (otros ficheros):** hoy nadie llama a `gpu_capture_vblank()` salvo `capture_test`; hay que engancharlo
en el VBlank del runtime (irq.c/hook de frame).

**Continuidad.** Aplicar TODAS las listas en orden mantiene la VRAM del consumidor igual a la del oráculo. La primera
lista tras `enable` (y la siguiente a una pérdida por cola llena, `gpu_capture_dropped()`) trae `vram_init`: volcado
completo de VRAM al principio de ese frame. Habilitar fuera de una subida en curso.

**Geometría de alta precisión (hecha).** `gpu_capture_hiprec(1)` enciende la sombra flotante de la GTE (`gte.c`, bloque
"Alta precisión") y la engancha a la captura. RTPS/RTPT siguen dando el resultado entero exacto; en paralelo se calcula en
double `P = TR + R·V/4096` (espacio de VISTA), `fx = H·P.x/P.z + OFX`, `fy` análogo y `fz = P.z` (sin truncar SZ ni redondear
SXY) y se guarda en una tabla asociativa de 16384 conjuntos x 4 vías con clave (sx,sy,SZ) saturados a 11 bits, vaciada por
generación (el cierre de frame de la captura la incrementa; una entrada vale dos generaciones). `gpu.c` consulta por vértice
con la coordenada cruda del juego (ANTES de sumar el offset E5: `v->x - g.ox`) y rellena `GpuVtx.fvalid, fx, fy, fz, vx, vy, vz`
(+ offset) y `GpuOp.proj_h/proj_ox/proj_oy` (H y OFX/OFY de la GTE) y `GpuOp.dox/doy` (offset de dibujo). Rects y polígonos 2D
salen con `fvalid=0`. Ver `runtime/test_geom.c`.

## 2. Modelo de referencia (`ref_raster`)

Dos destinos por operación:

* **Nativo**: VRAM 1024x512x16 exacta, misma aritmética de 5 bits que el oráculo. Es de donde se leen texturas/CLUT.
* **Alta resolución**: rejilla de M = N·K muestras lineales por píxel nativo (N = escala de salida 1..8, K = supermuestreo
  lineal; K=2 equivale a "MSAA x4"). El píxel nativo p ocupa las muestras [p·M, p·M+M-1]. Se reserva por tiles de 32x32
  px nativos al escribir, inicializados replicando la nativa. Sin tile, el valor es la nativa replicada.
  Salida = promedio KxK por píxel de salida.

Convención de muestreo: M=1 exacto muestrea en la **esquina** (coordenada entera) y usa la regla de aristas
superior-izquierda con el mismo sesgo `-1` que el oráculo, la misma división entera truncada para color/UV, el mismo
DDA de líneas y el mismo tramado. M>1 (o `REF_HQ`) muestrea en el **centro** de cada muestra ((s+0.5)/M): así un
rectángulo alineado cubre exactamente su huella nativa (los sprites quedan perfectos) y es lo que hará Metal.
En HQ el color es de 8 bits por canal, sin tramado, con mezcla de 4 modos en 8 bits; las texturas se expanden 5->8
(`v<<3|v>>2`) y la modulación neutra es 128. Las líneas a escala son una cinta de 1 px nativo con extremos
prolongados 0,5 px (cubre los mismos píxeles de extremo que el DDA). Los rects se sombrean una vez por píxel nativo
y se replican en el bloque (textura de resolución nativa = píxeles nítidos; filtrado sería una opción posterior).

**Eventos y alta resolución.** FILL/UPLOAD escriben la nativa y replican al tile si existe (HQ: el fill usa el color de
24 bits original). VCOPY copia el bloque **de alta resolución** (conserva el detalle de lo dibujado a escala: copias de
pantalla, efectos de desenfoque) leyendo la fila origen entera antes de escribir como hace el oráculo (solapes iguales).
Las decisiones de máscara (E6) las toma el píxel nativo de destino.

**Por qué dos destinos.** Una textura puede ser algo dibujado antes en el mismo frame (render a textura, copia del
framebuffer). El modelo lo resuelve dibujando cada primitiva dos veces: a escala y en la VRAM nativa exacta (de la que
se leen las texturas). Es la opción más simple y verificable (la nativa se compara bit a bit con el oráculo).

`ref_apply(r, lista, hires=0)` solo actualiza la nativa (rápido) y descarta el estado de alta resolución: se usa para
no pagar escala en los frames que no se vuelcan (el estado se reconstruye replicando la nativa).

## 3. Resultados medidos (1x frente al oráculo)

`capture_test` activa `gpu_capture_enable(2)`; tras cada cierre de frame compara (a) la VRAM nativa completa del modelo con
la del oráculo y (b) la imagen de display a 1x del modelo (ruta de alta resolución con M=1 exacto) con la del oráculo.

| Ejecución (4000 frames) | listas | ops (poly/rect/line/fill/copy/upload) | VRAM nativa distinta | display distinto |
|---|---|---|---|---|
| guion pedido (círculo cada 97, start 120/400): solo menús | 1930 | 22757/187332/0/1/5/14810 | 0 palabras en 0 listas | 0 de 158 064 640 px |
| guion largo (+izquierda/211, start/151): llega a 3D | 1915 | 162622/172209/216238/2/47/7948 | 0 palabras en 0 listas | 0 de 170 500 096 px |

Diferencia **0**, bit a bit, en 3845 listas (frames cerrados) reales (con polígonos Gouraud/texturizados 4/8/15 bits, semitransparencia,
líneas, copias y subidas). `ref_selftest 3000` (comandos GP0 aleatorios: todas las primitivas, `abr` 0..3, máscara,
ventana de textura, tramado, volteo, polilíneas, fills, copias solapadas, subidas con máscara, E4 > 511): 0 fallos; mutar
el sesgo de arista o un modo de mezcla lo detecta (89/100 y 56/100 fallos), es decir la prueba tiene dientes.
Con `CT_R1_HQ=1` (1x con muestreo al centro y 8 bits, sin tramado) el 31,8 % de los píxeles difiere (media |d| = 1,88/255,
máximo 255 en bordes): lo esperable al cambiar de convención (media muestra de desplazamiento en aristas diagonales,
sin tramado, 8 bits); es la desviación **intencionada** del modo de alta resolución, no un error.
Coste: ~44 s por 4000 frames con captura+verificación+dos modelos (1x en todos los frames y 4x/SSAA2 en 10 ventanas de
4 frames). Un frame típico tiene ~190 ops.

Imágenes (mirarlas): `<scratchpad>/render/` (guion pedido) y `<scratchpad>/render_play/` (llega a 3D):
`oraculo_*`, `ref1x_*`, `diff1x_*` (rojo = diferencia, toda negra) y `ref4x_ssaa2_*` (1280x1024).

## 4. Diseño Metal (`metal/`)

* **VRAM como textura entera** `R16Uint` 1024x512, no RGBA: las texturas paletizadas de 4/8 bits guardan **índices
  empaquetados** (4 por halfword / 2 por halfword) y las CLUT están en la misma memoria; con una textura de color se
  perdería el bit 15 (máscara/semitransparencia) y los índices. `texture.read()` exacto, sin filtrado ni espacio de color.
  El fragment hace lo mismo que `texel()` del modelo (ventana de textura incluida).
* **Dos objetivos**: VRAM nativa `R16Uint` (fuente de texturas, exacta; shader `rg_fs_prim_native`) y VRAM de alta
  resolución `RGBA8` de (1024·M)x(512·M), A = bit de máscara (`rg_fs_prim_hires`). Una textura no puede ser a la vez
  adjunto y fuente en el mismo pase, así que **la nativa no se dibuja "en vivo" y se muestrea a la vez**: o bien se dibuja
  cada primitiva en dos pases/adjuntos distintos (un segundo `MTLRenderPassDescriptor` sobre la nativa, solo lee de una
  copia), o (recomendado) se sigue el rectángulo sucio del destino de cada primitiva y, cuando una primitiva posterior
  muestrea (textura o CLUT) una región sucia, se termina el pase, se ejecuta `rg_k_resolve_native` sobre esa región
  y se continúa. Es lo que hacen los renderers de alta resolución habituales y cuesta cero en escenas sin render a textura.
* **Semitransparencia y máscara**: mezcla programable (`[[color(0)]]`, lectura del framebuffer; GPU Apple) en el fragment:
  los 4 modos `(B+F)/2, B+F, B-F, B+F/4` y la comprobación del bit de máscara por muestra, imposibles con mezcla fija
  (el bit 15 del texel decide por fragmento si hay mezcla y qué máscara se escribe). `RG_FC_EXACT5` reproduce el canal de 5
  bits y tramado (modo oráculo); sin él, 8 bits (HQ). Orden: Metal respeta el orden de envío de primitivas que se solapan
  sobre el mismo adjunto, por lo que basta emitirlas en el orden de la lista.
* **MSAA x4**: objetivo multimuestra con `RG_FC_PERSAMPLE` (el parámetro `[[sample_id]]` fuerza sombreado por muestra y
  `[[color(0)]]` pasa a ser por muestra): equivale al supermuestreo del modelo (K=2). Con MSAA los kernels de cómputo no
  pueden escribir el objetivo (las texturas multimuestra no se escriben desde compute): los eventos de VRAM sobre él
  (fill/subida/copia) se hacen con rectángulos (`rg_vs_rect` + `rg_fs_fill_hires/sync_hires/copy_hires`).
  Alternativa sin MSAA: M = N·K de una muestra y `rg_k_display` promedia KxK (igual que `ref_present`).
  Memoria: (1024·M)x(512·M)x4 B (M=4: 32 MB; M=8: 128 MB; x4 muestras: multiplicar). Reducible limitando el objetivo a
  las dos áreas de dibujo.
* **Orden de eventos y copias a mitad de frame**: la lista se recorre en orden. Una operación de VRAM termina el pase de
  render en curso (para que todo lo anterior esté escrito), se ejecuta (nativa por compute; alta resolución por
  rectángulo) y se reabre el pase con `loadAction = load`. Subidas CPU->VRAM sin máscara: `replaceRegion`
  directo en la nativa + `rg_fs_sync_hires`. Copia VRAM->VRAM: blit del rectángulo origen a una textura auxiliar
  (instantánea; no se lee y escribe la misma textura), luego `rg_k_copy_native` (nativa) y `rg_fs_copy_hires` (alta
  resolución, conserva detalle). Semántica de solape: el hardware copia fila a fila, la instantánea copia "a la vez";
  difiere solo en copias que se solapan hacia abajo (el modelo C replica la fila a fila).
* **Expansión en el host** (siguiente paso): cada `GpuOp` -> `RGVertex`/`RGPrim` (`rg_types.h`); quads = 2 triángulos
  (0,1,2),(1,2,3); sprites = 2 triángulos con u,v = u0 / u0+w (con volteo: u0+1 / u0+1-w, para que el suelo en el centro
  del píxel dé u0-i); líneas = cinta de 1 px nativo; color plano repetido en los vértices. Recorte E3/E4 en el shader
  (`rg_clipped`), así los lotes no se parten por cambios de recorte. `w=1` y `[[center_no_perspective]]`: interpolación
  afín como el original. Lote = primitivas consecutivas sin eventos intermedios.

### Límites conocidos de la versión Metal frente al modelo

* A M=1 el modelo es bit-exacto; la GPU interpola en coma flotante, así que alguna textura puede caer un texel de distancia
  en aristas (no se puede igualar). Por eso el oráculo/modelo son la verdad y los shaders se validarán con una vista de
  diferencias de la propia app (no bit a bit).
* Muestreo en el centro de la muestra (convención de Metal) = convención HQ del modelo, no la exacta de M=1.
* Sin compilar: revisar en la primera compilación (`function_constant` en argumentos de textura, lectura de color en
  `R16Uint`, `[[sample_id]]` junto a `[[color]]`).

## 5. Siguientes pasos

1. Enganchar `gpu_capture_vblank()` al VBlank real del runtime y `gpu_capture_frame()` al hilo de render (copiar o consumir
   dentro del hook de frame; el runtime es de un solo hilo).
2. Host Metal: empaquetado `GpuOp -> RGVertex/RGPrim`, lotes y pases por eventos, seguimiento de rectángulo sucio y
   `resolve_native`. Validar contra `ref_raster` volcando el mismo frame con `capture_test`.
3. (hecho en C, pendiente en el host Metal) Geometría de alta precisión: ver `RGVertex.iz` / `RGFrame.hiprec` y el diseño de
   profundidad en `rg_types.h`. Falta el empaquetado host y la matriz de proyección/vista del juego para el estéreo.
4. Filtrado de texturas opcional (bilineal sobre texels de 15 bits tras la CLUT, respetando transparencia 0x0000), y texto/HUD
   en capa aparte (intercepción de `RECT` de fuente).
5. Compilar los shaders y portar `ref_selftest` a una prueba de imagen en el dispositivo.

## 6. Alta precisión en el modelo de referencia (`REF_FLOAT_VTX`, `REF_DEPTH`)

* `REF_FLOAT_VTX`: si TODOS los vértices del polígono son `fvalid`, coherentes con el entero (|float-entero| <= 3 px; medido < 2 salvo
  12 de 106 058) y `fz > 0`, se usa la posición subpíxel (punto fijo 1/(32·M) px) y los atributos (UV, color) se interpolan con
  corrección de perspectiva (pesos de arista / z). Solo en el objetivo de alta resolución: el modo nativo 1x exacto no cambia
  (`ref_selftest 3000`: 0 fallos). Un polígono con algún vértice sin float (saturado, desalojado) cae entero.
* `REF_DEPTH` (requiere el anterior): búfer 1/z por muestra, por tile, a cero al inicio de cada lista. 3D opaco: prueba y escribe;
  3D semitransparente: prueba sin escribir; 2D (HUD, rects, líneas, polígonos sin float): sin prueba, y lo opaco BORRA la
  profundidad de las muestras que pinta (lo 2D pasa a ser lo más cercano, como el orden de pintor del juego: así un modelo 3D
  pintado después de un fondo/HUD no queda tapado por una profundidad vieja). Tolerancia relativa a favor del último dibujado
  (`RefOpts.depth_bias`, 1/128 por defecto) para calcomanías coplanares.
* `capture_test`: `CT_HIPREC=1` (captura float + tercer modelo `rh` con `REF_FLOAT_VTX`), `CT_DEPTH=1` (+`REF_DEPTH`),
  `CT_JITTER=1` (métrica de temblor). Vuelca `hiprec[_z]_4x_ssaa2_*.png` y `hpdiff_*` junto a `ref4x_*`.
