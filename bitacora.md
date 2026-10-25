# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico completo de secciones 1-103: `git log -p bitacora.md` (antes de la compactación del 2026-10-25).

## Last done
- 2026-10-25 `ab5544e` (IPA run 40243003012, artifact MVision-ipa 12618215766; CI verde):
  - Cuerpo: `vr_body.c` bit 0x400 (doble cara) en los 16 modelos del jugador (pset 0x8016D518, modelos 0x8013E818+0x58*i), restaurado byte a byte; `torso_follow` (torso a la mirada `gaze_yaw_deg`, tau 0,25 s, zona muerta 15°, cuello fijo; quita el giro de la pose de reposo con pie adelantado); `own_hands` oculta solo brazo/antebrazo/mano (2,3,4 / 7,8,9) de los lados movidos por la IK.
  - Guante original del juego a 90 Hz: `vr/hands/vr_glove.{c,h}` (malla+atlas desde RAM/VRAM, mano derecha = espejo de la izquierda (la derecha del juego trae el pulgar mal), rgba 255), `vr_fist.c` (head_deg[0] 100, pulgar ≤35°, puño 69 % de largo); `vrgame.c` `glove_update` (3 huecos, reconstrucción ≤ cada 15 VB, falla → brazos del juego), `mv_host_hands` + `vrbody_arm_resolve` (IK con agarre vivo); `RenderWeapon.swift` mallas dinámicas en anillo de MTLBuffer.
  - Radar: `mv_vrgame_forearm_live` (mismo antebrazo que el guante, 90 Hz), `lift_m` 0,065 (radio antebrazo 47-55 mm).
  - Menú: `vr_layer2d.c` `vl2d_set_menu`/`vl2d_menu_rect` (bbox de prims nuevas frente a baseline, histéresis, solo crece con prims a <8 px), `hudMenuUV` en RenderRG/XRPresent: la mano muestra solo la ventana del selector; `[menu2d]`.
  - Golpes: `vr_gesture.c` rearme 0,4 s tras golpe; `vr_melee.c` reintento si el enemigo en reacción está ≤700 mm y es el más cercano; `[gest]` ang_enemigo y latencia.
- 2026-10-25 `069da3f`: 2D de cine dentro del estéreo, mezcla en gamma, efectos a pantalla completa, coplanares, menús L2/R2 en marco físico con una apertura por pulsación. Visor: cinemáticas y sus letras OK; créditos en la mano, cuerpo hueco, radar atravesando, golpes fallidos (→ `ab5544e`).

## Current state
**Pendiente de probar en visor (`ab5544e`):** todo lo de "Last done" de `ab5544e`.
**Medido con volcados de 069da3f (geomstat en scratchpad, no en repo):** el mundo se reconstruye fiel (99,8 % hp, 0 huecos, z-fight ≤0,09 %); la "geometría rota" era el cuerpo del jugador de una cara a 25 Hz.
**Unverified:** convención/signo de `gaze_yaw_deg` frente a la guiñada del torso con datos reales; anillo dinámico de RenderWeapon asume ≤3 frames en vuelo y 2 encode/frame; textos de créditos que cambian durante el selector; forma de la mano derecha original quizá pose de arma intencionada; lado con IK movida y guante caducado (>250 ms) puede quedar oculto.
**Qué mirar en el log:** `[radar] desvio_mm/edad_IK_ms`, `[menu2d]`, `[gest] ang_enemigo/latencia`, `[guante]`.
**Pendiente:** RECT/sprites en 3D; PGXP por dirección en memcpy BIOS; scanlines en cinemáticas; `test_geom` 2 fallos previos, `test_gpu`/`vrhost_run` no enlazan.
**Límites conocidos:** semitransparentes no ocluyen el radar; precalentado de pipelines gpu no portado.

## Decisions
- Cuerpo: cuerpo del juego completo (doble cara) + guante original del juego a 90 Hz; nada de mallas de mano inventadas.
- Menús: zona fija por mano en marco físico + gatillo, una apertura por pulsación; panel sobre la mano que lo abre, solo con la ventana del selector.
- Créditos: como en el juego original (sin reinterpretar).
- 3D = lista de primitivas del juego con profundidad GTE (tipo PGXP), cámara movida por la cabeza; 2D de pantalla compuesto en la misma imagen estéreo.
- No emular/reproducir sesiones en la nube; diagnósticos con logs del visor que midan todo de una vez.
- Una sola build completa por bloque de trabajo.

## Files in play
- `vr/body/vr_body.{c,h}`, `vr_body_cam.c`, `vr_fist.{c,h}`; `vr/hands/vr_glove.{c,h}`; `vr/host/vrgame.{c,h}`, `vrhost.c`, `vrbodyg.c`.
- `vr/scene/vr_layer2d.{c,h}`, `vr_layer2d_async.{c,h}`; `vr/input/vr_gesture.c`, `vr_melee.c`; `vr/radar/vr_radar.{c,h}`.
- `app/Sources/RenderWeapon.swift`, `RenderRG.swift`, `XRPresent.swift`, `XRFrameEncoder.swift`, `XRStubRenderer.swift`, `XRRenderLoop.swift`.
- `vr/native/test_no_gpu3d.c` (grep de texto sobre fuentes); `bench/c/bench_host.c` (stub de todo símbolo C nuevo usado desde Swift sin `#if !MV_BENCH`).

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/host test`, `make -C vr/scene test test-layer2d test-split`, `make -C vr/input test`, `make -C vr/body test`, `make -C vr/hands test`, `make -C vr/radar test`, `make -f vr/native/extract.mk test`, `make -f vr/native/fx.mk test`.
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false` (push sin `[ipa]` no genera IPA). Logs de CI con 403: `gh api repos/<repo>/check-runs/<id>/annotations`.
- Datos locales (nunca al repo): `gamedata/`.

## Module index
- (sin notas/ todavía)
