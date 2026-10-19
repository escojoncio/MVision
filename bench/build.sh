#!/usr/bin/env bash
# Compila el banco de render para macOS (ejecutar desde cualquier carpeta). Salida: bench/build/{bench,default.metallib}.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=bench/build
rm -rf "$OUT" && mkdir -p "$OUT"
INC=(-Iruntime -Ivr/scene -Ivr/bridge -Ivr/camera -Ivr/input -Ivr/host -Ivr/body -Ivr/radar -Ivr/hands -Ivr/weapons -Ivr/gameplay -Ivr/ai -Irender/metal -Iapp/Sources -Ibench)

echo "== shaders Metal =="
for f in render/metal/vr_scene.metal render/metal/vr_vram.metal render/metal/vr_radar.metal render/metal/vr_weapon.metal app/Sources/XRShaders.metal; do
  xcrun -sdk macosx metal -c -Irender/metal "$f" -o "$OUT/$(basename "$f" .metal).air"
done
xcrun -sdk macosx metallib "$OUT"/*.air -o "$OUT/default.metallib"

echo "== C sintetico =="
OBJ=()
for f in bench/c/bench_host.c vr/scene/vr_scene.c vr/radar/vr_radar.c vr/gameplay/vr_scope.c; do
  xcrun clang -c -O1 "${INC[@]}" "$f" -o "$OUT/c_$(basename "$f" .c).o"
  OBJ+=("$OUT/c_$(basename "$f" .c).o")
done

echo "== Swift (mismos ficheros del renderer que la app) =="
XCC=()
for i in "${INC[@]}"; do XCC+=(-Xcc "$i"); done
SRC=()
for f in RenderRG RenderWeapon RadarPanel XRHud XRPresent XRMath XRFrameEncoder; do SRC+=("app/Sources/$f.swift"); done
xcrun swiftc -O -swift-version 5 -DMV_BENCH -import-objc-header bench/BenchBridge.h "${XCC[@]}" \
  "${SRC[@]}" bench/Sources/*.swift "${OBJ[@]}" -o "$OUT/bench"
echo "OK: $OUT/bench"
