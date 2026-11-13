# Bitácora — MVision

Runtime nativo para Apple Vision Pro: el juego (recompilación estática del EXE + overlays, intérprete de respaldo) en VR 6DOF en primera persona, con control corporal (cabeza + manos) y mandos.
Convención: repo neutro; repos externos solo vía variables de Actions (`DECOMP_REPO`, `DECOMP_COMMIT`, `SDK_REPO`); zonas por número. Histórico: `git log -p bitacora.md`.

## Last done
- 2026-11-13 (10ª tanda, tras Archivo_33; commit con este texto; ciclo de revisión adversarial cerrado LIMPIO; **Swift/Metal sin compilar hasta el CI de este commit; nada verificado en visor**):
  - Manos por arma (poses aprobadas por el usuario sobre renders, NO cambiarlas sin pedirlo): `vr/hands/vr_hand_grips.inc` (poses HP_W_*), `vr_grip_fit.inc` (zócalos FIT_*_GRIP/SUP/TRIG/TURN), `vr_grip_obj.inc` (desplazamiento objeto-mano de granadas 2/7/8). Generador `vr/hands/gripfit_gen.c` (herramienta; incluye `rast.h` de /tmp/w59, que NO está en el repo → hoy no se puede regenerar; `stage.sh` lo excluye). Datos de trabajo fuera del repo: /tmp/w66/res_*.txt.
  - **Contratos cambiados:** `VRHS_FIT_K` 1,27→1,0 (mano a escala real, 0,19 m muñeca-punta); `W_PIST_K` = 1,104 (SOCOM 0,202 m sin supresor; gatillo `PIST_TRIGGER` CLIP(0;25,7;−124,3;12;8;6) marcado por el usuario); gatillo FAMAS CLIP(0,32,−117,19,16,7); `test_gripfit`: penetración/contacto/yema son AVISOS (orden del usuario: prima la pose pedida), solo falla lo estructural; FAMAS con hueco de guardamanos y límite propio; `WP_SUPPORT` ya no es la posición de la mano (usar el zócalo / `vrgrip_weapon_from_hand`).
  - Giro de mano respecto al mando sin girar el arma: `vrbody_set_hand_turn(side,D)` (`vr_body.c`), `VrGripEntry.turn`, se fija ANTES de la toma de manos en `vrgame.c`; `mode = armed && gp.valid && wp.valid`; reset en `vrgame_reset`. Soldadura de la mano de apoyo se suelta con `duct` (`welded && !duct`).
  - Índice siempre doblado sobre el gatillo en SOCOM/FAMAS/PSG1/Nikita/Stinger (pull al disparar +12/+18/+10° igual que SOCOM). Pendiente: en PSG1/Nikita/Stinger la pieza animada del gatillo (wcut) NO es el «palito» donde apoya el dedo.
  - Granadas: chaff y stun usan `can_gren` 0x7a64 (`vrgame.c` tabla de modelos); `ns_wphide_carry_off` coloca el bote según `vrgrip_object_offset`; el agarre apoya yemas, no rodea el bote del todo.
  - PSG1: retirado el sustituto «de pie»; Snake se tumba como el original (ojo ~0,31 m). IK de brazos con cuerpo oculto tumbado/agachado (`vr_body.c` offscreen excluye BOX/DOWNED/GAME_OVER/NOT_PLAYABLE; HIDDEN sí lleva IK). `duct = INTRUDE || (gid!=9 && (stance==2||prone)) || view_hide`. Manos pueden quedar bajo el suelo tumbado (sin tope, decisión pendiente).
  - Resto de la tanda: recarga automática (sistema manual borrado, `vr_reload` espejo de `GM_Magazine`); sin disparo al acercar la mano a la zona del menú (`vrg_fire_gate`); láser = bala; SOCOM con silenciador sin ruido; C4 donde apuntas, detonación con grip derecho solo con C4 equipado; prismáticos sin espera de `scope.c`; sin puñetazos con arma (`vr_nopunch.h`, ACTATTACK punch=0x88); MMPX ×4 en 2D y texturas 3D (`ns_mmpx`, desde `vr/scene/mmpx.c` MIT); onda del agua de la intro (`NsScreenFx` 188 B `fb_wave`, `VrPostUniforms.fb_wv`); menús L2/R2 misma profundidad; R3 mantenido 3 s abre Diagnóstico (Select ya no); pantallas LED de Stinger/Nikita (`MV_VIEW_AA_SCREEN`=4, `MvVrSpecialView` poly/npoly/occlude/frame/standby, `SpcDrawU` 192 B); FAMAS 750 rpm; lente poligonal PSG1.
  - Infra: `app/stage.sh` copia `*.inc`; `project.yml` excluye `**/*.inc`; dependencias `.inc` en Makefiles (`GRIPINC`).
- 2026-11-12 (9ª tanda, commit f2f4a78): caras cercanas «Original/Recortadas» (parche I7 `vr_camera.c:650`), estéreo de la pantalla 3D por `ns_solid_view_near`, cielo `sphere.c`, escenario de todos los grupos, 2D en 3 planos premultiplicados (contrato `mv_vrhost_hud2d_acquire*` alto 3×), lente PSG1, truco de armas. Giro L3 180° (d0bc837). Detalle: `git log -p bitacora.md`.
- 2026-11-12 y anteriores (tandas 1-8): ver `git log -p bitacora.md`.

