#!/usr/bin/env bash
# Compila el banco del renderer nativo para macOS (ejecutar desde cualquier carpeta). Salida: bench/build_native/{nbench,native.metallib,test_native_gpu,nref}.
# Mismos fuentes que la app: app/Sources/Native/NativeRenderer.swift + NativeShaders.metal + vr/native/{ns_bridge,ns_gpu}.c; la referencia (ns_cpuref.c) y la escena
# sintética (bench/c/native_synth.c) son solo del banco.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=bench/build_native
rm -rf "$OUT" && mkdir -p "$OUT"
# -Iruntime: ns_fx.h incluye "gpu.h" (runtime/), que ns_extract_glue.c arrastra; -Ivr/scene: cabeceras de la capa 2D
INC=(-Ivr/bridge -Iruntime -Ivr/native -Ivr/scene -Ibench/c)

echo "== shaders Metal =="
xcrun -sdk macosx metal -c -Ivr/native app/Sources/Native/NativeShaders.metal -o "$OUT/native.air"
xcrun -sdk macosx metallib "$OUT/native.air" -o "$OUT/native.metallib"

echo "== C =="
OBJ=()
for f in bench/c/native_synth.c bench/c/fill_stub.c vr/native/ns_bridge.c vr/native/ns_gpu.c vr/native/ns_screen.c vr/native/ns_present.c vr/native/ns_cinema.c vr/native/ns_cpuref.c; do
  xcrun clang -c -O2 -Wall -Wextra -std=gnu11 "${INC[@]}" "$f" -o "$OUT/c_$(basename "$f" .c).o"
  OBJ+=("$OUT/c_$(basename "$f" .c).o")
done

echo "== pruebas C (empaquetado, orden, puente) =="
xcrun clang -O2 -Wall -Wextra -std=gnu11 "${INC[@]}" bench/c/test_native_gpu.c "${OBJ[@]}" -lpthread -o "$OUT/test_native_gpu"

echo "== prueba de enlace del almacén de mallas (extractor real, sin sustitutos débiles) =="
LOBJ=()
for f in vr/native/ns_extract.c vr/native/ns_extract_glue.c vr/native/ns_fx.c vr/native/ns_fx_export.c vr/native/ns_hook.c vr/native/ns_bridge.c vr/native/ns_player.c; do
  xcrun clang -c -O2 -Wall -Wextra -std=gnu11 "${INC[@]}" "$f" -o "$OUT/l_$(basename "$f" .c).o"
  LOBJ+=("$OUT/l_$(basename "$f" .c).o")
done
xcrun clang -O2 -Wall -Wextra -std=gnu11 "${INC[@]}" bench/c/test_native_link.c "${LOBJ[@]}" -lpthread -o "$OUT/test_native_link"
if xcrun nm -m "$OUT/test_native_link" | grep -i "weak" | grep "ns_store_"; then
  echo "error: queda algún símbolo ns_store_* débil en el enlace del extractor"; exit 1
fi
for sym in ns_store_get ns_store_get_baked ns_store_epoch ns_store_ack ns_store_is_stub; do
  xcrun nm -g "$OUT/l_ns_extract.o" | grep -q " T _$sym\$" || { echo "error: el extractor no define $sym"; exit 1; }
done

echo "== comprobación de compilación de lo nuevo que el banco no enlaza (ns_solid, cobertura, capa 2D, manos) =="
for f in vr/native/ns_solid.c vr/native/ns_adapt.c vr/native/ns_light.c vr/native/ns_cover.c vr/native/ns_fxcmp.c vr/native/ns_cover_glue.c vr/native/ns_fill.c vr/native/ns_fill_run.c vr/native/ns_fill_glue.c vr/native/ns_hand.c vr/body/vr_hand_mesh.c vr/scene/vr_layer2d.c vr/scene/vr_layer2d_async.c; do
  xcrun clang -c -O1 -Wall -Wextra -std=gnu11 "${INC[@]}" -Ivr/body "$f" -o "$OUT/chk_$(basename "$f" .c).o"
done

echo "== Swift (NativeRenderer de la app + banco) =="
XCC=()
for i in "${INC[@]}"; do XCC+=(-Xcc "$i"); done
xcrun swiftc -O -swift-version 5 -import-objc-header bench/NativeBridge.h "${XCC[@]}" \
  app/Sources/XRMath.swift app/Sources/Native/NativeRenderer.swift bench/native/main.swift "${OBJ[@]}" -o "$OUT/nbench"
echo "OK: $OUT/nbench"
