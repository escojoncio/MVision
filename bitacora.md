# Bitácora — MVision

Runtime nativo para Apple Vision Pro de un título de PS1 (juego objetivo), VR inmersiva 6DOF en primera persona, control corporal (cabeza + manos) y mandos PS VR2 Sense.

**Convención de repo (público):** no nombrar el juego, personajes ni repos externos en nada que se suba. Repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`). Zonas por su código interno (`s00a`…).

Estado: **disco analizado, arquitectura decidida (recompilación estática del EXE EU), herramientas de análisis.** Sin build todavía.

## 0. SIGUIENTES PASOS (orden de prioridad)

Estado: runtime arranca el juego (planificador, IRQ, CD virtual, GPU software) y vuelca los primeros frames (logos). Armas (lógica + modelos originales + piezas cortadas), carcasa de visión y E/S en `main` con pruebas. Todo pasa `make test` / `test_sys` / `vr/weapons test` / `vr/box test`.

**Build 1 (ventana plana, IPA por sideload):**
1. ~~Desatascar arranque tras logos~~ HECHO (§12). Siguiente: ejecutar 5000+ frames, ver si llega a pantalla de título/partida; vigilar FMV (MDEC/XA) y demos en motor.
2. ~~Overlays recompiladas~~ HECHO (§15). Los avisos del intérprete restantes son funciones del EXE llamadas desde overlays: volcadas a `gamedata/extra_funcs.txt` (local).
3. Lector BIN/CUE (MODE2/2352) y CHD opcional en `cdrom.c`; detección de discos por contenido (SYSTEM.CNF + EXE), carpeta de la app sin nombre fijo en código.
4. Renderer Metal: primitivas GPU a resolución nativa + MSAA x4 (rasterizador software = oráculo), sin temblor de vértices (GTE float/subpíxel), texto vectorial SDF interceptando el dibujo de cadenas, HUD en capa aparte, descarte del motor vía su ruta de primera persona.
5. Host visionOS: audio (`spu_render` 44,1 kHz), mando (`pad_set_state`, vibración `pad_get_rumble`), tarjeta (`card_open`), FMV por MDEC; workflow macOS en Actions → IPA.
6. Opcional: volcado/sustitución de texturas por hash (upscale local, nunca en repo).

**Build 2 (VR), en orden de dependencia:**
1. Puente RAM: inventario/equipar vía funciones del juego, munición, disparo, cámaras de proyectil, blancos, estado/pose animada de enemigos, ids de arma reales (`game_weapon_id` provisionales).
2. Cámara 6DOF (cabeza), control corporal, comodidad; cinemáticas en estéreo (defecto) / diorama (opción); códec envolvente a pantalla completa y subtítulos sobre escenas.
3. Armas: enlazar `wload`+`wcut` → `weapon_bind`; afinar regiones pequeñas (selector, cerrojo, palanca, round/case de pistola); rellenar piezas vacías si hace falta.
4. Impactos por hitbox de pieza + función de daño del juego; colisión del jugador con su pose; cuerpo a cuerpo por gestos; percepción enemiga gradual; enemigos visibles activos.
5. Audio binaural: posición 3D de cada voz SPU (interceptar cálculo de paneo del juego) → fuentes mono con HRTF del sistema; música/radio/narración en estéreo fijo a la cabeza; fallback al paneo original.
6. Carcasa de caja (hecha, falta cablear `box_view_set_active` al estado del juego); máscara y gafas: cortar de los modelos de cabeza + cara interior derivada; detector de vacíos (360° por zona) y cierres de geometría solo donde falte.

**Datos locales (nunca al repo):** `gamedata/main.exe`, `gamedata/names_eu.json`, `gamedata/stages.bin` (→ tabla de stages), `gamedata/disc/` (ficheros extraídos), `gamedata/extra_funcs.txt`. Discos del usuario en su PC; se copian a la sesión con las herramientas del dispositivo.
**Comandos:** `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`; headless: `MV_DISC=<dir disco> <OUT>/headless ../gamedata/main.exe 800 --dump DIR --every 50`; `make -C vr/weapons test test-san`; `make -C vr/box test`.

---

## 1. Base: decomp externa (`DECOMP_REPO`)

Revisada en commit `9830efd98559be84da59dc1a961f8fc0e628ba36` (2026-10-18):

- Objetivo upstream: edición JP (`JP-1/2`, `JP-VR` = disco de misiones VR). Matching byte a byte con SDK PsyQ 4.3/4.4 (GCC 2.x).
- **Ejecutable principal 100 % decompilado; 0 `INCLUDE_ASM` fuera de `source/overlays/`.**
- Overlays con asm pendiente (747 `INCLUDE_ASM`, 37 ficheros): `brf camera d18a d18ar rank s03d s05a s07a s07b s08b s08br s11d s11e s11g s11i s14e s15c s19b s19br`. Sin empezar: `s10a/ar s11b s11h s15a s17a/ar s18a/ar s19a/ar`.
- Se compila sin el ejecutable original: solo `source/` + `asm/` (4,9 MB, ya en upstream) + `SDK_REPO`. Toolchain Windows → wine/wibo en Linux.
- Sin LICENSE → dependencia externa, nunca copiar su código aquí.

## 2. Disco del usuario (analizado 2026-10-18)

- **Edición EU (España), 2 discos.** Extraídos por el usuario con `dumpsxiso` (+ `.xml` de layout). Solo en local/sesión, nunca en repo.
- `SYSTEM.CNF`: `BOOT` → cargador, `TCB=4`, `EVENT=16`, `STACK=0x801FFFF0`.
- **Cargador** (fichero de arranque de la raíz) (text `0x80010000`, size `0x58800`, pc `0x80066a98`, "Europe area") que hace `LoadExec` de EXE principal del directorio de datos (nombre distinto por disco: identifica el disco).
- **EXE principal idéntico en ambos discos.** text `0x80010000`, size `0x9D000` (643 072 B), pc `0x8009a434`, cabecera "Japan area", fecha 1999-07-15, cadenas `Nov 12 1998`. Tamaño ≈ ejecutable JP de la decomp (626 KB) → mismo motor, rama muy cercana.
- ~1500 funciones (prólogos `addiu sp,-X`).
- Directorio de datos: 7 ficheros (fichas 5,7M; demos/cinemáticas en motor 248M/184M; retratos 3,5M; textos de radio 1,7M; tabla de stages 70M; voces 194M/181M; vídeo FMV 54M/162M modo mixto XA). Raíz: fichero de relleno.
- `<stages>.DIR` (disco 1): u32 tamaño tabla = 1128 → **94 entradas** de 12 B (nombre[8] + sector). Incluye todas las zonas de la decomp (`s00a`…`s20a`, variantes `*r`, `d*`, `brf`, `rank`, `camera`, `title`, `select*`, `ending`) **y `vr01`…`vr10`, `vrdemo`, `selectvr`**. Overlays de código viajan dentro de cada stage (tag `.bin`, ver `source/libfs/cdstage.c` `SetupNextFile`).

- **Requisito: juego íntegro en castellano (textos, radio, voces, cinemáticas).** Cubierto por la opción A: se ejecuta el EXE y los datos del disco ES tal cual. Verificado: textos de radio y menús/tarjeta de memoria del EXE en castellano. Voces, demos y FMV del disco (doblaje ES). Regla: nunca sustituir datos ES por JP; cualquier parche de la decomp JP debe respetar tablas de texto/fuente del EXE ES. Textos nuevos (opciones VR de la app) en castellano.

## 3. Inventario de la capa de plataforma (main exe + overlays parciales)

Llamadas externas a la decomp (conteo aprox. por regex):

- **libgpu:** macros de primitivas (`setRGB0`, `setXY4`, `addPrim`, `setPolyFT4`, `setSprt`, `setTile`, `setLineF2`…; solo cabeceras) + `LoadImage/StoreImage/MoveImage/ClearImage`, `DrawSync`, `DrawOTag`, `ClearOTagR`, `PutDrawEnv/PutDispEnv`, `SetDefDrawEnv`, `GetTPage`, `ResetGraph`, `SetDispMask`, `DrawSyncCallback`. → Rasterizador de OT/primitivas sobre Metal.
- **libgte:** `ReadRotMatrix` (184), `ratan2`, `rsin/rcos`, `SquareRoot0/12`, `RotMatrix*`, `ScaleMatrix`, `ApplyMatrix*`, `MulMatrix*`, `RotTransPers*`, `VectorNormal*`, `OuterProduct*`, `SetRot/Trans/Light/ColorMatrix` + 114 macros `gte_*` inline asm (`source/include/inline_n.h`). → GTE en float.
- **libcd / libds:** `CdControl(B)`, `CdRead2`, `CdReady/SyncCallback`, `CdGetSector`, `CdIntToPos`, `St*` (streaming). → lector de imagen de disco.
- **libpress (MDEC):** `DecDCTin/out/vlc2/Reset`. → decodificador MDEC por software (FMV).
- **libspu:** ~30 funciones `Spu*` (voces, reverb, IRQ, transferencias) en `source/sound`. → emulación SPU (sonido usa secuenciador propio sobre SPU).
- **libpad / libcard / kernel:** `Pad*`, `_card_*`/`InitCARD`, eventos (`OpenEvent`…), **hilos `OpenTh/ChangeTh/CloseTh` en `source/mts`** (multitarea cooperativa) → corrutinas/fibras.
- **Accesos directos a hardware:** scratchpad `0x1F800000+` (65 `getScratchAddr` + literales), registros `0x1F801070` (I_STAT), `0x1F801040` (SIO pad). 11 ficheros con asm inline fuera de overlays.

## 4. Arquitectura — recompilación estática

Diseño anterior: compilar la decomp a nativo 64-bit + RAM virtual 32-bit con punteros como offsets + AOT solo para overlays sin decompilar.

**Problema detectado:** convertir todo el código C a punteros-offset es la parte más cara y frágil (structs con punteros en datos del disco, aritmética de punteros, asm inline, scratchpad), y además la decomp es JP mientras el disco es EU.

**Propuesta: recompilación estática total (estilo N64Recomp / Zelda64Recomp), con la decomp como mapa:**
1. Entrada = binarios MIPS (ejecutable + overlays). Opción A: los del disco EU directamente. Opción B: los generados por la decomp (JP) + datos EU.
2. Recompilador MIPS R3000 → C (un único traductor para todo, ya previsto para overlays). Memoria = RAM PS1 de 2 MB + scratchpad tal cual → punteros de 32 bits funcionan sin tocar nada.
3. Funciones de SDK (GPU/GTE/CD/SPU/pad/hilos) **sustituidas por implementaciones nativas (HLE)** identificadas por símbolo (mapa de la decomp) o por firma de bytes en el binario EU.
4. Cambios para VR: parches en C escritos sobre la decomp, compilados a MIPS con gcc moderno y recompilados (como Zelda64Recomp), o funciones nativas que operan sobre la RAM emulada.
5. Intérprete MIPS de depuración comparte decodificador.

Ventajas: sin refactor de punteros, juego completo desde el día 1 (overlays sin decompilar incluidas), validación diferencial trivial (mismo binario que el emulador de referencia).
Coste: mapear símbolos JP→EU (por firmas) si se elige opción A.

**Decisión: opción A** (recompilar los binarios del disco EU: EXE principal + overlays de `<stages>.DIR`). Código y datos coinciden por construcción; la decomp JP se usa como mapa de símbolos vía `tools/sig/sigs.py` (firmas enmascaradas JP → direcciones EU). Pendiente medir % de match.

## 5. Capa VR, control, armas (sin cambios respecto al diseño)

- CompositorServices, estéreo con proyección real; cámara 6DOF = cabeza; agacharse real → agacharse en juego; cabeza del modelo oculta; brazos por IK a las manos; disparo desde el cañón en la mano (lógica de primera persona existente en el código); arreglar culling/LOD pensados para cámara cenital; cinemáticas en pantalla flotante; confort configurable dentro de la app (giro por pasos, viñeta).
- Control: PS VR2 Sense (principal), mando normal (alternativo), hand tracking (opcional). Hápticos en disparo/daño/radio/latido.
- Armas en mano: modelos low-poly propios con estética PS1 y piezas móviles; mundo y enemigos originales. Recarga sobre el contador original (modos manual/simplificada/automática en opciones). Fundas corporales + rueda.
- Arsenal: pistola con supresor, fusil de asalto, rifle de francotirador (mira render-to-texture, balanceo, tranquilizante lo reduce), lanzamisiles guiado (vista de misil en pantalla del lanzador), lanzamisiles antiaéreo, granadas (frag/chaff/aturdidora), explosivo adhesivo, minas direccionales.
- Gestos: radio (mano a la oreja), raciones/cigarros (boca; humo revela láseres), gafas térmicas/nocturnas, caja de cartón, golpear pared, estrangular, pegarse a pared.
- Difícil: animaciones de tercera persona y bosses de cámara cenital.

- **Requisito objetos equipados:** la capa VR nunca decide el equipo; inventario leído de la RAM del juego y equipar/usar siempre vía la función de equipar del propio juego (recompilada) o secuencia de botones del menú original, nunca escribiendo RAM directamente. Efectos pasivos de objetos equipados (consumo automático, acceso por tarjeta, etc.) los ejecuta el código original. Persistencia = estado del juego (zonas, cinemáticas, cambio de disco, partidas). Representación física en el cuerpo solo visual. Selección de armas por la misma vía.

- **Requisito percepción enemiga (VR):** ampliar alcance de visión de los enemigos en las overlays con sospecha gradual por distancia (cerca: alerta inmediata original; media: sospecha/investigar; lejos: solo con exposición prolongada), periferia solo por movimiento, manteniendo línea de visión, caja, oscuridad y resto de lógica original. Ajuste configurable (original / VR recomendado por defecto / exigente). Verificar que enemigos lejanos visibles siguen activos (no desactivados por estar fuera de la cámara original). Tras arranque jugable.

- **Requisito cuerpo a cuerpo (VR):** gestos → acciones propias del juego (sin combate físico paralelo). Golpe: velocidad de mano hacia delante + enemigo al alcance → botón de golpe (combo original). Agarre: grip cerca de cuello/hombro desde la espalda → agarre del juego; apretar repetido/mantenido = pulsaciones; tirón brusco = proyección en esa dirección; soltar grip = soltar. Golpear pared, pegarse a pared. Sin mover la cabeza del usuario. Modos gestual (defecto) / asistido / botón. Requiere puente RAM (pos/orientación/estado de enemigos).

- **Requisito impactos (VR):** raycast 3D desde la boca del arma contra hitboxes por pieza del modelo jerárquico animado de cada enemigo + geometría del escenario; impacto comunicado a la función de daño propia del juego (reacciones/alertas/muerte originales). Daño por zona configurable: original (uniforme) / realista (defecto: multiplicador cabeza, menos extremidades). Proyectiles propios del juego reciben dirección 3D real. Colisión del jugador sigue la pose real de cabeza/torso (agacharse/asomarse reduce exposición). Requiere puente RAM.

- **Decisión caja:** estilo por defecto `BOX_STYLE_CARDBOARD` (textura interior del disco); negro puro como opción. Máscara visible siempre que el objeto caja esté equipado y puesto.
- **Requisito máscara de gas (VR):** el modelo equipado del disco es una cabeza completa con la máscara (ids de cabezas con equipo en el stage residente: ver /work/models/index.txt local). Separar solo la máscara de la cabeza (corte por textura/componentes con wcut, sin geometría nueva) y mostrarla anclada a la cabeza como en vr/box: marco de las lentes visible en la periferia, efecto de ver a través (viñeta del contorno de lentes, ligera distorsión/aberración de lente, tinte del cristal, empañado sutil sincronizado con la respiración, oclusión periférica fiel a las proporciones del modelo). Activación por el estado del juego (objeto equipado), nunca por la capa VR. Mismo patrón aplicable a las gafas de visión (nocturna/térmica) del otro modelo de cabeza. Al quitar la cabeza el modelo queda hueco por dentro: generar la cara interior (lo que ve el usuario) derivándola de la exterior original: duplicar caras con normales invertidas, desplazadas hacia dentro un grosor coherente, cerrar bordes entre capa exterior e interior, material interior (goma/plástico oscuro, reverso de lentes y oculares) muestreado de las texturas del propio modelo. Aplica a máscara y a ambas gafas.

- **Corrección corte cargador pistolas (36f6/21b6):** la región actual del cargador invade la empuñadura. Debe ir a ras del mango: lo único visible con el cargador insertado es la franja inferior (base) de la empuñadura; el cuerpo del cargador queda oculto dentro y, extraído, se extruye desde esa base hacia arriba siguiendo la inclinación del mango (eje de extracción = inclinación del mango, alineado con w_pist.c). Test: con cargador insertado nada sobresale del contorno del mango salvo la base.

- **Cañón de pistolas (36f6/21b6):** el modelo original no tiene cañón. Generar pieza `barrel` derivada del modelo: tubo bajo la corredera de la boca a la recámara, diámetro según la sección frontal de la corredera, material oscuro metálico del atlas; ánima visible en la cara frontal; cañón visible al retroceder la corredera; WP_MUZZLE en el centro de la boca; el supresor se acopla al extremo del cañón.

## 6. Herramientas en el repo

- `tools/disc/psxdisc.py` (stdlib): `info`, `ls`, `extract`, `dirtable <DISCO> <FICHERO>`, `manifest`. Sin probar con BIN real (el usuario extrajo con dumpsxiso; formato de tabla DIR confirmado sobre el fichero extraído).
- `tools/sig/sigs.py`: `make <exe> <map> <json>` / `match <json> <exe> <json>`. Enmascara inmediatos de lui/addiu/ori/loads/stores y destinos j/jal; guarda solo sha1 (prefijo 8 palabras + completo) y tamaño, nunca código. Autoprueba EU contra sí mismo (mapa falso por prólogos): 1518 firmas, 1392 exactas, 126 ambiguas (funciones duplicadas), 0 fallos.
- `.github/workflows/build.yml`, job `reference` (solo dispatch o `[build]`): clona decomp/SDK desde **secrets** `DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO` (la API de variables no es accesible desde la sesión; los crea el usuario), instala wine con caché apt, compila referencia JP, genera `sigs_ref.json` y lo sube como artefacto `sigs-ref`. **Nunca ejecutado.**

- `tools/stage/stages.py`: `list`, `check`, `overlays <DIR> <out>`. Formato DATACNF: cabecera `u8 ver, u8 pad, i16 sectores`; tags 8 B `u16 id, u8 modo, u8 ext, i32 tamaño` hasta modo 0; datos tras sector de cabecera, alineados a sector; grupo `c` empaquetado con tamaño en tag ext `0xFF`. **Verificado disco 1: 94/94 stages cuadran; 92 overlays (`s`/`b`) extraídos** (`init`, `init_tux` sin overlay).
- `sigs.py` tras revisión: solo símbolos dentro de secciones `text` del map psylink (tras "Names in address order"), fin acotado a la sección, enmascara también lwl/lwr/swl/swr, corregido off-by-one del índice. Nombres del map en MAYÚSCULAS.
- Workflow tras revisión: `setup-python` 3.12 (PEP 668), `wine wine32:i386` sin caché apt, `wineboot --init`, `build.py` tolerante a fallo de `compare.py` (paso de firmas exige `obj/_*.exe` + `obj/asm.map`), timeout 45 min.

- `tools/recomp/recomp.py` + `runtime/cpu.h`: recompilador MIPS R3000 → C. Funciones = destinos `jal` + entrada + punteros en datos que apuntan tras un `jr ra`+delay + `--funcs`. Región de código autodetectada (tramo decodificable más largo). Ramas internas → `goto`, externas → tail call, `jal` → llamada directa, `jr` no-ra → `switch` sobre todas las direcciones de la función, delay slots con condición evaluada antes del slot, GTE/COP0/memoria vía helpers del runtime. **EXE EU: código `0x80014894-0x8009e4b4`, 1841 funciones, 78 con tablas de salto, 1 instrucción no soportada (`0x80014898`, probablemente datos al inicio de la región), compila sin errores con clang -O1 -Wall** (runtime aún sin implementar).
- Build `reference` ejecutada (run 39136576826): **éxito**, 2 min 50 s. Artefacto `sigs-ref` generado; la sesión no puede descargar artefactos ni logs (almacenamiento de Actions bloqueado) → el usuario lo descarga y lo deja en su carpeta.

- **Mapa de símbolos EU (2026-10-18):** `sigs_ref.json` (1770 firmas JP, descargado a mano; el usuario lo guarda en su carpeta de discos) → `sigs.py match`: 582 exactas. `tools/sig/align.py` añade prefijo único (337), orden entre anclas (198), delta entre anclas (48 iguales + 83 por vecino). **Total 1248/1766 funciones de código, 57 % de palabras; SDK: 232/253 identificadas.** SDK sin localizar: `CdGetSector2 CdDataCallback CdDataSync GetDrawEnv GetDispEnv VSyncCallbacks PadChkVsync _padSioRW`. `main` sin match (EU difiere). Layout JP: rdata `0x80010000`, text desde `0x800148b8`; EU text `0x80014894`. Muchas funciones EU = JP +1 instrucción.
- Mejora futura del match: firma de opcodes (sin operandos) para similitud difusa — requiere nueva ejecución de CI + descarga manual.

---

## 7. Armas en mano — núcleo + "pist" (`vr/weapons/`, `tools/wmodel/`)

- `weapon.h` (añadidos mínimos): `#include <stddef.h>`; `WHeld held[W_MAX_HELD]` (objetos sueltos: cargador, vaina...); en `WeaponState` binding en ejecución `meshes/nmeshes/parts/nparts`; en `WeaponDef` `mesh_names[]`; funciones `weapon_bind`, `weapon_meshes`, `weapon_parts`. `ammo_take(n<0)` devuelve munición.
- `wcore.c/.h`: quaternions/poses, `wcore_part_poses(s,W,out,max)` (usa binding si existe; devuelve nº escrito), retroceso muelle-amortiguador (subpasos 2 ms), `wcore_kick`, `wcore_hand_dist`, `wcore_point_world`. `registry.c`: referencias fuertes a las 10 armas (los weak no extraen miembros de un .a).
- `w_pist.c`: sin meshes compilados (enlazar con `weapon_bind` por `pist_mesh_names`, static). Semiauto, ciclo 60 ms, slide lock, cargador extraíble (WB_B/WB_MAG_RELEASE), cadera+grip, guiado al pozo, rack con WP_CHARGE, supresor desmontable (CUSTOM0), modos manual/simplificado/automático.
- `tools/wmodel/preview.c + wprev.c`: rasterizador (ss=2 → MSAA x4), `--obj` externo, guion `--sim`. `make -C vr/weapons test` OK.
- `tools/wmodel/geom.py`, `m_*.py` y `vr/weapons/gen/`: ELIMINADOS (los modelos son los originales del disco, cargados en ejecución con `weapon_bind`). Las armas no llevan mallas compiladas (`meshes=NULL`, `nmeshes=0`).
- Pendiente: probar modo simplificado en test; validar con modelo real del disco (puntos/pivotes en `w_pist.c` son del placeholder).

