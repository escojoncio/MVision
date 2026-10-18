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

- **Requisito objetos equipados:** la capa VR nunca decide el equipo; inventario leído de la RAM del juego y equipar/usar siempre vía la función de equipar del propio juego (recompilada) o secuencia de botones del menú original, nunca escribiendo RAM directamente. Efectos pasivos de objetos equipados (consumo automático, acceso por tarjeta, etc.) los ejecuta el código original. Persistencia = estado del juego (zonas, cinemáticas, cambio de disco, partidas). Representación física en el cuerpo solo visual. Selección de armas por la misma vía.

- **Requisito percepción enemiga (VR):** ampliar alcance de visión de los enemigos en las overlays con sospecha gradual por distancia (cerca: alerta inmediata original; media: sospecha/investigar; lejos: solo con exposición prolongada), periferia solo por movimiento, manteniendo línea de visión, caja, oscuridad y resto de lógica original. Ajuste configurable (original / VR recomendado por defecto / exigente). Verificar que enemigos lejanos visibles siguen activos (no desactivados por estar fuera de la cámara original). Tras arranque jugable.

- **Requisito cuerpo a cuerpo (VR):** gestos → acciones propias del juego (sin combate físico paralelo). Golpe: velocidad de mano hacia delante + enemigo al alcance → botón de golpe (combo original). Agarre: grip cerca de cuello/hombro desde la espalda → agarre del juego; apretar repetido/mantenido = pulsaciones; tirón brusco = proyección en esa dirección; soltar grip = soltar. Golpear pared, pegarse a pared. Sin mover la cabeza del usuario. Modos gestual (defecto) / asistido / botón. Requiere puente RAM (pos/orientación/estado de enemigos).

- **Requisito impactos (VR):** raycast 3D desde la boca del arma contra hitboxes por pieza del modelo jerárquico animado de cada enemigo + geometría del escenario; impacto comunicado a la función de daño propia del juego (reacciones/alertas/muerte originales). Daño por zona configurable: original (uniforme) / realista (defecto: multiplicador cabeza, menos extremidades). Proyectiles propios del juego reciben dirección 3D real. Colisión del jugador sigue la pose real de cabeza/torso (agacharse/asomarse reduce exposición). Requiere puente RAM.

- **Decisión caja:** estilo por defecto `BOX_STYLE_CARDBOARD` (textura interior del disco); negro puro como opción. Máscara visible siempre que el objeto caja esté equipado y puesto.
- **Requisito máscara de gas (VR):** el modelo equipado del disco es una cabeza completa con la máscara (ids de cabezas con equipo en el stage residente: ver /work/models/index.txt local). Separar solo la máscara de la cabeza (corte por textura/componentes con wcut, sin geometría nueva) y mostrarla anclada a la cabeza como en vr/box: marco de las lentes visible en la periferia, efecto de ver a través (viñeta del contorno de lentes, ligera distorsión/aberración de lente, tinte del cristal, empañado sutil sincronizado con la respiración, oclusión periférica fiel a las proporciones del modelo). Activación por el estado del juego (objeto equipado), nunca por la capa VR. Mismo patrón aplicable a las gafas de visión (nocturna/térmica) del otro modelo de cabeza. Al quitar la cabeza el modelo queda hueco por dentro: generar la cara interior (lo que ve el usuario) derivándola de la exterior original: duplicar caras con normales invertidas, desplazadas hacia dentro un grosor coherente, cerrar bordes entre capa exterior e interior, material interior (goma/plástico oscuro, reverso de lentes y oculares) muestreado de las texturas del propio modelo. Aplica a máscara y a ambas gafas.

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

## 9. Carcasa de visión interior (`vr/box/`, `tools/box/`) — 2026-10-18

- Original (EXE EU recompilado): al usar el objeto en 1ª persona se crea un actor de "sight" con el recurso tipo `s` (id16 0xE2A9, stage `init`): máscara 2D, NO geometría 3D ni textura de cartón. 36 polígonos planos negros opacos (todo fuera del hueco) + 38 Gouraud semitransparentes modo resta (degradado interior ~16-19 px). Hueco = rectángulo redondeado en lienzo 320x256: x 17..302, y 50..160, centro (159.5,105); con foco 320 px y centro óptico fila 120 -> 24° izq/dcha, +12.3°/-7.1°. Se abre en 8 pasos (desplazamiento por vértice `(t-hi)*(dx,dy)/4`, inicial = final-2*(dx,dy)). Modelo 3D de la caja (id 0xE2A9 tipo `k`) es solo la caja en 3ª persona (se oculta en 1ª).
- Puente (RAM EU): sight activo = `u32 @0x800A07DC == 0xE2A9` (refcount @0x800A07D8); el actor vive mientras `s16 @0x800B898A == 1` (flag 1ª persona). Llamar `box_view_set_active(sight_id==0xE2A9)`.
- `vr/box/box_data.{h,c}` (lector de stage/máscara/PCX, parser endurecido, fuzz), `box_view.{h,c}` (carcasa cerrada, panel frontal con el contorno real, canto con grosor, degradado alfa, estilos CARDBOARD(defecto)/BLACK(fiel), inercia acotada 2.5°, bajada/subida, apertura animada, `set_wide` accesibilidad), `test_box.c` (4749 comprobaciones OK, también con ASan/UBSan). `make -C vr/box test|test-san|prev`.
- `tools/box/`: `mkpano.py` (fondo), `boxres.py` (extrae máscara/textura + render del original), `boxprev.c` (trazado de rayos de la carcasa), `render_compare.py`.
- Pendiente: integrar draws en el renderer Metal (malla dinámica `box_view_revision`, alfa, baldosa con repetición), puente con la RAM, ajustar altura del centro óptico si el lienzo PAL difiere (±8 px), prueba en visor de distancia/confort.
