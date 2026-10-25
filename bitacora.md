# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico completo de secciones 1-103: `git log -p bitacora.md` (antes de la compactación del 2026-10-25).

## Last done
- 2026-10-25 `6769f25` (IPA run 40219771232, artifact MVision-ipa 12608811575; el push sin `[ipa]` no genera IPA: lanzar `gh workflow run visionos.yml --ref main -f solo_render=false`):
  - Menús: `vr_gesture.c` dos zonas fijas por mano en marco de guiñada (izq 0,16 m a la izquierda, 0,26 m bajo la cabeza, −0,02 delante; der simétrica; sacadas de la marca L1+L2+R1+R2 del log 2026-10-25 l.5951), radio 0,15, se abre con el GATILLO de esa mano mantenido 0,30 s (histéresis `trig_on/off`), el agarre ya no abre; gatillo enmascarado (`out.mask`) solo con la mano dentro de su zona. Quitado `chest_depth`. `[pecho] zona=`.
  - Radar: `vr_radar.c` `along_m` 0,13 / `lift_m` 0,04 (muñeca) y `vrradar_wrist_quad_eye` (normal girada alrededor del antebrazo hacia el ojo; `RadarPanel.swift` pasa `headPosition`).
  - Agujeros: PGXP por dirección (`runtime/hps.h`, `gte.c` pool+etiquetas `gte_hp_resolve_a`, `interp.c` lw/sw/move/lwc2/mtc2, `mem.c`, `cpu.h` `hpt[32]`, `gpu.c` dirección por palabra); solo intérprete (`MV_NOGEN`), no `tools/recomp` ni memcpy BIOS. `gte.c` `hp_planar_fix`; `gpu.c` `cap_resolve` proj del vértice más fiable; `vr_scene.c` `repair_ambig_quad`, regla cercana solo si TODOS los vértices < `GPU3D_NEAR_EYE_M` (0,10). Culling del juego ampliado a ±1020 (`vrcam_patch_default` fov 179, `patch_exe.py`).
  - Render: `RenderRG.swift` `stereoCamera` con H/centro reales de la lista (`vr_scene_screen_window`); agua en cinemáticas: `stereoPreTextures` + `encodePost(warpOnly:)`; `vr_scene.c` `line_3d` (líneas con hp a quad 3D de 1 px).
  - `[gpu3d]` nuevos: drop_ambig/partial/incoh, reconstruidos, drop_huge/nan, hp_por_direccion/hp_por_valor.
- 2026-10-25 rama `k-pgxp` (fusionada en `main`): 3D desde la lista de la GPU con vértices de vista del GTE, por defecto (`vrhost.c` `MV_RENDER_DEFAULT_NATIVE 0`, `MV_RENDER=nativo` vuelve al nativo); `RenderRG.swift` `gpuMode`/`drawGPU`/`dsOpaque`/`dsSemi` (Z invertida, semitransparente sin escritura, sin descarte de reversos) recuperado de `a6cc811^`, más post-proceso de pantalla (agua/filtros, `planPost`/`encodePost`).
- `vr_view.c`: con `gpu3d` una escena 3D es ENGINE al instante; BACKUP (imagen plana VRAM) nunca en modo gpu.
- `vr_scene.c` (`eye_near_m` 0,25 m, `GPU3D_NEAR_EYE_M` en vrhost.c): se descarta un triángulo con vértice < 0,25 m del ojo SOLO si su arista máxima < 0,5 m (efectos); paredes se recortan contra el plano cercano.
- `vr_gesture.c`: pecho = cabeza −0,30 m vertical +0,18 m al frente (un punto), radio 0,15, delante del torso > 0,05 m, puño 0,30 s dentro; reinicio al perder seguimiento. `[pecho]` en `vrbodyg.c`.
- Radar: `vr_radar.c` hueso a 0,07 m hacia el codo y 0,035 m sobre +Y del grip (dorso); `RadarPanel.swift` `flipDorsal` (false) y `[radar]` 1/s; dibujado tras la lista con la misma profundidad (lo ocluye el brazo opaco).
- Registros nuevos: `[gpu3d]` 1/s (prims, hp, 2D, near, recortes, detrás, post, modo, vista, cámara), `[vista]` en cada cambio.
- Bitácora compactada (1953 -> <150 líneas).
- final6 (`main`): integra j-render (mallas ausentes toleradas, escenario nativo inservible no se dibuja, SIGUE descartado < 30 m, carteles >= 1 px) y j-control (capa de menú = mando original, ventanas por el pecho R2/L2, umbrales de gestos, radar anclado al mando izquierdo).
- final5: verificación del panel 2D contra el emulador (`vr_layer2d.c` `verify_panel`), cobertura nativo+relleno (`vrview_cover_feed`), seguimiento de efectos (`ns_gpu.c` `trk_update`).