## 8. Modelos originales en runtime (2026-10-18)

Sustituye a los modelos de arma hechos a mano: se cargan los del propio disco (datos locales, nunca al repo).
- `tools/wmodel/mdlfmt.py` (formato, documentado en su cabecera), `raster.py`, `extract.py <archivo_stages> <out> [--dict f] [--resident <stage>] [--only-resident]` -> OBJ/MTL/PNG + hojas de contacto + `index.txt`. Los nombres concretos de stage/ficheros se pasan por argumento, no están en el código.
- `vr/weapons/wload.c/.h`: `wload_model(dir,len,stage,id,&opts,&out)`; `opts.resident` = stage de recursos compartidos. Normales suavizadas, atlas RGBA8, escalado x2/x4 tipo xBR propio, `split_parts`. `test_wload.c` (ASan/UBSan): `./test_wload <archivo_stages> <stage>`.
- Pendiente: integrar con WeaponDef/registry (sustituir generador procedural), puntos WP_* y canales por pieza, semitransparencia/color de vértice, índice 32 bits para mallas grandes, build/CI.

## 9. Revisión adversarial de armas — correcciones (2026-10-18)

Estado: `make -C vr/weapons test` y `make -C vr/weapons test-san` (ASan+UBSan, `build-san/`) limpios, sin warnings. Registro 10/10 contra `libweapons.a` (`test_registry.c`).
- Contrato (`weapon.h`, `wcore.c/.h`): `WeaponDef.init` limpia `state_size` bytes con `wcore_state_reset(s,def)` conservando el binding (`meshes/parts`), pone `recoil_offset.q` identidad y `held[].mesh=-1`; orden init -> `weapon_bind`. `mesh_names` terminado en NULL; `weapon_mesh_count`, `weapon_part_mesh(s,i)` (pieza con `mesh=-1` -> hueco de su mismo nombre). `wcore_part_find(const WeaponState*,name)` mira `s->parts`. `wcore_dt(dt,max)`: 0 si NaN/<=0; todos los `update` salen sin tocar nada con dt inválido. Sonidos `WS_DRAW..WS_EMPTY` + `WS_COUNT` en el enum (alias `W5_S_*` en `w_throw.h`).
- `w_gunc.c`: recarga temporizada sin cargador (`SEQ_T_NOMAG`=0,40 < `SEQ_T_MAG_IN`=0,55); automático solo con reserva (sin DRY en bucle); `cd` acotado a -min(interval,dt); `cycle_bolt` devuelve la bala viva con `ammo_take(-1)`; `equip_latch` (gatillo apretado al equipar no dispara). `w_sniper.c`: viñeta con `vig0=min(0.60,edge-0.05)`; `w_rifle.c/w_sniper.c` con `.recoil` designado y `mesh_names` con NULL.
- `w_pist.c`: carga inicial en la 1ª actualización (descuenta min(cap+1,reserva)); `seat_mag` no pisa cargador montado; FM_ANIM no pisa; `auto_stage` se abandona al salir de WR_AUTOMATIC y se reanuda si no hay cargador; `trig_armed=0` en init; sin `part_poses` (pose del cargador = `W·k_parts[P_MAG].rest`); `nmeshes=0`; código muerto fuera.
- `w_aa.c`: sin `gen/aa_mesh`; piezas `mesh=-1` + `aa_mesh_names`; `game_weapon_id=7` (guided=6); `trig_armed=1` en init.
- `w_guided.c/.h`: `w_guided` const, sin globales mutables (eliminadas `guided_bind_model/part_table/set_point`); piezas renombradas (`tube,grip,trigger,sight,screen,frame,lid,round`) = `guided_mesh_names`; `prev_trig=1` en init.
- `w_throw.c`, `w_placed.c`: ids neutros `throw_frag/throw_b/throw_c`, `placed_a/placed_b` (símbolos `w_throw_b/c`, `w_placed_a/b`); un init por variante; dt<=0.1; palanca con subpasos 8 ms y canal en [0,1.5]; `w5_emit` rechazado no consume ni pasa a GONE (reintento cada 0,15 s); `ang_vel`<=45 rad/s; reserva>=0; detonación rechazada conserva la carga; `WVH_ACCEL_BLEND`=0,9 (mezcla parcial parábola/recta); placed: gatillo apretado al sacar no coloca.
- Makefile: sin GENS/GENOBJS; `-MMD`; `wload.c` en la lib; `test_registry` enlaza contra el .a; `test-san`, `test-wload` (test_wload fuera de `test`: su API aún cambia).
- Pendiente: `test_wload.c` usa `wload_hash16` y `wload.h` declara `wload_id16` (WIP del cargador); `game_weapon_id` siguen siendo provisionales; ajustar puntos/pivotes y canales a los modelos reales.

