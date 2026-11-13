#include <string.h>

#include "QoiStream.h"

// The QOI header: "qoif", width and height big-endian, channels, colorspace
#define QOI_HEADER_SIZE 14
#define QOI_MAGIC 0x716f6966	// "qoif" as a big-endian uint32

// The op tags, the first byte of an op
#define QOI_OP_RGB		0xfe
#define QOI_OP_RGBA		0xff
#define QOI_OP_INDEX	0x00	// 00xxxxxx
#define QOI_OP_DIFF		0x40	// 01xxxxxx
#define QOI_OP_LUMA		0x80	// 10xxxxxx
#define QOI_OP_RUN		0xc0	// 11xxxxxx
#define QOI_OP_MASK		0xc0

static uint8_t qoiHash(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	// (r*3 + g*5 + b*7 + a*11) % 64
	return (uint8_t)((r * 3 + g * 5 + b * 7 + a * 11) & 63);
}

void QoiStream::begin(uint16_t w, uint16_t h) {
	width = w;
	height = h;
	pixelsDone = 0;
	headerBytes = 0;
	opLen = 0;
	opNeed = 0;
	memset(index, 0, sizeof(index));
	prev[0] = 0;
	prev[1] = 0;
	prev[2] = 0;
	prev[3] = 255;
	err = "";
}

void QoiStream::emitPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a, PixelFn emit, void *ctx) {
	uint8_t slot = qoiHash(r, g, b, a);
	index[slot * 4 + 0] = r;
	index[slot * 4 + 1] = g;
	index[slot * 4 + 2] = b;
	index[slot * 4 + 3] = a;
	prev[0] = r;
	prev[1] = g;
	prev[2] = b;
	prev[3] = a;
	emit(ctx, r, g, b);
	pixelsDone++;
}

// Decodes the op in op[] once all opNeed bytes of it have arrived. Returns ERROR on a bad op.
QoiStream::Result QoiStream::decodeOp(PixelFn emit, void *ctx) {
	uint8_t b0 = op[0];

	if (b0 == QOI_OP_RGB) {
		// The alpha stays what the previous pixel had (it feeds the index hash)
		emitPixel(op[1], op[2], op[3], prev[3], emit, ctx);
	} else if (b0 == QOI_OP_RGBA) {
		emitPixel(op[1], op[2], op[3], op[4], emit, ctx);
	} else if ((b0 & QOI_OP_MASK) == QOI_OP_INDEX) {
		uint8_t slot = (b0 & 0x3f) * 4;
		emitPixel(index[slot], index[slot + 1], index[slot + 2], index[slot + 3], emit, ctx);
	} else if ((b0 & QOI_OP_MASK) == QOI_OP_DIFF) {
		uint8_t r = (uint8_t)(prev[0] + ((b0 >> 4) & 3) - 2);
		uint8_t g = (uint8_t)(prev[1] + ((b0 >> 2) & 3) - 2);
		uint8_t b = (uint8_t)(prev[2] + (b0 & 3) - 2);
		emitPixel(r, g, b, prev[3], emit, ctx);
	} else if ((b0 & QOI_OP_MASK) == QOI_OP_LUMA) {
		uint8_t b1 = op[1];
		int8_t dg = (int8_t)(b0 & 0x3f) - 32;
		int8_t dr = (int8_t)((b1 >> 4) & 0x0f) - 8 + dg;
		int8_t db = (int8_t)(b1 & 0x0f) - 8 + dg;
		emitPixel((uint8_t)(prev[0] + dr), (uint8_t)(prev[1] + dg), (uint8_t)(prev[2] + db), prev[3], emit, ctx);
	} else {
		// QOI_OP_RUN: 2 to 62 pixels of the previous pixel, the count is b0 & 0x3f + 1
		uint8_t count = (uint8_t)((b0 & 0x3f) + 1);
		uint32_t total = (uint32_t)width * height;
		for (uint8_t i = 0; i < count && pixelsDone < total; i++) {	// A run never writes past the image
			emit(ctx, prev[0], prev[1], prev[2]);
			pixelsDone++;
		}
	}

	opLen = 0;
	opNeed = 0;
	return complete() ? DONE : OK;
}

QoiStream::Result QoiStream::feed(const uint8_t *data, size_t len, PixelFn emit, void *ctx) {
	if (err[0] != 0) {
		return ERROR;
	}

	size_t offset = 0;
	while (offset < len) {
		if (complete()) {
			// The rest is the 8-byte end marker, which is ignored
			return DONE;
		}

		if (headerBytes < QOI_HEADER_SIZE) {
			// The header arrives byte by byte too, so it's checked once it's complete
			while (offset < len && headerBytes < QOI_HEADER_SIZE) {
				header[headerBytes] = data[offset++];
				headerBytes++;
			}
			if (headerBytes < QOI_HEADER_SIZE) {
				break;
			}

			uint32_t magic = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
			// width and height are 32-bit big-endian (bytes 4-7 and 8-11), then channels, colorspace
			uint32_t hw = ((uint32_t)header[4] << 24) | ((uint32_t)header[5] << 16) | ((uint32_t)header[6] << 8) | header[7];
			uint32_t hh = ((uint32_t)header[8] << 24) | ((uint32_t)header[9] << 16) | ((uint32_t)header[10] << 8) | header[11];
			uint8_t channels = header[12];
			if (magic != QOI_MAGIC) {
				err = "not a QOI file";
				return ERROR;
			}
			if (hw != width || hh != height) {
				err = "QOI size doesn't match w x h";
				return ERROR;
			}
			if (channels != 3 && channels != 4) {
				err = "QOI channels must be 3 or 4";
				return ERROR;
			}
			// header[13] is the colorspace, which doesn't change the decoding
			opLen = 0;
			opNeed = 0;
			continue;
		}

		if (opNeed == 0) {
			// The first byte of the next op decides how long it is
			op[0] = data[offset++];
			opLen = 1;
			if (op[0] == QOI_OP_RGB) {
				opNeed = 4;
			} else if (op[0] == QOI_OP_RGBA) {
				opNeed = 5;
			} else if ((op[0] & QOI_OP_MASK) == QOI_OP_LUMA) {
				opNeed = 2;
			} else {
				// INDEX, DIFF and RUN are one byte
				opNeed = 1;
			}
		}

		while (offset < len && opLen < opNeed) {
			op[opLen++] = data[offset++];
		}
		if (opLen < opNeed) {
			break;	// The rest of the op arrives in a later chunk
		}

		Result result = decodeOp(emit, ctx);
		if (result != OK) {
			return result;
		}
	}

	return complete() ? DONE : OK;
}
