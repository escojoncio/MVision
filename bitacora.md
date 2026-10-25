# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico completo de secciones 1-103: `git log -p bitacora.md` (antes de la compactación del 2026-10-25).

## Last done
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
**Probado en visor (logs 2026-10-20, builds e5fb9c2 y 9654b51) — NO funciona:**
- Ventanas/menús del pecho se abren con las manos lejos del cuerpo.
- Efectos pegados a la cara.
- Modelos 3D en los que se ve a través de los vértices.
- Escenas en 2D o con texturas rotas; al abrir códec / acabar escena, segundos de imagen plana a baja resolución (respaldo VRAM, `vr/host/vr_view.c`).
- Créditos mal.
- Radar detrás de la mano (invisible).
**Causa de fondo (confirmada por el código):** el 3D sale solo del render nativo (`vr/native/`, ~14.400 líneas de heurísticas: mallas reconstruidas desde la RAM + relleno + respaldo plano). El camino tipo PGXP (vértices de vista del GTE asociados a cada primitiva, `runtime/gte.c` hp, `gpu_capture_hiprec`) existe pero está apagado (`hp_on=0`) y el modo "gpu" se eliminó (`vrhost.c:242-249`, `MV_RENDER` solo acepta nativo).
**Pendiente de probar en visor (build de `main` con k-pgxp):** escenas/códec/créditos en 3D sin imagen plana; efectos sin pegarse a la cara; modelos sin ver a través; menús solo con la mano en el pecho; radar visible en la muñeca. Swift/Metal solo compilado por el CI.
**Qué mirar en el log:** `[gpu3d]` (hp vs sin hp, cerca_descartados, recortados, post, camara=head), `[vista]` (sin BACKUP), `[pecho]` (dist/profundidad/motivo de cada intento), `[radar]` (si el panel queda en la palma: `flipDorsal = true`).
**Límites conocidos:** semitransparentes no ocluyen el radar; con tiras de agua y sin 3D en el fotograma se pierden los fundidos; precalentado de pipelines gpu no portado (primer fotograma compila).

## Decisions
- Dirección 2026-10-25: el 3D pasa a ser la lista de primitivas del propio juego con profundidad del GTE (tipo PGXP), con la cámara del juego movida por la cabeza (`vr/camera`); primitivas sin GTE (HUD, texto, efectos de pantalla) a un plano a distancia fija; sin respaldo plano ni render nativo por defecto.
- No emular/reproducir sesiones en la nube (coste); diagnósticos solo con logs del visor, que deben medir todo de una vez.
- Una sola build completa por bloque de trabajo.
- Capa de menú = mando original sin fusionar (j-control).

## Files in play
- `runtime/gte.c`, `runtime/gpu.{c,h}` (hp / vértices de vista).
- `vr/host/vrhost.{c,h}`, `vr/host/vr_view.{c,h}` (selección de render, respaldo).
- `vr/scene/vr_scene.{c,h}`, `render/metal/vr_*`, `app/Sources/RenderRG.swift`, `app/Sources/Native/NativeRenderer.swift`, `app/Sources/XRPresent.swift`, `XRRenderLoop.swift`.
- `vr/camera/` (cámara del juego atada a la cabeza).
- `vr/input/vr_gesture.{h,c}`, `vr/host/vrbodyg.c` (pecho), `vr/radar/`, `vr/body/`, `RadarPanel` en Swift.

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/host test`, `make -C vr/scene test test-layer2d`, `make -C vr/input test`, `make -C vr/body test`, `make -C vr/radar test`, `make -f vr/native/extract.mk test`, `make -f vr/native/fx.mk test`.
- Swift/Metal: solo los compila el CI (`.github/workflows/visionos.yml`, push a `main` -> IPA como artifact).
- Datos locales (nunca al repo): `gamedata/` (EXE, nombres, stages, disco extraído).

## Module index
- (sin notas/ todavía)