## Current state
**Funciona (tests, 2026-11-13):** `vr/hands`, `extract.mk test test-hand`, `vr/native`, `vr/body`, `vr/weapons`, `vr/camera`, `vr/gameplay`, `vr/input`, `vr/host test check`, `fx.mk test` a 0 fallos. `test_gripfit` necesita `/tmp/gamedata/STAGE.DIR` (sin datos se omite).
**Pendiente de probar en visor:** todo lo de la 10ª y 9ª tanda.
**Si falla la build de la 10ª tanda mirar:** `XRPresent.swift` (R3 3 s, menú 32 filas `menuH` 1460, `.tex3D`), `XRPixelScale.swift` (MMPX ×4), `RenderRG.swift` (destino ampliado, `takeAuxLayer`), `RenderWeapon.swift` (`SpcDrawU` 192 B, `lensDepth`), `NativeShaders.metal` (`ns_mmpx`), `vr_weapon.metal` (poly, marco LED, standby), `vr_scene.metal` (onda), `XRFrameEncoder.swift` (`binocularWorldZoom`), `VRInput.swift` (`weaponReload` borrado), `NativeRenderer.swift` (`tex3DMMPX`), `bench/native/main.swift`; staging de `vr/hands/*.inc`.
**Unverified (10ª):** supuesto de pieza raíz única en granadas (medido en 0x3b88/0x7a64); `REFLECT_LOCK_VBL`=40 estimado; láser solo ve puertas del mapa del jugador; FOV cámara misil 1,05 rad; texto «LISTO» Nikita; mina con colocación antigua; ojo tumbado PSG1 de un solo volcado (a33).
**Decidido:** «lista como verdad» no se hace; relleno `ns_fill` retirado; revisiones adversariales en ciclo hasta LIMPIA; código de DuckStation prohibido (licencia), MMPX solo desde `vr/scene/mmpx.c`.
**Sin causa encontrada:** d0010 (sepia), d0011 (superficie clara), distorsión al morir (d0018).
**Pendiente técnico:** `WReloadMode`/`reload_mode` sin efecto; `G.det_t`/`WL_DETONATE` muertos; `test_geom` 2 fallos previos; `vrhost.c` sin test enlazado (solo `vrhost_run`); `test_gripfit.c:149-158` (offset granadas) es circular; `opt_right` leído sin cerrojo en `vrgame.c` para `vrbodyg_pad`.
**Código muerto (no borrado):** `hudMenuUV`, `opt_aim`, `grip_to_wrist_mm`/`wrist_swing_deg`/`hand_twist_deg`, `WL_INHAND`/`WL_DETONATE`/`G.dets`, `vrbody_set_armed`, `l2d_target`/`smooth2D`, `gpuMode`, `ns_fill*`/`ns_cover*`, `eye_shift`, `vrhzd_wall_near`; en `vr_grip.c` reservas `MAIN_YAW`, `MAIN_PIST`, `SUP_*`, `TRIG`, `TRIG_PIST`.

## Decisions
- Mundo = renderer nativo desde la RAM del juego; nunca reconstrucción desde la lista GPU ni respaldo plano.
- Lista GPU solo para 2D, clasificada por PROCEDENCIA, nunca por forma/posición/mezcla.
- Cuerpo del juego completo + mano esquelética WebXR (MIT, nuestro modelo) soldada al antebrazo; arma del juego oculta en nativo. Nunca ocultar torso para arreglar la vista.
- Poses de mano por arma: las define el usuario (fotos/dibujos sobre renders); cambiar solo lo que pide, nada más.
- Arma = orientación del mando; la malla de la mano puede girar respecto al mando (`FIT_*_TURN`). Mano izquierda sin tracking con arma de dos manos (soldada), salvo con `duct`.
- Orden de profundidad: correcciones por pieza con causa medida, nunca sesgo global.
- Acciones sin primera persona posible: vista del juego con SU cámara en pantalla 3D; nunca 3ª persona propia.
- PSG1: Snake se tumba como el original, sin levitación; quieto mientras apunta.
- Efectos originales: se reproduce lo que hace el juego, medido; nada inventado.
- Ninguna build sin revisión cerrada; una build por bloque.

## Files in play
- `vr/hands/` (`vr_grip.{c,h}`, `vr_hand_pose.{c,h}`, `*.inc`, `gripfit*`, `test_gripfit.c`); `vr/body/vr_body.{c,h}`, `vr_hand_skin.{c,h}`; `vr/weapons/weapon.h`, `wcut*`, `w_pist.c`; `vr/native/ns_wphide.{c,h}`, `ns_hand.c`; `vr/host/vrgame.{c,h}`, `vrhost.c`, `test_vrgame_fire.c`; `vr/gameplay/vr_reload.*`, `vr_fire*`.
- `app/Sources/XRPresent.swift`, `XRPixelScale.swift`, `RenderRG.swift`, `RenderWeapon.swift`; `NativeShaders.metal`; `render/metal/vr_weapon.metal`, `vr_scene.metal`, `vr_types.h`; `app/stage.sh`, `app/project.yml`.

## Comandos
- Runtime: `cd runtime && make gen GEN=… EXE=../gamedata/main.exe NAMES=../gamedata/names_eu.json && make GEN=… OUT=… CC=clang && make test test_sys`.
- Pruebas VR: `make -C vr/{host,camera,scene,input,body,hands,radar,weapons,gameplay} test`; `make -C vr/host check`; `make -f vr/native/extract.mk test test-hand …`; `make -C vr/native test`; `make -f vr/native/fx.mk test`.
- Banco: `make -f vr/native/extract.mk test-dumps DUMPS=<dir>`; `nshito1`, `nscover`, `dumpview`, `l2d-dump` (ver `git log -p bitacora.md`).
- Swift/Metal: solo CI. IPA: `gh workflow run visionos.yml --ref main -f solo_render=false`.
- Datos locales (nunca al repo): `gamedata/`, `/tmp/gamedata`, volcados.

## Module index
- (sin notas/ todavía)
