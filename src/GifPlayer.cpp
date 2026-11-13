#include <Arduino.h>
#include <LittleFS.h>
#include <new>
#include <AnimatedGIF.h>

#include "GifPlayer.h"
#include "IPSClock.h"
#include "OTA.h"
#include "ScreenSaver.h"
#include "TFTs.h"

extern TFTs *tfts;
extern ScreenSaver *screenSaver;
extern SemaphoreHandle_t memMutex;

#define GIF_DIR "/gif"
#define GIF_MAX_FILE (600 * 1024)		// One GIF
#define GIF_FS_RESERVE (300 * 1024)		// Always left free on LittleFS for faces and icons
#define GIF_MIN_DELAY 20				// ms between frames, whatever the file says

// Each player's decoder lives in one slot of the display buffer, see GifPlayer.h
#define GIF_SLOT_SIZE ((sizeof(StaticSprite::output_buffer) / GifPlayer::GIF_SLOTS) & ~(size_t)7)
static_assert(sizeof(AnimatedGIF) <= GIF_SLOT_SIZE, "An AnimatedGIF doesn't fit a slot of the display buffer");

namespace {

struct Player {
	bool active = false;
	uint8_t tube = 0;
	int16_t x = 0, y = 0;		// Where the GIF's canvas starts on the tube
	bool loop = true;
	uint32_t nextAt = 0;		// millis() when the next frame is due
	AnimatedGIF *gif = NULL;	// In the slot's part of the display buffer
	char name[48] = "";
};

Player players[GifPlayer::GIF_SLOTS];
SemaphoreHandle_t stateMutex = NULL;	// Guards players[]; the task holds it while it plays a frame

// Upload in progress (POST /gif)
File uploadFile;
char uploadName[48] = "";
size_t uploadTotal = 0;
size_t uploadDone = 0;
uint8_t uploadHead[10];		// "GIF8xa" + logical screen width and height, checked once all are in

AnimatedGIF *slotGif(uint8_t slot) {
	return (AnimatedGIF*)(StaticSprite::output_buffer + (size_t)slot * GIF_SLOT_SIZE);
}

void *gifOpen(const char *path, int32_t *size) {
	File *f = new File(LittleFS.open(path, "r"));
	if (f == NULL || !*f) {
		delete f;
		return NULL;
	}
	*size = (int32_t)f->size();
	return f;
}

void gifClose(void *handle) {
	File *f = (File*)handle;
	if (f != NULL) {
		f->close();
		delete f;
	}
}

int32_t gifRead(GIFFILE *file, uint8_t *buf, int32_t len) {
	File *f = (File*)file->fHandle;
	int32_t left = file->iSize - file->iPos;
	if (len > left) {
		len = left;
	}
	if (len <= 0) {
		return 0;
	}
	int32_t n = (int32_t)f->read(buf, (size_t)len);
	file->iPos = (int32_t)f->position();
	return n;
}

int32_t gifSeek(GIFFILE *file, int32_t position) {
	File *f = (File*)file->fHandle;
	f->seek((uint32_t)position);
	file->iPos = (int32_t)f->position();
	return file->iPos;
}

// Draws one decoded line, skipping transparent pixels so what's behind them stays
void gifDraw(GIFDRAW *draw) {
	Player *p = (Player*)draw->pUser;
	int16_t y = (int16_t)(p->y + draw->iY + draw->y);
	if (y < 0 || y >= TFT_HEIGHT) {
		return;
	}
	int16_t x0 = (int16_t)(p->x + draw->iX);
	int16_t start = x0 < 0 ? (int16_t)-x0 : 0;
	int16_t end = (int16_t)draw->iWidth;
	if (x0 + end > TFT_WIDTH) {
		end = (int16_t)(TFT_WIDTH - x0);
	}

	uint16_t line[TFT_WIDTH];	// GIF_PALETTE_RGB565_BE: the bytes are already big-endian in memory
	const uint8_t *px = draw->pPixels;
	const uint16_t *palette = draw->pPalette;
	bool transparency = draw->ucHasTransparency != 0;
	int16_t i = start;
	while (i < end) {
		if (transparency && px[i] == draw->ucTransparent) {
			i++;
			continue;
		}
		int16_t run = i;
		int16_t n = 0;
		while (i < end && !(transparency && px[i] == draw->ucTransparent)) {
			line[n++] = palette[px[i]];
			i++;
		}
		tfts->drawPanelRows(p->tube, (int16_t)(x0 + run), y, n, 1, (const uint8_t*)line);
	}
}

// Holding stateMutex. The last frame stays on the tube.
void stopLocked(Player &p) {
	if (!p.active) {
		return;
	}
	p.gif->close();
	p.gif->~AnimatedGIF();
	p.gif = NULL;
	p.active = false;
	p.name[0] = 0;
}

bool playAllowed() {
	return IPSClock::getTimeOrDate().value == IPSClock::PANELS && !OTA::otaBusy() && !OTA::otaRestartPending()
		&& (screenSaver == NULL || !screenSaver->isOn());
}

void gifTask(void *param) {
	(void)param;
	for (;;) {
		uint32_t wait = 50;
		if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
			bool allowed = playAllowed();
			for (uint8_t i = 0; i < GifPlayer::GIF_SLOTS; i++) {
				Player &p = players[i];
				if (!p.active) {
					continue;
				}
				if (!allowed || !tfts->isPanelTube(p.tube)) {
					stopLocked(p);
					continue;
				}
				int32_t due = (int32_t)(p.nextAt - millis());
				if (due > 0) {
					if ((uint32_t)due < wait) {
						wait = (uint32_t)due;
					}
					continue;
				}
				// The decoder's state is in the display buffer, which the main loop uses holding memMutex
				if (xSemaphoreTake(memMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
					wait = 5;
					continue;
				}
				int delayMs = 0;
				int result = p.gif->playFrame(false, &delayMs, &p);
				xSemaphoreGive(memMutex);
				if (result < 0) {
					stopLocked(p);
					continue;
				}
				if (result == 0) {		// That was the last frame
					if (p.loop) {
						p.gif->reset();
					} else {
						stopLocked(p);
						continue;
					}
				}
				p.nextAt = millis() + (uint32_t)(delayMs < GIF_MIN_DELAY ? GIF_MIN_DELAY : delayMs);
			}
			xSemaphoreGive(stateMutex);
		}
		vTaskDelay(pdMS_TO_TICKS(wait == 0 ? 1 : wait));
	}
}

void clearTube(uint8_t tube, uint16_t bg565) {
	uint8_t row[TFT_WIDTH * 2];
	for (uint16_t i = 0; i < TFT_WIDTH; i++) {
		row[i * 2] = (uint8_t)(bg565 >> 8);
		row[i * 2 + 1] = (uint8_t)(bg565 & 0xFF);
	}
	for (int16_t y = 0; y < TFT_HEIGHT; y++) {
		tfts->drawPanelRows(tube, 0, y, TFT_WIDTH, 1, row);
	}
}

String gifPath(const char *name) {
	return String(GIF_DIR "/") + name;
}

} // namespace

