# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico completo de secciones 1-103: `git log -p bitacora.md` (antes de la compactación del 2026-10-25).

## Last done
- 2026-10-26 `0078864` (IPA run 40257786653, artifact MVision-ipa 12623168195; CI verde):
  - `vr_scene.c`: `hw_cull_mask(op,&dl->disp)` solo descarta si el bbox toca la ventana mostrada (disp w/h≤0 → descarta siempre); `eye_near_m>0` → `near_units=1e-3` (plano cercano real en el renderer); quads SEMI con 2 esquinas limpias en diagonal → otras 2 DERIVED (salta si ya lo son). Volcados en partida: n_hw_cull ~55→2-10, partial→0, dif ipd0 idéntica (6.92/8.03/4.28/7.47).
  - Radar: `vrradar_wrist_quad_mesh` sin `eye` (normal = dorsal ortogonalizada, fija); `vrradar_wrist_quad_eye` eliminada; visibilidad = ángulo dorsal-ojo (antes circular, siempre 1).
  - Menú `vr_layer2d.c`: `op_key` relativo a ox/oy (doble búfer disp.x 0/320 rompía la histéresis → menú doble); `menu_pick` por componentes conexos (hueco <8 px), opacos primero, mayor área, reelección 8 listas y congelado; máscara `mchg` (celdas 8 px que cambiaban en las 16 listas previas sin menú) descarta radar/créditos; respaldo = mayor componente; nunca capa entera.
  - `XRPresent.swift`: selector sin rect → la mano no dibuja nada; hudRegions 8 entradas: región 0 recortada a v<0.17, región 1 nueva (créditos, v 0.17-0.35) framed.
  - `vr_layer2d_async.c`: `vl2a_scale_step(cur,target,ms,Vl2aScale*)` media móvil de 8, baja si media>12 ms, sube tras 15 (antes caía a 1 en la radio y no subía).
- 2026-10-26 `3c4b18e` (IPA run 40252313684, artifact MVision-ipa 12621257805; CI verde):
  - `vr_scene.c`: descarte hardware PS1 por triángulo (extensión >1023 X o >511 Y, `n_hw_cull`, fate HWCULL); brillos (quad semi con 2 esquinas DERIVED) como billboard al ojo (`VR_F_BILLBOARD`, esquinas por ojo en `vr_vs_scene`/`_amp`, estados fuera del hash); RECT con XY del GTE como cartel 3D (`rect3d`; `runtime/gpu.c` cap_rect + `gte_hp_resolve_addr`, solo intérprete).
  - L2/R2: `vrg_step_pad` mantiene el botón mientras el gatillo físico esté pulsado (histéresis, tolera 3 VB sin seguimiento); antes oscilaba (79 aperturas en 5 gestos).
  - Vista mundo: 2D no-HUD (créditos, textos) en plano fijo a cabeza con el ancho del cine (`XRPresent.swift` `hudIsFramed`); HUD sigue en paneles.
  - Radar: `vrradar_visibility` (aparece <45°, desaparece >55°, fundido 0,15 s); `vrradar_wrist_quad_mesh` (elevación = radio real del antebrazo del guante + 8 mm, `mv_vrgame_forearm_mesh`); sesgo de profundidad 3e5 (depth32Float, Z invertida).
  - Cuerpo: `hide_near` oculta piezas (no piernas 10-15, no brazos IK) con centro delante del ojo <0,30 m o vértice <0,12 m.
  - Autovolcado por anomalía (`vrdump.h` `autodump_decide`: máx 8/sesión, 2/motivo, 5 s; carpetas auto01..08; `[autodump]`). Logs `[manos]` own_hands/glv_pub/piezas_ocultas y `[cuerpo]`.
  - Banco: `vr/scene/tools/dumpview.c` (`make -C vr/scene dumpview ARGS=...`; replica el render gpu de la app; `--batch <dir>` tabla por volcado, `--sheets`). Medido: d0002 30,2 % → 11,3 % de diferencia con frame.png tras el descarte hardware.
