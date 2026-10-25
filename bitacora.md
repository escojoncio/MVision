# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico completo de secciones 1-103: `git log -p bitacora.md` (antes de la compactación del 2026-10-25).

## Last done
- 2026-10-25 `069da3f` (IPA run 40228394774, artifact MVision-ipa 12612262250; ipa OK; render-test verde en `a820452` tras actualizar `vr/native/test_no_gpu3d.c` y stub `mv_vrgame_menu_hand` en `bench/c/bench_host.c`; el código del IPA no cambia):
  - Cine/créditos: 2D (kind 2) compuesto DENTRO de las texturas estéreo (`RenderRG.swift` `encodeStereo2D`, `stereoBase` por ojo con `stereoBaseStale`, `stereoWindow` solo de la lista actual); listas sin 3D pintan su 2D sobre la base; `vrhost_stereo2d_inside` (vr_view.h) decide si se quita el panel hp2 (`XRPresent.swift`). `vr_scene.c` `keep_late_clear` (TILE opaco ≥90 % tras el 3D se conserva = relleno de créditos) y `veil_as_2d` (velo semi a kind 2), ambos solo gpu+ENGINE.
  - Mezcla semitransparente en gamma: `render/metal/vr_blend.h` (C+MSL), `vr_fs_scene_blend` con `[[color(0)]]` + function constant `VR_FC_BLEND`; sin mezcla fija en pipelines semi.
  - Efectos de pantalla completa: `vrhost_keep_filter` (vr_view.h) en gpu conserva el filtro en mundo/cine con 3D (alarma/destellos a `planPost`, no al panel HUD).
  - Coplanares: `vr_scene.c` `coplanar_lift` (posterior en orden de lista más cerca del ojo 0,2 mm·min(d,50)), `coplanar_m` 0 por defecto, 0,005 solo en gpu; tope 400k pares.
  - Capa 2D async: `Out.mode/view`, `vl2a_layer_accept` (el overlay del menú solo acepta VL_PANEL de su vista), `vl2a_acquire` solo "nuevo" si cambia el seq; `vl2a_scale_step` estimación lineal, RECOVER 60.
  - Menús L2/R2: zona en marco físico (`vr_gesture.c` `hfs` desde `hrel`+`head_fwd`, reserva `in->yaw` si !fwd_ok), centro (±0,16, −0,26, −0,02), radio 0,15 entrar / 0,20 salir, `chest_lock` (una apertura por pulsación). Panel sobre la mano que lo abre: `mv_vrgame_menu_hand` (atómico), `XRMath.handModel` (0,30 m, +0,12 m, mín 0,45 m del ojo); sin mano → layout 1,40 m.
  - Radar: `vrradar_pick_arm` (antebrazo IK primero, mando de reserva); `[radar] fuente=IK|mando` + distancia al segmento codo-muñeca.
  - Volcado gpu con list.bin (`vr/host/vrdump.h`). Logs nuevos: `[estereo]`, `[geom]`, `[gpu3d] n_rect/n_rect_semi_tex`, `[pecho]` con hf/upm/yaws/rearme.
- 2026-10-25 `6769f25`: menús zona fija+gatillo, radar muñeca, PGXP por dirección, stereoCamera con H real (probado: no funcionaba, ver log de 069da3f pendiente).

## Current state
**Pendiente de probar en visor (`069da3f`):** todo lo de "Last done". Swift/Metal compilan en CI.
**Qué mirar en el log:** `[estereo]` (ok, lista3D vs lista2D), `[geom]` (grupos H, coplanares), `[pecho]` (hf real de la mano al abrir, rearme), `[radar] fuente=IK`, `[capa2d]` escala en créditos.
**Unverified:** causa de "geometría rota" (cobertura hp 99,2 %; sospecha z-fight de coplanares, sin confirmar); `[[color(0)]]` con MSAA/amplificación (coste por muestra); texto de créditos por `vr_fs_scene_blend` (antes un shader similar "rompía los textos", L2A_STALE); radar sobre `mv_vrgame_forearm` a tasa del juego (posibles tirones a 90 Hz).
**Pendiente:** créditos 100 % planos tipo original si hiciera falta (`vrview_kind_of` necesita umbral `flat2d` medido con log de créditos); RECT/sprites en 3D; PGXP por dirección en memcpy BIOS; scanlines en cinemáticas; `test_geom` 2 fallos previos, `test_gpu`/`vrhost_run` no enlazan.
**Límites conocidos:** semitransparentes no ocluyen el radar; precalentado de pipelines gpu no portado.

## Decisions
- Menús: zona fija por mano en marco físico + gatillo, una apertura por pulsación; panel sobre la mano que lo abre.
- Créditos: como en el juego original (sin reinterpretar).
- 3D = lista de primitivas del juego con profundidad GTE (tipo PGXP), cámara movida por la cabeza; 2D de pantalla compuesto en la misma imagen estéreo.
- No emular/reproducir sesiones en la nube; diagnósticos con logs del visor que midan todo de una vez.
- Una sola build completa por bloque de trabajo.

## Files in play
- `app/Sources/RenderRG.swift`, `XRPresent.swift`, `XRFrameEncoder.swift`, `XRMath.swift`, `RadarPanel.swift`, `XRRenderLoop.swift`.
- `render/metal/vr_scene.metal`, `vr_blend.h`, `vr_types.h`.
- `vr/scene/vr_scene.{c,h}`, `vr_layer2d_async.{c,h}`; `vr/host/vrhost.c`, `vr_view.h`, `vrdump.h`, `vrbodyg.c`; `vr/input/vr_gesture.{c,h}`; `vr/radar/vr_radar.{c,h}`.
- `vr/native/test_no_gpu3d.c` (test de texto sobre fuentes: actualizar al cambiar esas líneas).

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/host test`, `make -C vr/scene test test-layer2d test-split`, `make -C vr/input test`, `make -C vr/body test`, `make -C vr/radar test`, `make -f vr/native/extract.mk test`, `make -f vr/native/fx.mk test`.
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false` (push sin `[ipa]` no genera IPA).
- Datos locales (nunca al repo): `gamedata/`.

## Module index
- (sin notas/ todavía)