namespace GifPlayer {

void begin() {
	stateMutex = xSemaphoreCreateMutex();
	if (!LittleFS.exists(GIF_DIR)) {
		LittleFS.mkdir(GIF_DIR);
	}
	xTaskCreatePinnedToCore(gifTask, "gif", 6144, NULL, 1, NULL, 1);
}

const char *play(uint8_t tube, const char *name, int16_t x, int16_t y, bool loop, uint16_t bg565) {
	if (tube >= NUM_DIGITS) {
		return "tube must be 0 to 5";
	}
	if (!validName(name)) {
		return "bad GIF name";
	}
	if (!playAllowed() || !tfts->isPanelTube(tube)) {
		return "not a panel tube";
	}
	String path = gifPath(name);
	if (!LittleFS.exists(path)) {
		return "no such GIF";
	}

	const char *result = NULL;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	// The same tube's player is reused; otherwise a free slot
	int slot = -1;
	for (uint8_t i = 0; i < GIF_SLOTS; i++) {
		if (players[i].active && players[i].tube == tube) {
			stopLocked(players[i]);
			slot = i;
			break;
		}
	}
	for (uint8_t i = 0; slot < 0 && i < GIF_SLOTS; i++) {
		if (!players[i].active) {
			slot = i;
		}
	}
	if (slot < 0) {
		xSemaphoreGive(stateMutex);
		return "two GIFs are already playing";
	}

	if (xSemaphoreTake(memMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
		xSemaphoreGive(stateMutex);
		return "the display buffer is busy, try again";
	}
	Player &p = players[slot];
	p.gif = new (slotGif((uint8_t)slot)) AnimatedGIF();
	p.gif->begin(GIF_PALETTE_RGB565_BE);
	if (!p.gif->open(path.c_str(), gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
		p.gif->~AnimatedGIF();
		p.gif = NULL;
		result = "not a GIF the clock can play";
	} else if (p.gif->getCanvasWidth() > TFT_WIDTH || p.gif->getCanvasHeight() > TFT_HEIGHT) {
		p.gif->close();
		p.gif->~AnimatedGIF();
		p.gif = NULL;
		result = "GIF must be at most 135x240";
	} else {
		p.tube = tube;
		p.loop = loop;
		p.x = x >= 0 ? x : (int16_t)((TFT_WIDTH - p.gif->getCanvasWidth()) / 2);
		p.y = y >= 0 ? y : (int16_t)((TFT_HEIGHT - p.gif->getCanvasHeight()) / 2);
		strlcpy(p.name, name, sizeof(p.name));
		clearTube(tube, bg565);
		tfts->clearPanelNeeded(tube);
		p.nextAt = millis();
		p.active = true;
	}
	xSemaphoreGive(memMutex);
	xSemaphoreGive(stateMutex);
	return result;
}

void stop(uint8_t tube) {
	if (stateMutex == NULL) {
		return;
	}
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	for (uint8_t i = 0; i < GIF_SLOTS; i++) {
		if (players[i].active && players[i].tube == tube) {
			stopLocked(players[i]);
		}
	}
	xSemaphoreGive(stateMutex);
}

void stopAll() {
	if (stateMutex == NULL) {
		return;
	}
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	for (uint8_t i = 0; i < GIF_SLOTS; i++) {
		stopLocked(players[i]);
	}
	xSemaphoreGive(stateMutex);
}

void stopAllForDisplay() {
	// The caller holds memMutex. The task waits at most 20 ms for memMutex while holding stateMutex,
	// so taking stateMutex here can't deadlock: it just waits for that timeout.
	if (stateMutex == NULL || !active()) {
		return;
	}
	stopAll();
}

bool active() {
	for (uint8_t i = 0; i < GIF_SLOTS; i++) {
		if (players[i].active) {
			return true;
		}
	}
	return false;
}

uint8_t playingMask() {
	uint8_t mask = 0;
	for (uint8_t i = 0; i < GIF_SLOTS; i++) {
		if (players[i].active) {
			mask = (uint8_t)(mask | (1 << players[i].tube));
		}
	}
	return mask;
}

bool playingFile(const char *name) {
	for (uint8_t i = 0; i < GIF_SLOTS; i++) {
		if (players[i].active && strcmp(players[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

bool validName(const char *name) {
	size_t len = strlen(name);
	if (len < 5 || len > 44 || strcmp(name + len - 4, ".gif") != 0) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
			return false;
		}
	}
	return name[0] != '.';
}

size_t freeBytes() {
	size_t total = LittleFS.totalBytes();
	size_t used = LittleFS.usedBytes();
	size_t left = total > used ? total - used : 0;
	return left > GIF_FS_RESERVE ? left - GIF_FS_RESERVE : 0;
}

String listJson() {
	String json = "{\"files\":[";
	File dir = LittleFS.open(GIF_DIR);
	bool first = true;
	if (dir && dir.isDirectory()) {
		File f = dir.openNextFile();
		while (f) {
			String name = f.name();
			int slash = name.lastIndexOf('/');
			if (slash >= 0) {
				name = name.substring((unsigned int)(slash + 1));
			}
			if (!f.isDirectory() && name.endsWith(".gif")) {
				json += String(first ? "" : ",") + "{\"name\":\"" + name + "\",\"size\":" + String((unsigned long)f.size()) + "}";
				first = false;
			}
			f = dir.openNextFile();
		}
	}
	json += "],\"free\":" + String((unsigned long)freeBytes()) + "}";
	return json;
}

bool remove(const char *name) {
	if (!validName(name)) {
		return false;
	}
	if (stateMutex != NULL) {
		xSemaphoreTake(stateMutex, portMAX_DELAY);
		for (uint8_t i = 0; i < GIF_SLOTS; i++) {
			if (players[i].active && strcmp(players[i].name, name) == 0) {
				stopLocked(players[i]);
			}
		}
		xSemaphoreGive(stateMutex);
	}
	return LittleFS.remove(gifPath(name));
}

const char *uploadBegin(const char *name, size_t total) {
	uploadAbort();
	if (!validName(name)) {
		return "bad GIF name: letters, digits, . _ - and ending .gif";
	}
	if (total > GIF_MAX_FILE) {
		return "GIF too big (at most 600 KB)";
	}
	size_t existing = 0;
	File old = LittleFS.open(gifPath(name), "r");
	if (old) {
		existing = old.size();	// Replacing a file frees its space
		old.close();
	}
	if (total > freeBytes() + existing) {
		return "not enough space on the clock";
	}
	strlcpy(uploadName, name, sizeof(uploadName));
	uploadFile = LittleFS.open(gifPath(name) + ".tmp", "w");
	if (!uploadFile) {
		return "can't create the file";
	}
	uploadTotal = total;
	uploadDone = 0;
	return NULL;
}

const char *uploadWrite(const uint8_t *data, size_t len) {
	if (!uploadFile) {
		return "no upload in progress";
	}
	// Check the header once its first 10 bytes are in
	for (size_t i = 0; i < len && uploadDone + i < sizeof(uploadHead); i++) {
		uploadHead[uploadDone + i] = data[i];
	}
	if (uploadDone < sizeof(uploadHead) && uploadDone + len >= sizeof(uploadHead)) {
		if (memcmp(uploadHead, "GIF87a", 6) != 0 && memcmp(uploadHead, "GIF89a", 6) != 0) {
			uploadAbort();
			return "not a GIF";
		}
		uint16_t w = (uint16_t)(uploadHead[6] | (uploadHead[7] << 8));
		uint16_t h = (uint16_t)(uploadHead[8] | (uploadHead[9] << 8));
		if (w > TFT_WIDTH || h > TFT_HEIGHT) {
			uploadAbort();
			return "GIF must be at most 135x240";
		}
	}
	if (uploadFile.write(data, len) != len) {
		uploadAbort();
		return "writing the file failed (full?)";
	}
	uploadDone += len;
	return NULL;
}

const char *uploadEnd() {
	if (!uploadFile) {
		return "no upload in progress";
	}
	uploadFile.close();
	if (uploadDone != uploadTotal || uploadDone < sizeof(uploadHead)) {
		LittleFS.remove(gifPath(uploadName) + ".tmp");
		return "the upload was incomplete";
	}
	String path = gifPath(uploadName);
	remove(uploadName);	// Replacing: stops tubes playing the old one
	if (!LittleFS.rename(path + ".tmp", path)) {
		LittleFS.remove(path + ".tmp");
		return "can't save the file";
	}
	return NULL;
}

void uploadAbort() {
	if (uploadFile) {
		uploadFile.close();
		LittleFS.remove(gifPath(uploadName) + ".tmp");
	}
}

} // namespace GifPlayer
