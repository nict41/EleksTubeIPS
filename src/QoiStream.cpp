#include <string.h>

#include "QoiStream.h"

// It decodes every picture the panels show, the flip many times over: worth more than its size
#pragma GCC optimize("O2")

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
	runLeft = 0;
	col = 0;
	rowBuf = nullptr;
	rowFn = nullptr;
	rowCtx = nullptr;
	memset(index, 0, sizeof(index));
	prev[0] = 0;
	prev[1] = 0;
	prev[2] = 0;
	prev[3] = 255;
	err = "";
}

inline void QoiStream::put(uint8_t r, uint8_t g, uint8_t b, PixelFn emit, void *ctx) {
	if (rowBuf != nullptr) {
		uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
		rowBuf[col * 2] = (uint8_t)(c >> 8);
		rowBuf[col * 2 + 1] = (uint8_t)c;
		if (++col == width) {
			col = 0;
			rowFn(rowCtx, rowBuf);
		}
	} else {
		emit(ctx, r, g, b);
	}
}

inline void QoiStream::emitPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a, PixelFn emit, void *ctx) {
	index[qoiHash(r, g, b, a)] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
	prev[0] = r;
	prev[1] = g;
	prev[2] = b;
	prev[3] = a;
	put(r, g, b, emit, ctx);
	pixelsDone++;
}

// The rest of a run, up to `limit` pixels in all
void QoiStream::emitRun(uint32_t limit, PixelFn emit, void *ctx) {
	uint32_t n = runLeft;
	if (pixelsDone + n > limit) {
		n = limit > pixelsDone ? limit - pixelsDone : 0;
	}
	runLeft = (uint8_t)(runLeft - n);
	pixelsDone += n;
	if (rowBuf == nullptr) {
		while (n-- > 0) {
			emit(ctx, prev[0], prev[1], prev[2]);
		}
		return;
	}
	uint16_t c = (uint16_t)(((prev[0] & 0xF8) << 8) | ((prev[1] & 0xFC) << 3) | (prev[2] >> 3));
	uint8_t hi = (uint8_t)(c >> 8), lo = (uint8_t)c;
	while (n > 0) {
		uint32_t room = (uint32_t)(width - col);
		uint32_t k = n < room ? n : room;
		uint8_t *o = rowBuf + col * 2;
		for (uint32_t i = 0; i < k; i++) {
			o[i * 2] = hi;
			o[i * 2 + 1] = lo;
		}
		col = (uint16_t)(col + k);
		n -= k;
		if (col == width) {
			col = 0;
			rowFn(rowCtx, rowBuf);
		}
	}
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
		uint32_t e = index[b0 & 0x3f];
		emitPixel((uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16), (uint8_t)(e >> 24), emit, ctx);
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
		// QOI_OP_RUN: 1 to 62 pixels of the previous pixel, the count is b0 & 0x3f + 1. Emitted by
		// feedUntil(), which can pause in the middle of one. A run never writes past the image.
		uint8_t count = (uint8_t)((b0 & 0x3f) + 1);
		uint32_t left = (uint32_t)width * height - pixelsDone;
		runLeft = count < left ? count : (uint8_t)left;
	}

	opLen = 0;
	opNeed = 0;
	return complete() ? DONE : OK;
}

QoiStream::Result QoiStream::feed(const uint8_t *data, size_t len, PixelFn emit, void *ctx) {
	Result result;
	feedUntil(data, len, UINT32_MAX, emit, ctx, &result);
	return result;
}

