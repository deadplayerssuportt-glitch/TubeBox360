# Componentes de terceros

| Componente | Dónde | Licencia |
|---|---|---|
| **mbedTLS 2.28.8** (Arm / Trusted Firmware) | `mbedtls360/` (con parches C89, ver `PARCHES.txt`) | Apache License 2.0 (`mbedtls360/LICENSE`) |
| **FFmpeg** (decodificador AAC) | vía *ffmpeg360* de HG-ERIK, **no incluido** | LGPL 2.1 o posterior (ver el proyecto original) |
| **Roboto** (letra de la interfaz, como atlas en `ui_assets.c`) | `TubeBox360/ui_assets.c`, `font_data.h` | Apache License 2.0 |
| **Noto Sans CJK JP** (japonés) | `TubeBox360/font_extra.c` | SIL Open Font License 1.1 |
| **DejaVu Sans** (árabe) | `TubeBox360/font_extra.c` | Licencia libre Bitstream Vera / DejaVu |
| **Google Trust Services Root R1, R3, R4** (certificados raíz públicos) | `TubeBox360/tls_roots.c` | Certificados públicos de Google Trust Services |

## Marcas

- *YouTube* es una marca de Google LLC. *Xbox* y *Xbox 360* (y su logotipo, que aparece en el contador de usuarios activos) son marcas de Microsoft Corporation. Se mencionan solo para describir con qué funciona el proyecto.
- El logotipo de **DeadPIayers** pertenece a su comunidad.

## No incluido

- **XDK de Xbox 360** (cabeceras y librerías de Microsoft, incluida `xavatar2`): necesitas tu propia copia.
- **ffmpeg360** de HG-ERIK: descárgalo de su repositorio.
