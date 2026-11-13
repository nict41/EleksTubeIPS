#ifndef JPEG_PANEL_H
#define JPEG_PANEL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Draws a JPEG that's already in memory onto a panel tube, for POST /panel?format=jpeg. It uses the
 * JPEG decoder in the ESP32's ROM (TJpgDec), so it needs no library and only a 3100-byte work area,
 * and draws each decoded block as it comes, so the picture is never held decoded in memory.
 *
 * Returns NULL on success, else why it failed. The JPEG must be exactly w x h pixels.
 */
const char *jpegDrawPanel(const uint8_t *jpeg, size_t len, uint8_t tube, int16_t x, int16_t y, int16_t w, int16_t h);

#endif // JPEG_PANEL_H