---

## 9b. Carcasa de visión interior (`vr/box/`, `tools/box/`) — 2026-10-18

- Original (EXE EU recompilado): al usar el objeto en 1ª persona se crea un actor de "sight" con el recurso tipo `s` (id16 0xE2A9, stage `init`): máscara 2D, NO geometría 3D ni textura de cartón. 36 polígonos planos negros opacos (todo fuera del hueco) + 38 Gouraud semitransparentes modo resta (degradado interior ~16-19 px). Hueco = rectángulo redondeado en lienzo 320x256: x 17..302, y 50..160, centro (159.5,105); con foco 320 px y centro óptico fila 120 -> 24° izq/dcha, +12.3°/-7.1°. Se abre en 8 pasos (desplazamiento por vértice `(t-hi)*(dx,dy)/4`, inicial = final-2*(dx,dy)). Modelo 3D de la caja (id 0xE2A9 tipo `k`) es solo la caja en 3ª persona (se oculta en 1ª).
- Puente (RAM EU): sight activo = `u32 @0x800A07DC == 0xE2A9` (refcount @0x800A07D8); el actor vive mientras `s16 @0x800B898A == 1` (flag 1ª persona). Llamar `box_view_set_active(sight_id==0xE2A9)`.
- `vr/box/box_data.{h,c}` (lector de stage/máscara/PCX, parser endurecido, fuzz), `box_view.{h,c}` (carcasa cerrada, panel frontal con el contorno real, canto con grosor, degradado alfa, estilos CARDBOARD(defecto)/BLACK(fiel), inercia acotada 2.5°, bajada/subida, apertura animada, `set_wide` accesibilidad), `test_box.c` (4749 comprobaciones OK, también con ASan/UBSan). `make -C vr/box test|test-san|prev`.
- `tools/box/`: `mkpano.py` (fondo), `boxres.py` (extrae máscara/textura + render del original), `boxprev.c` (trazado de rayos de la carcasa), `render_compare.py`.
- Pendiente: integrar draws en el renderer Metal (malla dinámica `box_view_revision`, alfa, baldosa con repetición), puente con la RAM, ajustar altura del centro óptico si el lienzo PAL difiere (±8 px), prueba en visor de distancia/confort.

