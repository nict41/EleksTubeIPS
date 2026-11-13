#include "PanelBatch.h"

namespace PanelBatch {

static uint16_t le16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

const char *parse(const uint8_t *buf, size_t len, Record *out, uint8_t max, uint8_t *count,
		int16_t tubeWidth, int16_t tubeHeight) {
	*count = 0;
	size_t off = 0;
	while (off < len) {
		if (len - off < HEADER) {
			return "batch ends part way through a record";
		}
		if (*count == max) {
			return "too many records in one batch";
		}
		const uint8_t *hd = buf + off;
		Record &r = out[*count];
		r.tube = hd[0];
		r.format = hd[1];
		r.x = (int16_t)le16(hd + 2);
		r.y = (int16_t)le16(hd + 4);
		r.w = (int16_t)le16(hd + 6);
		r.h = (int16_t)le16(hd + 8);
		r.len = (uint32_t)hd[10] | ((uint32_t)hd[11] << 8) | ((uint32_t)hd[12] << 16) | ((uint32_t)hd[13] << 24);
		off += HEADER;

		if (r.tube >= 6) {
			return "batch record: tube must be 0 to 5";
		}
		if (r.w <= 0 || r.h <= 0 || r.w > tubeWidth || r.h > tubeHeight
				|| r.x < 0 || r.y < 0 || r.x > tubeWidth - r.w || r.y > tubeHeight - r.h) {
			return "batch record: rectangle must be inside the tube";
		}
		if (r.len > len - off) {
			return "batch ends part way through a record";
		}
		if (r.format == RGB565 && r.len != (uint32_t)r.w * (uint32_t)r.h * 2) {
			return "batch record: rgb565 length must be w*h*2";
		}
		if (r.format == QOI && r.len < 14 + 8) {
			return "batch record: not a QOI image";
		}
		if (r.format == JPEG && r.len < 4) {
			return "batch record: not a JPEG";
		}
		if (r.format != RGB565 && r.format != QOI && r.format != JPEG) {
			return "batch record: format must be 0 (rgb565), 1 (qoi) or 2 (jpeg)";
		}
		r.data = buf + off;
		off += r.len;
		(*count)++;
	}
	if (*count == 0) {
		return "a batch needs at least one record";
	}
	return 0;
}

} // namespace PanelBatch
