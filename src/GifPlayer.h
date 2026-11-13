#ifndef GIF_PLAYER_H
#define GIF_PLAYER_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Animated GIFs stored on the clock (LittleFS /gif/) and played on panel tubes by the clock itself,
 * so an animation costs no Wi-Fi traffic while it plays. Uses bitbank2's AnimatedGIF, which decodes
 * line by line, so no frame buffer is needed.
 *
 * Memory: each player's decoder state (~25 KB) lives in a slot of StaticSprite::output_buffer, the
 * 64.8 KB buffer that only the clock's own faces, the screen saver and TLS use - none of which run
 * while the tubes show panels. So at most GIF_SLOTS GIFs play at once and they take no heap. The
 * main loop, holding memMutex, calls stopAllForDisplay() before it draws anything else into that
 * buffer, and every frame is decoded holding memMutex too.
 */
namespace GifPlayer {
	static const uint8_t GIF_SLOTS = 2;

	void begin();	// Starts the player task. Call after LittleFS and the displays are up.

	// Starts playing /gif/<name> on a panel tube. x/y < 0 = centred. Returns NULL or why not.
	const char *play(uint8_t tube, const char *name, int16_t x, int16_t y, bool loop, uint16_t bg565);
	void stop(uint8_t tube);	// The last frame stays on the tube
	void stopAll();
	// For the main loop, already holding memMutex: stop everything because the display buffer is
	// about to be used for something else
	void stopAllForDisplay();

	bool active();				// Any GIF playing
	uint8_t playingMask();		// Bit i set when tube i plays a GIF
	bool playingFile(const char *name);	// Some tube plays this file

	// Storage
	bool validName(const char *name);	// [A-Za-z0-9._-]{1,40}.gif
	size_t freeBytes();					// What's left for GIFs, keeping a reserve for faces and icons
	String listJson();					// {"files":[{"name":..,"size":..}],"free":..}
	bool remove(const char *name);		// Stops tubes playing it first

	// POST /gif?name=<name>: the body arrives in pieces. Each returns NULL or why it failed (then
	// the upload is abandoned and its temporary file removed).
	const char *uploadBegin(const char *name, size_t total);
	const char *uploadWrite(const uint8_t *data, size_t len);
	const char *uploadEnd();
	void uploadAbort();
}

#endif // GIF_PLAYER_H
