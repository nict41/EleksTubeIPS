#include <Arduino.h>
#include <esp_heap_caps.h>

#include "PanelDraw.h"
#include "GifPlayer.h"
#include "JpegPanel.h"
#include "OTA.h"
#include "QoiStream.h"
#include "TFTs.h"

extern TFTs *tfts;
extern void requestMqttPublish();

namespace PanelDraw {

#define MAX_RECORDS 64
#define STRIP_ROWS 8			// Rows decoded and drawn at a time
#define TUBE_SLOTS (6 + 1)		// One per tube in a transition, and one for drawing straight away
#define MS_PER_TUBE 40			// Roughly what decoding and drawing a whole tube takes

enum Format : uint8_t { RGB565 = 0, QOI = 1, JPEG = 2 };

struct Record {
	uint8_t tube;
	uint8_t format;
	int16_t x, y, w, h;
	const uint8_t *data;
	uint32_t len;
};

// A record being drawn, a few rows at a time
struct TubeJob {
	const Record *rec;
	QoiStream qoi;
	uint32_t pos;		// Bytes of a QOI picture fed to the decoder
	int16_t rows;		// Rows drawn
	uint16_t rowBytes;	// Bytes of the row being decoded
	uint8_t row[TFT_WIDTH * 2];
	bool done;
};

struct Job {
	uint8_t *buf;
	size_t len;
	Transition t;
	Record records[MAX_RECORDS];
	uint8_t count;
	TubeJob tubes[TUBE_SLOTS];
	uint8_t rows[STRIP_ROWS * TFT_WIDTH * 2];
};

static TaskHandle_t task = NULL;
static Job *volatile job = NULL;			// The batch handed to the task
static volatile bool running = false;		// From start() until the task has freed the batch
static volatile bool stopRequested = false;
static volatile uint32_t endsAt = 0;		// millis() when the batch should be drawn, roughly
static volatile uint32_t lastDrawMs = 0;	// How long the last batch took to draw

// Decoded rows of the record being decoded, waiting to be drawn: in the job, as static memory is
// short, see Job::rows
static uint8_t *rows = NULL;
static uint8_t rowCount = 0;

static bool isWholeTube(const Record &r) {
	return r.x == 0 && r.y == 0 && r.w == TFT_WIDTH && r.h == TFT_HEIGHT;
}

static uint16_t le16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

void wholeTubeReplaced(uint8_t tube) {
	tfts->claim();
	tfts->chip_select.setDigit(TFTs::panelToDigit(tube));
	tfts->drawStatus();
	tfts->release();
	tfts->clearPanelNeeded(tube);
}

static void flushRows(TubeJob *tj) {
	if (rowCount == 0) {
		return;
	}
	const Record *r = tj->rec;
	tfts->drawPanelRows(r->tube, r->x, (int16_t)(r->y + tj->rows), r->w, rowCount, rows);
	tj->rows += rowCount;
	rowCount = 0;
}

static void putPixel(void *ctx, uint8_t red, uint8_t green, uint8_t blue) {
	TubeJob *tj = (TubeJob*)ctx;
	size_t rowLen = (size_t)tj->rec->w * 2;
	uint16_t c = (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
	tj->row[tj->rowBytes++] = (uint8_t)(c >> 8);
	tj->row[tj->rowBytes++] = (uint8_t)(c & 0xFF);
	if (tj->rowBytes == rowLen) {
		memcpy(rows + (size_t)rowCount * rowLen, tj->row, rowLen);
		tj->rowBytes = 0;
		rowCount++;
		if (rowCount == STRIP_ROWS) {
			flushRows(tj);
		}
	}
}

static void tubeJobBegin(TubeJob *tj, const Record *r) {
	tj->rec = r;
	tj->pos = 0;
	tj->rows = 0;
	tj->rowBytes = 0;
	tj->done = false;
	if (r->format == QOI) {
		tj->qoi.begin((uint16_t)r->w, (uint16_t)r->h);
	}
}

// Draws the record down to at least row `target` (all of it for a JPEG)
static void advance(TubeJob *tj, int16_t target) {
	const Record *r = tj->rec;
	if (tj->done) {
		return;
	}
	if (target > r->h) {
		target = r->h;
	}

	if (r->format == JPEG) {
		jpegDrawPanel(r->data, r->len, r->tube, r->x, r->y, r->w, r->h);	// A bad one just isn't drawn
		tj->rows = r->h;
		tj->done = true;
		return;
	}

	if (r->format == RGB565) {
		if (target > tj->rows) {
			tfts->drawPanelRows(r->tube, r->x, (int16_t)(r->y + tj->rows), r->w, (int16_t)(target - tj->rows),
				r->data + (size_t)tj->rows * r->w * 2);
			tj->rows = target;
		}
		tj->done = tj->rows >= r->h;
		return;
	}

	rowCount = 0;
	// A few bytes at a time when only part of it is wanted now (a wipe), so it stops close to
	// the target; all of it at once otherwise
	uint32_t step = target >= r->h ? r->len : 8;
	while (tj->rows + rowCount < target && tj->pos < r->len) {
		uint32_t n = r->len - tj->pos;
		if (n > step) {
			n = step;
		}
		QoiStream::Result res = tj->qoi.feed(r->data + tj->pos, n, putPixel, tj);
		tj->pos += n;
		if (res == QoiStream::ERROR) {
			tj->pos = r->len;	// Malformed: what was decoded stays
		}
	}
	flushRows(tj);
	tj->done = tj->qoi.complete() || tj->pos >= r->len;
}

static bool aborted() {
	return stopRequested || OTA::otaBusy();
}

// Draws one record straight away. Returns false if the tube isn't a panel tube any more.
static bool drawNow(TubeJob *tj) {
	if (!tfts->isPanelTube(tj->rec->tube)) {
		tj->done = true;
		return false;
	}
	advance(tj, tj->rec->h);
	return true;
}

static void runJob(Job *j) {
	bool publish = false;
	TubeJob *ordered[6];
	uint8_t n = 0;
	bool inTransition[MAX_RECORDS] = { false };
	bool drawn[MAX_RECORDS] = { false };	// Whole tubes that got all of their picture

	if (j->t.kind != NONE) {
		for (uint8_t k = 0; k < j->t.orderLen; k++) {
			for (uint8_t i = 0; i < j->count; i++) {
				if (!inTransition[i] && j->records[i].tube == j->t.order[k] && isWholeTube(j->records[i])) {
					inTransition[i] = true;
					tubeJobBegin(&j->tubes[n], &j->records[i]);
					ordered[n] = &j->tubes[n];
					n++;
					break;
				}
			}
		}
	}

	// Everything that isn't part of the transition, straight away and in order
	TubeJob *now = &j->tubes[TUBE_SLOTS - 1];
	for (uint8_t i = 0; i < j->count && !aborted(); i++) {
		if (inTransition[i]) {
			continue;
		}
		tubeJobBegin(now, &j->records[i]);
		if (drawNow(now) && isWholeTube(j->records[i])) {
			wholeTubeReplaced(j->records[i].tube);
			drawn[i] = true;
			publish = true;
		}
		if (isWholeTube(j->records[i])) {
			vTaskDelay(1);	// Let the clock's loop and the GIF player have the core for a moment
		}
	}

	uint32_t t0 = millis();
	if (n > 0 && j->t.kind == CASCADE) {
		for (uint8_t k = 0; k < n && !aborted(); k++) {
			uint32_t due = t0 + (uint32_t)k * j->t.gapMs;
			while ((int32_t)(due - millis()) > 0 && !aborted()) {
				vTaskDelay(1);
			}
			if (!aborted() && drawNow(ordered[k])) {
				wholeTubeReplaced(ordered[k]->rec->tube);
				publish = true;
			}
		}
	} else if (n > 0 && j->t.kind == WIPE) {
		// Each tube wipes down over `each` ms and starts a sixth of that after the one before it,
		// so the whole wipe takes durationMs
		uint32_t each = (uint32_t)j->t.durationMs * 6 / (n + 5);
		uint32_t stagger = each / 6;
		if (each == 0) {
			each = 1;
		}
		bool allDone = false;
		while (!allDone && !aborted()) {
			uint32_t elapsed = millis() - t0;
			allDone = true;
			for (uint8_t k = 0; k < n; k++) {
				TubeJob *tj = ordered[k];
				if (tj->done) {
					continue;
				}
				allDone = false;
				uint32_t start = k * stagger;
				if (elapsed < start) {
					continue;
				}
				if (!tfts->isPanelTube(tj->rec->tube)) {
					tj->done = true;
					continue;
				}
				uint32_t target = (elapsed - start) * TFT_HEIGHT / each;
				target = (target + STRIP_ROWS - 1) / STRIP_ROWS * STRIP_ROWS;	// Whole strips
				advance(tj, (int16_t)(target > TFT_HEIGHT ? TFT_HEIGHT : target));
				if (tj->done) {
					wholeTubeReplaced(tj->rec->tube);
					publish = true;
				}
			}
			if (!allDone) {
				vTaskDelay(1);
			}
		}
	}

	if (aborted()) {
		// The tubes that didn't get all of their picture still need one
		for (uint8_t k = 0; k < n; k++) {
			if (ordered[k]->rows >= ordered[k]->rec->h) {
				drawn[ordered[k]->rec - j->records] = true;
			}
		}
		for (uint8_t i = 0; i < j->count; i++) {
			if (isWholeTube(j->records[i]) && !drawn[i] && tfts->isPanelTube(j->records[i].tube)) {
				tfts->setPanelNeeded(j->records[i].tube);
				publish = true;
			}
		}
	}
	if (publish) {
		requestMqttPublish();	// Home Assistant hears which tubes have their pictures now
	}
}

static void drawTask(void *) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		Job *j = job;
		if (j == NULL) {
			continue;
		}
		uint32_t t0 = millis();
		rows = j->rows;
		runJob(j);
		rows = NULL;
		lastDrawMs = millis() - t0;
		free(j->buf);
		free(j);
		job = NULL;
		stopRequested = false;
		running = false;
	}
}

