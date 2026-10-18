#!/bin/sh
# Compila render/capture_test enlazando el runtime como runtime/Makefile (objetivo headless), a mano.
# Uso: render/build.sh   -> build-render/capture_test  (GEN y GENOV como en el Makefile)
set -e
cd "$(dirname "$0")/../runtime"
GEN=${GEN:-../build/gen}; GENOV=${GENOV:-../build/gen_ov}; OUT=${OUT:-../build-render}
CC=${CC:-clang}
mkdir -p "$OUT/gen"
# objetos del C generado (una vez; se reutilizan)
for f in "$GEN"/*.c; do
  o="$OUT/gen/$(basename "$f" .c).o"
  [ "$o" -nt "$f" ] || $CC -I. -I"$GEN" -O2 -w -foptimize-sibling-calls -c -o "$o" "$f"
done
RT=$(ls *.c | grep -v '^headless.c$' | grep -v '^test_' | grep -v '^wip_' | grep -v '^nogen.c$')
$CC -I. -I"$GEN" -I../render -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
  -o "$OUT/capture_test" $RT ../render/capture_test.c ../render/ref_raster.c "$OUT"/gen/*.o $(ls "$GENOV"/*.o 2>/dev/null) -lm
# prueba aleatoria del modelo de referencia contra el oráculo (no necesita disco)
$CC -I. -I"$GEN" -I../render -O2 -g -Wall -Wno-unused-label -Wno-unused-but-set-variable -foptimize-sibling-calls \
  -o "$OUT/ref_selftest" $RT ../render/ref_selftest.c ../render/ref_raster.c "$OUT"/gen/*.o $(ls "$GENOV"/*.o 2>/dev/null) -lm
