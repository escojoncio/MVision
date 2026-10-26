# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-26 `5ada745` (IPA run 40390169755, artifact MVision-ipa 12683510334; CI verde tras re-ejecutar render-test, fallo intermitente exit 2 sin log accesible):
  - Gateo: `ns_extract.c` extrae el jugador aunque tenga OF_HIDDEN (`NsExtractOpts.player_key`, `ns_extract_glue.c`); `NS_SV_PLAYER_PRESENT` (256, `ns_types.h`); `ns_scene_native_ok` = TOVR y (PLAYER_OK|PLAYER_PRESENT) sin MAP_INCOMPLETE/STORE_FULL (`ns_bridge.c/.h`); sin enlace o sin cabeza → todas sus piezas NS_PF_HIDDEN (`ns_hook.c`).
  - Cine en overlay: `ns_hook_backup_begin/ns_hook_backup_capture` (`ns_hook.c`, `ns_hook_rt.c`) desde `hle_vrbody_sort` (`vr_camera_hle.c`, stubs débiles) tras `rt_orig`; sin `stage_cb`; diag `[captura] canales por s: envoltura/respaldo | entrada0_etapas` (`vrhost.c`).
  - Vista (`vr_view.c/.h`, `VrViewIn.native_ok` desde `ns_hook_native_ready`): sin control + cámara VR + apta → WORLD; sin cámara + apta → ENGINE; si no → UI.
  - 2D opción 1: `vl2a_set_split` (defecto 0, `MV_L2D_SPLIT=1`), `XRLayer2D.split` (`XRHud.swift`); paso 6b de `XRPresent.swift` = capa completa en un panel (`XRFramedLayout`, 1,6 m, sharp); paso 5b/6, `RadarPanel`, `hudMenuUV`, `vrradar_split` inertes sin split.
  - `vl2d_encode` (`vr_layer2d.c`): resta+suma → aditivo; aditivo en luz lineal (créditos con brillo correcto).
  - Banco: `make -f vr/native/extract.mk test-dumps DUMPS=<dir>`, `make -C vr/scene l2d-dump ARGS="<volcados> --png DIR"` (+`L2D_ZONA`).
- 2026-10-26 `cac83e1`: bridge `read_mode` cinematic solo DEMO/PAD*; diag `[cine3d]` píxeles/pasada; migración única cutscene3; filtro 2D sharp por defecto.

## Current state
**Pendiente de probar en visor (`5ada745`):** todo lo anterior.
**Banco (volcados 26-oct):** d0001 (gateo) apta con jugador; auto01/02 (intro, entrada0=0x8008240c) respaldo da 9 inst; capa 2D d0002 = menú completo; composición lineal vs frame.png dif 3/255.
**Si el 2D en panel único no convence al usuario:** opción 2 = 2D nativo desde RAM (inventario, radar, vida).
**CI:** render-test falló una vez en dispatch (exit 2) y pasó al re-ejecutar; logs de CI 403.
**Unverified:** destino del panel lineal (aditivo asume lineal); estado RAM tras 7 etapas en el respaldo (efectos/colores, OFX/OFY); aspecto de Snake al gatear; cartel del ascensor parpadeante (origen 2D o 3D sin ver); mallas de brazo estiradas; reparto de giro antebrazo/mano; deslizamiento lateral de pies al girar pelvis; imagen 3D 0,75 vs panel 2D 0,8 en PAL; signo ±45° dorso radar; `sync_epoca`.
**Qué mirar en el log:** `[captura] canales por s` (respaldo>0 en intro), `[cine3d] pantalla3d-pixeles/pasada`, `[vista] cambio` al gatear/ascensor/muerte, `[manos] ... estirados/factor_estirado`, `[capa2d]`, `[radar] visibilidad`, `[vista] cambio`.
**Pendiente:** efectos del 3D sin concretar (volcados auto02/auto03 "quads no coherentes"); mono dibuja dos pases idénticos; `test_geom` 2 fallos previos; `test_gpu`/`vrhost_run` no enlazan.
**Código muerto (no borrado):** `l2d_target`, `l2d_smooth`, `MV_HUD2D_SCALE`, `vl2a_scale_step`, menú "Suavizado 2D" (`smooth2D`), raster hq/EPX de `vr_layer2d.c`; `wrist_swing_deg`/`hand_twist_deg` (SIN USO); camino `gpuMode` en `RenderRG.swift`; `vr/native/ns_fill*`, `ns_cover*`; rutas RECT/DERIVED/billboard de `vr/scene/vr_scene.c`.

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D: HUD, menús, códec, créditos, radar 2D.
- Cuerpo: cuerpo del juego completo (doble cara) + guante original a 90 Hz.
- 2D: capa completa del juego en un único panel plano (opción 1); troceo (menú en mano, regiones HUD, radar aparte) desactivado tras `vl2a_split`.
- 2D: escalado solo en GPU (sharp-bilinear por defecto; xBR opcional), nunca re-raster CPU adaptativo.
- Cinemáticas del motor: pantalla de cine con cámara del juego por defecto; estéreo inmersivo solo como opción.
- Cuerpo: mano siempre en la pose del mando; huesos se estiran (no se separa la muñeca); torso VRIK, piernas del juego.
- Vista: cinemática solo por banderas reales del motor; la cámara 1ª persona del juego (gateo, conductos) es gameplay en VR.
- Aptitud del mundo nativo: no depende del enlace del jugador (PLAYER_PRESENT basta); captura independiente de la entrada 0 de la tabla de etapas.
- Ninguna build sin banco offline sobre volcados del visor revisado.
- Sin emular sesiones en la nube; verificación con volcados del visor + banco offline. Una build por bloque.

## Files in play
- `vr/host/vrhost.c`, `vr_view.{c,h}`, `vrgame.c`; `vr/native/ns_gpu.{c,h}`, `ns_types.h`, `ns_extract.c`; `runtime/gpu.{c,h}`.
- `vr/scene/vr_layer2d.{c,h}`, `vr_layer2d_async.{c,h}`; `vr/body/vr_body.{c,h}`, `vr/camera/vr_camera_hle.c`; `vr/native/ns_cinema.{c,h}`, `ns_player.h`; `vr/radar/vr_radar.{c,h}`.
- `app/Sources/Native/NativeRenderer.swift`, `XRPresent.swift`, `XRMath.swift`, `XRFrameEncoder.swift`, `RenderRG.swift`, `RadarPanel.swift`, `XRShaders.metal`, `XRPixelScale.swift`, `XRHud.swift`, `app/project.yml`.
- Tests: `vr/body/test_body.c`, `vr/native/test_ns_present.c`, `vr/native/test_no_gpu3d.c` (grep de fuentes), `test_ns_gpu_fx.c`, `vr/host/test_view.c`, `vr/scene/test_layer2d.c`, `vr/radar/test_radar.c`; `bench/Sources/main.swift`.

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/host test`, `make -C vr/scene test test-layer2d test-split`, `make -C vr/input test`, `make -C vr/body test`, `make -C vr/hands test`, `make -C vr/radar test`, `make -f vr/native/extract.mk test test-fill`, `make -f vr/native/fx.mk test`.
- Banco: `make -C vr/scene dumpview ARGS=...` (`--batch <dir>`, `--sheets`).
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false`. Logs de CI con 403: `gh api repos/<repo>/check-runs/<id>/annotations`.
- Datos locales (nunca al repo): `gamedata/`.

## Module index
- (sin notas/ todavía)
