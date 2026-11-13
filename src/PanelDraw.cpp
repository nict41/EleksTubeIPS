#include <Arduino.h>
#include <esp_heap_caps.h>

#include "PanelDraw.h"
#include "PanelBatch.h"
#include "GifPlayer.h"
#include "JpegPanel.h"
#include "OTA.h"
#include "QoiStream.h"
#include "TFTs.h"

extern TFTs *tfts;
extern void requestMqttPublish();
extern SemaphoreHandle_t memMutex;

namespace PanelDraw {

#define MAX_RECORDS 48
#define STRIP_ROWS 8			// Rows decoded and drawn at a time
#define TUBE_SLOTS (6 + 1)		// One per tube in a transition, and one for drawing straight away
#define MS_PER_TUBE 40			// Roughly what decoding and drawing a whole tube takes
#define TEST_HOLD_MS 3000		// How long the test pattern stays before the tubes ask for their pictures again

using PanelBatch::Record;
using PanelBatch::RGB565;
using PanelBatch::QOI;
using PanelBatch::JPEG;
using PanelBatch::PATTERN;

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
	bool test;			// The test pattern: shown for TEST_HOLD_MS, then the tubes are marked needed
	TubeJob tubes[TUBE_SLOTS];
	uint8_t rows[STRIP_ROWS * TFT_WIDTH * 2];
};

/*
 * Where a batch is held. Normally in the display buffer (StaticSprite::output_buffer, 64.8 KB), which
 * is idle while the tubes show panels, so a batch costs no heap at all. It's guarded by memMutex, and
 * a FreeRTOS mutex must be given back by the task that took it, so this task takes and gives it,
 * asked by the web server's task through the command queue: RESERVE before the body arrives, RUN
 * once it's in, or RELEASE if the upload fails. While GIFs play they use that buffer, so a batch then
 * comes from the heap, smaller and leaving more heap free.
 */
static uint8_t *const displayBuffer = StaticSprite::output_buffer;
static const size_t DISPLAY_BUFFER = sizeof(StaticSprite::output_buffer);
enum Command : uint8_t { CMD_RESERVE, CMD_RELEASE, CMD_RUN };
static QueueHandle_t commands = NULL;
static SemaphoreHandle_t reserveDone = NULL;	// Given by the task once it has answered a RESERVE
static volatile bool reserveOk = false;
static volatile bool displayReserved = false;	// The display buffer is (being) held for a batch
static bool holdingMutex = false;				// Only the task touches this

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
	return PanelBatch::isWholeTube(r, TFT_WIDTH, TFT_HEIGHT);
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

// The test pattern (POST /panel/test), one pixel at a time: eight colour bars, a grey ramp and, at the
// bottom, one block per tube number, so each tube and the order they're drawn in can be told apart.
// Two looks, swapped on every test, so a transition between them shows.
static uint8_t testLook = 0;

