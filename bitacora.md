# Bitácora — MVision

Runtime nativo para Apple Vision Pro de un título de PS1 (juego objetivo), VR inmersiva 6DOF en primera persona, control corporal (cabeza + manos) y mandos PS VR2 Sense.

**Convención de repo (público):** no nombrar el juego, personajes ni repos externos en nada que se suba. Repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`). Zonas por su código interno (`s00a`…).

Estado: **disco analizado, arquitectura decidida (recompilación estática del EXE EU), herramientas de análisis.** Sin build todavía.

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
- **Cargador** (`<arranque>`) (text `0x80010000`, size `0x58800`, pc `0x80066a98`, "Europe area") que hace `LoadExec` de EXE principal `<exe disco 1>` (disco 2 `<exe disco 2>`).
- **EXE principal idéntico en ambos discos.** text `0x80010000`, size `0x9D000` (643 072 B), pc `0x8009a434`, cabecera "Japan area", fecha 1999-07-15, cadenas `Nov 12 1998`. Tamaño ≈ ejecutable JP de la decomp (626 KB) → mismo motor, rama muy cercana.
- ~1500 funciones (prólogos `addiu sp,-X`).
- Ficheros en `D/` (dir de datos): `<fichas> 5,7M`, `<demos> 248M/184M`, `<retratos> 3,5M`, `<radio> 1,7M`, `<stages>.DIR 70M`, `<voces> 194M/181M`, `<video> 54M/162M (mixed, XA)`. Raíz: `FILLER.DAT` (relleno).
- `<stages>.DIR` (disco 1): u32 tamaño tabla = 1128 → **94 entradas** de 12 B (nombre[8] + sector). Incluye todas las zonas de la decomp (`s00a`…`s20a`, variantes `*r`, `d*`, `brf`, `rank`, `camera`, `title`, `select*`, `ending`) **y `vr01`…`vr10`, `vrdemo`, `selectvr`**. Overlays de código viajan dentro de cada stage (tag `.bin`, ver `source/libfs/cdstage.c` `SetupNextFile`).

- **Requisito: juego íntegro en castellano (textos, radio, voces, cinemáticas).** Cubierto por la opción A: se ejecuta el EXE y los datos del disco ES tal cual. Verificado: textos de radio (`<radio>`) y menús/tarjeta de memoria del EXE en castellano. Voces/FMV en `<voces>`, `<demos>`, `<video>` (doblaje ES del disco). Regla: nunca sustituir datos ES por JP; cualquier parche de la decomp JP debe respetar tablas de texto/fuente del EXE ES. Textos nuevos (opciones VR de la app) en castellano.

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

## 7. Pendiente siguiente sesión

- [ ] Pasar `names_eu.json` al recompilador (`--funcs` + nombres en comentarios) y localizar a mano el SDK que falta.
- [ ] Runtime C: memoria, rt_call/rt_jump por tabla, div, GTE, COP0; primer arranque headless en Linux.
- [ ] Cargar overlays extraídos en recompilador: dirección de carga = `StageCharacterEntries` (localizar en EXE EU vía firmas).
- [ ] Recompilador MIPS→C: decodificador + emisor, empezando por el ejecutable principal.
- [ ] HLE: GPU (OT/primitivas → Metal), GTE float, CD sobre imagen, hilos `mts` → fibras.
- [ ] Esqueleto de app visionOS (ventana plana primero).
