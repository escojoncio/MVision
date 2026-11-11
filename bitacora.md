# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.


## Last done
- 2026-11-11 (5ª tanda, tras Archivo_28: log + 18 volcados; sin verificar en visor; Swift/Metal sin compilar en local; 2 rondas de revisión adversarial):
  - Cuerpo propio oculto / brazos flotando: causa = adelanto global de 0,35 m a todos los dinámicos (`NS_GPU_DYN_RAISE_M`, 4ª tanda): los vértices a <0,45 m del ojo cruzaban el plano cercano y los dinámicos salían por delante de paredes. Quitado. Pies bajo el suelo ahora por pieza: `ns_sink.{c,h}` (bits 8-15 de `NsPiece.flags`, pasos de 8 uds; solo cuerpos articulados no rígidos ≥8 piezas hundidos ≤450 uds y ≤|sesgo| bajo suelo estático con `depth_bias`<0; enganche en `ns_extract_glue.c`, aplicado en `ns_gpu_make_piece`). Medido 0,03-0,15 ms por captura.
  - Conductos: quitado `NS_PARTS_CHEST`; el cuerpo tumbado se ancla 250 uds detrás del ojo según el rumbo del cuerpo (`cfg.prone_back`, `vr_camera.c`).
  - Pared: eliminada la 3ª persona propia y su fontanería (`third`, `cam_off`, `wall_back_m`...). `vrcam_wall_view()` (PS_WALL visto en 10 VBlanks; entra con 25 seguidos y contacto en ese VBlank; sale 6 después o sin control) apaga la cámara VR; `VRV_R_WALL` = pantalla 3D del motor con su cámara y mando original (`g.menu`); el toque es el botón de golpe.
  - Fondo 2D entre escenas / personajes que desaparecen: `BA_UNDRAW_COUNT` 0x800AC524 (contador de fotogramas sin dibujo del motor). Con él >0 `ns_gate` no captura (tope 3000 VBlanks, solo `vr_ok`) y `VRV_R_FROZEN` retiene modo y clase si la vista es nativa. `VRV_R_HOLD`: lista con 3D sin cámara del motor viniendo de vista nativa (motivos NO_GAME/CINEMATIC/LOST_CONTROL/WALL) retiene modo y clase ≤100 VBlanks (`vr_view.c`).
  - Térmicas: `NSG_PF_IRCLUT` (CLUT fija 0x3FFF para instancias no estáticas con `NS_IF_THERMAL_OK` y `NS_SFX_MODE_B`; shader). Cabeza flotante: piezas no estáticas con sesgo ≤ -30000 = retiradas por el motor (`PIECE_BIAS_HIDDEN`, `ns_extract.c`) y `ns_player_hide_head_attached` oculta instancias colgadas de la matriz de la cabeza.
  - Mosaico: `vr_mosaic.h` convierte las celdas 2D (mosaic.c) en cuadriláteros lisos del mundo (`NsEffect`, `MOSAIC_KEY`); salen del panel 2D solo si la última escena publicada ya lleva el parche (`nat_mosaic`, `VPC_COV_MOSAIC`).
  - Sin disparo al final: recarga automática bloqueada con reserva virtual 0 y balas en el cargador del juego (9 disparos VR, 6 del juego). `WeaponDef.make_ready` + `AUTO_READY_S` 2 s (`vrgame.c`, pistola/fusil/francotirador).
  - Bloques enormes en cinemáticas: el motor no dibuja polígonos con algún vértice a z ≤ H/2. `U.misc.y` = H/2+4 con `cullBack` (`NativeShaders.metal`, `NativeRenderer.swift`), réplica `NsRCam.zreject`.
- 2026-11-10 (tandas 1-4, sin verificar): prismáticos fieles al original (`binocularFrame`, zoom solo al mundo nativo), pistola a dos manos (`WP_SUPPORT`), viñeta de conductos fuera (`nat_duct_cut`), granada nace en la mano, guante con textura propia, balas solo HZD, locomoción ojo = raíz + altura por postura, impactos por cápsulas (`vr_hit`), cine estéreo (`ns_cinema_stereo`). Detalle: `git log -p bitacora.md`.
- 2026-11-02 y anteriores: procedencia por escritor, láser con colisión, granada por gatillo, 3 capas 2D, FX nativos, manos WebXR soldadas.