---

## 10. Cortador de modelos (`vr/weapons/wcut.{h,c}`, `wcut_tables.c`, `test_wcut.c`) — 2026-10-18

- Separa un `WLoaded` rígido en piezas articuladas sin geometría nueva. Marco: salida de wload corregida con `WCUT_FIX_UPRIGHT|WINDING` (los modelos salen boca abajo, horarios, normales hacia dentro). Selección por reglas (caja orientada CLIP/WHOLE/CENTROID, máscara de componentes, rango UV); CLIP = Sutherland-Hodgman por las 6 caras con intersecciones canónicas por arista (mismo punto en ambos lados) + tapas por cara (cadenas de aristas de contorno -> polígono -> orejas con UV planar a un bloque 3x3 del atlas).
- Cierre: los puntos de corte se insertan en las aristas vecinas (`conform_tl`, uniones en T) y se eliminan triángulos de área nula cosiendo su vértice medio (`clean_collinear`); sin esto el volumen no cerraba. Bugs resueltos: esquinas de cara insertadas fuera de orden angular; orejas que dejaban vértices colineales fuera de la tapa (rechazar oreja si hay vértice sobre su borde); caja con cara a ras de la superficie del modelo (cambiar ±1 mm: las caras coplanares no dan contorno).
- Tablas: solo cajas/máscaras paramétricas (mm), sin vértices. `wcut_open_edges`, `wcut_volume`, `wcut_degenerate` para comprobar. Piezas con tolerancia de aristas abiertas documentada en `test_wcut.c` (originales abiertos y copias de piezas abiertas).
- `test_wcut.c` ya NO incluye los .c: wcut.c y wcut_tables.c van en libweapons.a y el test en `make test` / `test-san` (parte con modelos reales omitida sin `gamedata/stages.bin`; salida solo OK/FALLO, `WCUT_VERBOSE=1` imprime medidas). Parte sintetica: entradas corruptas, -4 por tabla ajena, >32 componentes, prisma con agujero (tapa anillo, sin solape).
- Revision adversarial (hallazgos 1-16): `wcut_run` valida `WLoaded` y acota trabajo (tope global de triangulos 4*src+4096 -> -3, tolerancias escaladas por tamano), tapas con agujeros por ear clipping con puentes, orientacion por cascara conexa (aristas + volumen con signo), normales de salida siempre acordes al orden, `src_tris`/caja en la tabla (-4), `wcut_fix` fuera de la API. Aristas abiertas esperadas exactas en el test (heredadas del original: 4725 body 4, 2d3b body 4, 15a9 tube 6, 57f8 body 4/tube 3, 9a90 round 10).
- Pistolas (36f6/21b6): el cargador se corta a ras del fondo del mango (solo la base visible) y se extruye 30 mm por el interior siguiendo el eje de w_pist (test: ningun vertice del cuerpo fuera del contorno del mango); pieza `barrel` generada (tubo bajo la corredera, boca en su cara frontal, anima oscura); WP_MUZZLE = centro de la boca (sin supresor `w_pist_bare_muzzle_z`, con supresor su punta). Excepcion de volumen 10 % en 36f6: el original sin cortar tiene 6 aristas abiertas.
- Pendiente: cuerpos del fusil/francotirador/lanzador guiado con unas pocas aristas abiertas en el pozo del cargador/cuña (superficies sin aristas compartidas); varias regiones pequeñas (selector, cerrojo, mango) a afinar visualmente; huecos vacíos sin geometría original (barrel, hammer, stock, bezel, marco, misil).

