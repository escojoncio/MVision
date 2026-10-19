#!/usr/bin/env bash
# Prepara app/.stage/ con los fuentes que compila el target (todo local, fuera del repo; .stage/ está ignorado):
#   .stage/runtime  runtime/*.c y *.h (sin headless ni pruebas; nogen.c entra: el runtime corre sin código generado)
#   .stage/vr/<x>   vr/{bridge,camera,input,scene,host,body,radar,hands,weapons,gameplay,ai}: solo el código de biblioteca (sin pruebas ni herramientas con main)
#   .stage/metal    render/metal/vr_*.metal y vr_types.h (rg_gpu.metal es del renderer de referencia: no entra)
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
    case "$(basename "$f")" in test_*|*_dump.c|camtest.c|vrhost_run.c|bodyrun.c|eyeshot.c|eyeshot.h|padrun.c|bodytest.c|radar_run.c|gptest*|hands_preview.c|hud_probe.c|hud_emu.c|hud_emu.h|cinerun.c|hle_vrfire_weak.c|ai_validate.c|vai_local_patch.c|vr_activity_run.c|vr_activity_ai2_hooks.c) continue ;; esac
    cp "$f" ".stage/vr/$d/"
  done
done

cp ../render/metal/vr_*.metal ../render/metal/vr_types.h .stage/metal/
cp "$SIGS_HLE" .stage/res/sigs_hle.json

if [ -n "${EXTRAS_DIR:-}" ]; then
  # Ficheros opcionales del repo de extras: los controladores de mando (patrón *Controller.swift) y los auxiliares fijos.
  for p in "$EXTRAS_DIR"/platform/visionos/Sources/*Controller.swift \
           "$EXTRAS_DIR/platform/visionos/Sources/LogFiles.swift" "$EXTRAS_DIR/platform/visionos/Sources/Language.swift"; do
    [ -f "$p" ] && cp "$p" ".stage/extras/$(basename "$p")" && echo "extra: $(basename "$p")"
  done
fi
echo "stage: $(ls .stage/runtime/*.c | wc -l) .c de runtime, $(ls .stage/vr/*/*.c | wc -l) .c de vr, $(ls .stage/metal/*.metal | wc -l) .metal"
