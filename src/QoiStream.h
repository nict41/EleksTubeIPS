#ifndef QOI_STREAM_H
#define QOI_STREAM_H

#include <stddef.h>
#include <stdint.h>

/*
 * A streaming QOI decoder (https://qoiformat.org/qoi-specification.pdf), for POST /panel?format=qoi.
 * The body arrives in chunks of any size, even one byte, so the decoder keeps its state between
 * calls and emits a pixel only when all the bytes of its op have arrived. No memory is allocated:
 * the 64-entry index and the partial op live in the object, which the caller keeps in static storage.
 *
 * Pixels are handed to a callback as RGB888; the caller converts them to RGB565 big-endian and fills
 * its row buffer, see panelBodyHandler() in main.cpp. Alpha is decoded but ignored, as the displays
 * have no alpha.
 */
class QoiStream {
public:
	// What feed() did with the bytes it was given
	enum Result {
		OK,		// Consumed them, more may follow
		DONE,	// All w*h pixels were emitted; the 8-byte end marker may follow and is ignored
		ERROR	// Malformed, see error()
	};

	// Called for every decoded pixel, in order
	typedef void (*PixelFn)(void *ctx, uint8_t r, uint8_t g, uint8_t b);

	// Resets the decoder for a new image of w x h pixels. The 14-byte header is checked against them.
	void begin(uint16_t w, uint16_t h);

	// Feeds body bytes. Stops early on ERROR, or when the image is complete (DONE). Returns the
	// result; on ERROR, error() says why.
	Result feed(const uint8_t *data, size_t len, PixelFn emit, void *ctx);

	bool complete() const { return pixelsDone >= (uint32_t)width * height; }
	const char *error() const { return err; }

private:
	void emitPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a, PixelFn emit, void *ctx);
	Result decodeOp(PixelFn emit, void *ctx);

	uint16_t width = 0, height = 0;
	uint32_t pixelsDone = 0;	// Pixels emitted so far
	uint8_t headerBytes = 0;	// Header bytes taken so far
	uint8_t header[14];			// The 14-byte header, assembled byte by byte
	uint8_t op[5];				// The op being assembled, at most 5 bytes (RGBA)
	uint8_t opLen = 0;			// Bytes of op[] filled
	uint8_t opNeed = 0;			// Bytes the current op needs in total
	uint8_t index[64 * 4];		// The 64-entry index of RGBA pixels, zeroed at begin()
	uint8_t prev[4] = { 0, 0, 0, 255 };	// The previous pixel, starts as 0,0,0,255
	const char *err = "";
};

#endif // QOI_STREAM_H