---

## 11. Interrupciones asíncronas en el runtime (`runtime/irq.c`, `io.c`, `fiber.c`) — 2026-10-18

- Recompilador: `RT_POLL(c)` en aristas hacia atrás y `JR_DISPATCH` (`recomp.py`; `--poll-entry` opcional, sin medir); también en `interp.c`. `--rt_budget<0` llama a `rt_poll` (reloj virtual 33,8688 MHz, 12 ciclos/sondeo, 64/lectura de E/S, `sys_idle` salta al siguiente evento).
- `io.c`: reloj de eventos con VBlank PAL 50 Hz; `vblank_hw` integra `sys_frame`, `cd_pump`, `rt_host_frame`. `irq.c` (nuevo): excepción HLE (guarda Cpu, colas SysEnqIntRP C0:02/03, HookEntryInt B0:18/19, callbacks Interrupt/VSync/DMA reales, retorno tipo ReturnFromException).
- `fiber.c`: PCB/TCB en memoria de kernel (0x108/0x110), SR por hilo (IEc e IM2), `fiber_return_from_exception` conmuta de hilo si el PCB cambió. `rt.c`: `cop0_write` -> `rt_irq_check`.
- `hle.c`: `hle_VSync` espera VBlank real; `ResetCallback` idempotente (libspu lo llama tras instalar el VSyncCallback). `gpu.c`: `ResetGraph` delega en el original (`rt_orig`) para inicializar libgpu. `cdrom.c`: callbacks de CdControl diferidos como IRQ de CD.
- `headless.c`: `--dump DIR --every N`, MV_DISC, MV_CARD, estadísticas. Makefile: objetivo `test_sys`. `make test` y `make test_sys` OK.
- Funcional: planificador del juego corriendo, FS lee la tabla de stages, GPU recibe listas, frames de logos volcados.
- Pendiente: tras ~frame 650 el bucle de frames sigue (t3) pero no se emiten más DrawOTag/PutDrawEnv (GPU estática); la última lectura de CD termina en pos=254778 y aparecen lecturas de argumentos del intérprete de scripts y liberaciones del motor gráfico (actores terminando). Tarea ociosa gira en 8008d828. Hipótesis: comando del intérprete de scripts o carga de overlay no tratados. Medir `--poll-entry`; reducir ~626k instrucciones interpretadas; limpiar trazas MV_CDLOG.

