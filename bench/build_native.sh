#!/usr/bin/env bash
# Compila el banco del renderer nativo para macOS (ejecutar desde cualquier carpeta). Salida: bench/build_native/{nbench,native.metallib,test_native_gpu,nref}.
# Mismos fuentes que la app: app/Sources/Native/NativeRenderer.swift + NativeShaders.metal + vr/native/{ns_bridge,ns_gpu}.c; la referencia (ns_cpuref.c) y la escena
# sintética (bench/c/native_synth.c) son solo del banco.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=bench/build_native
rm -rf "$OUT" && mkdir -p "$OUT"
INC=(-Ivr/native -Ibench/c)

echo "== shaders Metal =="
xcrun -sdk macosx metal -c -Ivr/native app/Sources/Native/NativeShaders.metal -o "$OUT/native.air"
xcrun -sdk macosx metallib "$OUT/native.air" -o "$OUT/native.metallib"

echo "== C =="
OBJ=()
for f in bench/c/native_synth.c vr/native/ns_bridge.c vr/native/ns_gpu.c vr/native/ns_cpuref.c; do
  xcrun clang -c -O2 -Wall -Wextra -std=gnu11 "${INC[@]}" "$f" -o "$OUT/c_$(basename "$f" .c).o"
  OBJ+=("$OUT/c_$(basename "$f" .c).o")
done

echo "== pruebas C (empaquetado, orden, puente) =="
xcrun clang -O2 -Wall -Wextra -std=gnu11 "${INC[@]}" bench/c/test_native_gpu.c "${OBJ[@]}" -lpthread -o "$OUT/test_native_gpu"

echo "== Swift (NativeRenderer de la app + banco) =="
XCC=()
for i in "${INC[@]}"; do XCC+=(-Xcc "$i"); done
xcrun swiftc -O -swift-version 5 -import-objc-header bench/NativeBridge.h "${XCC[@]}" \
  app/Sources/Native/NativeRenderer.swift bench/native/main.swift "${OBJ[@]}" -o "$OUT/nbench"
echo "OK: $OUT/nbench"
