# Color e iluminación del motor original (para el renderer nativo)

Resumen de cómo el motor calcula el color de cada vértice de los modelos y del mapa, y cómo reproducirlo bit a bit
(`vr/native/ns_light.{c,h}`) o en un shader. Todo lo descrito está contrastado con el código de referencia del motor y
con volcados de RAM de partidas reales (sección 9). Sin datos del juego: solo fórmulas y desplazamientos de estructuras.

## 1. Respuesta corta

- **No hay luz por píxel ni por fragmento.** El color es siempre **por vértice de polígono** (cada polígono guarda 4 colores
  de 8 bits, uno por esquina) y la GPU lo multiplica por la textura (`texel * c / 128`, luego saturación). 128 = neutro,
  255 = x1,99.
- Hay **dos rutas**, según una marca del objeto:
  1. **Objetos dinámicos** (personajes, enemigos, armas, vehículos, objetos sueltos con marca de sombreado): luz **en tiempo
     real cada fotograma** con la unidad de geometría, **por normal de modelo** (no por vértice): un cálculo de color por normal
     (desplazamiento 12, intensidades sin negativos) por normal; luego cada esquina de cada polígono toma el color de su normal por índice. Una matriz de luz y una
     de color **por objeto** (hasta 3 luces: 1 principal direccional + 2 puntuales), más el ambiente.
  2. **Mapa y decorado fijo** (marca de "color precalculado"): el color de cada esquina se calcula **una vez al cargar** (y
     al colocar el objeto), por **vértice**, con la luz principal + las 2 luces puntuales más cercanas de ESE vértice, y se
     guarda en un array (4 colores por cara). No se recalcula por fotograma. Es la ruta del 100 % del mapa.
- **Ambiente**: color de fondo, global por nivel (orden de script), o propio del objeto si lleva la marca de
  ambiente propio.
- **Niebla**: no hay niebla ni atenuación por distancia por defecto. Existe un **actor opcional de niebla** (cargado por
  algunas zonas) que sustituye cuatro etapas del motor: mezcla de profundidad hacia **negro** y descarte de primitivas
  lejanas. Ver sección 6.
- **Sombras**: no son iluminación; son polígonos texturizados planos proyectados sobre el suelo, semitransparentes con color
  fijo. **Destellos/chispas/linterna**: polígonos o sprites con color fijo y mezcla aditiva; algunos añaden una **luz puntual
  temporal** (sección 4.4) que sí alumbra modelos dinámicos y mapa dinámico.
- Visión térmica: sustituye las matrices de color por cero y el ambiente por un tinte fijo (sección 7).

## 2. Núcleo exacto: color por normal

Entradas: normal `n` (4096 = 1,0, en el espacio de la matriz de luz), matriz de luz `L` (3x3, **fila i = dirección de la
luz i**), matriz de color `C` (3x3, **columna j = color de la luz j**), color de fondo `G` (por canal).

```
I_i    = clamp( (L[i][0]*n0 + L[i][1]*n1 + L[i][2]*n2) >> 12 , 0 , 0x7FFF )          i = 0..2   (intensidad de la luz i)
A_c    = ( G_c*4096 + C[c][0]*I_0 + C[c][1]*I_1 + C[c][2]*I_2 ) >> 12                 c = R,G,B
byte_c = clamp( A_c >> 4 , 0 , 255 )
```

Los desplazamientos son aritméticos sobre la suma completa (44 bits). Implementado como `ns_light_color` y verificado contra
la unidad de geometría de referencia del runtime con 200 000 casos aleatorios (incluyendo saturaciones), con igualdad exacta de color y de acumuladores.
Unidades: color de luz principal y de luces puntuales y ambiente se guardan **x16** respecto al valor del script (0..255).
Intensidad 1,0 de una luz = 4096 en `I`; con color 4080 (255x16) y `G`=0 el resultado satura en 255.

Existe una variante que además modula por un color base, pero solo se usa en un panel concreto, no en la escena 3D.

### Fórmula para shader (flotante)

Precalcular en CPU por objeto (en espacio de la normal del modelo; ver 3): `L0..L2 = filas de L / 4096`,
`K0..K2 = filas de C / 4096`, `B = G / 4096`.

