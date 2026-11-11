#!/usr/bin/env bash
# Prepara app/.stage/ con los fuentes que compila el target (todo local, fuera del repo; .stage/ está ignorado):
#   .stage/runtime  runtime/*.c y *.h (sin headless ni pruebas; nogen.c entra: el runtime corre sin código generado)
#   .stage/vr/<x>   vr/{bridge,camera,input,scene,host,body,radar,hands,weapons,gameplay,ai}: solo el código de biblioteca (sin pruebas ni herramientas con main)
#   (vr/tools/ -el probador local vrprobe, que usa el disco real- NO se copia: la lista de directorios de arriba es cerrada)
#   .stage/metal    render/metal/vr_*.metal, vr_types.h y vr_blend.h (rg_gpu.metal es del renderer de referencia: no entra)
#   .stage/res      sigs_hle.json (firmas del SDK enmascaradas, sin código del juego)
#   .stage/extras   ficheros Swift opcionales (EXTRAS_DIR)
# No se copia código generado ni el ejecutable del juego: el EXE sale del disco del usuario en tiempo de ejecución.
# Variables: SIGS_HLE (por defecto ../sigs_hle.json; obligatoria), EXTRAS_DIR.
set -euo pipefail
cd "$(dirname "$0")"
SIGS_HLE="${SIGS_HLE:-../sigs_hle.json}"

[ -f "$SIGS_HLE" ] || { echo "Falta el fichero de firmas ($SIGS_HLE). Se genera con tools/sig/hlesigs.py (ver CI: artefacto sigs-hle)."; exit 1; }
[ -f ../runtime/nogen.c ] || { echo "Falta runtime/nogen.c"; exit 1; }

rm -rf .stage
mkdir -p .stage/runtime .stage/vr .stage/metal .stage/extras .stage/res

for f in ../runtime/*.c ../runtime/*.h; do
  case "$(basename "$f")" in headless.c|test_*|wip_*) continue ;; esac
  cp "$f" .stage/runtime/
done

# Excluidos: pruebas (test_*), herramientas con main (camtest, vrhost_run, bodytest, radar_run, gptest*, hands_preview, hud_probe/hud_emu)
# más hle_vrfire_weak.c (ya está en runtime/). vrbody_weak.c entra: los símbolos fuertes de vr/body lo sustituyen al enlazar.
for d in bridge camera input scene host body radar hands weapons gameplay ai; do
  mkdir -p ".stage/vr/$d"
  for f in ../vr/$d/*.c ../vr/$d/*.h; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in test_*|*_dump.c|camtest.c|vrhost_run.c|bodyrun.c|eyeshot.c|eyeshot.h|padrun.c|fuzzrun.c|bodytest.c|radar_run.c|gptest*|hands_preview.c|hud_probe.c|hud_emu.c|hud_emu.h|cinerun.c|smoothrun.c|smooth_replay.c|hle_vrfire_weak.c|ai_validate.c|vai_local_patch.c|vr_activity_run.c|vr_activity_ai2_hooks.c) continue ;; esac
    cp "$f" ".stage/vr/$d/"
  done
done

# vr/native: el escritor de volcados, los tipos del contrato y el puente/empaquetado del renderer nativo (el cargador ns_dump.c, el rasterizador de referencia y las herramientas son de escritorio)
mkdir -p .stage/vr/native
cp ../vr/native/ns_types.h ../vr/native/ns_dump.h ../vr/native/ns_dumpw.c .stage/vr/native/
# puente productor/consumidor, jugador y enganche de la captura (integración del renderer nativo)
cp ../vr/native/ns_bridge.[ch] ../vr/native/ns_player.[ch] ../vr/native/ns_wphide.[ch] ../vr/native/ns_hook.[ch] ../vr/native/ns_hook_rt.[ch] .stage/vr/native/
# renderer Metal nativo: empaquetado para la GPU y extractor real con su almacén de mallas (ns_store_*: sin definiciones débiles) y su registro en el enganche
# (sin test_*, sin tools/, sin ns_cpuref.c: validador de escritorio; ns_adapt/ns_light/ns_cover/ns_cover_glue = comprobación de cobertura en tiempo real, ver ns_cover.h; ns_solid = geometría sólida 360 grados para la colisión de la cabeza)
cp ../vr/native/ns_gpu.[ch] ../vr/native/ns_gpu_types.h ../vr/native/ns_extract.[ch] ../vr/native/ns_extract_glue.c ../vr/native/ns_sink.[ch] .stage/vr/native/
cp ../vr/native/ns_screen.[ch] ../vr/native/ns_present.[ch] ../vr/native/ns_cinema.[ch] .stage/vr/native/   # efectos de pantalla nativos, presentación (repetir último válido) y cinemáticas con el render nativo
cp ../vr/native/ns_fx.[ch] ../vr/native/ns_fxcmp.[ch] ../vr/native/ns_fx_export.[ch] .stage/vr/native/   # efectos del renderer nativo (extractor, clasificación de la lista, exportación al contrato)
cp ../vr/native/ns_adapt.[ch] ../vr/native/ns_light.[ch] ../vr/native/ns_cpuref.h ../vr/native/ns_cover.[ch] ../vr/native/ns_cover_glue.c ../vr/native/ns_solid.[ch] .stage/vr/native/
cp ../vr/native/ns_fill.[ch] ../vr/native/ns_fill_run.c ../vr/native/ns_fill_glue.[ch] .stage/vr/native/   # relleno por la verdad del motor (ver ns_fill.h)
cp ../vr/native/ns_hand.[ch] .stage/vr/native/   # manos articuladas propias en la escena nativa (usa vr/body/vr_hand_skin.[ch] y vr_hand_asset.[ch], que ya entran con vr/body)

cp ../render/metal/vr_*.metal ../render/metal/vr_types.h ../render/metal/vr_blend.h .stage/metal/
cp ../vr/native/ns_gpu_types.h .stage/metal/   # lo incluye app/Sources/Native/NativeShaders.metal
cp "$SIGS_HLE" .stage/res/sigs_hle.json

if [ -n "${EXTRAS_DIR:-}" ]; then
  # Ficheros opcionales del repo de extras: los controladores de mando (patrón *Controller.swift) y los auxiliares fijos.
  for p in "$EXTRAS_DIR"/platform/visionos/Sources/*Controller.swift \
           "$EXTRAS_DIR/platform/visionos/Sources/LogFiles.swift" "$EXTRAS_DIR/platform/visionos/Sources/Language.swift"; do
    [ -f "$p" ] && cp "$p" ".stage/extras/$(basename "$p")" && echo "extra: $(basename "$p")"
  done
fi
echo "stage: $(ls .stage/runtime/*.c | wc -l) .c de runtime, $(ls .stage/vr/*/*.c | wc -l) .c de vr, $(ls .stage/metal/*.metal | wc -l) .metal"