## Current state
**Funciona (tests):** todas las baterías de `Comandos` a 0 fallos. `test-dumps` sobre Archivo_28: 6 fallos ya presentes en el commit anterior (d0007 y prov_motse son volcados en plena carga, sin escena).
**Pendiente de probar en visor:** todo lo de 2026-11-11 y 2026-11-10.
**Sin causa encontrada:** d0010 (recuerdo en sepia, área 19291: 217 caras con otra textura y 287 ausentes en el mapa 0x8013f704); d0011 (superficie clara de más, piezas 54/57/64 del mapa 0x80162d0c); distorsión al morir (d0018, cobertura 100 %); agarre inferior de la pistola sin ajustar.
**Unverified (5ª tanda):**
- Vista de pared: rumbo del mando congelado al pegarse; 0,5 s para entrar; se presenta como las cinemáticas (opción `cutscene`).
- `BA_UNDRAW_COUNT` medido en 18 volcados (0 en juego, 0x7FFEFF7D en carga); que suba en el mismo fotograma del desmontaje no está medido.
- `VRV_R_HOLD`/`FROZEN` con modo mundo y cámara VR apagada: la app no oscurece (solo el motivo 12).
- Descarte cercano por triángulo (el motor lo hace por polígono: puede quedar medio cuadrilátero).
- Mosaico: celdas lisas opacas (sin semitransparencia), un fotograma de retraso de posición, primer fotograma en el panel 2D.
- `ns_sink`: pies hundidos más que |sesgo| del suelo no se adelantan; un pie adelantado puede asomar ante un objeto a menos de ese margen.
- IRCLUT también en las manos propias (`ns_hand.c` copia `THERMAL_OK`).
- `make_ready` con el juego en pausa; `ns_player_hide_head_attached` oculta cualquier objeto colgado de la cabeza.
**Unverified (anteriores):** agarre `MAIN_YAW` de la pistola; zona VRAM del guante; `VRHZD_SKIP_SIGHT`=4; rango 16000 uds de balas enemigas; ventanas 6/15/75 VBlanks de la granada; latencia de locomoción +~55 ms; brazos/IK a 25 Hz; carrera viewsHook vs acquire (`RenderRG.swift`).
**Si falla la build de Swift/Metal mirar:** `NativeShaders.metal` `ns_vertex` (bloque `one_sided`/`near_reject`, `NSG_PF_IRCLUT`), `NativeRenderer.swift` `u.misc`.
**Qué mirar en el log:** `[vista]` (pared=, sin_dibujo=, lista3D=), `[mosaico]`, `[recarga]`, `[impacto]`, `[ojo]`, `[cine3d]`, `[prov]`.
**Pendiente técnico:** `test_geom` 2 fallos previos; `vr_hit.c` sin test de clamp int16; `vrhost.c` sin test enlazado (solo `-fsyntax-only` y `vrhost_run`).
**Código muerto (no borrado):** `hudMenuUV`, `opt_aim`/`mv_vrgame_set_weapon_aim`, `grip_to_wrist_mm`/`wrist_swing_deg`/`hand_twist_deg`, `WL_INHAND`/`WL_DETONATE`/`G.dets`, `vrbody_set_armed`, `l2d_target`/`smooth2D`, `gpuMode`, `ns_fill*`/`ns_cover*`, `eye_shift` (diagnóstico), `vrhzd_wall_near` (normal de pared de la 3ª persona quitada: sin llamadores, solo `test_hzd.c`; comparte `wall_core` con `vrhzd_wall_dist`, que sí se usa).