---

## 12. Reloj de SPU en tiempo virtual (`runtime/io.c`, `spu.c/.h`, `test_sys.c`) — 2026-10-18

- Causa del atasco tras logos: la SPU solo avanzaba si el host llamaba a `spu_render`; en headless nunca -> el IRQ por dirección no saltaba. El driver de sonido usa una voz en bucle sobre un bloque vacío de 512 B con la dirección de IRQ alternando base/base+256 como reloj de su tarea de interrupción (consume el stream de voz del CD). Sin IRQ, el stream se llena, el CD se pausa y el juego espera para siempre.
- `io.c`: evento `next_spu` cada `SPU_BATCH`=32 muestras (`SPU_BATCH_CYC`=24576 ciclos, 768/muestra exactos) en `next_event`/`run_events` con bucle de puesta al día -> `spu_clock(32)` + `spu_irq_poll()`.
- `spu.c`: `spu_clock(frames)` renderiza bajo el mutex a un anillo de 16384 frames (descarta lo más antiguo) y al WAV; `spu_pull(out,frames)` para el host (no avanza la SPU, rellena con ceros). `spu_render/spu_advance` solo para tests: un host real NUNCA debe llamarlos (doble avance).
- `test_sys.c`: `t_spu_clock` (3 ciclos armar/disparar/reconocer IRQ, `spu_pull`); `tm_wait` 4000 -> 400000 iteraciones (sys_idle ahora salta como mucho un lote de SPU).
- Resultado headless 1500 frames: logos -> intro con créditos y rótulo de zona en castellano; excepciones SPU 2910; CD sigue leyendo (stream) hasta el final. `make test test_sys` OK. Revisión adversarial: sin bugs.
- Disco sintetizado: no dejar en `gamedata/disc/` el fichero de relleno de la raíz (se duplica con `FILLER.DAT` y desplaza todos los LBA); copiar solo SYSTEM.CNF, cargador y el directorio de datos.
- Datos regenerables: `gamedata/names_eu.json` se obtiene con `sigs.py match` + `align.py` (escribe en el directorio actual: ejecutarlo desde `gamedata/`).

