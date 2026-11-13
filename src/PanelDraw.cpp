#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

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
extern TaskHandle_t weatherTask;	// NULL in the Home Assistant-only build

namespace PanelDraw {

#define MAX_RECORDS 48
#define STRIP_ROWS 8			// Rows decoded and drawn at a time
#define TUBE_SLOTS (6 + 1)		// One per tube in a transition, and one for drawing straight away
#define MS_PER_TUBE 40			// Roughly what decoding and drawing a whole tube takes
#define TEST_HOLD_MS 3000		// How long the test pattern stays before the tubes ask for their pictures again
#define HINGE (TFT_HEIGHT / 2)	// The flip's hinge: the first row of the bottom half
#define KEEP_SLOT 5120			// Bytes of IRAM per tube for the top half of its picture, see keep()
#define BOUNCE 64				// Bytes read from IRAM at a time

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
	uint32_t halfPos;	// Bytes of a whole-tube QOI picture that hold its top half (0: not got there)
	int16_t rows;		// Rows drawn
	uint8_t row[TFT_WIDTH * 2];	// The row being decoded
	bool done;
	// The flip
	bool flipOld;		// Folding the old top half (else uncovering the new one behind a dark edge)
	bool flipStarted;
	int16_t flipLast;	// Height of the fold drawn last, or -1
	uint8_t flipPhase;	// 1: the top half, 2: the bottom half
};

// Draws half a tube's picture squashed into fewer rows, as a flap seen at an angle: the rows of the
// picture arrive in order, and each one that lands on a screen row is drawn, shaded
struct Squash {
	uint8_t tube;
	int16_t y0;			// Screen row of the first row drawn
	int16_t n;			// Rows drawn, 0 to HINGE
	int16_t next;		// Next of them to draw
	int16_t src;		// Rows of the half picture seen so far
	uint16_t shade;		// 256 = as it is, less is darker
	uint8_t strip;		// Rows waiting in the strip buffer
	uint8_t row[TFT_WIDTH * 2];	// The row being decoded
};

struct Job {
	uint8_t *buf;
	size_t len;
	Transition t;
	Record records[MAX_RECORDS];
	uint8_t count;
	bool test;			// The test pattern: shown for TEST_HOLD_MS, then the tubes are marked needed
	TubeJob tubes[TUBE_SLOTS];
	alignas(4) uint8_t rows[STRIP_ROWS * TFT_WIDTH * 2];	// Aligned: drawPanelRows() pushes it as it is
	Squash squash;			// The flip's fold being drawn
	QoiStream scratch;		// Decodes the half picture of a fold
	uint8_t bounce[BOUNCE];	// Bytes of a kept picture, copied out of IRAM
};

/*
 * The flip's old pictures. When a tube gets a whole QOI picture, the bytes that hold its top half
 * (2 to 5 KB for a typical screen) are kept here, one slot per tube, so the next flip can fold them.
 * In IRAM, which nothing else uses (tens of KB of it are free), so no heap or display buffer: IRAM can
 * only be read and written 32 bits at a time, so the bytes go in and out a word at a time.
 * `draws` is the tube's TFTs::panelDrawCount() once the picture was drawn: if anything has drawn on
 * the tube since, the picture showing isn't this one any more.
 */
struct Kept {
	uint16_t len;	// 0: none
	uint32_t draws;
};
static volatile uint32_t *keepArena = NULL;	// 6 slots of KEEP_SLOT bytes
static Kept kept[6];

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
static volatile uint8_t lastFlipFolded = 0;	// Tubes of the last flip that folded their old picture
static volatile uint16_t lastFlipFrames = 0;	// Frames drawn in the last flip, all tubes together

// Decoded rows of the record being decoded, waiting to be drawn: in the job, as static memory is
// short, see Job::rows
static uint8_t *rows = NULL;
static uint8_t rowCount = 0;

static bool isWholeTube(const Record &r) {
	return PanelBatch::isWholeTube(r, TFT_WIDTH, TFT_HEIGHT);
}

size_t flipMemory() {
	return keepArena != NULL ? 6 * KEEP_SLOT : 0;
}

// Keeps the first len bytes of a tube's picture, which hold its top half. Call once it's drawn.
static void keep(uint8_t tube, const uint8_t *data, size_t len) {
	if (tube >= 6) {
		return;
	}
	kept[tube].len = 0;
	if (keepArena == NULL || len == 0 || len > KEEP_SLOT) {
		return;		// Too big for its slot (a photo): the next flip uncovers this tube instead
	}
	volatile uint32_t *dst = keepArena + (size_t)tube * (KEEP_SLOT / 4);
	for (size_t i = 0; i < len; i += 4) {
		uint32_t w = 0;
		for (size_t b = 0; b < 4 && i + b < len; b++) {
			w |= (uint32_t)data[i + b] << (8 * b);
		}
		*dst++ = w;
	}
	kept[tube].len = (uint16_t)len;
	kept[tube].draws = tfts->panelDrawCount(tube);
}