```
ir   = clamp( float3(dot(L0,n), dot(L1,n), dot(L2,n)), 0, 7.999756 )
c    = B + float3(dot(K0,ir), dot(K1,ir), dot(K2,ir))
tint = clamp( floor(c * 256.0) / 128.0 , 0, 255.0/128.0 )      // factor que multiplica el texel; 1.0 = neutro
frag = texel.rgb * tint                                         // luego mezcla/semitransparencia
```

`floor(...)` reproduce el byte del motor (error medio 0,008 byte, máximo 1 byte frente al entero; sin `floor` error medio
0,28 y máximo ~1,1). Para un renderer de alta resolución se puede quitar el `floor` y evaluar por píxel con la normal
interpolada (da iluminación suave; cambia el aspecto respecto al original, es una opción estética). Equivalente en C:
`ns_light_tint_f`.

MSL:

```metal
struct Light { float3 l0, l1, l2;  float3 k0, k1, k2;  float3 b; };   // filas precalculadas en CPU
float3 light_tint(constant Light& L, float3 n) {
    float3 ir = clamp(float3(dot(L.l0, n), dot(L.l1, n), dot(L.l2, n)), 0.0, 7.999756);
    float3 c  = L.b + float3(dot(L.k0, ir), dot(L.k1, ir), dot(L.k2, ir));
    return clamp(floor(c * 256.0) / 128.0, 0.0, 255.0 / 128.0);
}
```

Las bytes de color precalculado (mapa) se usan directamente: `tint = byte / 128`.

## 3. Espacios y matrices (objetos dinámicos)

La normal está en el espacio del modelo. El motor no rota la normal: rota las luces al espacio del modelo con
`L_local = L_mundo x R_pieza` (R_pieza = rotación 3x3 de la pieza en mundo, 4096 = 1,0) usando la multiplicación de
matrices de la unidad de geometría (cada elemento `(suma>>12)` saturado a 16 bits con signo): `ns_light_to_local`. Hay una `L_local`
distinta **por pieza** de cada objeto (los modelos articulados tienen hasta 24 piezas). `C` y `G` son del objeto.

- Las matrices de luz de cada objeto (`L_mundo`, `C`, ambiente propio) **ya están en RAM** y las rellena el juego cada
  fotograma (llamada de "luz en la posición"). El renderer nativo debe **leerlas de RAM** en vez de recalcularlas; así
  coincide siempre (matices por escena incluidos). `ns_light_object` las reconstruye si hiciera falta (objetos creados por
  nosotros, p. ej. manos/armas VR).
- Estructura de objeto 3D (desplazamientos): matriz de mundo +0x00 (3x3 s16 + pad + traslación 3xs32), `def` +0x24,
  `flag` +0x28, nº de piezas u16 +0x2E, puntero a matrices de luz +0x34 (dos MATRIX de 32 B seguidas: luz y color; `t` de la
  primera = ambiente propio en unidades 0..255), piezas desde +0x48 con paso 0x5C: matriz mundo de la pieza +0x00,
  `model` +0x40, `rgbs` (colores precalculados) +0x44, `extend` +0x48, nº de paquetes u16 +0x52, paquetes (2 búferes) +0x54/+0x58.
  Modelo: `n_faces` +0x04, `n_verts` +0x34, `verts` +0x38 (s16x4), `vindices` +0x3C, `n_norms` +0x40, `norms` +0x44 (s16x4),
  `nindices` +0x48 (4 bytes por cara: índice de normal de cada esquina). Marcas del objeto: bit 3 (0x08) sombreado en tiempo real,
  bit 1 (0x02) color precalculado, bit 8 (0x100) ambiente propio, bit 2 (0x04) semitransparente. Marcas de modelo: 0x02 semitransparente,
  0x04 sin luz (color fijo neutro), 0x10000 indirecto (índices de vértice con compartición entre paquetes).
- Orden de esquinas en el paquete: `r0 <- nidx[0]`, `r1 <- nidx[1]`, `r2 <- nidx[3]`, `r3 <- nidx[2]`.
- **Atención a cobertura:** el motor solo sombrea los polígonos que pasan su prueba de visibilidad ese fotograma (los
  paquetes con 16 bits bajos de etiqueta a 0 conservan el color antiguo). Un renderer 360° que dibuje polígonos fuera del campo del
  juego **no puede leer los colores de los paquetes** de objetos dinámicos: debe calcular con `ns_light_color` a partir de las
  normales del modelo. Para el mapa (`rgbs`) sí vale leer el array: se calcula para toda la geometría.

