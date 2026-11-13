#ifndef PANEL_DRAW_H
#define PANEL_DRAW_H

#include <stddef.h>
#include <stdint.h>

/*
 * Draws a POST /panel/batch that has arrived whole, on a task of its own.
 *
 * Decoding while the body arrives keeps the web server's task, which also handles everything else
 * on the network, busy for tens of milliseconds at a time, and packets for the upload get lost
 * meanwhile: each loss costs a retransmission timeout of 0.2 s or more. So the body is only copied
 * into memory as it arrives (tens of milliseconds for a whole screen), and this task decodes and
 * draws it. The web server's task stays free, and the reply goes out as soon as the body is in.
 *
 * Because the whole batch is in memory, the task can also play a transition with it, at the
 * displays' own speed and with exact timing:
 *   wipe      the tubes are drawn from the top down, each starting a little after the one before
 *             it in `order`, the whole wipe taking `durationMs`
 *   cascade   the tubes are drawn one after another in `order`, `gapMs` apart
 * Records that aren't a whole tube of a tube in `order` are drawn first, straight away.
 *
 * Records: a 14-byte little-endian header (tube, format 0 = rgb565 / 1 = qoi / 2 = jpeg, x, y, w,
 * h, length), then the picture, exactly as POST /panel takes it.
 */
namespace PanelDraw {
	enum Kind : uint8_t { NONE, WIPE, CASCADE };

	struct Transition {
		Kind kind = NONE;
		uint16_t durationMs = 500;	// wipe: the whole wipe
		uint16_t gapMs = 90;		// cascade: between the tubes
		uint8_t order[6] = { 0, 1, 2, 3, 4, 5 };
		uint8_t orderLen = 6;
	};

	static const size_t HEADER = 14;
	// A batch is held in the display buffer (64.8 KB, no heap) unless GIFs are using it; then on
	// the heap, at most this big and leaving this much heap free
	static const size_t MAX_BUFFER = 24 * 1024;
	static const size_t HEAP_HEADROOM = 24 * 1024;

	void begin();	// Starts the task. Call after the displays are up.

	// For the web server's task. bufferFree() is the largest batch that can be held right now (0
	// while one is being drawn): 64800 bytes in the display buffer, less from the heap while GIFs play.
	size_t bufferFree();
	uint8_t *takeBuffer(size_t len);	// NULL when it can't be held now
	void freeBuffer(uint8_t *buf);

	// Checks the records in buf and hands them to the task, which frees buf when it's done.
	// Returns NULL, or why not (buf is then still the caller's), with the HTTP status in *code.
	const char *start(uint8_t *buf, size_t len, const Transition &t, int *code);

	// Draws a test pattern on every panel tube, made on the clock (no upload), with the transition:
	// for trying transitions and checking the tubes without Home Assistant. NULL or why not.
	const char *startTest(const Transition &t, int *code);

	bool busy();			// A batch is being drawn
	uint32_t msLeft();		// Roughly how long until it's done
	uint32_t lastDurationMs();	// How long the last batch took to draw, transition included
	void interrupt();		// Stop the batch being drawn: its tubes not yet drawn are marked needed

	// A whole tube was replaced: puts the status message back on top of it and marks it as done
	void wholeTubeReplaced(uint8_t tube);
}

#endif // PANEL_DRAW_H
