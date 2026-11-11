#!/usr/bin/env bash
# Compila el banco de render para macOS (ejecutar desde cualquier carpeta). Salida: bench/build/{bench,default.metallib}.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=bench/build
rm -rf "$OUT" && mkdir -p "$OUT"
INC=(-Iruntime -Ivr/scene -Ivr/bridge -Ivr/camera -Ivr/input -Ivr/host -Ivr/body -Ivr/radar -Ivr/hands -Ivr/weapons -Ivr/gameplay -Ivr/ai -Ivr/native -Irender/metal -Iapp/Sources -Ibench)

echo "== shaders Metal =="
for f in render/metal/vr_scene.metal render/metal/vr_vram.metal render/metal/vr_radar.metal render/metal/vr_weapon.metal app/Sources/XRShaders.metal app/Sources/Native/NativeShaders.metal; do
  xcrun -sdk macosx metal -c -Irender/metal -Ivr/native "$f" -o "$OUT/$(basename "$f" .metal).air"
done
xcrun -sdk macosx metallib "$OUT"/*.air -o "$OUT/default.metallib"

echo "== C sintetico =="
OBJ=()
for f in bench/c/bench_host.c bench/c/native_synth.c bench/c/fill_stub.c vr/native/ns_bridge.c vr/native/ns_gpu.c vr/native/ns_screen.c vr/native/ns_present.c vr/native/ns_cinema.c vr/native/ns_cpuref.c vr/scene/vr_scene.c vr/scene/vr_split.c vr/radar/vr_radar.c vr/gameplay/vr_scope.c vr/host/vr_smooth.c; do
  xcrun clang -c -O1 "${INC[@]}" "$f" -o "$OUT/c_$(basename "$f" .c).o"
  OBJ+=("$OUT/c_$(basename "$f" .c).o")
done

echo "== comprobación de compilación de lo nuevo que el banco no enlaza (efectos, sólido, capa 2D, manos) =="
for f in vr/native/ns_fx.c vr/native/ns_fx_export.c vr/native/ns_extract_glue.c vr/native/ns_sink.c vr/native/ns_fill.c vr/native/ns_fill_run.c vr/native/ns_fill_glue.c vr/native/ns_solid.c vr/native/ns_hand.c vr/body/vr_hand_skin.c vr/body/vr_hand_asset.c vr/scene/vr_layer2d.c vr/scene/vr_layer2d_async.c; do
  xcrun clang -c -O1 "${INC[@]}" "$f" -o "$OUT/chk_$(basename "$f" .c).o"
done

echo "== Swift (mismos ficheros del renderer que la app) =="
XCC=()
for i in "${INC[@]}"; do XCC+=(-Xcc "$i"); done
SRC=()
for f in RenderRG RenderWeapon RadarPanel XRHud XRPresent XRMath XRPixelScale XRFrameEncoder XRDiag XRDebugAxes Native/NativeRenderer; do SRC+=("app/Sources/$f.swift"); done
xcrun swiftc -O -swift-version 5 -DMV_BENCH -DMV_BENCH_NATIVE -import-objc-header bench/BenchBridge.h "${XCC[@]}" \
  "${SRC[@]}" bench/Sources/*.swift "${OBJ[@]}" -o "$OUT/bench"
echo "OK: $OUT/bench"