## 4. Luces

### 4.1 Luz principal y ambiente (globales del nivel; órdenes de script)
- Dirección: vector normalizado a 4096, **apunta hacia la luz** (cara iluminada si `n . dir > 0`). Estado típico de exteriores
  del volcado: `(0,-4096,0)` (el eje Y del juego va hacia abajo, luz cenital).
- Color: `valor * 16` en la columna 0 de `C` (los tres canales).
- Ambiente: `valor` 0..255; `G = valor * 16`.

### 4.2 Luces puntuales (mapa y temporales)
Formato: posición s16x3, `r_range` u16, `e_range` u16, color RGB u8. Una luz alumbra un punto si **cada** `|dx|,|dy|,|dz| <= e_range`
(caja, no esfera). Se eligen las **2 primeras** por orden de lista:

- Objetos dinámicos (posición = origen del objeto): primero las luces temporales del fotograma **anterior** (hasta 8,
  disparos/chispas/linterna), luego las listas de luces fijas de los mapas cargados, en el orden en que se registraron
  (hasta 8 listas; la primera lista vacía termina). `ns_light_pick`.
- Mapa precalculado: solo la lista de luces del propio bloque de mapa, evaluadas en la **posición mundo de cada vértice**.

Vector de luz (réplica exacta, `ns_light_point_vector`): `d = pos_punto - pos_luz` (¡del punto a la luz restada: apunta de la luz
al punto!), `dist = raiz_motor(dx²+dy²+dz²)` (con cada componente truncada a 16 bits como al cargarla en la unidad de geometría; se trunca una sola vez, para la distancia y para el numerador, y la suma de cuadrados se satura en vez de desbordar), `dist=1` si 0,
`s = max(0, 2*r_range - dist)`, `v = ((d*3072)/dist * s) / r_range` (divisiones enteras truncadas). Módulo = 3072 a distancia
`r_range`, 6144 en el centro, 0 a `2*r_range`. **Convención verificada con datos:** con esa orientación (signo contrario a la luz
principal) el mapa del volcado coincide el 100 %; con el signo invertido solo el 66 %. Es una peculiaridad del original que se
replica tal cual.

La raíz del motor **no es floor(sqrt)**: normaliza a una mantisa de 8 bits (ceros iniciales pares) y usa una tabla
`floor(512*sqrt(m))`, `m=64..255`, con desplazamiento (`ns_light_isqrt`; error relativo < 0,9 %). El error relativo máximo medido es 0,85 %.

### 4.3 Montaje de la matriz de luz
`L_mundo`: fila 0 = dirección principal, fila 1 = vector de la luz puntual 1, fila 2 = vector de la 2. `C`: columna 0 =
color principal x16, columna 1/2 = color de la luz puntual x16. Las filas sin luz son 0. `ns_light_assemble`.

### 4.4 Luces temporales
Las crean disparos, chispas, explosiones, linternas/focos (hasta 8 por fotograma, radio de caída y alcance propios). Están
solo un fotograma (se recoge la lista del anterior). Para el renderer nativo: leer la lista de RAM (formato 4.2) o ignorarlas
en el primer paso (efecto sutil sobre dinámicos cercanos; **no** afectan al mapa precalculado ya guardado).

## 5. Color precalculado del mapa (por vértice)

Para cada cara-vértice `k` (4 por cara, orden de la cara): vértice `v = vindices[k]` (índice < 126), normal `n = norms[nindices[k]]`;
posición mundo `p = T + (R*v >> 12)` (`ns_light_world_pos`, con la traslación y rotación del objeto en el momento de precalcular);
2 luces puntuales de la lista del bloque más cercanas a `p` en orden de lista; matriz de luz `L = [dir; vec0; vec1] x R`; color por normal.
`ns_light_preshade_vertex`. Modelos con marca "sin luz": 0x808080. El array resultante se copia a los paquetes (`r0..r3`).
**Excepción:** objetos con vértices animados (líquidos) se deforman después de precalcular; sus colores siguen siendo los del
instante de precalcular (probable causa del 4,4 % de vértices de objetos pequeños que no coinciden en el volcado: sus alturas de vértice varían unas unidades; no confirmado).

