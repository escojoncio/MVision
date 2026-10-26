# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-26 `3db74bb` (IPA run 40328464591, artifact MVision-ipa 12652870830; CI verde: render-test, probe, ipa):
  - 2D: raster fijo 1 (`vl2a_options_native`, `vr_layer2d_async.c`; llamada en `vrhost.c` ~979); xBR-lv2 x3 compute `xr_xbr3` (`XRShaders.metal`) + sharp-bilinear en `xr_panel_color`; `XRPixelScale.swift` (`MV_2D_FILTER` xbr|nearest); vídeo (`view_kind==3`) sin xBR; `hq8` conserva 24 bits a raster 1 (`vr_layer2d.c`).
  - Créditos en partida: regiones HUD 1 y 4 con `XRFramedLayout` (`XRMath.swift`, píxel cuadrado, 1,6 m, capa 1,30 m; región 1 ~7° izq/5,6° arriba).
  - Cinemáticas: `Cutscene.screen` por defecto (clave `xr.view.cutscene3`; `stereo` antiguo migra a `.screen`): mundo nativo desde cámara del juego, mono (ipd 0, `XRStereo.eye`/`ns_cinema.c` aceptan ipd>=0) en panel de cine; panel 2D con aspecto lógico. Diag `[cine3d]` (`XRFrameEncoder.swift`, `RenderRG.cine3dDiag`).
  - Brazos (`vr_body.c/.h`): mano = pose exacta del mando; `vrbody_stretch_bone` (tope 2,5) en `apply_main` y ruta viva (`VrBodyArmPose.stretch`); límite de hombro con smoothstep 1–1,1; tau continuo ≤25°/cálculo; log `estirados/hombro/antebrazo_recortado/factor_estirado`.
  - Torso VRIK: pelvis zona muerta 40°, alineación a marcha >0,3 m/s, pecho 50% tope 30°, manos 0,3; piernas del juego giran con la pelvis; `NS_PARTS_BODY` (`ns_player.h`, `vrhost.c` ~354); dt medido `vrbody_set_dt` desde `vr_camera_hle.c` (vbl/vblank_hz).
- 2026-10-26 `cab3992`: mundo 3D solo con renderer nativo; lista GPU solo 2D (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (`3db74bb`):** todo lo anterior.
**Medido (log 26-oct, cab3992):** IK de brazos se aplica en juego (`brazos=+50`/25 applies); `sin_IK`/`jugador enlazado 0` solo en menú/cinemática/muerte. Cinemática del motor: el nativo dibujaba con cámara del juego en .stereo (5588 tris/ojo), pero el usuario veía el mundo "pausado" sin escenas → causa no confirmada; mirar `[cine3d]` (pantalla3d, seqNativo vs seqEstereo, drawEyes).
**Unverified:** constantes xBR (umbral 0,1, rampa 1/6); xBR sobre retratos con tramado; mallas de brazo estiradas; reparto de giro antebrazo/mano; deslizamiento lateral de pies al girar pelvis; imagen 3D 0,75 vs panel 2D 0,8 en PAL; signo ±45° dorso radar; `sync_epoca`.
**Qué mirar en el log:** `[cine3d]`, `[manos] ... estirados/factor_estirado`, `[capa2d]`, `[radar] visibilidad`, `[vista] cambio`.
**Pendiente:** efectos del 3D sin concretar (volcados auto02/auto03 "quads no coherentes"); mono dibuja dos pases idénticos; `test_geom` 2 fallos previos; `test_gpu`/`vrhost_run` no enlazan.
**Código muerto (no borrado):** `l2d_target`, `l2d_smooth`, `MV_HUD2D_SCALE`, `vl2a_scale_step`, menú "Suavizado 2D" (`smooth2D`), raster hq/EPX de `vr_layer2d.c`; `wrist_swing_deg`/`hand_twist_deg` (SIN USO); camino `gpuMode` en `RenderRG.swift`; `vr/native/ns_fill*`, `ns_cover*`; rutas RECT/DERIVED/billboard de `vr/scene/vr_scene.c`.

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D: HUD, menús, códec, créditos, radar 2D.
- Cuerpo: cuerpo del juego completo (doble cara) + guante original a 90 Hz.
- Menús: panel sobre la mano que lo abre, solo la ventana del selector, tamaño/aspecto fijos, nunca créditos.
- Créditos: como el original (imagen plana a pantalla completa).
- 2D: escalado solo en GPU (xBR-lv2 x3 + sharp-bilinear), nunca re-raster CPU adaptativo; vídeo sin xBR.
- Cinemáticas del motor: pantalla de cine con cámara del juego por defecto; estéreo inmersivo solo como opción.
- Cuerpo: mano siempre en la pose del mando; huesos se estiran (no se separa la muñeca); torso VRIK, piernas del juego.
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
