# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-27 `27069dd` (IPA run 40567512416):
  - Cine: `vrhost_drop_world2d` (vr_view.h; vrhost.c:902) también en kind ENGINE; `XRPresent.swift` `nativeOnly`: en cinemática del motor nunca el item `.screen` VRAM (salvo opción `.cinema`); negro si nativo no listo. Banco: d0005 panel 83,7→28,6 % opaco. Nativo de cine fiel (cobertura "pose" baja = cámara de lista ~1° vieja, no bug).
  - Agua: onda quitada de `ns_fs`; post-proceso por ojo `vr_fs_wave(_layered)` (render/metal/vr_scene.metal), `RenderRG.swift` planWave/encodePost(wave:), `VrPostUniforms.wave`, `NativeRenderer.waveParams`; `ns_screen_wave_dx/row/rgb8`, `wave_space`=1 fila; test_ns_wave.c. Radar/arma de escena también ondulan.
  - Selector: revertida omisión por rect/base de 9555775; créditos = par de RECT texturados semi idénticos abr2+abr1 → ocultos solo en vista selector (`vl2d_hide_credits`, `credit_op`, vr_layer2d.c; worker vr_layer2d_async.c).
  - Conductos: `vr/camera/vr_hzd.{c,h}` (incluido en vr_camera.c bajo `VRHZD_STANDALONE`) limita el ojo con paredes HZD del juego (CONTROL 0x800ACB84+0x2C→map+8→wrapper→header; máscara 0x800ACB38); d0007 ojo -8996→-9198. Diag `[ojo-hzd]`.
  - Armas/manos: `ns_wphide` oculta piezas del arma del juego (PW_WEAPON_ACTOR objs) en nativo; supresor por `WGF_SUPP` (`VRGAME_SUPPRESSOR_ITEM`=-1 TODO → nunca visible); `vr_hand_mesh.c` mano nueva soldada 552 tris; `vr_hand_pose`, `vr_grip` (sockets en m, `vrgrip_mirror`), `VRH_GRIP_SOCKET`; ns_hand blend (grip=1 con HP_GRIP_*); disparo >0,5/rearme <0,3; arranque cargador corregido; sin gestos chaleco; recarga auto; granada agarre→READY→soltar (40 u/tick por m/s, `VRFIRE_GAME_G`=16), caída suave si se pierde; C4/mina por gatillo, detonar = círculo del juego. Diag `[arma]`, `[agarre]`, `[granada]` (12 líneas/3 s).
- 2026-10-27 `9555775`: bandas agua, vida (≤1 vértice → 2D), IA x1 + radar range_scale, diag `[radio-src]` (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (`27069dd`):** todo lo anterior.
**Usuario (log 27-oct 17:34, 9555775):** agua ondulando texturas; granada sin gesto + pieza flotante; conductos atravesables; pistola con supresor sin tenerlo, desmontable, fantasma detrás, sin disparo; helipuerto con PS1 baja resolución + cuadros; créditos en L2/R2 y menús cortados; efectos 4:3 (usuario: mantener pantalla cine 4:3 pero solo nativo); HUD desaparece con menús; manos no encajan (usuario: manos nuevas con poses).
**Necesita volcados del visor:** SOCOM sin/con supresor (índice de item supresor); granada en vuelo 2 VBlanks (confirmar actor kind 5 visible en nativo).
**Sin causa demostrada:** códec brillante (`[radio-src]` hqFull=true, ok); estela al girar (probar fluidez off); glitches de render en conductos (posible ojo fuera, ya corregido).
**Unverified:** créditos par abr2+abr1 sin falsos positivos en selector real; franja vertical HZD (Y arriba); escala socket `1/(vrhand_unit_m·upm)`; geometría gatillo de pistola primitiva; IA `range_mult`=2.4 + `head_see`.
**Volcados:** zip14 d0002 = selector; auto05-08 = agua; zip15 d0002-d0005 = helipuerto cine; d0006/d0007 = gateo conducto.
**Pendiente:** radar junto al agujero con caja (más tarde). Fuego ON_TRIGGER ~0,4 s latencia (ON_RAISE bloquea andar).
**CI:** render-test intermitente (exit 2) visto una vez; logs de CI 403.
**Qué mirar en el log:** `[arma]`, `[agarre] delta_mm`, `[granada]`, `[ojo-hzd]`, `[radio-src]`, `[vista] engine-por-sin-partida`, `[cine3d]`, `[capa2d]`.
**Pendiente técnico:** mono dibuja dos pases idénticos; `test_geom` 2 fallos previos; `test_gpu`/`vrhost_run` no enlazan (faltan ns_fx* en SRC_VR).
**Código muerto (no borrado):** `l2d_target`, `l2d_smooth`, `MV_HUD2D_SCALE`, `vl2a_scale_step`, `smooth2D`, raster hq/EPX de `vr_layer2d.c`; `wrist_swing_deg`/`hand_twist_deg`; `gpuMode` en `RenderRG.swift`; `vr/native/ns_fill*`, `ns_cover*`; RECT/DERIVED/billboard de `vr_scene.c`; `WL_INHAND`/`WL_DETONATE`/`G.dets`/`det_t`/`PAD_DETONATE` (vrgame.c, w_throw.h); `vrbody_set_armed`/`G.armed` (vr_body.c).

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D: HUD, menús, códec, créditos, radar 2D.
- Cuerpo: cuerpo del juego completo (doble cara) + mano VR propia (vr_hand_mesh) con poses por objeto; arma del juego oculta en nativo.
- 2D: capa completa del juego en un único panel plano (opción 1); troceo (menú en mano, regiones HUD, radar aparte) desactivado tras `vl2a_split`.
- 2D: escalado solo en GPU (sharp-bilinear por defecto; xBR opcional), nunca re-raster CPU adaptativo.
- Cinemáticas del motor: pantalla de cine 4:3 con cámara del juego, SOLO render nativo (nunca VRAM PS1); estéreo inmersivo opción.
- Armas: sin gestos de chaleco/cinturón; arma en mano al equiparla en el menú; accesorios por estado del juego.
- Efectos de pantalla (agua) = post-proceso por ojo, nunca por textura.
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
