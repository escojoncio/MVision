// Comprueba el layout de RadarQuadU (MSL) frente al de app/Sources/RadarPanel.swift: 5 x SIMD4<Float> = 80 B, alineación 16.
#include <metal_stdlib>
#include <cstddef>
#include "vr_radar.metal"
static_assert(sizeof(RadarQuadU) == 80, "RadarQuadU debe medir 80 bytes (5 x float4)");
static_assert(offsetof(RadarQuadU, center) == 0 && offsetof(RadarQuadU, right) == 16 && offsetof(RadarQuadU, up) == 32 &&
              offsetof(RadarQuadU, params) == 48 && offsetof(RadarQuadU, bezel_color) == 64, "orden de campos de RadarQuadU");
