/* Kernels de VRAM original (R16Uint 1024x512) del renderer VR: relleno, subida y copia. SIN COMPILAR (ver vr_scene.metal).
 * Se ejecutan en orden de la lista de captura, en la misma cola que los pases de render. */
#include <metal_stdlib>
#include "vr_types.h"
using namespace metal;

/* Fill: ignora máscara y recorte (como el hardware). */
kernel void vr_k_fill(texture2d<ushort, access::write> vram [[texture(0)]], constant VrVramEvent &ev [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(ev.w) || gid.y >= uint(ev.h)) return;
    vram.write(ushort4(ushort(ev.fill15), 0, 0, 0), uint2((uint(ev.x) + gid.x) & 1023u, (uint(ev.y) + gid.y) & 511u));
}

kernel void vr_k_upload(texture2d<ushort, access::read_write> vram [[texture(0)]], constant VrVramEvent &ev [[buffer(0)]],
                        const device ushort *data [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(ev.w) || gid.y >= uint(ev.h)) return;
    uint idx = gid.y * uint(ev.w) + gid.x;
    if (idx >= ev.data_n) return; /* subida abortada: solo lo recibido */
    uint2 q = uint2((uint(ev.x) + gid.x) & 1023u, (uint(ev.y) + gid.y) & 511u);
    if ((ev.flags & VR_MASK_CHK) && (vram.read(q).x & 0x8000u)) return;
    uint v = uint(data[ev.data_off + idx]) | ((ev.flags & VR_MASK_SET) ? 0x8000u : 0u);
    vram.write(ushort4(ushort(v), 0, 0, 0), q);
}

/* Copia VRAM->VRAM FILA A FILA como el oráculo (gpu.c cmd_copy): se lee la fila origen entera, y después se escribe
 * (con la regla de máscara por píxel) antes de pasar a la siguiente fila; así las copias solapadas dan lo mismo.
 * Un único threadgroup (lo lanza el host con 256 hilos; w <= 1024 => hasta 4 píxeles por hilo; reserva 8). */
kernel void vr_k_copy(texture2d<ushort, access::read_write> vram [[texture(0)]], constant VrVramEvent &ev [[buffer(0)]],
                      uint tid [[thread_index_in_threadgroup]], uint tgs [[threads_per_threadgroup]]) {
    ushort line[8];
    for (int j = 0; j < ev.h; j++) {
        uint sy = uint(ev.sy + j) & 511u, dy = uint(ev.y + j) & 511u;
        for (uint k = 0; k < 8; k++) {
            uint i = tid + k * tgs;
            if (i < uint(ev.w)) line[k] = vram.read(uint2((uint(ev.sx) + i) & 1023u, sy)).x;
        }
        threadgroup_barrier(mem_flags::mem_texture);
        for (uint k = 0; k < 8; k++) {
            uint i = tid + k * tgs;
            if (i >= uint(ev.w)) continue;
            uint2 q = uint2((uint(ev.x) + i) & 1023u, dy);
            if ((ev.flags & VR_MASK_CHK) && (vram.read(q).x & 0x8000u)) continue;
            uint v = uint(line[k]) | ((ev.flags & VR_MASK_SET) ? 0x8000u : 0u);
            vram.write(ushort4(ushort(v), 0, 0, 0), q);
        }
        threadgroup_barrier(mem_flags::mem_texture);
    }
}
