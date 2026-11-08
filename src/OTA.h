#ifndef ELEKSTUBE_OTA_H
#define ELEKSTUBE_OTA_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Firmware updates over Wi-Fi (OTA) into the other app slot, see partitions_8M.csv. One update at a
 * time, started either by POST /update in main.cpp or by the MQTT install command in mqttBroker.cpp.
 * The update itself is streamed with Update.write(), so the whole image is never in memory.
 */
namespace OTA {

enum State {
	IDLE,
	RUNNING,
	SUCCEEDED,
	FAILED
};

// True when the partition table has a second app slot to update into. 4 MB clocks have one slot.
bool otaAvailable();

// size 0 means the total size isn't known yet (UPDATE_SIZE_UNKNOWN). md5 may be NULL or empty,
// otherwise 32 hex characters. Only one update at a time: fails if one is already running.
bool otaBegin(size_t size, const char *md5);
bool otaWrite(const uint8_t *data, size_t len);
// Finishes the update. On success the restart is pending, see otaLoop().
bool otaEnd();
void otaAbort(const char *why);

// Called from the main loop: restarts the clock once an update has succeeded and been pending for a second.
void otaLoop();

bool otaBusy();             // An update is running
int otaPercentage();        // 0 to 100, or -1 when the total size isn't known
const char *otaError();     // The last error, empty when there wasn't one
bool otaRestartPending();

}

#endif
