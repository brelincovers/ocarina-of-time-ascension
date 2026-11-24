#pragma once
#include <stddef.h>
#include <stdint.h>

/* CPU-only capture for a VERIFIED BT.2020/ST2084 (PQ) terminal image.
 * Metal format 90 = RGB10A2Unorm, 94 = BGR10A2Unorm. This function does not
 * infer color semantics from a pixel format and performs NO tone mapping.
 * The caller owns bytes and guarantees row_bytes * height readable bytes.
 * Writes: png_path (tagged 16-bit RGB PQ), png_path.raw (original packed bytes,
 * including row padding), png_path.json (metadata/completion marker).
 * flip_vertical affects only the PNG; raw bytes retain source orientation.
 * Existing outputs are refused. Returns 1 only when all three files exist.
 */
int dm_hdr_capture_write(const void *bytes, size_t width, size_t height,
                         size_t row_bytes, unsigned metal_format,
                         int flip_vertical, const char *png_path,
                         char *error, size_t error_capacity);