// The quickest way, for rowOutput(): whole ops straight from data, the pixel kept in a register and
// written as RGB565 straight into the row. Stops at `limit` pixels (a run may be left in runLeft), or
// with fewer than 5 bytes left for the slow way. Returns the bytes it used.
size_t QoiStream::fastRows(const uint8_t *data, size_t len, uint32_t limit) {
	uint32_t px = (uint32_t)prev[0] | ((uint32_t)prev[1] << 8) | ((uint32_t)prev[2] << 16) | ((uint32_t)prev[3] << 24);
	uint32_t done = pixelsDone;
	uint32_t total = (uint32_t)width * height;
	uint16_t c = col;
	uint8_t *out = rowBuf + (size_t)c * 2;
	size_t offset = 0;
	while (done < limit && len - offset >= 5) {
		uint8_t b0 = data[offset];
		if (b0 >= QOI_OP_RUN && b0 < QOI_OP_RGB) {
			uint32_t n = (uint32_t)(b0 & 0x3f) + 1;
			if (n > total - done) {
				n = total - done;
			}
			uint32_t now = n < limit - done ? n : limit - done;
			offset++;
			uint16_t v = (uint16_t)(((px & 0xF8) << 8) | ((px >> 5) & 0x07E0) | ((px >> 19) & 0x1F));
			uint8_t hi = (uint8_t)(v >> 8), lo = (uint8_t)v;
			done += now;
			runLeft = (uint8_t)(n - now);
			while (now > 0) {
				uint32_t k = width - c < now ? width - c : now;
				for (uint32_t i = 0; i < k; i++) {
					*out++ = hi;
					*out++ = lo;
				}
				c = (uint16_t)(c + k);
				now -= k;
				if (c == width) {
					c = 0;
					out = rowBuf;
					rowFn(rowCtx, rowBuf);
				}
			}
			if (runLeft > 0) {
				break;
			}
			continue;
		}
		if (b0 < QOI_OP_DIFF) {			// INDEX: already in the index where it belongs
			px = index[b0];
			offset++;
		} else {
			if (b0 < QOI_OP_LUMA) {		// DIFF
				uint32_t r = (px + ((b0 >> 4) & 3) - 2) & 0xff;
				uint32_t g = ((px >> 8) + ((b0 >> 2) & 3) - 2) & 0xff;
				uint32_t b = ((px >> 16) + (b0 & 3) - 2) & 0xff;
				px = r | (g << 8) | (b << 16) | (px & 0xff000000);
				offset++;
			} else if (b0 < QOI_OP_RUN) {	// LUMA
				uint8_t b1 = data[offset + 1];
				int32_t dg = (int32_t)(b0 & 0x3f) - 32;
				int32_t dr = dg - 8 + ((b1 >> 4) & 0x0f);
				int32_t db = dg - 8 + (b1 & 0x0f);
				uint32_t r = (uint32_t)((int32_t)(px & 0xff) + dr) & 0xff;
				uint32_t g = (uint32_t)((int32_t)((px >> 8) & 0xff) + dg) & 0xff;
				uint32_t b = (uint32_t)((int32_t)((px >> 16) & 0xff) + db) & 0xff;
				px = r | (g << 8) | (b << 16) | (px & 0xff000000);
				offset += 2;
			} else if (b0 == QOI_OP_RGB) {
				px = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8)
					| ((uint32_t)data[offset + 3] << 16) | (px & 0xff000000);
				offset += 4;
			} else {						// RGBA
				px = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8)
					| ((uint32_t)data[offset + 3] << 16) | ((uint32_t)data[offset + 4] << 24);
				offset += 5;
			}
			index[qoiHash((uint8_t)px, (uint8_t)(px >> 8), (uint8_t)(px >> 16), (uint8_t)(px >> 24))] = px;
		}
		uint16_t v = (uint16_t)(((px & 0xF8) << 8) | ((px >> 5) & 0x07E0) | ((px >> 19) & 0x1F));
		*out++ = (uint8_t)(v >> 8);
		*out++ = (uint8_t)v;
		done++;
		if (++c == width) {
			c = 0;
			out = rowBuf;
			rowFn(rowCtx, rowBuf);
		}
	}
	prev[0] = (uint8_t)px;
	prev[1] = (uint8_t)(px >> 8);
	prev[2] = (uint8_t)(px >> 16);
	prev[3] = (uint8_t)(px >> 24);
	pixelsDone = done;
	col = c;
	return offset;
}

