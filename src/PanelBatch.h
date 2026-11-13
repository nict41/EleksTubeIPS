#ifndef PANEL_BATCH_H
#define PANEL_BATCH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Reads the records of a POST /panel/batch body: each a 14-byte little-endian header (tube, format
 * 0 = rgb565 / 1 = qoi / 2 = jpeg, x, y, w, h, length) and then the picture. Plain C++ with no
 * Arduino, so it's tested on a computer too, see test/native.
 */
namespace PanelBatch {
	static const size_t HEADER = 14;
	enum Format : uint8_t { RGB565 = 0, QOI = 1, JPEG = 2, PATTERN = 0xFF };	// PATTERN: made on the clock

	struct Record {
		uint8_t tube;
		uint8_t format;
		int16_t x, y, w, h;
		const uint8_t *data;	// Points into the body
		uint32_t len;
	};

	// Reads at most `max` records from buf into out and sets *count. The pictures aren't decoded,
	// only checked for the right length. Returns NULL, or what's wrong with the body.
	const char *parse(const uint8_t *buf, size_t len, Record *out, uint8_t max, uint8_t *count,
		int16_t tubeWidth, int16_t tubeHeight);

	inline bool isWholeTube(const Record &r, int16_t tubeWidth, int16_t tubeHeight) {
		return r.x == 0 && r.y == 0 && r.w == tubeWidth && r.h == tubeHeight;
	}
}

#endif // PANEL_BATCH_H