// A whole tube finished drawing: keeps its top half for the next flip, if it's a QOI picture
static void finishTube(TubeJob *tj) {
	const Record *r = tj->rec;
	if (isWholeTube(*r) && r->format == QOI && tj->halfPos > 0) {
		keep(r->tube, r->data, tj->halfPos);
	} else if (r->tube < 6) {
		kept[r->tube].len = 0;
	}
}

// The tube still shows the picture kept of it
static bool keptShowing(uint8_t tube) {
	return tube < 6 && keepArena != NULL && kept[tube].len > 0
		&& kept[tube].draws == tfts->panelDrawCount(tube)
		&& (tfts->getPanelsNeeded() & (1 << tube)) == 0;
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

// A decoded row of a record (QoiStream::rowOutput()), waiting in the strip to be drawn
static void putRow(void *ctx, const uint8_t *row) {
	TubeJob *tj = (TubeJob*)ctx;
	size_t rowLen = (size_t)tj->rec->w * 2;
	memcpy(rows + (size_t)rowCount * rowLen, row, rowLen);
	rowCount++;
	if (rowCount == STRIP_ROWS) {
		flushRows(tj);
	}
}

static void tubeJobBegin(TubeJob *tj, const Record *r) {
	tj->rec = r;
	tj->pos = 0;
	tj->halfPos = 0;
	tj->rows = 0;
	tj->done = false;
	tj->flipStarted = false;
	if (r->format == QOI) {
		tj->qoi.begin((uint16_t)r->w, (uint16_t)r->h);
		tj->qoi.rowOutput(tj->row, putRow, tj);
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

	// Exactly down to the target row, pausing at the hinge on the way to note where the top half's
	// bytes end (for keep(), and the flip carries on from there)
	rowCount = 0;
	uint32_t want = (uint32_t)target * r->w;
	uint32_t half = (uint32_t)HINGE * r->w;
	bool whole = isWholeTube(*r);
	bool ended = false;		// Malformed, or the bytes ran out
	while (tj->qoi.pixels() < want) {
		uint32_t limit = want;
		if (whole && tj->halfPos == 0 && tj->qoi.pixels() < half && limit > half) {
			limit = half;
		}
		QoiStream::Result res;
		size_t used = tj->qoi.feedUntil(r->data + tj->pos, r->len - tj->pos, limit, NULL, NULL, &res);
		tj->pos += used;
		if (whole && tj->halfPos == 0 && tj->qoi.pixels() == half) {
			tj->halfPos = tj->pos;
		}
		if (res == QoiStream::DONE) {
			break;
		}
		if (res == QoiStream::ERROR || (tj->qoi.pixels() < limit && tj->pos >= r->len)) {
			ended = true;	// What was decoded stays
			break;
		}
	}
	flushRows(tj);
	tj->done = tj->qoi.complete() || ended;
}

// --- The flip -------------------------------------------------------------------------------

static void squashFlush(Job *j) {
	Squash *s = &j->squash;
	if (s->strip == 0) {
		return;
	}
	tfts->drawPanelRows(s->tube, 0, (int16_t)(s->y0 + s->next - s->strip), TFT_WIDTH, s->strip, j->rows, s->shade);
	s->strip = 0;
}

// The next row of the half picture: drawn on each screen row it lands on (at most one)
static void squashRow(Job *j, const uint8_t *row) {
	Squash *s = &j->squash;
	while (s->next < s->n && (int32_t)s->next * HINGE / s->n == s->src) {
		// Shaded as it's drawn, see squashFlush()
		memcpy(j->rows + (size_t)s->strip * TFT_WIDTH * 2, row, TFT_WIDTH * 2);
		s->strip++;
		s->next++;
		if (s->strip == STRIP_ROWS) {
			squashFlush(j);
		}
	}
	s->src++;
}

static void squashRowOut(void *ctx, const uint8_t *row) {	// QoiStream::rowOutput()
	squashRow((Job*)ctx, row);
}

static void squashBegin(Job *j, uint8_t tube, int16_t y0, int16_t n, uint16_t shade) {
	Squash *s = &j->squash;
	s->tube = tube;
	s->y0 = y0;
	s->n = n;
	s->next = 0;
	s->src = 0;
	s->shade = shade;
	s->strip = 0;
}

// Feeds QOI bytes to the scratch decoder until the fold is drawn, up to `limit` pixels in all
static void squashFeed(Job *j, const uint8_t *data, size_t len, uint32_t limit) {
	size_t off = 0;
	while (off < len && j->squash.next < j->squash.n && j->scratch.pixels() < limit) {
		size_t n = len - off < BOUNCE ? len - off : BOUNCE;	// Small steps, to stop soon after the last row
		QoiStream::Result res;
		size_t used = j->scratch.feedUntil(data + off, n, limit, NULL, NULL, &res);
		off += used;
		if (res != QoiStream::OK || used == 0) {
			break;
		}
	}
}

// Folds the old top half: the kept picture, squashed into the h rows above the hinge
static void foldOld(Job *j, uint8_t tube, int16_t h, uint16_t shade) {
	squashBegin(j, tube, (int16_t)(HINGE - h), h, shade);
	j->scratch.begin(TFT_WIDTH, TFT_HEIGHT);
	j->scratch.rowOutput(j->squash.row, squashRowOut, j);
	const volatile uint32_t *src = keepArena + (size_t)tube * (KEEP_SLOT / 4);
	size_t len = kept[tube].len;
	uint32_t limit = (uint32_t)HINGE * TFT_WIDTH;
	for (size_t off = 0; off < len && j->squash.next < h && j->scratch.pixels() < limit; off += BOUNCE) {
		size_t n = len - off < BOUNCE ? len - off : BOUNCE;
		for (size_t w = 0; w < (n + 3) / 4; w++) {
			uint32_t v = src[off / 4 + w];
			j->bounce[w * 4] = (uint8_t)v;
			j->bounce[w * 4 + 1] = (uint8_t)(v >> 8);
			j->bounce[w * 4 + 2] = (uint8_t)(v >> 16);
			j->bounce[w * 4 + 3] = (uint8_t)(v >> 24);
		}
		squashFeed(j, j->bounce, n, limit);
	}
	squashFlush(j);
}

// Folds the new bottom half: squashed into the h rows below the hinge. The tube's own decoder is
// paused at the hinge, so a copy of it carries on from there.
static void foldNew(Job *j, TubeJob *tj, int16_t h, uint16_t shade) {
	const Record *r = tj->rec;
	squashBegin(j, r->tube, HINGE, h, shade);
	if (r->format == QOI) {
		j->scratch = tj->qoi;
		j->scratch.rowOutput(j->squash.row, squashRowOut, j);
		squashFeed(j, r->data + tj->pos, r->len - tj->pos, (uint32_t)TFT_HEIGHT * TFT_WIDTH);
	} else if (r->format == RGB565) {
		for (int16_t y = HINGE; y < TFT_HEIGHT && j->squash.next < h; y++) {
			squashRow(j, r->data + (size_t)y * TFT_WIDTH * 2);
		}
	} else if (r->format == PATTERN) {
		for (int16_t y = HINGE; y < TFT_HEIGHT && j->squash.next < h; y++) {
			for (int16_t x = 0; x < TFT_WIDTH; x++) {
				uint16_t c = patternPixel(r->tube, (uint8_t)r->len, x, y);
				j->squash.row[x * 2] = (uint8_t)(c >> 8);
				j->squash.row[x * 2 + 1] = (uint8_t)(c & 0xFF);
			}
			squashRow(j, j->squash.row);
		}
	}
	squashFlush(j);
}

// A dark edge across the tube at row y, 2 rows, above the hinge: where the old top half would be
// folding, for a tube whose old picture wasn't kept
static void darkEdge(Job *j, uint8_t tube, int16_t y) {
	int16_t n = HINGE - y < 2 ? HINGE - y : 2;
	if (n <= 0) {
		return;
	}
	memset(j->rows, 0, (size_t)n * TFT_WIDTH * 2);
	tfts->drawPanelRows(tube, 0, y, TFT_WIDTH, n, j->rows);
}

// One frame of a tube's flip, t from 0 to 1. Returns true once it's done.
static bool flipFrame(Job *j, TubeJob *tj, float t) {
	const Record *r = tj->rec;
	if (!tj->flipStarted) {
		tj->flipStarted = true;
		tj->flipOld = keptShowing(r->tube);
		if (tj->flipOld) {
			lastFlipFolded++;
		}
		tj->flipLast = -1;
		tj->flipPhase = 1;
	}
	if (t > 1) {
		t = 1;
	}
	// The flap's angle from upright (0) to down (pi): slow to start, then faster, like it falls
	float angle = (float)M_PI * (0.6f * t * t + 0.4f * t);
	float c = cosf(angle);
	if (tj->flipPhase == 1 && c > 0) {
		// The top half: the old one folds down onto the hinge, uncovering the new one above it
		int16_t h = (int16_t)lroundf(HINGE * c);
		if (h == tj->flipLast) {
			return false;
		}
		tj->flipLast = h;
		lastFlipFrames++;
		advance(tj, (int16_t)(HINGE - h));
		if (tj->flipOld) {
			foldOld(j, r->tube, h, (uint16_t)(256 * (0.55f + 0.45f * c)));
		} else {
			darkEdge(j, r->tube, (int16_t)(HINGE - h));
		}
		return false;
	}
	if (tj->flipPhase == 1) {
		advance(tj, HINGE);		// All of the new top half
		tj->flipPhase = 2;
		tj->flipLast = -1;
	}
	// The bottom half: the new one folds down from the hinge over the old one
	int16_t h = t >= 1 ? HINGE : (int16_t)lroundf(HINGE * -c);
	if ((h != tj->flipLast || t >= 1) && h > 0) {	// The last frame always, unshaded
		tj->flipLast = h;
		lastFlipFrames++;
		foldNew(j, tj, h, t >= 1 ? 256 : (uint16_t)(256 * (0.55f + 0.45f * -c)));
	}
	if (t >= 1) {
		tj->rows = r->h;
		tj->done = true;
		return true;
	}
	return false;
}

// Whether a record can flip: the bottom half has to be drawn many times
static bool canFlip(const Record &r) {
	return r.format == QOI || r.format == RGB565 || r.format == PATTERN;
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
			finishTube(now);
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
				finishTube(ordered[k]);
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
				if (tj->done) {
					finishTube(tj);
					if (!j->test) {
						wholeTubeReplaced(tj->rec->tube);
						publish = true;
					}
				}
			}
			if (!allDone) {
				vTaskDelay(1);
			}
		}
	} else if (n > 0 && j->t.kind == FLIP) {
		// Each tube flips over durationMs, starting gapMs after the one before it
		lastFlipFolded = 0;
		lastFlipFrames = 0;
		uint32_t each = j->t.durationMs > 0 ? j->t.durationMs : 1;
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
				uint32_t start = (uint32_t)k * j->t.gapMs;
				if (elapsed < start) {
					continue;
				}
				if (!tfts->isPanelTube(tj->rec->tube)) {
					tj->done = true;
					continue;
				}
				bool finished;
				if (canFlip(*tj->rec)) {
					finished = flipFrame(j, tj, (float)(elapsed - start) / each);
				} else {
					finished = drawNow(tj);	// A JPEG is drawn whole, at its turn
				}
				if (finished && tj->done) {
					finishTube(tj);
					if (!j->test) {
						wholeTubeReplaced(tj->rec->tube);
						publish = true;
					}
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
			// The clock's loop holds the display buffer for a moment every time round: wait for that,
			// but not past takeBuffer()'s 50 ms, so it doesn't give up and use the heap meanwhile
			if (!holdingMutex && !GifPlayer::active() && xSemaphoreTake(memMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
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
		// IRAM (executable memory), not the heap: see Kept
		keepArena = (volatile uint32_t*)heap_caps_malloc(6 * KEEP_SLOT, MALLOC_CAP_EXEC);
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
	// Not while the standard build's weather fetch holds the display buffer (for TLS, seconds at a
	// time); the clock's task and MQTT only hold it for a moment, see CMD_RESERVE
	TaskHandle_t holder = xSemaphoreGetMutexHolder(memMutex);
	if (!GifPlayer::active() && (holder == NULL || holder != weatherTask)) {
		return DISPLAY_BUFFER;
	}
	return heapFree();
}

uint8_t *takeBuffer(size_t len, bool *retrySoon) {
	if (retrySoon != nullptr) {
		*retrySoon = false;
	}
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
			// Held for a moment by the clock's loop or MQTT (a brightness change blanks every tube,
			// for instance): the heap would do, but then up to 24 KB of it goes for nothing. Only
			// the standard build's weather fetch holds it for long, and then the heap it is.
			TaskHandle_t holder = xSemaphoreGetMutexHolder(memMutex);
			if (retrySoon != nullptr && !GifPlayer::active() && (holder == NULL || holder != weatherTask)) {
				*retrySoon = true;
				return NULL;
			}
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
		: t.kind == CASCADE ? (uint32_t)t.gapMs * (t.orderLen > 0 ? t.orderLen - 1 : 0)
		: t.kind == FLIP ? t.durationMs + (uint32_t)t.gapMs * (t.orderLen > 0 ? t.orderLen - 1 : 0) : 0;
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
		: t.kind == CASCADE ? (uint32_t)t.gapMs * (t.orderLen > 0 ? t.orderLen - 1 : 0)
		: t.kind == FLIP ? t.durationMs + (uint32_t)t.gapMs * (t.orderLen > 0 ? t.orderLen - 1 : 0) : 0;
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

uint8_t lastFlipFoldedTubes() {
	return lastFlipFolded;
}

uint16_t lastFlipFrameCount() {
	return lastFlipFrames;
}


void interrupt() {
	if (running) {
		stopRequested = true;
	}
}

} // namespace PanelDraw
