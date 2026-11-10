# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.


## Last done
- 2026-11-10 (4ª tanda, tras Archivo_27; sin verificar; Swift sin compilar):
  - Conducto (stance 2 o PS_GROUND, 1ª persona): se ocultan torso/cuello/hombros (`NS_PARTS_CHEST`, `hide_parts` en `NsCtxOut`, `ns_hook.c`, `vrhost.c ns_ctx`); antebrazos/manos/pelvis visibles. Arma no dibujada ni en mano con `duct` (`vrgame.c`, `stance==2||prone`), láser y dos manos también desactivados; disparo sin tocar. Unverified: máscara 1,2,5,7 (quizá pelvis 0), stance 1.
  - Prismáticos 3D: `RenderRG.swift worldZoom/zoomed` (P00/P11 × zoom 2..8 solo al mundo nativo), `XRFrameEncoder.swift binocularZoom/binocularFrame` (marco HUD SCOPE_BINO en rectángulo fijo a 1 m, 58° alto, `MV_VIEW_AA_SIGHT`); `binocularView` solo para sight.c sin scope.c. Con prismáticos (`fx_hold.fx.scope_on && binocular`, `bino_view()`) se ocultan brazos/manos (`NS_PARTS_ARMS`), arma y objeto sujeto (`vrgame_set_view_hide`). Unverified: compila, zoom brusco sin suavizado, mareo a 8x, fuera del marco sin máscara.
  - Pistola a dos manos: `WP_SUPPORT` en (0,0.030,-0.150) (`w_pist.c`, estimado), enganche con agarre >0,5 a ≤0,12 m (suelta <0,3 o >0,16 m), puntería por línea mano principal→apoyo con rampa 0,15 s (`two_hand_aim`, `vrgame.c`), mano de apoyo a `pub_sup` en IK. Solo pistola; fusil/francotirador sin cambio. Unverified: pose de dedos, salto al enganchar.
  - Viñeta 4:3 de conductos: `ns_hands_finalize` quita NS_SFX_GAS pero las prims 2D de `gas_efct.c` quedaban en HUD (UNKNOWN); ahora el recorte cuenta como cobertura nativa (`vrhost_scene_sfx` en `vr_view.h`, `nat_duct_cut` en `vrhost.c`). Unverified: actor sin nombre, cobertura exige renderer nativo listo y escena ≤8 sync.
  - Prismáticos fieles al original (volcados auto05/auto06): el original es una lista 2D plana 320x256 (tinte, bandas negras, rejilla, gráfica de barras animada, textos), sin círculo ni distorsión. Ahora el HUD vivo se dibuja entero en un rectángulo de 58° a 1 m y negro opaco fuera (`binocularFrame` round=2, `RenderWeapon.swift encodeSpecial binoMask` quad 48 m, rama nueva en `vr_weapon.metal vr_fs_spc`). Unverified: compila (Metal/Swift solo en CI), tamaño del campo (`h` en binocularFrame), HUD oscuro en modo lineal.
  - Pendiente: soldados sin pies (dumps sin evidencia; pedir volcado con soldado visible; hipótesis `feet_below_pos`/`vr_scene_player_origin` vr_scene.c:579, `eye_near_m` vr_scene.c:1176); mano de prismáticos bajo los ojos no investigada (ahora ocultas); distorsión al morir sin dump; sin líneas `[toque]` en el log.
