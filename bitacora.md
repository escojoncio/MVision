# Bitácora — MVision

Runtime nativo para Apple Vision Pro de un título de PS1 (juego objetivo), VR inmersiva 6DOF en primera persona, control corporal (cabeza + manos) y mandos PS VR2 Sense.

**Convención de repo (público):** no nombrar el juego, personajes ni repos externos en nada que se suba. Repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`). Zonas por su código interno (`s00a`…).

Estado: **fase de diseño + herramientas.** Sin build todavía.

---

## 1. Base: decomp externa (`DECOMP_REPO`)

Revisada en commit `9830efd98559be84da59dc1a961f8fc0e628ba36` (2026-10-18):

- Objetivo upstream: edición JP (`SLPM_862.47/48`, `862.49` = disco de misiones VR). Matching byte a byte con SDK PsyQ 4.3/4.4 (GCC 2.x).
- **Ejecutable principal 100 % decompilado; 0 `INCLUDE_ASM` fuera de `source/overlays/`.**
- Overlays con asm pendiente (747 `INCLUDE_ASM`, 37 ficheros): `brf camera d18a d18ar rank s03d s05a s07a s07b s08b s08br s11d s11e s11g s11i s14e s15c s19b s19br`. Sin empezar: `s10a/ar s11b s11h s15a s17a/ar s18a/ar s19a/ar`.
- Se compila sin el ejecutable original: solo `source/` + `asm/` (4,9 MB, ya en upstream) + `SDK_REPO`. Toolchain Windows → wine/wibo en Linux.
- Sin LICENSE → dependencia externa, nunca copiar su código aquí.

## 2. Disco del usuario

- **SLES-01734 (España), 2 discos, BIN/CUE.** Edición europea original (no la JP de la decomp).
- No transferible tal cual (>400 MB/fichero por el puente del PC). Solicitado partir en zip de 300 MB.
- Pendiente: `psxdisc.py info/ls/manifest` de ambos discos → ejecutable de arranque (`SLES_017.34` esperado), lista de ficheros, comparar contra estructura que espera `source/libfs`.

## 3. Inventario de la capa de plataforma (main exe + overlays parciales)

Llamadas externas a la decomp (conteo aprox. por regex):

- **libgpu:** macros de primitivas (`setRGB0`, `setXY4`, `addPrim`, `setPolyFT4`, `setSprt`, `setTile`, `setLineF2`…; solo cabeceras) + `LoadImage/StoreImage/MoveImage/ClearImage`, `DrawSync`, `DrawOTag`, `ClearOTagR`, `PutDrawEnv/PutDispEnv`, `SetDefDrawEnv`, `GetTPage`, `ResetGraph`, `SetDispMask`, `DrawSyncCallback`. → Rasterizador de OT/primitivas sobre Metal.
- **libgte:** `ReadRotMatrix` (184), `ratan2`, `rsin/rcos`, `SquareRoot0/12`, `RotMatrix*`, `ScaleMatrix`, `ApplyMatrix*`, `MulMatrix*`, `RotTransPers*`, `VectorNormal*`, `OuterProduct*`, `SetRot/Trans/Light/ColorMatrix` + 114 macros `gte_*` inline asm (`source/include/inline_n.h`). → GTE en float.
- **libcd / libds:** `CdControl(B)`, `CdRead2`, `CdReady/SyncCallback`, `CdGetSector`, `CdIntToPos`, `St*` (streaming). → lector de imagen de disco.
- **libpress (MDEC):** `DecDCTin/out/vlc2/Reset`. → decodificador MDEC por software (FMV).
- **libspu:** ~30 funciones `Spu*` (voces, reverb, IRQ, transferencias) en `source/sound`. → emulación SPU (sonido usa secuenciador propio sobre SPU).
- **libpad / libcard / kernel:** `Pad*`, `_card_*`/`InitCARD`, eventos (`OpenEvent`…), **hilos `OpenTh/ChangeTh/CloseTh` en `source/mts`** (multitarea cooperativa) → corrutinas/fibras.
- **Accesos directos a hardware:** scratchpad `0x1F800000+` (65 `getScratchAddr` + literales), registros `0x1F801070` (I_STAT), `0x1F801040` (SIO pad). 11 ficheros con asm inline fuera de overlays.

## 4. Arquitectura — revisión propuesta (pendiente de cerrar con el disco)

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

Decisión A/B tras analizar el ejecutable del disco EU.

## 5. Capa VR, control, armas (sin cambios respecto al diseño)

- CompositorServices, estéreo con proyección real; cámara 6DOF = cabeza; agacharse real → agacharse en juego; cabeza del modelo oculta; brazos por IK a las manos; disparo desde el cañón en la mano (lógica de primera persona existente en el código); arreglar culling/LOD pensados para cámara cenital; cinemáticas en pantalla flotante; confort configurable dentro de la app (giro por pasos, viñeta).
- Control: PS VR2 Sense (principal), mando normal (alternativo), hand tracking (opcional). Hápticos en disparo/daño/radio/latido.
- Armas en mano: modelos low-poly propios con estética PS1 y piezas móviles; mundo y enemigos originales. Recarga sobre el contador original (modos manual/simplificada/automática en opciones). Fundas corporales + rueda.
- Arsenal: pistola con supresor, fusil de asalto, rifle de francotirador (mira render-to-texture, balanceo, tranquilizante lo reduce), lanzamisiles guiado (vista de misil en pantalla del lanzador), lanzamisiles antiaéreo, granadas (frag/chaff/aturdidora), explosivo adhesivo, minas direccionales.
- Gestos: radio (mano a la oreja), raciones/cigarros (boca; humo revela láseres), gafas térmicas/nocturnas, caja de cartón, golpear pared, estrangular, pegarse a pared.
- Difícil: animaciones de tercera persona y bosses de cámara cenital.

## 6. Herramientas en el repo

- `tools/disc/psxdisc.py` (stdlib): `info`, `ls`, `extract` (Form 2 en crudo 2336 B/sector), `dirtable <DISCO> <FICHERO>` (tabla de archivo DIR: entradas de 12 B = nombre[8] + sector, cabecera u32 tamaño), `manifest` (LBA/tamaño/sha256). **Sin probar aún con disco real.**
- `.github/workflows/build.yml`: job `reference` (solo dispatch o `[build]`): clona decomp/SDK desde variables, instala wine con caché apt, compila referencia. **Nunca ejecutado.** Requiere crear variables `DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO` en el repo.

## 7. Pendiente siguiente sesión

- [ ] Recibir discos partidos → `info`/`ls`/`manifest` de ambos; validar `psxdisc.py`.
- [ ] Extraer ejecutable EU; comparar con build de referencia JP (tamaño, secciones, firmas de funciones) → decidir opción A/B.
- [ ] Crear variables de Actions y lanzar `reference` (con permiso del usuario).
- [ ] Recompilador MIPS→C: decodificador + emisor, empezando por el ejecutable principal.
- [ ] HLE: GPU (OT/primitivas → Metal), GTE float, CD sobre imagen, hilos `mts` → fibras.
- [ ] Esqueleto de app visionOS (ventana plana primero).