## 13. Funciones ampliadas por ramas a inicios débiles (`tools/recomp/recomp.py`) — 2026-10-18

- Causa del cuelgue a ~frame 3120: `after_end` acepta lo que sigue a `j`+delay; un epílogo (0x80084a30) se tomó como inicio heurístico y las ramas de la función real a 0x80084a3c/40 salían por `rt_jump` al intérprete, que anidaba una llamada del host por iteración del bucle de la tarea de sonido -> pila agotada.
- `find_funcs(exe, extra, weak_out=None)`: misma lista; `weak_out` recibe los inicios débiles (punteros en datos, lui/addiu) que no son fuertes (pc0, jal, extra/--funcs/nombres fiables, code_start).
- `extend_ends(exe, starts, weak)`: a punto fijo, si una rama condicional sin enlace o `j` va a t >= fin y todos los inicios en [fin,t] son débiles, el fin pasa al de la región de t (tope 64 KB). Los débiles absorbidos siguen siendo funciones propias (código duplicado, intencionado). Los nombres HLE y `setjmp` nunca son débiles (no se saltan su envoltorio). 220 funciones ampliadas; 2461 funciones, 96 con tabla jr.
- Resultado: 6000 frames sin fallo (~28 s); intro en motor en 3D con subtítulos en castellano y rótulo de personaje con actor de doblaje. Quedan 127 avisos `interpretando` (direcciones 0x80010000/100/200 con ra=0, también en test_sys).
- Siguiente: revisar qué son esas llamadas a 0x8001xxxx (zona del cargador/rdata), ejecutar 20000+ frames hasta tomar el control (pulsar Start con `pad_set_state`), y comprobar FMV.

## 14. Guion de mando en headless (`runtime/headless.c`) — 2026-10-18

- `--pad FRAME:BOTONES[:DUR],...` (botones con `+`, duración 6 por defecto) y `--pad-every N:BOTON` (repetibles, máx. 256 eventos). `buttons` de `pad_set_state` es activo-alto (pad.c invierte). Se aplica en `rt_host_frame`.
- **Confirmar = círculo** (cruz no confirma en menús). Start: salta intro y pausa en juego.
- Guion que llega a partida: `--pad 120:start,400:start --pad-every 97:circle --pad-every 211:left --pad-every 151:start` -> aviso sin tarjeta (Sí con izquierda+círculo), dificultad, códec en castellano, **jugabilidad en el muelle (radar, créditos sobre el juego)** hacia frame 3000-4000. 24000 frames en 93 s, sin fallos.
- Siguiente: guion sin Start periódico (lo pausa), mover al personaje (cruceta) y comprobar movimiento en los frames.

## 15. Overlays recompiladas (`recomp.py --ov-batch`, `rt.c` rt_ov_*, `Makefile gen_ov`) — 2026-10-18