- 2026-11-10 (3ª tanda; sin verificar; HEAD tras esta entrada):
  - Pared: cámara 3ª persona con PS_WALL (`vr_camera.c`, `wall_back_m`=1,2 m `wall_up_m`=0,3 `wall_s`=0,25 s; antirrebote `wall_hold` 0,2 s; ojo por el lado libre con normal HZD `vrhzd_wall_near`, `vr_hzd_wall.h`, normal filtrada τ=0,15 s ≤20°/tick; `cam_off`/`third` en `VrCamPlan/VrCamRecord`, manos/disparo/gestos anclados al ojo de 1ª persona: `cam_view`, `vrfire_map_from_cam`). Cabeza visible con `third` (histéresis 0,5/0,35 m).
  - Toque: distancia a pared con `vrhzd_wall_dist` (paredes HZD) cuando `on_wall` (`vrbodyg.c`, `[toque] fuente=c|h`); gesto con `knock_touch_wall` 0,12 m, `knock_speed_wall` 0,2 m/s. Unverified: falsos toques por balanceo de puño; PS_WALL durante el golpe de nudillos.
  - Granada lanzada: el actor nace en la mano (`vrfire_set_throw_origin/body`, `ns_wphide_pos/has`, `vrfire_flier_seen`, ventanas 6/15/75 VBlanks estimadas), validación pared/1,5 m con `vrhzd_raycast` skip 0; aturdidora id 7 con ranura {7,0}. Retardo juego≈16 VBlanks entre soltar y actor (deducido). `[granada]` con `retardo_vbl`, `origen`, `gid`, `grupo`, `modelos`.
  - Guante: paleta real de la CLUT 0x38b1 (tela 0x1484 azul marino, placa 0x294a gris #505050; región u240-251 t101-148, dorso supuesto).
- 2026-11-10 (2ª tanda, tras logs del visor; sin verificar; HEAD 9661fd6):
  - Arma/mano: causa = `RenderRG.swift` dibujaba el arma con vistas corregidas por `vr_smooth` (`m`) y la mano con `inv(m)`; ahora `bodyViews` (sin corrección) para arma/`encodeSpecial`. Piezas `live` de mano y objeto sujeto siguen el marco del mando de ahora (`vbl_hand_delta`, `vrgame_hand_frame_sim/now`, `vr_bodylive.h`). Antebrazo (pieza 0) sigue en pose de escena (estiramiento 1-4 cm en transición, unverified).
  - Granada en mano: `ns_wphide_key/carry` (`ns_wphide.c`) la lleva a la mano IK; `vrbody_hand_anim` (`vr_body.c`). Supuesto sin verificar: el juego calcula la matriz del arma desde la mano animada antes de la IK.
  - Selector L2/R2: cuadrado fantasma = cielo 2D opaco (fn 0x800c7bd0 id 122 UNKNOWN, 0x800c7234 id 120 WORLD); `vrhost_drop_world2d` ahora también con selector (pausa sigue excluida).
  - Prismáticos: clase `SCOPE_BINO` (`scope.c` en la lista; sight.c sin scope.c = PSG-1/Stinger/Nikita, sin tocar), marco original a capa HUD, lente rectangular 4:3 (`XRFrameEncoder.swift binocularView`, `RenderWeapon.swift` round=0).
  - Balas: `hit_wall` solo HZD (`vrhzd_raycast`) + trazado del juego; `vrhc` ya no bloquea. `[impacto]` con `motivo=` y `mas_cercano=`. Alcance mínimo `VRHIT_RANGE_MIN` 16 m (soldado más cercano a 17,2 m en d0007; puede ser alcance).
  - Agarre: con botón R1/L1 ya mantenido, mano en zona cuello/espalda por detrás, lenta, manos libres → pendiente (`vr_gesture.c` ~468). Toque a pared: log `[toque]` con motivo (`cerca`, `lento`, `sin_puno`, `postura`...); toque agachado rechazado en el gesto. En el log no hubo ningún intento de toque (causa sin confirmar).
  - Guante: textura propia 64x32 4 bpp 12 colores (`ns_hand.c`, `ns_hand_glove_block/blit`), placa gris #6e7378 ~72% ancho dorso, resto casi negro; luz neutra por luminancia (`neutral_light`). Bloque VRAM x992..1023 y0..31 (tpage 0x0F, clut 0x3F x1008) inyectado como evento 0 en cada lista (`vrhost.c fill_slot`, desplaza ev_end/src_op +1). Unverified: que el juego no use esa zona.
- 2026-11-10 (sin verificar en visor; Swift sin compilar en local; HEAD a8370ab):
  - Efectos de pantalla (nieve, chaff, fundido, gas) fuera del panel 2D solo con cobertura nativa probada: clases `VPC_WORLD`/`VPC_FX_SCREEN` (`vr_prov_rules.c`), `ns_fx_covers_packet` (`ns_fx.c`), `prov_covered`/`vrhost_native_sfx` (`vrhost.c`). `NSFX_A_MAP`=0x800ACAF8. `runtime/prov.c` interna por clave (fn, es-actor, nombre).
  - Viñeta de conductos (gas_efct.c 0x800de1f0) suprimida: `vrhost_duct_ring` (`vr_view.h`).
  - Locomoción: ojo = raíz BC_MOV + altura por postura (de pie/agachado/tumbado/caja), sin joint_lead; empuje HZD amortiguado (0,12 s, ≤1,5 m/s, instantáneo al entrar) en `vr_camera.c`; `vr_smooth` interpola una escena atrás; cuerpo a 90 Hz desde anillo de 2-3 poses (`mv_vrhost_body_piece`, `vr_bodylive.h`), manos/brazos última pose.
  - Impactos: `vr/gameplay/vr_hit.{c,h}` (cápsulas por pieza, zonas cabeza/cuerpo/miembro/cámara, línea de fuego con `vrhzd_raycast`, tiros entre plantas, balas enemigas con rango ampliado bajo 4 condiciones); letal solo a soldado normal (cls&4, side 2, vida ≤3×daño).
  - Puños/pared: barrido de cápsula (`vr_gesture.c`, `vr_melee.c`, `vrbodyg.c`), combos por objetivo; toque de pared llama a 0x80061294 vía 0x800513D8 (superficie casi vertical).
  - Cine estéreo: `ns_cinema_stereo` (base limitada por convergencia/divergencia) + `ns_cinema_depth_filter` (τ=0,3 s, reset en cortes); Swift: `NativeRenderer.swift cineNear/cineSubject/cineFilter`, `RenderRG.swift encodeStereo`.
  - Guantes: textura dorsal del guante original (texid 56829, tpage 0x000c, clut 0x38b1, parche gris u242-249 t120-123) en `ns_hand.c`.
  - Revisión adversarial ronda 2 aprobada; correcciones: `vpc_fx_extract` sin callback de cobertura, `combo_ct=(s>=0)` en etapa 1, clamp int16 en `vr_hit.c`, `ns_fx.c` en Makefiles de `vr/host` y `vr/tools/vrprobe` (`vrhost_run` y `vrprobe` enlazan).
  - Una sola `ns_fx_extract` por lista (`hx_extract()` en `vrhost.c`, `hx_cov` a 0 en `build_one`; `ns_fx_extract` no es idempotente: `g_shadow_tex`, `g_szl`). Toque de pared rechazado con stance≠0 en `vr_gesture.c` (BOX 0x1000 no visible para el gesto; `vrm_knock_check` no mira postura).
- 2026-11-02: procedencia efectiva por escritor (`F_CHAIN`), láser con colisión (`vrhzd_raycast`, `w_laser.c`), granada por gatillo en dos fases (`w_throw.c`), cabeza oculta con visor (`vrcam_viewer_active`).
- 2026-11-01: procedencia internada, láser pistola, `vrbody_set_aim_fix`, granadas 2/7/8 con modelo del juego, agua en cine estéreo (`encodeStereo`).
- 2026-10-28 `fa3dc61`: procedencia (`runtime/prov.*`, `vr/scene/vr_prov*`), 3 capas 2D, FX nativos, manos WebXR soldadas al antebrazo (detalle: `git log -p bitacora.md`).

## Current state
**Pendiente de probar en visor (build de a8370ab):** todo lo de 2026-11-10 y anteriores.
**Unverified:** pistola: agarre `MAIN_YAW` -35°/-17° (cañón a 34,5° del eje de dedos; probar -15°); antebrazo en pose de escena;  zona VRAM del guante usada por el juego en otras fases; láser aún corta con geometría de render (balas no); `ns_hand.c:156` GT_OK no atómico; `vbl_*` una muestra de mano por pieza (40/escena, mutex HM) en vez de una por fotograma; SCOPE_BINO solo por scope.c; ventana de armas con drop_world2d sin volcado real;  parpadeo 1-2 fotogramas de fundido/gas (cobertura mira la última escena publicada); máscara HZD `VRHZD_SKIP_SIGHT`=4 (suposición); rango 16000 uds de balas enemigas (dificultad); normal de pared con nudillos <30 mm del suelo (`vrbodyg.c:110`); cilindro de reserva usa centro de caja como cintura (`vrbodyg.c:71`); borde dentado del parche del guante (11×8 texels); binoculares (verde/zoom), chaff activo, `vrhit_aim` (anotación de apunte, 120 VBlanks); latencia de locomoción +~55 ms; brazos/IK residuales a 25 Hz; carrera residual viewsHook vs acquire (`RenderRG.swift` ~888); realimentación renderer→host de overlays presentados (Swift).
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