void begin() {
	if (task == NULL) {
		// Core 1 like the clock and the GIF player, above them (the clock's loop never sleeps, so at
		// its priority drawing would get half the core at best). It sleeps between strips of a
		// transition and after each tube, so they still get their turn.
		xTaskCreatePinnedToCore(drawTask, "panel draw", 6144, NULL, 3, &task, 1);
	}
}

size_t bufferFree() {
	if (running) {
		return 0;
	}
	size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
	size_t reserve = HEAP_HEADROOM + sizeof(Job);
	if (largest <= reserve) {
		return 0;
	}
	size_t free = largest - reserve;
	return free > MAX_BUFFER ? MAX_BUFFER : free;
}

uint8_t *takeBuffer(size_t len) {
	if (len == 0 || len > bufferFree()) {
		return NULL;
	}
	return (uint8_t*)malloc(len);
}

void freeBuffer(uint8_t *buf) {
	free(buf);
}

const char *start(uint8_t *buf, size_t len, const Transition &t, int *code) {
	*code = 400;
	if (running || task == NULL) {
		*code = 503;
		return "busy drawing";
	}
	Job *j = (Job*)malloc(sizeof(Job));
	if (j == NULL) {
		*code = 503;
		return "not enough memory";
	}
	memset(j, 0, sizeof(Job));

	const char *err = NULL;
	size_t off = 0;
	while (off < len && err == NULL) {
		if (len - off < HEADER) {
			err = "batch ends part way through a record";
			break;
		}
		if (j->count == MAX_RECORDS) {
			err = "a batch can have at most 64 records";
			break;
		}
		const uint8_t *hd = buf + off;
		Record &r = j->records[j->count];
		r.tube = hd[0];
		r.format = hd[1];
		r.x = (int16_t)le16(hd + 2);
		r.y = (int16_t)le16(hd + 4);
		r.w = (int16_t)le16(hd + 6);
		r.h = (int16_t)le16(hd + 8);
		r.len = (uint32_t)hd[10] | ((uint32_t)hd[11] << 8) | ((uint32_t)hd[12] << 16) | ((uint32_t)hd[13] << 24);
		off += HEADER;

		if (r.tube >= 6) {
			err = "batch record: tube must be 0 to 5";
		} else if (r.w <= 0 || r.h <= 0 || r.w > TFT_WIDTH || r.h > TFT_HEIGHT
				|| r.x < 0 || r.y < 0 || r.x > TFT_WIDTH - r.w || r.y > TFT_HEIGHT - r.h) {
			err = "batch record: rectangle must be inside the tube";
		} else if (r.len > len - off) {
			err = "batch ends part way through a record";
		} else if (r.format == RGB565 && r.len != (uint32_t)r.w * r.h * 2) {
			err = "batch record: rgb565 length must be w*h*2";
		} else if (r.format == QOI && r.len < 14 + 8) {
			err = "batch record: not a QOI image";
		} else if (r.format == JPEG && r.len < 4) {
			err = "batch record: not a JPEG";
		} else if (r.format > JPEG) {
			err = "batch record: format must be 0 (rgb565), 1 (qoi) or 2 (jpeg)";
		} else if (!tfts->isPanelTube(r.tube)) {
			*code = 409;
			err = "not a panel tube";
		}
		if (err == NULL) {
			r.data = buf + off;
			off += r.len;
			j->count++;
		}
	}
	if (err == NULL && j->count == 0) {
		err = "a batch needs at least one record";
	}
	if (err != NULL) {
		free(j);
		return err;
	}

	for (uint8_t i = 0; i < j->count; i++) {
		GifPlayer::stop(j->records[i].tube);	// A picture replaces whatever GIF played on the tube
	}

	j->buf = buf;
	j->len = len;
	j->t = t;
	uint32_t drawing = (uint32_t)j->count * MS_PER_TUBE;
	uint32_t transition = t.kind == WIPE ? t.durationMs
		: t.kind == CASCADE ? (uint32_t)t.gapMs * (t.orderLen > 0 ? t.orderLen - 1 : 0) : 0;
	endsAt = millis() + (transition > drawing ? transition : drawing) + 20;

	running = true;
	stopRequested = false;
	job = j;
	xTaskNotifyGive(task);
	return NULL;
}

bool busy() {
	return running;
}

uint32_t msLeft() {
	if (!running) {
		return 0;
	}
	int32_t left = (int32_t)(endsAt - millis());
	return left < 10 ? 10 : (uint32_t)left;
}

uint32_t lastDurationMs() {
	return lastDrawMs;
}

void interrupt() {
	if (running) {
		stopRequested = true;
	}
}

} // namespace PanelDraw