Para el renderer nativo: **leer `rgbs`** (exacto, gratis). Recalcular solo si se cambia la geometría (p. ej. subdivisión) o se
quiere iluminación dinámica propia.

## 6. Niebla / atenuación por distancia (actor opcional por zona)

Cuando existe el actor de niebla, parámetros `start` y `len` (unidades de 256 de profundidad Z de vista; por defecto 50 y 50; `start`
sube hacia 255 al hacer zoom: `start += (255-start)*(zoom-360)/3640`; `len` se recorta a `255-start`). Rampa:

```
zi = Z_vista >> 8                       // 0..255
f  = clamp((zi - start) * 4096 / len0, 0, 4096)      // len0 = len original; zi - start == 255 -> 0 (hueco de la tabla)
color_lejano = negro
```

- **Mapa/decorado precalculado:** por vértice, `byte' = mezcla(byte, 0, f)`: mezcla de profundidad exacta con color lejano 0 (`ns_fog_apply`; `f` se recorta a 0..4096): `byte*(4096-f)/4096`.
- **Objetos dinámicos:** a la profundidad de la pieza raíz (`screen.t[2]/256`) el motor escala **la matriz de color y el ambiente** por
  `s = 4096 - f` (`ns_fog_light_object`; el ambiente se recorta con división entera antes de x16).
- **Primitivas** (efectos, líneas, sprites): mezcla por vértice con la profundidad de la primitiva; **las que tienen `zi >= start+len` no se
  ordenan (no se dibujan)**.
- Sin ese actor no hay niebla en absoluto.

## 7. Casos especiales
- Modo visión térmica: el rellenado de luz del objeto pone la matriz de color a 0 y su ambiente propio a `(130,104,80)`; con la marca de
  ambiente propio el objeto sale en color plano `(130,104,80)` sin relieve (sin ella se usa el ambiente global). No verificado con volcado.
- Objetos de color fijo: 0x808080 (neutro), 0x3E si son semitransparentes (marca de modelo): el byte alto del color es el código
  de primitiva (0x3C opaca, 0x3E semitransparente).
- Marca de textura infrarroja / mapa de entorno: no analizadas (no afectan al cálculo de color de vértice).
- Los 4 colores de cada polígono se **interpolan linealmente en RGB** (Gouraud) al subdividir polígonos grandes; para el renderer
  nativo no hace falta replicarlo (la interpolación por hardware ya es Gouraud).

## 8. Equivalencias con el renderer
- `tint = byte/128` multiplica el texel; con textura 15 bpp el original calcula `(texel5 * byte) >> 7` saturado a 31 (cuantiza a 5 bits;
  ver `ns_cpuref` para el modelo del rasterizador de referencia). Sin textura el color va directo.
- Semitransparencia: bit 1 del código.

## 9. Validación (`vr/native/ns_lightcheck.c`, `make -f vr/native/luz.mk test`)
- Pruebas sintéticas contra la unidad de geometría de referencia: color por normal 200 000 casos (color y acumuladores exactos), matriz local y
  posición mundo 100 000, mezcla de profundidad 100 000, raíz 500 000 valores, selección de luces, caída, casos límite (componentes de
  ±32768 y 30000x3, truncado a 16 bits, r=0/1), niebla (incl. `start+len>255`) y cadena completa del mapa. Float vs entero: error medio
  0,008 byte y máximo 1 byte con valores reales; con color de luz negativo o saturando la intensidad hasta 7.999756 el máximo medido sigue en 1 byte (cota ≤ 3).
- `ns_lightcheck` lee un volcado de RAM de 2 MiB, localiza los objetos 3D por su estructura y compara los colores por vértice de los paquetes
  de dibujo (lo que recibe la GPU) con el cálculo de `ns_light`. Las tres direcciones de los globales (luz global, ambiente, luces fijas) son
  obligatorias, por entorno `NS_DIR_LUZ`, `NS_AMBIENTE`, `NS_LUCES_FIJAS`. La prueba usa RAM sintética (incluye punteros en el último byte) y corre
  también con sanitizers (`test-san`, cualquier UB falla).
- Con volcados de partidas reales: objetos dinámicos y bloque principal del mapa 100 % exactos (error 0); objetos pequeños precalculados
  ~95 % exactos (diferencias de 1-5 niveles en vértices cuya altura varía; ver sección 5).
- Pendiente: comparar con la lista GPU capturada de un fotograma real y con niebla activa.
