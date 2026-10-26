# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-26 `cab3992` (IPA run 40309745536, artifact MVision-ipa 12645090276; CI verde: render-test, probe, ipa). Cambio de arquitectura:
  - Mundo 3D SOLO con renderer nativo (RAM del juego: `vr/native/ns_extract.c` → `ns_gpu.c` → `app/Sources/Native/NativeRenderer.swift`/`NativeShaders.metal`). La lista GPU solo alimenta 2D (`vr_layer2d`). `vrhost.c`: MV_RENDER nativo por defecto y único; `mv_vrhost_set_render_mode` no-op.
  - Eliminados: modo gpu/PGXP para el mundo, `VRV_K_BACKUP`, `vrview_native_trusted`, `vrview_cover_feed` (`vr_view.c`), relleno `ns_fill`/scenery_stale (`NativeRenderer.swift`), red de 72 frames (`XRPresent.swift`, `RenderRG.swift` stereoHealthy/engineHealthy), heurísticas fx track/hold_near/comfort/drop_oncam (`ns_gpu.c`). Escena ausente → se mantiene la última (`[nativo] escena_ausente=N`).
  - Efectos: ON_CAMERA no-overlay descartados; `drop_inside_m` 0,15 m; tamaño en mundo fijo.
  - Sincronía VRAM/época de mallas: `NsScene.list_index`, `ns_scene_vram_ready`, `gpu_capture_next_index()`; `drain()` espera ≤6 frames (`sync_epoca=N`).
  - Códec/cine: ancho por `c.w` lógico (no `c.width` de textura), aspecto real (`XRCinemaLayout.snappedWidth/aspect`).
  - Menú L2/R2: `vl2d_menu_settled` crece por unión (<8 px) hasta 8 listas sin crecer (tope 60); banda de créditos v<0.35 excluida; `menuPending` en `XRPresent.swift` (mano vacía, HUD región 0 visible); `XRMenuPanelLayout.handSize/frontSize` con aspecto real.
  - Radar: `vrradar_dorsal_from_grip` con `hand_rot_offset` en ruta viva y de reserva (`vrgame.c`); umbrales 60/75; `width_m` 0,095; `depthBiasToEye` 1e6 (`RadarPanel.swift`).
  - Proyección del sistema por defecto (sin near propio); sin MTL_FAST_MATH/PRESERVE_INVARIANCE.

## Current state
**Pendiente de probar en visor (`cab3992`):** todo lo anterior.
**Medido offline (volcados d0002/d0003, `natrep`):** huecos ≤0,10 % en panorama 360 y barridos de cabeza; efecto más cercano 1,41 m; `ns_extract` caliente 0,5-0,64 ms x86.
**Unverified:** fallos del nativo de 20-oct "se ve a través de vértices" y "texturas rotas" no reproducibles offline (puede seguir); signo ±45° del dorso del radar; depth bias radar 1e6; negro en escenas sin escena nativa (intros/cambios de zona: HOLD 9,3 s en `ns_present.c` y luego negro); `sync_epoca` no debe crecer de forma continua; aspecto del códec ya no es 0,75 fijo.
**Qué mirar en el log:** `[nativo] escena_ausente/sync_epoca`, `[menu2d]`, `[radar] visibilidad`, `[capa2d] escala`.
**Pendiente:** cinemáticas/FMV y escenas con escenario de otra zona fuera del nativo; `test_geom` 2 fallos previos; `test_gpu`/`vrhost_run` no enlazan.
**Código muerto (no borrado):** camino `gpuMode` en `RenderRG.swift`; `vr/native/ns_fill*`, `ns_cover*` (solo diagnóstico); rutas RECT/DERIVED/billboard de `vr/scene/vr_scene.c`.

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D: HUD, menús, códec, créditos, radar 2D.
- Cuerpo: cuerpo del juego completo (doble cara) + guante original a 90 Hz.
- Menús: panel sobre la mano que lo abre, solo la ventana del selector, tamaño/aspecto fijos, nunca créditos.
- Créditos: como el original (imagen plana a pantalla completa).
- Sin emular sesiones en la nube; verificación con volcados del visor + banco offline. Una build por bloque.

## Files in play
- `vr/host/vrhost.c`, `vr_view.{c,h}`, `vrgame.c`; `vr/native/ns_gpu.{c,h}`, `ns_types.h`, `ns_extract.c`; `runtime/gpu.{c,h}`.
- `vr/scene/vr_layer2d.{c,h}`, `vr_layer2d_async.c`; `vr/radar/vr_radar.{c,h}`.
- `app/Sources/Native/NativeRenderer.swift`, `XRPresent.swift`, `XRMath.swift`, `XRFrameEncoder.swift`, `RenderRG.swift`, `RadarPanel.swift`, `app/project.yml`.
- Tests: `vr/native/test_no_gpu3d.c` (grep de fuentes), `test_ns_gpu_fx.c`, `vr/host/test_view.c`, `vr/scene/test_layer2d.c`, `vr/radar/test_radar.c`; `bench/Sources/main.swift`.

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/host test`, `make -C vr/scene test test-layer2d test-split`, `make -C vr/input test`, `make -C vr/body test`, `make -C vr/hands test`, `make -C vr/radar test`, `make -f vr/native/extract.mk test test-fill`, `make -f vr/native/fx.mk test`.
- Banco: `make -C vr/scene dumpview ARGS=...` (`--batch <dir>`, `--sheets`).
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false`. Logs de CI con 403: `gh api repos/<repo>/check-runs/<id>/annotations`.
- Datos locales (nunca al repo): `gamedata/`.

## Module index
- (sin notas/ todavía)
