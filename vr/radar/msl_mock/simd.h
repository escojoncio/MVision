#pragma once
#include <metal_stdlib>
typedef metal::float4 vector_float4;
struct matrix_float4x4 { vector_float4 columns[4]; };
inline vector_float4 operator*(const matrix_float4x4&, const vector_float4& v){ return v; }

namespace metal { using float4x4 = ::matrix_float4x4; }
