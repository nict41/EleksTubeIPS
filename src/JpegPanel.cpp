#include <Arduino.h>
#include "esp32/rom/tjpgd.h"

#include "JpegPanel.h"
#include "TFTs.h"

extern TFTs *tfts;

#define JPEG_WORK_SIZE 3100	// What TJpgDec needs for any JPEG it can decode

// Where the decoder reads from and draws to, reached through JDEC::device
struct JpegJob {
	const uint8_t *data;
	size_t len;
	size_t pos;
	uint8_t tube;
	int16_t x, y;
	uint8_t block[16 * 16 * 2];	// One decoded block (at most 16x16) as RGB565, big-endian
};

static UINT jpegInput(JDEC *jd, BYTE *buf, UINT n) {
	JpegJob *job = (JpegJob*)jd->device;
	size_t left = job->len - job->pos;
	if ((size_t)n > left) {
		n = (UINT)left;
	}
	if (buf != NULL) {
		memcpy(buf, job->data + job->pos, n);
	}
	job->pos += n;	// buf == NULL asks to skip the bytes
	return n;
}

static UINT jpegOutput(JDEC *jd, void *bitmap, JRECT *rect) {
	JpegJob *job = (JpegJob*)jd->device;
	uint16_t bw = (uint16_t)(rect->right - rect->left + 1);
	uint16_t bh = (uint16_t)(rect->bottom - rect->top + 1);
	if ((size_t)bw * bh * 2 > sizeof(job->block)) {
		return 0;	// Can't happen with scale 0 (blocks are at most 16x16); stops decoding if it does
	}
	const uint8_t *rgb = (const uint8_t*)bitmap;	// RGB888, row by row
	for (size_t i = 0; i < (size_t)bw * bh; i++) {
		uint16_t c = (uint16_t)(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
		job->block[i * 2] = (uint8_t)(c >> 8);
		job->block[i * 2 + 1] = (uint8_t)(c & 0xFF);
		rgb += 3;
	}
	tfts->drawPanelRows(job->tube, (int16_t)(job->x + rect->left), (int16_t)(job->y + rect->top),
		(int16_t)bw, (int16_t)bh, job->block);
	return 1;
}

const char *jpegDrawPanel(const uint8_t *jpeg, size_t len, uint8_t tube, int16_t x, int16_t y, int16_t w, int16_t h) {
	void *work = malloc(JPEG_WORK_SIZE);
	JpegJob *job = (JpegJob*)malloc(sizeof(JpegJob));
	if (work == NULL || job == NULL) {
		free(work);
		free(job);
		return "not enough memory to decode the JPEG";
	}
	job->data = jpeg;
	job->len = len;
	job->pos = 0;
	job->tube = tube;
	job->x = x;
	job->y = y;

	const char *result = NULL;
	JDEC jd;
	JRESULT res = jd_prepare(&jd, jpegInput, work, JPEG_WORK_SIZE, job);
	if (res != JDR_OK) {
		result = res == JDR_FMT3 ? "unsupported JPEG (progressive?)" : "not a JPEG the clock can decode";
	} else if (jd.width != (UINT)w || jd.height != (UINT)h) {
		result = "JPEG size doesn't match w x h";
	} else if (jd_decomp(&jd, jpegOutput, 0) != JDR_OK) {
		result = "JPEG decoding failed";
	}

	free(job);
	free(work);
	return result;
}