- **Base de carga: 0x800c4380 para todas las overlays** (el fichero crudo del stage se copia tal cual con `CdGetSector`). Cómo se determinó: `MV_RAMDUMP=fichero` (interp.c) vuelca la RAM al primer aviso del intérprete con dirección >= 0x800c0000; buscar los 64 primeros bytes de cada `.bin` extraído (`stages.py overlays`) en el volcado -> título en 0x800c4380 y coincide byte a byte. Confirmación para el resto: los punteros de la cabecera (pares id/puntero en las primeras palabras) apuntan a prólogos `addiu sp,sp,-N` con esa base en 20/20, 19/19... de los casos (con +0xc fallan).
- **Layout del fichero:** cabecera/tablas de datos al principio, código de funciones después (a partir de ~0x800c4390..0x800c5xxx), datos al final. Las tablas de saltos van dentro del código (decodifican como `lb`; inofensivo).
- `recomp.py`: `--overlay NOMBRE --base HEX --main-gen DIR` (uno) y `--ov-batch DIR_BINS SALIDA --base HEX --main-gen DIR` (todos). `--main-gen` = salida de `make gen` del EXE: de `funcs.h` saca los inicios del principal (llamadas `f_XXXXXXXX` directas) y de `dispatch.c` la dirección de `setjmp`. Región de código = `detect_code` afinada (el inicio sube a la primera función real, para que el hash no cubra cabeceras mutables); funciones = destinos `jal` + punteros en datos (incl. datos tras el código) + inicio; mismo tratamiento de ramas/ampliación (§13). Símbolos `static ov_<nombre>_f_XXXXXXXX`, tabla `ov_<nombre>_table`. Overlays con idéntico código (hash FNV-1a 64 de [off,off+len) + off/len) comparten fichero y tabla (92 overlays -> 77 códigos). Índice `ov_index.c`: `OvDesc {nombre, base, off, len, w0 (1ª palabra), hash, tabla, nº}`.
- Runtime (`rt.c`, tipos en `cpu.h`): una overlay activa (`ov_t`) buscada en `rt_lookup` tras la tabla principal. `rt_ov_written(addr,len)` (llamado desde `hle_CdGetSector`, cdrom.c) invalida la activa si se escribe sobre su código, o marca sucio si se escribe en la ventana (unión de códigos del índice). En un fallo de búsqueda dentro de la ventana sin overlay activa, `ov_detect`: filtro por 1ª palabra y FNV-1a 64 de la RAM; si coincide se activa. Si nada coincide (carga a medias) no se registra nada y solo se reintenta si cambia una muestra de 64 palabras de la ventana (o hay escritura nueva). Sin índice enlazado (`rt_ov_count` débil = 0, p.ej. tests) todo igual que antes. `MV_OVLOG=1` imprime cada detección. No usa `tables[]`/`rt_register_table` (ranura propia, para no interferir con tablas de test).
- Makefile: `make gen_ov [-j]` (extrae a `../build/ov`, recompila a `../build/gen_ov/*.c`, objetos `.o` con `OVCFLAGS=-O1`, ~118 MB de C, ~4 min con 2 núcleos); el headless enlaza `$(GENOV)/*.o` si existen (`OVOBJ=` vacío para desactivar). Variables: `STAGES OVBIN GENOV OVBASE OVCFLAGS`.
- Resultado (6000 frames): instrucciones interpretadas 29 556 955 -> 8 541; avisos `interpretando` 127 -> 1 (`0x80091be0`, función del EXE no detectada, ra en overlay); detecciones: title, opening. Frames `--dump d5 --every 500` idénticos bit a bit (diferencia media 0) a la misma build sin overlays; ciclos virtuales idénticos (4064256008). `make test test_sys` OK.
- Revisión adversarial (correcciones): `rt_ov_written` también desde BIOS memcpy/memset (`bios.c`) y con suma de 64 bits; `Makefile`: `CC ?= clang` nunca aplicaba (make define `CC=cc`) -> `ifeq ($(origin CC),default)` fuerza clang; `gen_ov` ya no oculta el fallo de recomp.py (`| tail` -> log en `$(OVBIN)/recomp.log`); `.o` de overlays dependen de `funcs.h` y `cpu.h`; `test_sys.c`: buffer `o[64]` de `spu_pull(o, 64)` desbordaba (estéreo, 128 muestras) y con clang reventaba la pila. Avisos del intérprete restantes (8 funciones del EXE llamadas desde overlays) volcados a `gamedata/extra_funcs.txt` (lo lee `make gen` junto al EXE); tras cambiar ese fichero hay que repetir `make gen` y `make gen_ov` (las overlays enlazan directo a `f_XXXXXXXX` del principal).
- Pendiente: probar más zonas (en 8000 frames con el guion de mando solo se cargan title, d00a y s00a; las tres se detectan); el hash cubre todo el rango de código (si alguna overlay mutara datos incrustados en él no coincidiría: aparecería `[ov] ninguna overlay coincide` con MV_OVLOG); la overlay activa solo se invalida por `CdGetSector` y BIOS memcpy/memset (el DMA de CD es un stub; stores del propio código recompilado sobre el código de la overlay no se vigilan).

## 16. GTE: registros de 16 bits normalizados en mtc2/lwc2 (`runtime/gte.c`) — 2026-10-18

- Causa de la "explosión de vértices" en modelos articulados: `gte_mtc2` guardaba la palabra de 32 bits entera en registros de 16 bits y los cálculos (IR0-3, SZ, OTZ, VZ) la leían tal cual; con lwc2 de pares de shorts la mitad alta basura disparaba SQR/OP/GPF/GPL/MVMVA/INTPL/NCDS.
- Corrección: al escribir, regs 1,3,5,8-11 extendidos con signo desde 16 bits; 7 y 16-19 extendidos con cero.
- Validado en local contra una implementación de referencia (fuera del repo): 12 M de casos aleatorios + 0,75 M de transferencias, todos los comandos y bits sf/lm/mx/v/cv -> 0 discrepancias.
- Regresión en el repo: `runtime/gte_vectors.h` (cmd, semilla, FLAG, MAC0, hash FNV-1a de 64 registros; generado con nuestro gte.c ya corregido) + `t_gte_vec` y `t_gte_regs16` (casos a mano) en `test_runtime.c`. `make test` OK con clang y gcc.
- En juego: el personaje del muelle se ve correcto. Pendiente menor: UBSan "left shift of negative value" en gte.c (código previo, mismo resultado en gcc/clang).

## 17. Cargador de pistolas generado y cierre de cuerpos (`vr/weapons/`, `tools/wmodel/`) — 2026-10-18

- `wcut.h`: `WCutExtrude` -> `WCutMag` (slide, bore, dir, clear, wall/lip/rec/case fracs, case/bullet len, materiales); `WCutSeal` + `WCutModel.seals/nseals`.
- `wcut.c`: `gen_mag` (sección = contorno interior del mango en la base con `poly_inset` de mitra, altura del pozo hasta bajo la corredera, eje 20°, labios en el último 10 %, cartucho derivado de la boca del cañón, material oscuro muestreado del atlas); `seal_open` cierra bordes abiertos por modelo (`SEALS()` en `wcut_tables.c`). Endurecido en revisión: rechazo -7 si el inset invierte aristas, ranura fuera del contorno (`pt_in_convex_xz`), `case_frac<1`, NaN.
- `w_pist.c`: eje del cargador QX=-0.173648 QW=0.984808 (UY=0.939693 UZ=-0.342020).
- Tests: `test_wcut` (cerrado, contención, holgura, altura, anchura, calibre, barrido de extracción, `mag_bad_params`); OPEN solo 57f8 body 4, 57f8 tube 3, 9a90 round 10; rifle/sniper/guiado a 0. `test_wload` usa `wload_id16` y entra en TESTS (se omite sin datos). `make -C vr/weapons test test-san` OK.
- Herramienta: `make -C vr/weapons cutprev` (`tools/wmodel/cutprev.c`; `wprev_set_atlas/winding/twosided`; salida de wcut CCW -> `wprev_set_winding(1)`).
- Pendiente: marco macizo (sin pozo hueco al sacar el cargador); `PIST_MAG_HALF/CY/CZ` en w_pist.c para el cargador de ~86 mm; `game_weapon_id` real (puente RAM: pistola 0, fusil 1, granada 2, guiado 3, antiaéreo 4, mina 5, adhesivo 6, aturdidora 7, chaff 8, francotirador 9).