size_t QoiStream::feedUntil(const uint8_t *data, size_t len, uint32_t limit, PixelFn emit, void *ctx, Result *result) {
	*result = OK;
	if (err[0] != 0) {
		*result = ERROR;
		return 0;
	}
	uint32_t total = (uint32_t)width * height;
	if (limit > total) {
		limit = total;
	}

	size_t offset = 0;
	for (;;) {
		// The rest of a run, which may have been paused
		if (runLeft > 0) {
			emitRun(limit, emit, ctx);
		}
		if (headerBytes == QOI_HEADER_SIZE && complete()) {
			// The rest is the 8-byte end marker, which is ignored
			*result = DONE;
			return offset;
		}
		if (pixelsDone >= limit && headerBytes == QOI_HEADER_SIZE) {
			return offset;	// Paused
		}
		if (offset >= len) {
			return offset;	// Wants more bytes
		}

		if (headerBytes < QOI_HEADER_SIZE) {
			// The header arrives byte by byte too, so it's checked once it's complete
			while (offset < len && headerBytes < QOI_HEADER_SIZE) {
				header[headerBytes] = data[offset++];
				headerBytes++;
			}
			if (headerBytes < QOI_HEADER_SIZE) {
				return offset;
			}

			uint32_t magic = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
			// width and height are 32-bit big-endian (bytes 4-7 and 8-11), then channels, colorspace
			uint32_t hw = ((uint32_t)header[4] << 24) | ((uint32_t)header[5] << 16) | ((uint32_t)header[6] << 8) | header[7];
			uint32_t hh = ((uint32_t)header[8] << 24) | ((uint32_t)header[9] << 16) | ((uint32_t)header[10] << 8) | header[11];
			uint8_t channels = header[12];
			if (magic != QOI_MAGIC) {
				err = "not a QOI file";
			} else if (hw != width || hh != height) {
				err = "QOI size doesn't match w x h";
			} else if (channels != 3 && channels != 4) {
				err = "QOI channels must be 3 or 4";
			}
			if (err[0] != 0) {
				*result = ERROR;
				return offset;
			}
			// header[13] is the colorspace, which doesn't change the decoding
			opLen = 0;
			opNeed = 0;
			continue;
		}

		// The quick way, while whole ops are there (always but near the end of the bytes): straight
		// from data, without assembling the op byte by byte
		if (opNeed == 0 && rowBuf != nullptr && runLeft == 0) {
			offset += fastRows(data + offset, len - offset, limit);
		}
		if (opNeed == 0) {
			while (runLeft == 0 && pixelsDone < limit && len - offset >= 5) {
				uint8_t b0 = data[offset];
				if (b0 == QOI_OP_RGB) {
					emitPixel(data[offset + 1], data[offset + 2], data[offset + 3], prev[3], emit, ctx);
					offset += 4;
				} else if (b0 == QOI_OP_RGBA) {
					emitPixel(data[offset + 1], data[offset + 2], data[offset + 3], data[offset + 4], emit, ctx);
					offset += 5;
				} else if ((b0 & QOI_OP_MASK) == QOI_OP_INDEX) {
					uint32_t e = index[b0 & 0x3f];
					emitPixel((uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16), (uint8_t)(e >> 24), emit, ctx);
					offset += 1;
				} else if ((b0 & QOI_OP_MASK) == QOI_OP_DIFF) {
					emitPixel((uint8_t)(prev[0] + ((b0 >> 4) & 3) - 2), (uint8_t)(prev[1] + ((b0 >> 2) & 3) - 2),
						(uint8_t)(prev[2] + (b0 & 3) - 2), prev[3], emit, ctx);
					offset += 1;
				} else if ((b0 & QOI_OP_MASK) == QOI_OP_LUMA) {
					uint8_t b1 = data[offset + 1];
					int8_t dg = (int8_t)(b0 & 0x3f) - 32;
					int8_t dr = (int8_t)((b1 >> 4) & 0x0f) - 8 + dg;
					int8_t db = (int8_t)(b1 & 0x0f) - 8 + dg;
					emitPixel((uint8_t)(prev[0] + dr), (uint8_t)(prev[1] + dg), (uint8_t)(prev[2] + db), prev[3], emit, ctx);
					offset += 2;
				} else {
					uint8_t count = (uint8_t)((b0 & 0x3f) + 1);
					uint32_t left = total - pixelsDone;
					runLeft = count < left ? count : (uint8_t)left;
					offset += 1;
					emitRun(limit, emit, ctx);
				}
			}
			if (runLeft > 0 || pixelsDone >= limit) {
				continue;	// The checks at the top: a run to finish, or paused
			}
			// Otherwise fewer than 5 bytes are left: the slow way, for the last few
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
			return offset;	// The rest of the op arrives in a later chunk
		}

		decodeOp(emit, ctx);
	}
}