static uint16_t patternPixel(uint8_t tube, uint8_t look, int16_t x, int16_t y) {
	static const uint16_t bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
	if (x < 3 || y < 3 || x >= TFT_WIDTH - 3 || y >= TFT_HEIGHT - 3) {
		return look ? 0xFD20 : 0xFFFF;					// Border: orange or white
	}
	if (y < 150) {
		uint8_t bar = (uint8_t)((x * 8 / TFT_WIDTH + tube + look * 4) % 8);
		return bars[bar];
	}
	if (y < 190) {
		uint8_t v = (uint8_t)(x * 255 / (TFT_WIDTH - 1));
		if (look) {
			v = 255 - v;
		}
		return (uint16_t)(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3));	// Grey ramp
	}
	// Tube number: tube + 1 blocks of 14 px with 4 px gaps, centred
	int16_t blocks = tube + 1;
	int16_t width = blocks * 18 - 4;
	int16_t left = (TFT_WIDTH - width) / 2;
	if (y >= 205 && y < 225 && x >= left && x < left + width && (x - left) % 18 < 14) {
		return look ? 0x07E0 : 0xFFE0;					// Green or yellow blocks
	}
	return 0x0000;
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

	if (r->format == PATTERN) {
		while (tj->rows < target) {
			int16_t n = target - tj->rows < STRIP_ROWS ? target - tj->rows : STRIP_ROWS;
			uint8_t *out = rows;
			for (int16_t yy = 0; yy < n; yy++) {
				for (int16_t xx = 0; xx < TFT_WIDTH; xx++) {
					uint16_t c = patternPixel(r->tube, (uint8_t)r->len, xx, (int16_t)(tj->rows + yy));
					*out++ = (uint8_t)(c >> 8);
					*out++ = (uint8_t)(c & 0xFF);
				}
			}
			tfts->drawPanelRows(r->tube, 0, tj->rows, TFT_WIDTH, n, rows);
			tj->rows += n;
		}
		tj->done = tj->rows >= r->h;
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
			if (!j->test) {
				wholeTubeReplaced(j->records[i].tube);
			}
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
				if (!j->test) {
					wholeTubeReplaced(ordered[k]->rec->tube);
				}
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
				if (tj->done && !j->test) {
					wholeTubeReplaced(tj->rec->tube);
					publish = true;
				}
			}
			if (!allDone) {
				vTaskDelay(1);
			}
		}
	}

	if (j->test) {
		// Leave the pattern up for a moment, then ask for the real pictures back
		uint32_t shown = millis();
		while (millis() - shown < TEST_HOLD_MS && !aborted()) {
			vTaskDelay(pdMS_TO_TICKS(20));
		}
		for (uint8_t i = 0; i < j->count; i++) {
			if (tfts->isPanelTube(j->records[i].tube)) {
				tfts->setPanelNeeded(j->records[i].tube);
			}
		}
		requestMqttPublish();
		return;
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

static void releaseDisplayBuffer() {
	if (holdingMutex) {
		holdingMutex = false;
		xSemaphoreGive(memMutex);
	}
	displayReserved = false;
}

static void drawTask(void *) {
	for (;;) {
		uint8_t cmd;
		if (xQueueReceive(commands, &cmd, portMAX_DELAY) != pdTRUE) {
			continue;
		}
		if (cmd == CMD_RESERVE) {
			bool ok = false;
			if (!holdingMutex && !GifPlayer::active() && xSemaphoreTake(memMutex, 0) == pdTRUE) {
				if (GifPlayer::active()) {	// One started meanwhile
					xSemaphoreGive(memMutex);
				} else {
					holdingMutex = true;
					ok = true;
				}
			}
			reserveOk = ok;
			xSemaphoreGive(reserveDone);
			continue;
		}
		if (cmd == CMD_RELEASE) {
			releaseDisplayBuffer();
			continue;
		}

		Job *j = job;
		if (j == NULL) {
			continue;
		}
		uint32_t t0 = millis();
		rows = j->rows;
		runJob(j);
		rows = NULL;
		lastDrawMs = millis() - t0;
		if (j->buf == displayBuffer) {
			releaseDisplayBuffer();
		} else {
			free(j->buf);
		}
		free(j);
		job = NULL;
		stopRequested = false;
		running = false;
	}
}

void begin() {
	if (task == NULL) {
		commands = xQueueCreate(4, sizeof(uint8_t));
		reserveDone = xSemaphoreCreateBinary();
		// Core 1 like the clock and the GIF player, above them (the clock's loop never sleeps, so at
		// its priority drawing would get half the core at best). It sleeps between strips of a
		// transition and after each tube, so they still get their turn.
		xTaskCreatePinnedToCore(drawTask, "panel draw", 6144, NULL, 3, &task, 1);
	}
}

// The largest batch the heap can hold now, leaving HEAP_HEADROOM (and the job) free
static size_t heapFree() {
	size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
	size_t reserve = HEAP_HEADROOM + sizeof(Job);
	if (largest <= reserve) {
		return 0;
	}
	size_t free = largest - reserve;
	return free > MAX_BUFFER ? MAX_BUFFER : free;
}

size_t bufferFree() {
	if (running || displayReserved || task == NULL) {
		return 0;
	}
	if (!GifPlayer::active() && xSemaphoreGetMutexHolder(memMutex) == NULL) {
		return DISPLAY_BUFFER;
	}
	return heapFree();
}

uint8_t *takeBuffer(size_t len) {
	if (len == 0 || running || displayReserved || task == NULL) {
		return NULL;
	}
	if (len <= DISPLAY_BUFFER && !GifPlayer::active()) {
		xSemaphoreTake(reserveDone, 0);	// Clear an answer that came too late last time
		uint8_t cmd = CMD_RESERVE;
		displayReserved = true;
		if (xQueueSend(commands, &cmd, 0) == pdTRUE) {
			if (xSemaphoreTake(reserveDone, pdMS_TO_TICKS(50)) == pdTRUE && reserveOk) {
				return displayBuffer;
			}
			cmd = CMD_RELEASE;	// No answer in time, or no: make sure it isn't held for nothing
			xQueueSend(commands, &cmd, 0);
		} else {
			displayReserved = false;
		}
	}
	if (len > heapFree()) {
		return NULL;
	}
	return (uint8_t*)malloc(len);
}

void freeBuffer(uint8_t *buf) {
	if (buf == displayBuffer) {
		uint8_t cmd = CMD_RELEASE;
		xQueueSend(commands, &cmd, portMAX_DELAY);
	} else {
		free(buf);
	}
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

	const char *err = PanelBatch::parse(buf, len, j->records, MAX_RECORDS, &j->count, TFT_WIDTH, TFT_HEIGHT);
	for (uint8_t i = 0; err == NULL && i < j->count; i++) {
		if (!tfts->isPanelTube(j->records[i].tube)) {
			*code = 409;
			err = "not a panel tube";
		}
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
	uint8_t cmd = CMD_RUN;
	xQueueSend(commands, &cmd, portMAX_DELAY);
	return NULL;
}

const char *startTest(const Transition &t, int *code) {
	*code = 503;
	if (running || displayReserved || task == NULL) {
		return "busy drawing";
	}
	Job *j = (Job*)malloc(sizeof(Job));
	if (j == NULL) {
		return "not enough memory";
	}
	memset(j, 0, sizeof(Job));
	testLook ^= 1;
	for (uint8_t tube = 0; tube < 6; tube++) {
		if (!tfts->isPanelTube(tube)) {
			continue;
		}
		Record &r = j->records[j->count++];
		r.tube = tube;
		r.format = PATTERN;
		r.w = TFT_WIDTH;
		r.h = TFT_HEIGHT;
		r.len = testLook;	// The look, for PATTERN
	}
	if (j->count == 0) {
		free(j);
		*code = 409;
		return "no panel tubes: set Display to Panels on the Clock page";
	}
	for (uint8_t i = 0; i < j->count; i++) {
		GifPlayer::stop(j->records[i].tube);
	}
	j->buf = NULL;
	j->t = t;
	j->test = true;
	uint32_t transition = t.kind == WIPE ? t.durationMs
		: t.kind == CASCADE ? (uint32_t)t.gapMs * (t.orderLen > 0 ? t.orderLen - 1 : 0) : 0;
	uint32_t drawing = (uint32_t)j->count * MS_PER_TUBE;
	endsAt = millis() + (transition > drawing ? transition : drawing) + TEST_HOLD_MS + 20;
	running = true;
	stopRequested = false;
	job = j;
	uint8_t cmd = CMD_RUN;
	xQueueSend(commands, &cmd, portMAX_DELAY);
	*code = 200;
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
