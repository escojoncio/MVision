# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-26 `cac83e1` (IPA run 40364000314, artifact MVision-ipa 12670435641; CI verde):
  - Cámara VR continua: `bridge.c` `read_mode` → `cinematic` solo con DEMO/PADDEMO/PADMASK/PADRELEASE (o NOT_PLAYABLE/GAME_OVER con vida+radar+menú apagados); quitada heurística `player_cam`. `vr_view.c` `vrview_kind_of`: `LOST_CONTROL` ya no da ENGINE. `XRPresent.compose` sin `gameCamScene`; `RenderRG`: `cinemaGameView` solo con `engineView` (`sceneUnfit` sin lectores, no borrado).
  - Cine: cull de reversos (`cull_eye`, ojo del juego) se mantiene en pantalla/cine. Diag `[cine3d]` añade `pasada=[cull,color,prof,viewport,tris/ojo,dibujos]` y `pantalla3d-pixeles` (rejilla 16x16, `XRStereoProbe`, cada 2 s).
  - Migración única `xr.view.cutscene3` stereo→screen (marca `xr.view.cutscene3.migrated`); bench fija la marca.
  - Menú L2/R2: `vr_layer2d.c` `menu_skip` (rect asentado: omite base y lo que no corta rect+`MENU_KEEP_PX`); `XRMath.handSize` ancho 0,30 m, aspecto del rect, tope 0,45 m; sin paneles HUD con ventana 2D.
  - 2D: `XR2DFilter.sharp` por defecto (sin xBR; `MV_2D_FILTER=xbr|nearest`).
  - Tests: `test_bridge` (conducto), `test_view` (sin control ≠ ENGINE), `test_layer2d` `menu_only_window_cases`, `test_no_gpu3d` actualizado.
- 2026-10-26 `3db74bb`: xBR, créditos HUD 1/4 `XRFramedLayout`, `Cutscene.screen`, brazos estirados, torso VRIK (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (`cac83e1`):** todo lo anterior.
**Cine sin imagen (causa no confirmada):** log 26-oct: pantalla3d=ok, estereoDibujado=true, color final muy oscuro (2-58/255). Leer `pantalla3d-pixeles` y `pasada=` en `[cine3d]`: si luminancia≈0 → render a textura vacío/cull; si >0 → composición del panel.
**Cine (usuario):** no es negro: se ve la escena con la cámara desde arriba pero congelada (agua quieta, como imagen 2D); al pulsar Start se ve ~1 s la escena real. Encaja con el log 26-oct: en cinemática `[captura] escenas=0` y `seqNativo` estancado (603) → la captura nativa de la RAM no se refresca en cinemáticas y el panel muestra un mundo antiguo (unverified). Siguiente: averiguar qué condición detiene la captura nativa durante cinemáticas (in_control/has_player/cam o lista de objetos distinta).
**Riesgo:** cinemáticas sin banderas DEMO/PAD* quedan en mundo VR; zona/intro sin control → cine plano 2D.
**Unverified:** mallas de brazo estiradas; reparto de giro antebrazo/mano; deslizamiento lateral de pies al girar pelvis; imagen 3D 0,75 vs panel 2D 0,8 en PAL; signo ±45° dorso radar; `sync_epoca`.
**Qué mirar en el log:** `[cine3d] pantalla3d-pixeles/pasada`, `[vista] cambio` al gatear/conductos, `[manos] ... estirados/factor_estirado`, `[capa2d]`, `[radar] visibilidad`, `[vista] cambio`.
**Pendiente:** efectos del 3D sin concretar (volcados auto02/auto03 "quads no coherentes"); mono dibuja dos pases idénticos; `test_geom` 2 fallos previos; `test_gpu`/`vrhost_run` no enlazan.
**Código muerto (no borrado):** `l2d_target`, `l2d_smooth`, `MV_HUD2D_SCALE`, `vl2a_scale_step`, menú "Suavizado 2D" (`smooth2D`), raster hq/EPX de `vr_layer2d.c`; `wrist_swing_deg`/`hand_twist_deg` (SIN USO); camino `gpuMode` en `RenderRG.swift`; `vr/native/ns_fill*`, `ns_cover*`; rutas RECT/DERIVED/billboard de `vr/scene/vr_scene.c`.

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D: HUD, menús, códec, créditos, radar 2D.
- Cuerpo: cuerpo del juego completo (doble cara) + guante original a 90 Hz.
- Menús: panel sobre la mano que lo abre, solo la ventana del selector, tamaño/aspecto fijos, nunca créditos.
- Créditos: como el original (imagen plana a pantalla completa).
- 2D: escalado solo en GPU (sharp-bilinear por defecto; xBR opcional), nunca re-raster CPU adaptativo.
- Cinemáticas del motor: pantalla de cine con cámara del juego por defecto; estéreo inmersivo solo como opción.
- Cuerpo: mano siempre en la pose del mando; huesos se estiran (no se separa la muñeca); torso VRIK, piernas del juego.
- Vista: cinemática solo por banderas reales del motor; la cámara 1ª persona del juego (gateo, conductos) es gameplay en VR.
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