- 2026-10-25 `ab5544e` (IPA run 40243003012, artifact MVision-ipa 12618215766; CI verde):
  - Cuerpo: `vr_body.c` bit 0x400 (doble cara) en los 16 modelos del jugador (pset 0x8016D518, modelos 0x8013E818+0x58*i), restaurado byte a byte; `torso_follow` (torso a la mirada `gaze_yaw_deg`, tau 0,25 s, zona muerta 15°, cuello fijo; quita el giro de la pose de reposo con pie adelantado); `own_hands` oculta solo brazo/antebrazo/mano (2,3,4 / 7,8,9) de los lados movidos por la IK.
  - Guante original del juego a 90 Hz: `vr/hands/vr_glove.{c,h}` (malla+atlas desde RAM/VRAM, mano derecha = espejo de la izquierda (la derecha del juego trae el pulgar mal), rgba 255), `vr_fist.c` (head_deg[0] 100, pulgar ≤35°, puño 69 % de largo); `vrgame.c` `glove_update` (3 huecos, reconstrucción ≤ cada 15 VB, falla → brazos del juego), `mv_host_hands` + `vrbody_arm_resolve` (IK con agarre vivo); `RenderWeapon.swift` mallas dinámicas en anillo de MTLBuffer.
  - Radar: `mv_vrgame_forearm_live` (mismo antebrazo que el guante, 90 Hz), `lift_m` 0,065 (radio antebrazo 47-55 mm).
  - Menú: `vr_layer2d.c` `vl2d_set_menu`/`vl2d_menu_rect` (bbox de prims nuevas frente a baseline, histéresis, solo crece con prims a <8 px), `hudMenuUV` en RenderRG/XRPresent: la mano muestra solo la ventana del selector; `[menu2d]`.
  - Golpes: `vr_gesture.c` rearme 0,4 s tras golpe; `vr_melee.c` reintento si el enemigo en reacción está ≤700 mm y es el más cercano; `[gest]` ang_enemigo y latencia.

## Current state
**Pendiente de probar en visor (`0078864`):** todo lo de `0078864`. Visor con `3c4b18e`: 3D peor con agujeros (hw_cull fuera de ventana), radar siempre visible, efectos mal, créditos en panel L2/R2, menú L2 doble, códec a baja resolución.
**Unverified (`0078864`):** dorsal del radar = +Y del marco de AGARRE (~45° respecto al dorso visual, sin `hand_rot_offset`); sin volcado con selector L2/R2 abierto ni con vida/objeto/arma (rects HUD no medidos); efectos ripple/bubble/particle como RECT (n_rect3d=0 en todo el log) siguen en el panel 2D; `test-layer2d` asíncrono inestable bajo carga.
**Flujo de verificación:** volcados del visor (manual o `[autodump]`) → `dumpview --batch` → arreglar por tipo de primitiva; no builds para probar.
**Medido con volcados de 069da3f (geomstat en scratchpad, no en repo):** el mundo se reconstruye fiel (99,8 % hp, 0 huecos, z-fight ≤0,09 %); la "geometría rota" era el cuerpo del jugador de una cara a 25 Hz.
**Unverified:** convención/signo de `gaze_yaw_deg` frente a la guiñada del torso con datos reales; anillo dinámico de RenderWeapon asume ≤3 frames en vuelo y 2 encode/frame; textos de créditos que cambian durante el selector; forma de la mano derecha original quizá pose de arma intencionada; lado con IK movida y guante caducado (>250 ms) puede quedar oculto.
**Qué mirar en el log:** `[autodump]`, `[gpu3d] n_hw_cull/n_billboard/n_rect3d`, `[radar] visibilidad/malla`, `[cuerpo] piezas_cerca_ocultas`, `[vista] motivo=selector` (1 apertura por gesto).
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
