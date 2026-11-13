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
	// Called with every decoded row, as RGB565 big-endian, see rowOutput()
	typedef void (*RowFn)(void *ctx, const uint8_t *row);

	// Instead of a call per pixel, puts the pixels in `row` (width * 2 bytes) as RGB565 big-endian and
	// calls fn once it holds a whole row: much quicker. Call after begin(); NULL row to stop. A copy
	// of the decoder (a checkpoint) gets its own with another rowOutput().
	void rowOutput(uint8_t *row, RowFn fn, void *ctx) { rowBuf = row; rowFn = fn; rowCtx = ctx; }

	// Resets the decoder for a new image of w x h pixels. The 14-byte header is checked against them.
	void begin(uint16_t w, uint16_t h);

	// Feeds body bytes. Stops early on ERROR, or when the image is complete (DONE). Returns the
	// result; on ERROR, error() says why.
	Result feed(const uint8_t *data, size_t len, PixelFn emit, void *ctx);

	// Like feed(), but also pauses once `limit` pixels have been emitted in all, even in the middle
	// of a run. Returns how many of the bytes it used; feed the rest later to carry on. *result is
	// OK (paused, or wants more bytes), DONE or ERROR.
	//
	// Paused, the decoder is a checkpoint: a copy of it, with the rest of the bytes, carries on from
	// that pixel as often as wanted. The flip transition uses that to draw a picture's bottom half
	// many times without decoding the top half again.
	size_t feedUntil(const uint8_t *data, size_t len, uint32_t limit, PixelFn emit, void *ctx, Result *result);

	uint32_t pixels() const { return pixelsDone; }

	bool complete() const { return pixelsDone >= (uint32_t)width * height; }
	const char *error() const { return err; }

private:
	void emitPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a, PixelFn emit, void *ctx);
	void put(uint8_t r, uint8_t g, uint8_t b, PixelFn emit, void *ctx);
	void emitRun(uint32_t limit, PixelFn emit, void *ctx);
	Result decodeOp(PixelFn emit, void *ctx);
	size_t fastRows(const uint8_t *data, size_t len, uint32_t limit);

	uint16_t width = 0, height = 0;
	uint32_t pixelsDone = 0;	// Pixels emitted so far
	uint8_t headerBytes = 0;	// Header bytes taken so far
	uint8_t header[14];			// The 14-byte header, assembled byte by byte
	uint8_t op[5];				// The op being assembled, at most 5 bytes (RGBA)
	uint8_t opLen = 0;			// Bytes of op[] filled
	uint8_t opNeed = 0;			// Bytes the current op needs in total
	uint8_t runLeft = 0;		// Pixels of a run not emitted yet, when it was paused
	uint8_t *rowBuf = nullptr;	// See rowOutput()
	RowFn rowFn = nullptr;
	void *rowCtx = nullptr;
	uint16_t col = 0;			// Pixels of the row in rowBuf
	uint32_t index[64];			// The 64-entry index of RGBA pixels (r | g << 8 | b << 16 | a << 24), zeroed at begin()
	uint8_t prev[4] = { 0, 0, 0, 255 };	// The previous pixel, starts as 0,0,0,255
	const char *err = "";
};

#endif // QOI_STREAM_H
