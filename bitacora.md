# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-10-28 `fa3dc61` (IPA run 40603913439):
  - Procedencia: `runtime/prov.{c,h}` (ámbito en `rt_call`/JALR de interp.c con pila sombra por hilo, fiber.c, longjmp; sombra u16 de escrituras en mem.c/interp wr*); `GpuOp.prov/prov2/pkt_addr`, `GpuDrawList.prov`; volcados NSL2 + `prov.bin`; diag `[prov]` y autovolcados `prov_<motivo>` (scope/cinema/telop/motse/selector/pausa/códec).
  - Clasificación `vr/scene/vr_prov.{c,h}`, `vr_prov_rules.c`: cinema.c→FX_LETTERBOX, motse.c→FX_VIGNETTE, telop/famaslit→FULL, scope/sight→SCOPE_FRAME/TEXT, menuman→HUD_WORLD, WINDOW aprendida (ids callados ≥30 listas en mundo y activos con selector); UNKNOWN→HUD. 3 capas (HUD fijo 6b, WINDOW a 0,9 m 6c, FULL plano 6d); borradas heurísticas credit_op/MENU_BAND_V/menu_skip/drop por rect.
  - FX nativos: `VrPostUniforms.vfx` (viñeta, letterbox) en `vr_fs_wave(_layered)`; prismáticos = MvVrSpecialView SCOPE sintético (disco 0,34 m a 0,45 m, FOV/zoom, zoom=screen/160 clamp 2..8).
  - Manos: WebXR generic-hand MIT (`tools/hand/glb2c.py`, `vr/body/vr_hand_asset.c`, `vr_hand_skin.c`), skinning CPU, escala al antebrazo del juego (×1,27), costura: pieza 0 = 4 vértices fin de antebrazo + transición 6 anillos soldada (`ns_hand.c`), `fore_twist` reparte el giro en el antebrazo (normales giradas), `test_watertight` (hombro→dedos). `vr_body.c`: antebrazo 100 % del giro, muñeca = agarre − R·centro, límites de muñeca, swivel de codo. Borrado `vr_hand_mesh.*`. Herramienta `nshands` (render real sobre volcado).
  - Armas: `hand_frame_from_grip` pura del mando (sin IK); arma = agarre·offset·socket⁻¹; cabeceo fijo armado; granadas con textura incompleta → modelo nativo del juego (`Slot.native`); diag `[vrgame] arma VR N texturas`.
- 2026-10-27 `27069dd`: cine solo nativo, agua post-proceso, HZD conductos, primeras manos/armas (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (`fa3dc61`):** todo lo anterior. Usuario: queda 1 build más tras esta.
**Usuario (27-oct, 27069dd):** manos horribles (giro propio, dedos palo, pulgar) → rehechas; 2D roto 40 builds (L2/R2 secuestra HUD) → procedencia; efectos viñeta/cine en 4:3 → nativos; pistola apunta raro → mando directo; granadas no reales (blancas; chaff no es blanca); prismáticos 2D sin zoom → vista nativa.
**Unverified (necesita log):** reglas de procedencia por nombre de actor (`[prov]`), aprendizaje WINDOW, zoom real de prismáticos (hook "ZOOM LEVEL" no hecho), escala de viñeta, coste de prov en intérprete, índice de item supresor, granada en vuelo visible, aturdidora id 7 sin modelo.
**Riesgos conocidos:** tabla prov 1024 ids sin liberar (`tabla_llena`); pila sombra con frames huérfanos (`rt_prov_ovf`); arista del fin del antebrazo (sección cuadrada) visible.
**Volcados:** zip16 d0002 viñeta+vida+ración; d0003 cine+créditos; d0004/5 prismáticos; zip14 d0002 selector.
**Qué mirar en el log:** `[prov]`, `[arma]`, `[agarre] delta_mm`, `[granada]`, `[vrgame] arma VR N texturas`, `[ojo-hzd]`, autovolcados `prov_*`.
**Pendiente técnico:** `vrhost_run`/bench no enlazan (ns_fx*, vr_prov*); `test_geom` 2 fallos previos.
**Código muerto (no borrado):** `hudMenuUV` (XRPresent/RenderRG), `opt_aim`/`mv_vrgame_set_weapon_aim`, `grip_to_wrist_mm`/`wrist_swing_deg`/`hand_twist_deg`, `WL_INHAND`/`WL_DETONATE`/`G.dets`, `vrbody_set_armed`, `l2d_target`/`smooth2D`, `gpuMode`, `ns_fill*`/`ns_cover*`.

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D, clasificada por PROCEDENCIA (actor/rutina emisora), nunca por forma/posición/mezcla.
- Cuerpo: cuerpo del juego completo + mano esquelética WebXR (MIT) soldada al antebrazo del juego en una malla continua; arma del juego oculta en nativo.
- 2D: 3 capas por clase de procedencia: HUD fijo, ventana L2/R2 (única que se mueve), FULL plano; efectos de pantalla del juego → post-proceso nativo.
- 2D: escalado solo en GPU (sharp-bilinear por defecto; xBR opcional), nunca re-raster CPU adaptativo.
- Cinemáticas del motor: pantalla de cine 4:3 con cámara del juego, SOLO render nativo (nunca VRAM PS1); estéreo inmersivo opción.
- Armas: pose del arma = mando (nunca IK);
  sin gestos de chaleco/cinturón; arma en mano al equiparla en el menú; accesorios por estado del juego.
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