## Current state
**Probado en visor (log 2026-10-25, build 64e36ce) — NO funcionaba:** menús abiertos lejos del cuerpo; radar pegado al hueso de la muñeca; agujeros en el 3D; 2D/créditos mal; agua de la intro mal; faltan efectos/polígonos.
**Build OK `6769f25` (todas las pruebas C pasan; Swift/Metal compilan en CI). Pendiente de probar en visor:** todo lo de "Last done". Unverified: huecos en suelo pegado a la cámara (medido en volcado d0018 sin sombra: 5,2 %, debería caer con PGXP por dirección), coste de `hp_planar_fix` (hasta 8⁴ combinaciones por quad), efectos del culling ±1020 (IA ve a todos "en cámara", más prims/OT).
**Qué mirar en el log:** `[gpu3d]` hp_por_direccion vs hp_por_valor, drop_ambig, cerca_descartados; `[pecho] zona=`; `[radar]`; xr-diag `mostrando=stereo post=1` en la intro.
**Pendiente:** mezcla semitransparente en gamma (hoy lineal: aditivos oscuros) — `vr_fs_scene`; RECT/sprites en 3D (`cap_rect` sin hp); PGXP por dirección en memcpy BIOS/polilíneas; scanlines/máscara en cinemáticas; `test_geom` 2 fallos previos (esquina compartida/sin ancla, término espacial), `test_gpu` no enlaza (`rt_orig`), `vrhost_run` no enlaza (`ns_fx_*`).
**Límites conocidos:** semitransparentes no ocluyen el radar; precalentado de pipelines gpu no portado.

## Decisions
- Menús: zona fija por mano + gatillo; nada de calibración.
- Dirección 2026-10-25: el 3D pasa a ser la lista de primitivas del propio juego con profundidad del GTE (tipo PGXP), con la cámara del juego movida por la cabeza (`vr/camera`); primitivas sin GTE (HUD, texto, efectos de pantalla) a un plano a distancia fija; sin respaldo plano ni render nativo por defecto.
- No emular/reproducir sesiones en la nube (coste); diagnósticos solo con logs del visor, que deben medir todo de una vez.
- Una sola build completa por bloque de trabajo.
- Capa de menú = mando original sin fusionar (j-control).

## Files in play
- `runtime/gte.c`, `runtime/gpu.{c,h}` (hp / vértices de vista).
- `vr/host/vrhost.{c,h}`, `vr/host/vr_view.{c,h}` (selección de render, respaldo).
- `vr/scene/vr_scene.{c,h}`, `render/metal/vr_*`, `app/Sources/RenderRG.swift`, `app/Sources/Native/NativeRenderer.swift`, `app/Sources/XRPresent.swift`, `XRRenderLoop.swift`.
- `vr/camera/` (cámara del juego atada a la cabeza).
- `runtime/hps.h`, `runtime/interp.c`, `runtime/mem.c` (PGXP por dirección).
- `vr/input/vr_gesture.{h,c}`, `vr/host/vrbodyg.c` (pecho), `vr/radar/`, `vr/body/`, `RadarPanel` en Swift.

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/host test`, `make -C vr/scene test test-layer2d`, `make -C vr/input test`, `make -C vr/body test`, `make -C vr/radar test`, `make -f vr/native/extract.mk test`, `make -f vr/native/fx.mk test`.
- Swift/Metal: solo los compila el CI (`.github/workflows/visionos.yml`, push a `main` -> IPA como artifact).
- Datos locales (nunca al repo): `gamedata/` (EXE, nombres, stages, disco extraído).

## Module index
- (sin notas/ todavía)
