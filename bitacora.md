# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-27 `bef1f45` (IPA run 40425733598, artifact MVision-ipa 12699617122; CI verde):
  - Cine: `Cutscene.stereo` por defecto (IPD real); migración única screen→stereo (`xr.view.cutscene3.toStereo`, `XRPresent.swift`).
  - 2D: panel 6b siempre en mundo sin split (ignora hudPanels); selector/pausa a 0,9 m centrados en la mirada al abrir (`overlayAim`, `XRFramedLayout.overlayModel`, `XRMath.swift`); opciones de troceo ocultas (`mainItems`). `vr_scene` `drop_world2d`/`VR_FATE_WORLD2D` (POLY sin vista antes del último POLY 3D fuera del panel) solo en vista mundo sin selector/pausa (`vrhost.c` build_one).
  - Rígidos (0x40, OF_ONE_MATRIX): `ns_extract.c` usa la matriz raíz (en overlay 0x80081200 las piezas guardan traslación relativa al ojo). `ns_cover` mide factor Y (66/64) con `ns_extract_screen_ys`.
  - Captura: parche dinámico de la entrada 0 de 0x8009E534 (`ns_hook_rt.c` dyn_wrap, hash, reinstalación en `ns_hook_poll` c/32 frames) + respaldo; `[captura] canales por s: envoltura/dinamico/respaldo`.
  - Agua: `NSG_OV_BARS` apagado (`bars_gain`=0, `ns_screen.c`; era la viñeta circular); `NSG_CULL_SEMI` con la cabeza bajo el agua (`NativeRenderer.swift`, `NativeShaders.metal`, `ns_gpu_cull_applies`); onda reintenta sin desplazar si el texel es transparente.
  - Caja: máscara `NSG_OV_FRAME` (textura R8 160x128, `ns_screen_frame_build`) solo con IN_BOX (`vrhost_box_mask`); textura 1x1 de respaldo; diag `[caja] entra|mascara|sale`.
- 2026-10-27 `5ada745`: gateo (PLAYER_PRESENT), captura de respaldo, 2D en un panel (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (`bef1f45`):** todo lo anterior.
**Banco (volcados 26/27-oct):** cobertura intro auto01 37→91 %, auto03 9→49 %, d0004 73→96 %; auto02 sigue 20 % (lista de un fotograma anterior: cámara ~3,5°/300 mm distinta; rec_M caducado en cine, unverified). Agua d0005/auto04 idénticas byte a byte.
**Usuario (log 27-oct, 5ada745):** gateo OK; menús OK pero lejos; créditos/vida/radar no visibles (hudPanels=false persistido → arreglado); cine plano (mono → arreglado); agua con círculo (BARS → arreglado).
**Pendiente:** radar junto al agujero mientras se usa la caja (pedido para más tarde). Superficie de agua 80165558 `otra_textura(semi)` (UV/CLUT animado sin evidencia). Si el 2D en panel único no convence: opción 2 = 2D nativo desde RAM.
**CI:** render-test intermitente (exit 2) visto una vez; logs de CI 403.
**Unverified:** caja (sin volcado: cámara VR activa, OF_HIDDEN; ver `[caja]`); NSG_CULL_SEMI quita semitransparentes de una cara vistos por detrás bajo el agua; selector/pausa con drop_world2d excluido; destino del panel lineal (aditivo asume lineal); estado RAM tras 7 etapas en el respaldo (efectos/colores, OFX/OFY); aspecto de Snake al gatear; cartel del ascensor parpadeante (origen 2D o 3D sin ver); mallas de brazo estiradas; reparto de giro antebrazo/mano; deslizamiento lateral de pies al girar pelvis; imagen 3D 0,75 vs panel 2D 0,8 en PAL; signo ±45° dorso radar; `sync_epoca`.
**Qué mirar en el log:** `[caja]`, `xr-diag items` (hudPanels, hud!=nil), `[captura] canales por s` (dinamico>0 en intro), `[cine3d] pantalla3d-pixeles/pasada`, `[vista] cambio` al gatear/ascensor/muerte, `[manos] ... estirados/factor_estirado`, `[capa2d]`, `[radar] visibilidad`, `[vista] cambio`.
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
- Rígidos (OF_ONE_MATRIX): matriz raíz, nunca la de pieza.
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
- Banco: `make -C vr/scene dumpview ARGS=...`; `make -C vr/scene l2d-dump ARGS="<volcados sin / final> --png DIR"` (`L2D_SIN_W2D=1`); `make -f vr/native/extract.mk test-dumps DUMPS=<dir>`; nscover/nscine (`NSCINE_AGUA`).
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false`. Logs de CI con 403: `gh api repos/<repo>/check-runs/<id>/annotations`.
- Datos locales (nunca al repo): `gamedata/`.

## Module index
- (sin notas/ todavía)
