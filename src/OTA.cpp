#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "OTA.h"
#include "TFTs.h"

// Set by anything that starts an update, read by the clock task. The web server's task and the MQTT
// download task are the only writers, and only one update runs at a time, so a mutex isn't needed.
extern TFTs *tfts;
extern void requestMqttPublish();

#define OTA_ERROR_LEN 96
#define OTA_RESTART_DELAY 1000	// ms to show "Updated, restarting" before the restart

namespace OTA {

static State state = IDLE;
static char lastError[OTA_ERROR_LEN] = "";
static bool restartPending = false;
static uint32_t restartAt = 0;
static size_t written = 0;
static size_t total = 0;	// 0 when unknown
static int lastShownPercent = -1;
static uint32_t lastMqttPublish = 0;

static void setError(const char *error) {
	Update.abort();
	state = FAILED;
	restartPending = false;
	strncpy(lastError, error, OTA_ERROR_LEN - 1);
	lastError[OTA_ERROR_LEN - 1] = 0;
	if (tfts != NULL) {
		String status = "Update failed: ";
		status += lastError;
		tfts->setStatus(status);
	}
	requestMqttPublish();
}

// Shows the progress on the tubes and tells Home Assistant, at most once a second and only when the
// percentage changed. Called from whichever task is writing the update.
static void progress() {
	int percent = total > 0 ? (int)(written * 100 / total) : -1;
	if (percent == lastShownPercent) {
		return;
	}
	lastShownPercent = percent;

	if (percent >= 0 && tfts != NULL) {
		String status = "Updating ";
		status += percent;
		status += "%";
		tfts->setStatus(status);
	}
	if (millis() - lastMqttPublish >= 1000) {
		lastMqttPublish = millis();
		requestMqttPublish();
	}
}

bool otaAvailable() {
	return esp_ota_get_next_update_partition(NULL) != NULL;
}

bool otaBegin(size_t size, const char *md5, bool filesystem) {
	if (state == RUNNING) {
		return false;
	}
	if (!filesystem && !otaAvailable()) {
		strncpy(lastError, "no second app slot", OTA_ERROR_LEN - 1);
		lastError[OTA_ERROR_LEN - 1] = 0;
		state = FAILED;
		return false;
	}

	if (md5 != NULL && md5[0] != 0) {
		Update.setMD5(md5);
	}
	if (filesystem) {
		// The file system image replaces the whole LittleFS partition, so stop using it first.
		// Anything that tries to open a file until the restart just gets nothing.
		LittleFS.end();
	}
	if (!Update.begin(size > 0 ? size : UPDATE_SIZE_UNKNOWN, filesystem ? U_SPIFFS : U_FLASH)) {
		setError(Update.errorString());
		return false;
	}

	state = RUNNING;
	restartPending = false;
	lastError[0] = 0;
	written = 0;
	total = size;
	lastShownPercent = -1;
	lastMqttPublish = 0;
	requestMqttPublish();
	return true;
}

bool otaWrite(const uint8_t *data, size_t len) {
	if (state != RUNNING) {
		return false;
	}
	// Update.write() takes a non-const pointer but only reads the data
	if (Update.write(const_cast<uint8_t *>(data), len) != len) {
		setError(Update.errorString());
		return false;
	}
	written += len;
	progress();
	return true;
}

bool otaEnd() {
	if (state != RUNNING) {
		return state == SUCCEEDED;
	}
	// When the size wasn't known Update.begin() reserved the whole slot, so end(true) accepts the
	// shorter image; with a known size end() fails a short one
	if (!Update.end(total == 0)) {
		setError(Update.errorString());
		return false;
	}
	if (Update.hasError()) {
		setError(Update.errorString());
		return false;
	}

	state = SUCCEEDED;
	restartPending = true;
	restartAt = millis();
	lastShownPercent = 100;
	if (tfts != NULL) {
		tfts->setStatus("Updated, restarting");
	}
	requestMqttPublish();
	return true;
}

// Also records an update that failed before it started, e.g. a download that couldn't be fetched, but
// never undoes one that has succeeded and is about to restart
void otaAbort(const char *why) {
	if (state == SUCCEEDED) {
		return;
	}
	setError(why != NULL ? why : "aborted");
}

void otaLoop() {
	if (restartPending && millis() - restartAt >= OTA_RESTART_DELAY) {
		ESP.restart();
	}
}

bool otaBusy() {
	return state == RUNNING;
}

int otaPercentage() {
	if (state != RUNNING) {
		return state == SUCCEEDED ? 100 : -1;
	}
	return total > 0 ? (int)(written * 100 / total) : -1;
}

const char *otaError() {
	return lastError;
}

bool otaRestartPending() {
	return restartPending;
}

}
