# MVision

Runtime y herramientas experimentales para Apple Vision Pro.

## Estructura

- `runtime/` — núcleo en C y pruebas.
- `vr/` — módulos de entrada, cámara, escena y render inmersivo.
- `render/` — shaders y código de render.
- `app/` — aplicación para visionOS.
- `tools/` — utilidades de análisis y conversión.
- `.github/workflows/` — CI (disparo manual o `[build]` en el mensaje de commit).

## Pruebas

```
cd runtime && make test test_sys
```