## Decisions
- Mundo = renderer nativo desde la RAM del juego (objetos + mapa completos, sin culling de cámara); nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D, clasificada por PROCEDENCIA (actor/rutina emisora), nunca por forma/posición/mezcla.
- Cuerpo: cuerpo del juego completo + mano esquelética WebXR (MIT) soldada al antebrazo; arma del juego oculta en nativo. Nunca ocultar torso/pecho para arreglar la vista: se mueve el ancla.
- Orden de profundidad: correcciones por pieza y con causa medida (`ns_sink`), nunca un sesgo global a los dinámicos.
- Acciones que la primera persona no puede hacer (pegarse a la pared, golpearla): vista del juego con SU cámara en la pantalla 3D y mando original; nunca una 3ª persona propia.
- Viniendo de una vista nativa nunca se enseña en pantalla plana un fotograma 3D de PS1: se retiene la vista (FROZEN/HOLD/PENDING, acotados).
- 2D: 3 capas por clase de procedencia (HUD fijo, ventana L2/R2, FULL plano); efectos de pantalla del juego → post-proceso nativo. Escalado solo en GPU.
- Cinemáticas del motor: pantalla con cámara del juego, SOLO render nativo; mismas reglas de descarte que el motor (reversos, cercano).
- Armas: pose del arma = mando (nunca IK); arma en mano al equiparla; en recarga automática el arma VR nunca queda muerta con balas en el cargador del juego.
- Efectos originales (prismáticos, térmicas, mosaico): se reproduce lo que hace el juego, medido en volcados; nada inventado.
- Vista: cinemática solo por banderas reales del motor; la cámara 1ª persona del juego (gateo, conductos) es gameplay en VR.
- Aptitud del mundo nativo: no depende del enlace del jugador; rígidos (OF_ONE_MATRIX): matriz raíz.
- Ninguna build sin banco offline sobre volcados del visor revisado; una build por bloque; sin emular sesiones en la nube.

## Files in play
- `vr/host/vrhost.c`, `vr_view.{c,h}`, `vr_mosaic.h`, `vrgame.c`; `vr/camera/vr_camera.{c,h}`, `vr_camera_hle.c`; `vr/bridge/bridge_addrs.h`.
- `vr/native/ns_sink.{c,h}`, `ns_gpu.{c,h}`, `ns_gpu_types.h`, `ns_extract.c`, `ns_extract_glue.c`, `ns_player.{c,h}`, `ns_hook.{c,h}`, `ns_cpuref.{c,h}`.
- `vr/scene/vr_prov.{c,h}`, `vr_prov_rules.c`; `vr/weapons/weapon.h`, `w_pist.c`, `w_gunc.{c,h}`; `vr/body/vr_body.{c,h}`, `vr_body_cam.c`; `vr/gameplay/vr_fire.c`.
- `app/Sources/Native/NativeShaders.metal`, `NativeRenderer.swift`; `app/Sources/XRPresent.swift` (consume `view_mode`/`view_kind`/`view_reason`: 12 oscurece, 10/11 panel, clase 2 = pantalla 3D); `app/stage.sh`, `bench/build*.sh`.
- Tests: `vr/host/test_view.c`, `test_mosaic.c`, `test_vrgame_fire.c`; `vr/camera/test_camera.c`; `vr/native/test_ns_sink.c`, `test_ns_net.c`, `test_ns_cpuref.c`, `test_ns_hook.c`, `test_ns_extract.c`; `vr/weapons/test_rifle_sniper.c`; `vr/scene/test_prov_class.c`.

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/{host,camera,scene,input,body,hands,radar,weapons,gameplay} test`; `make -C vr/camera test-headcol test-native-coh`; `make -C vr/scene test-layer2d test-split`; `make -f vr/native/extract.mk test test-net test-hand test-solid test-cover test-sink test-cine test-fill`; `make -C vr/native test`; `make -f vr/native/fx.mk test`; `make -f vr/native/integra.mk test`.
- `vrhost.c`: `make -C vr/host ../../build-host/vrhost_run` (enlaza) o `clang -fsyntax-only` con los `-I` de `vr/*` y `runtime`.
- Banco: `make -f vr/native/extract.mk test-dumps DUMPS=<dir>`; `make -f vr/native/extract.mk nshito1 nscover` (`nshito1 --dump D --frame F --out O --name N --only game`, `NSHITO_RECHAZO=1` = descarte cercano; `nscover STAGE FRAME`, `NSCOVER_DETALLE=fichero`); `make -C vr/scene dumpview ARGS=...`, `l2d-dump`.
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false`. Logs de CI con 403: `gh api repos/<repo>/check-runs/<id>/annotations`.
- Datos locales (nunca al repo): `gamedata/`, volcados.

## Module index
- (sin notas/ todavía)
