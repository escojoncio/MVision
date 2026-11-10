# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.


## Last done
- 2026-11-10 (sin verificar en visor; Swift sin compilar en local; HEAD a8370ab):
  - Efectos de pantalla (nieve, chaff, fundido, gas) fuera del panel 2D solo con cobertura nativa probada: clases `VPC_WORLD`/`VPC_FX_SCREEN` (`vr_prov_rules.c`), `ns_fx_covers_packet` (`ns_fx.c`), `prov_covered`/`vrhost_native_sfx` (`vrhost.c`). `NSFX_A_MAP`=0x800ACAF8. `runtime/prov.c` interna por clave (fn, es-actor, nombre).
  - Viñeta de conductos (gas_efct.c 0x800de1f0) suprimida: `vrhost_duct_ring` (`vr_view.h`).
  - Locomoción: ojo = raíz BC_MOV + altura por postura (de pie/agachado/tumbado/caja), sin joint_lead; empuje HZD amortiguado (0,12 s, ≤1,5 m/s, instantáneo al entrar) en `vr_camera.c`; `vr_smooth` interpola una escena atrás; cuerpo a 90 Hz desde anillo de 2-3 poses (`mv_vrhost_body_piece`, `vr_bodylive.h`), manos/brazos última pose.
  - Impactos: `vr/gameplay/vr_hit.{c,h}` (cápsulas por pieza, zonas cabeza/cuerpo/miembro/cámara, línea de fuego con `vrhzd_raycast`, tiros entre plantas, balas enemigas con rango ampliado bajo 4 condiciones); letal solo a soldado normal (cls&4, side 2, vida ≤3×daño).
  - Puños/pared: barrido de cápsula (`vr_gesture.c`, `vr_melee.c`, `vrbodyg.c`), combos por objetivo; toque de pared llama a 0x80061294 vía 0x800513D8 (superficie casi vertical).
  - Cine estéreo: `ns_cinema_stereo` (base limitada por convergencia/divergencia) + `ns_cinema_depth_filter` (τ=0,3 s, reset en cortes); Swift: `NativeRenderer.swift cineNear/cineSubject/cineFilter`, `RenderRG.swift encodeStereo`.
  - Guantes: textura dorsal del guante original (texid 56829, tpage 0x000c, clut 0x38b1, parche gris u242-249 t120-123) en `ns_hand.c`.
  - Revisión adversarial ronda 2 aprobada; correcciones: `vpc_fx_extract` sin callback de cobertura, `combo_ct=(s>=0)` en etapa 1, clamp int16 en `vr_hit.c`, `ns_fx.c` en Makefiles de `vr/host` y `vr/tools/vrprobe` (`vrhost_run` y `vrprobe` enlazan).
- 2026-11-02: procedencia efectiva por escritor (`F_CHAIN`), láser con colisión (`vrhzd_raycast`, `w_laser.c`), granada por gatillo en dos fases (`w_throw.c`), cabeza oculta con visor (`vrcam_viewer_active`).
- 2026-11-01: procedencia internada, láser pistola, `vrbody_set_aim_fix`, granadas 2/7/8 con modelo del juego, agua en cine estéreo (`encodeStereo`).
- 2026-10-28 `fa3dc61`: procedencia (`runtime/prov.*`, `vr/scene/vr_prov*`), 3 capas 2D, FX nativos, manos WebXR soldadas al antebrazo (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (build de a8370ab):** todo lo de 2026-11-10 y anteriores.
**Unverified:** parpadeo 1-2 fotogramas de fundido/gas (cobertura mira la última escena publicada); coste de `ns_fx_extract` doble (`prov_covered` + `fx_classify_list`, no unificado porque `ns_fx_extract` no es idempotente: `g_shadow_tex`, `g_szl`); máscara HZD `VRHZD_SKIP_SIGHT`=4 (suposición); rango 16000 uds de balas enemigas (dificultad); toque de pared con stance≠0 pegado a pared (gesto lo deja pasar, `vr_melee.c:141` lo rechaza tras gastar háptica); normal de pared con nudillos <30 mm del suelo (`vrbodyg.c:110`); cilindro de reserva usa centro de caja como cintura (`vrbodyg.c:71`); borde dentado del parche del guante (11×8 texels); binoculares (verde/zoom), chaff activo, `vrhit_aim` (anotación de apunte, 120 VBlanks); latencia de locomoción +~55 ms; brazos/IK residuales a 25 Hz; carrera residual viewsHook vs acquire (`RenderRG.swift` ~888); realimentación renderer→host de overlays presentados (Swift).
**Si falla la build de Swift mirar:** `NativeRenderer.swift` (~782 llamada `mv_vrhost_body_piece`, cineFilter), `RenderRG.swift` (`encodeStereo`, `StereoRequest`).
**Qué mirar en el log:** `[impacto]`, `[ojo]`, `[lanzar]`, `[cine3d]`, `[prov]`, `[granada]`.
**Pendiente técnico:** `test_geom` 2 fallos previos; `vr_hit.c` sin test de clamp int16.
**Código muerto (no borrado):** `hudMenuUV`, `opt_aim`/`mv_vrgame_set_weapon_aim`, `grip_to_wrist_mm`/`wrist_swing_deg`/`hand_twist_deg`, `WL_INHAND`/`WL_DETONATE`/`G.dets`, `vrbody_set_armed`, `l2d_target`/`smooth2D`, `gpuMode`, `ns_fill*`/`ns_cover*`, `eye_shift` (diagnóstico).

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
