#include <IPAddress.h>
#include <WiFi.h>
#include <AsyncWiFiManager.h>
#include <ArduinoJson.h>
#include <ConfigItem.h>
#include <EEPROMConfig.h>
#include <LittleFS.h>

#include <HTTPClient.h>
#include <WiFiClient.h>

#include "ScreenSaver.h"
#include "Backlights.h"
#include "mqttBroker.h"
#include "IPSClock.h"
#include "TFTs.h"
#include "OTA.h"

extern AsyncWiFiManager *wifiManager;
extern CompositeConfigItem rootConfig;
extern EEPROMConfig config;
extern ScreenSaver *screenSaver;
extern TFTs *tfts;
extern void broadcastUpdate(const BaseConfigItem&);
extern void filesChanged(const String &dir, const String &name, bool uploaded);
extern void requestMqttPublish();
extern void fsChanged();
extern size_t fsFreeBytes();
extern void requestMqttDiscovery();
extern IRAMPtrArray<const char*> manifest;
extern SemaphoreHandle_t memMutex;

IRAMPtrArray<const char*> MQTTBroker::displayStates {
    "Time",
    "Date",
#ifndef HA_ONLY	// No weather in the Home Assistant-only build
    "Weather",
#endif
    "Slideshow",
    "Panels",
    0
};

// Select options, indexed by the config item's value. "" is a value that can't be selected.
// Values match Backlights::patterns. 'Test' isn't offered by the web UI either.
static const char *const ledPatterns[] = { "Dark", "", "Constant", "Rainbow", "Pulse", "Breath", "Aurora", 0 };
// Values match IPSClock::Dimming
static const char *const offHoursModes[] = { "Blank", "Dim", "Matrix", 0 };
// Values match IPSClock::Display
#ifndef HA_ONLY
static const char *const digitCounts[] = { "Six", "Four AM/PM", "Four + Weather", "Four + Picture", "Four + Panel", 0 };
#else
static const char *const digitCounts[] = { "Six", "Four AM/PM", "", "Four + Picture", "Four + Panel", 0 };	// No weather
#endif
// Values match IPSClock::DateFormat
static const char *const dateFormats[] = { "DD-MM-YY", "MM-DD-YY", "YY-MM-DD", 0 };
#ifndef HA_ONLY
// The web UI's choices
static const char *const weatherUnits[] = { "Imperial", "Metric", 0 };
#endif

// A TEXT_SELECT item holds the option in lower case rather than its index, e.g. "metric" for "Metric".
// A COORDINATE item holds degrees as text.
enum SettingType { NUMBER, SELECT, SWITCH, TEXT_SELECT, COORDINATE };

// A config item that Home Assistant shows as an entity, and that can be set with ~/set/<item name>
struct Setting {
    SettingType type;
    const char *item;       // Also the entity's object id
    const char *group;      // The config group the item is in, see configSetRoot in main.cpp
    const char *name;
    const char *icon;
    bool config;            // Listed with the device's configuration rather than its controls
    int min;                // Numbers only, the same range as the web UI
    int max;
    const char *unit;
    const char *const *options;     // Selects only
};

// Const so it lives in flash, DRAM is full
static const Setting settings[] = {
    { SELECT, "led_pattern", "leds", "LED Pattern", "mdi:led-on", false, 0, 0, 0, ledPatterns },
    // Used by the Constant, Pulse and Breath patterns. Rainbow and Aurora cycle through the hues.
    { NUMBER, "led_hue", "leds", "LED Hue", "mdi:looks", false, 0, 255 },
    { NUMBER, "led_saturation", "leds", "LED Saturation", "mdi:palette", false, 0, 255 },
    { NUMBER, "led_value", "leds", "LED Brightness", "mdi:brightness-5", false, 0, 255 },
    // Used by the Rainbow, Pulse, Breath and Aurora patterns
    { NUMBER, "breath_per_min", "leds", "LED Speed", "mdi:speedometer", false, 1, 20 },
    // Used by the Aurora pattern
    { NUMBER, "hue_per_led", "leds", "LED Colors Per LED", "mdi:gradient-horizontal", true, 1, 10 },

    // Minutes, 0 is never
    { NUMBER, "screen_saver_delay", "matrix", "Screen Saver Delay", "mdi:timer-sand", false, 0, 120 },

    // The matrix animation, shown by the screen saver and outside the display hours
    { NUMBER, "matrix_hue", "matrix", "Matrix Hue", "mdi:looks", false, 0, 255 },
    { NUMBER, "matrix_saturation", "matrix", "Matrix Saturation", "mdi:palette", false, 0, 255 },
    { NUMBER, "matrix_value", "matrix", "Matrix Brightness", "mdi:brightness-5", false, 0, 255 },
    { SWITCH, "matrix_hue_cycling", "matrix", "Matrix Hue Cycling", "mdi:sync", false },
    { NUMBER, "matrix_hue_cycle_time", "matrix", "Matrix Hue Cycle Time", "mdi:timer-outline", true, 12, 1200, "s" },
    { NUMBER, "matrix_speed", "matrix", "Matrix Speed", "mdi:speedometer", true, 1, 50, "fps" },

    // Display hours, and what the clock shows outside them. On == off means always on.
    { NUMBER, "display_on", "clock", "Display On Hour", "mdi:clock-start", true, 0, 24, "h" },
    { NUMBER, "display_off", "clock", "Display Off Hour", "mdi:clock-end", true, 0, 24, "h" },
    { SELECT, "dimming", "clock", "Off Hours Mode", "mdi:weather-night", true, 0, 0, 0, offHoursModes },

    { SWITCH, "hour_format", "clock", "12 Hour Clock", "mdi:clock-time-twelve-outline", true },
    { SWITCH, "leading_zero", "clock", "Leading Zero", "mdi:numeric-0-box-outline", true },
    { SELECT, "four_digit_display", "clock", "Number of Digits", "mdi:numeric", true, 0, 0, 0, digitCounts },
    { SELECT, "date_format", "clock", "Date Format", "mdi:calendar", true, 0, 0, 0, dateFormats },
    // Seconds on each of time, date and weather, 0 stays on one
    { NUMBER, "display_cycle", "cycle", "Display Cycle Time", "mdi:autorenew", true, 0, 240, "s" },

#ifndef HA_ONLY
    // The color of monochrome weather icons
    { NUMBER, "weather_hue", "weather", "Weather Hue", "mdi:looks", true, 0, 255 },
    { NUMBER, "weather_saturation", "weather", "Weather Saturation", "mdi:palette", true, 0, 255 },
    { NUMBER, "weather_value", "weather", "Weather Brightness", "mdi:brightness-5", true, 0, 255 },

    // Changing these fetches the weather again. The API key has no entity because it would have to be published.
    { TEXT_SELECT, "units", "weather", "Weather Units", "mdi:thermometer", true, 0, 0, 0, weatherUnits },
    { COORDINATE, "weather_latitude", "weather", "Weather Latitude", "mdi:latitude", true, -90, 90, "°" },
    { COORDINATE, "weather_longitude", "weather", "Weather Longitude", "mdi:longitude", true, -180, 180, "°" },
#endif
};

#ifdef HA_ONLY
// The weather entities an earlier build published: removed from Home Assistant on every discovery
static const char *const removedWeatherEntities[][2] = {
    { "number", "weather_hue" }, { "number", "weather_saturation" }, { "number", "weather_value" },
    { "select", "units" }, { "number", "weather_latitude" }, { "number", "weather_longitude" },
    { "select", "weather_icons" }
};
#endif

// The directories under /ips whose files can be managed over MQTT, and the config item that selects one of them
struct FileSet {
    const char *dir;
    const char *item;
    const char *name;
    const char *icon;
};

static const FileSet fileSets[] = {
    { "faces", "clock_face", "Clock Face", "mdi:clock-digital" },
#ifndef HA_ONLY
    { "weather", "weather_icons", "Weather Icons", "mdi:weather-partly-cloudy" },
#endif
    { "slides", "slide_show", "Slide Show", "mdi:image-multiple" }
};

// Uploads are written here first so a failed upload doesn't destroy the file it was replacing
static const char *const uploadTempFile = "/ips/upload.tmp";

static const FileSet *findFileSet(const String &dir) {
    for (const FileSet &fileSet : fileSets) {
        if (dir == fileSet.dir) {
            return &fileSet;
        }
    }

    return 0;
}

MQTTBroker::MQTTBroker() {
    // AsyncMqttClient adds callbacks to a list rather than replacing them, so only do this once
    client.onConnect([this](bool sessionPresent) { this->onConnect(sessionPresent); });
#ifdef ASYNC_MQTT_HA_CLIENT
    client.onDisconnect([this](AsyncMqttClientDisconnectReason reason) { this->onDisconnect(reason); });
    client.onMessage([this](char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t length, size_t index, size_t total_length) {
        this->onMessage(topic, payload, properties, length, index, total_length); });
#else
    client.onDisconnect([this](espMqttClientTypes::DisconnectReason reason) { this->onDisconnect(reason); });
    client.onMessage([this](const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t*  payload, size_t length, size_t index, size_t total_length) {
        this->onMessage(properties, topic, payload, length, index, total_length); });
#endif
}

void MQTTBroker::onConnect(bool sessionPresent)
{
	reconnect = false;
//	Serial.println("Connected to MQTT");
    char topic[64];

    client.subscribe("homeassistant/status", 2);

    strcpy(topic, home);
    strcat(topic, "/set/#");
    client.subscribe(topic, 0);

    strcpy(topic, home);
    strcat(topic, "/files/+/+/+");
    client.subscribe(topic, 0);

    strcpy(topic, home);
    strcat(topic, "/ota/latest");
    client.subscribe(topic, 0);

    // Discovery needs a lot of memory so let the clock task do it, see sendHADiscoveryMessage()
    requestMqttDiscovery();
}

#ifdef ASYNC_MQTT_HA_CLIENT
void MQTTBroker::onDisconnect(AsyncMqttClientDisconnectReason reason)
#else
void MQTTBroker::onDisconnect(espMqttClientTypes::DisconnectReason reason)
#endif
{
//	Serial.printf("Disconnected from MQTT: %u\n", static_cast<uint8_t>(reason));

    reconnect = true;
    lastReconnect = millis();

    if (uploadFile) {
        // The connection dropped part way through an upload
        uploadFile.close();
        LittleFS.remove(uploadTempFile);
        fsChanged();
    }
}

#ifdef ASYNC_MQTT_HA_CLIENT
void MQTTBroker::onMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t length, size_t index, size_t total_length)
#else
void MQTTBroker::onMessage(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t*  payload, size_t length, size_t index, size_t total_length)
#endif
{
	uint8_t mqttMessageBuffer[32];

    // Files are streamed straight to the file system, so they can be much bigger than mqttMessageBuffer
    size_t homeLength = strlen(home);
    size_t filesLength = strlen(filesTopic + 1);
    if (strncmp(topic, home, homeLength) == 0 && strncmp(topic + homeLength, filesTopic + 1, filesLength) == 0) {
        onFileMessage(topic + homeLength + filesLength, (const uint8_t*)payload, length, index, total_length);
        return;
    }

		// payload is bigger then max: return chunked
	if (total_length >= sizeof(mqttMessageBuffer)) {
		Serial.printf("MQTT message too large (%d): topic=%s\n", total_length, topic);	// payload isn't null terminated
		return;
	}

	// add data and dispatch when done
	memcpy(&mqttMessageBuffer[index], payload, length);
	if (index + length == total_length) {
		// message is complete here
		mqttMessageBuffer[total_length] = 0;

        /*
         e.g. home = "/fred"
         topic = "/fred/topic"
         topicIndex = 5
         */
        int topicIndex = strlen(home);
        if (strcmp(topic, "homeassistant/status") == 0) {
            if (strcmp("online", (const char*)mqttMessageBuffer) == 0) {
                Serial.println("HA online");
                requestMqttDiscovery();
            }
        } else if (strcmp(topic + topicIndex, screenSaverTopic + 1) == 0) {
            if (strcmp((const char*)mqttMessageBuffer, "OFF") == 0) { 
                screenSaver->reset();
            } else {
                screenSaver->start();
            }
        } else if (strcmp(topic + topicIndex, brightnessTopic + 1) == 0) {
            setConfigNumber(IPSClock::getBrightnessConfig(), (const char*)mqttMessageBuffer, 10, 255);
        } else if (strcmp(topic + topicIndex, displayTopic + 1) == 0) {
            setConfigNumber(IPSClock::getTimeOrDate(), (const char*)mqttMessageBuffer, IPSClock::TIME, IPSClock::PANELS);
        } else if (strcmp(topic + topicIndex, customDataTopic + 1) == 0) {
            IPSClock::setCustomData((const char*)mqttMessageBuffer);	// Ignored if it is too long
        } else if (strcmp(topic + topicIndex, backlightStateTopic + 1) == 0) {
            if (strcmp((const char*)mqttMessageBuffer, "OFF") == 0) {
                Backlights::backlightState = false;
            } else {
                Backlights::backlightState = true;
            }
        } else if (strcmp(topic + topicIndex, backlightHSTopic + 1) == 0) {
            float hue, sat;
            int count;

            // The format string "%f,%f" tells sscanf to read a float, then a comma, then another float.
            count = sscanf((const char*)mqttMessageBuffer, "%f,%f", &hue, &sat);

            if (count == 2) {
                Backlights::backlightHue = hue * 255 / 360;
                Backlights::backlightSaturation = sat * 255 / 100;
            }
        } else if (strcmp(topic + topicIndex, backlightBrightnessTopic + 1) == 0) {
            Backlights::backlightBrightness = atoi((const char*)mqttMessageBuffer);
#if (NUM_LEDS > 6)
        } else if (strcmp(topic + topicIndex, underlightStateTopic + 1) == 0) {
            if (strcmp((const char*)mqttMessageBuffer, "OFF") == 0) {
                Backlights::backlightState = false;
            } else {
                Backlights::backlightState = true;
            }
        } else if (strcmp(topic + topicIndex, underlightHSTopic + 1) == 0) {
            float hue, sat;
            int count;

            // The format string "%f,%f" tells sscanf to read a float, then a comma, then another float.
            count = sscanf((const char*)mqttMessageBuffer, "%f,%f", &hue, &sat);

            if (count == 2) {
                Backlights::underlightHue = hue * 255 / 360;
                Backlights::underlightSaturation = sat * 255 / 100;
            }
        } else if (strcmp(topic + topicIndex, underlightBrightnessTopic + 1) == 0) {
            Backlights::underlightBrightness = atoi((const char*)mqttMessageBuffer);
#endif
        } else if (strcmp(topic + topicIndex, restartTopic + 1) == 0) {
            // A retained restart message would be redelivered on every connect, causing a reboot loop
            if (!properties.retain) {
                config.commit();
                ESP.restart();
            }
        } else if (strcmp(topic + topicIndex, otaLatestSub + 1) == 0) {
            onOtaLatest((const char*)mqttMessageBuffer);
        } else if (strcmp(topic + topicIndex, otaInstallTopicName + 1) == 0) {
            // A retained install message would be redelivered on every connect, causing an update loop
            if (!properties.retain && strcmp((const char*)mqttMessageBuffer, "install") == 0) {
                startOtaInstall();
            }
        } else if (strcmp(topic + topicIndex, screenSaverModeTopic + 1) == 0) {
            setConfigItem(ScreenSaver::getScreenSaver(), (const char*)mqttMessageBuffer);
        } else {
            setSetting(topic + topicIndex, (const char*)mqttMessageBuffer);
        }
	}

    requestMqttPublish();
}

// Same as a change from the web UI
void MQTTBroker::setConfigItem(BaseConfigItem &item, const char *value) {
    item.fromString(value);
    item.put();
    broadcastUpdate(item);
    item.notify();
}

// Keeps the number in range like the web UI does, the rest of the clock assumes it is
void MQTTBroker::setConfigNumber(BaseConfigItem &item, const char *value, int min, int max) {
    setConfigItem(item, String(constrain(atoi(value), min, max)).c_str());
}

// Ignores anything that isn't one of the options in lower case
void MQTTBroker::setConfigOption(BaseConfigItem &item, const char *value, const char *const *options) {
    for (int i = 0; options[i] != 0; i++) {
        String option(options[i]);
        option.toLowerCase();
        if (option == value) {
            setConfigItem(item, value);
            return;
        }
    }
}

// Ignores anything that isn't a number in range. The weather service puts it straight into the request URL.
void MQTTBroker::setConfigCoordinate(BaseConfigItem &item, const char *value, int min, int max) {
    char *end;
    double degrees = strtod(value, &end);
    if (end == value || *end != 0 || !isfinite(degrees) || degrees < min || degrees > max) {
        return;
    }

    // The entity's step, about 100 m. The longest, -180.000, fits in the 10 characters the item holds.
    char coordinate[12];
    snprintf(coordinate, sizeof(coordinate), "%.3f", degrees);
    setConfigItem(item, coordinate);
}

// topic looks like /set/<item name>, where the item is one of the settings or selects a file
void MQTTBroker::setSetting(const char *topic, const char *value) {
    const char *setPrefix = "/set/";
    size_t prefixLength = strlen(setPrefix);

    if (strncmp(topic, setPrefix, prefixLength) != 0) {
        return;
    }

    const char *name = topic + prefixLength;
    BaseConfigItem *item = rootConfig.get(name);
    if (item == 0) {
        return;
    }

    for (const FileSet &fileSet : fileSets) {
        if (strcmp(name, fileSet.item) == 0) {
            setConfigItem(*item, value);
            return;
        }
    }

    for (const Setting &setting : settings) {
        if (strcmp(name, setting.item) == 0) {
            if (setting.type == SWITCH) {
                setConfigItem(*item, value);    // "true", anything else is false
            } else if (setting.type == TEXT_SELECT) {
                setConfigOption(*item, value, setting.options);
            } else if (setting.type == COORDINATE) {
                setConfigCoordinate(*item, value, setting.min, setting.max);
            } else {
                int max = setting.max;
                if (setting.type == SELECT) {
                    for (max = 0; setting.options[max + 1] != 0; max++);
                }
                setConfigNumber(*item, value, setting.min, max);
            }
            return;
        }
    }
}

/*
 * Uploads or deletes a .tar.gz file in one of the file sets. path is <action>/<set>/<name>,
 * e.g. "upload/slides/solar" or "delete/faces/neon". Uploads arrive in chunks which are written
 * to a temporary file that replaces the real one once the whole file has been received.
 */
void MQTTBroker::onFileMessage(const char *path, const uint8_t *payload, size_t length, size_t index, size_t total_length) {
    if (index == 0) {
        startFileMessage(path, payload, length, total_length);
    }

    if (uploadFile) {
        if (uploadFile.write(payload, length) != length) {
            fileError = "write failed";
            uploadFile.close();
            LittleFS.remove(uploadTempFile);
        }
        fsChanged();
    }

    if (index + length < total_length) {
        return;     // Wait for the rest of the file
    }

    if (fileError == 0) {
        String fullName = "/ips/" + fileDir + "/" + fileName + ".tar.gz";

        if (uploadFile) {
            uploadFile.close();
            LittleFS.remove(fullName);
            if (!LittleFS.rename(uploadTempFile, fullName)) {
                fileError = "rename failed";
            }
        } else if (!LittleFS.remove(fullName)) {
            fileError = "file not found";
        }
        fsChanged();
    }

    publishFileStatus();

    if (fileError == 0) {
        filesChanged(fileDir, fileName, fileAction == "upload");
    }
}

void MQTTBroker::startFileMessage(const char *path, const uint8_t *payload, size_t length, size_t total_length) {
    if (uploadFile) {
        // The previous upload never finished
        uploadFile.close();
    }

    // The subscription only matches topics with exactly three levels after ~/files/
    String pathStr(path);
    int dirStart = pathStr.indexOf('/') + 1;
    int nameStart = pathStr.indexOf('/', dirStart) + 1;

    fileAction = pathStr.substring(0, dirStart - 1);
    fileDir = pathStr.substring(dirStart, nameStart - 1);
    fileName = pathStr.substring(nameStart);
    if (fileName.endsWith(".tar.gz")) {
        fileName.remove(fileName.length() - 7);
    }
    fileError = 0;

    if (findFileSet(fileDir) == 0) {
        fileError = "unknown set";
    } else if (!ImageUnpacker::isValidName(fileName)) {
        fileError = "invalid name";
    } else if (fileAction == "upload") {
        LittleFS.remove(uploadTempFile);    // In case an earlier upload was cut short by a reset
        fsChanged();    // Also means the space check below uses an up to date figure

        // Check for the gzip magic number
        if (total_length < 2 || payload[0] != 0x1f || (length > 1 && payload[1] != 0x8b)) {
            fileError = "not a .tar.gz file";
        } else if (total_length + 8192 > fsFreeBytes()) {
            fileError = "not enough space";
        } else {
            uploadFile = LittleFS.open(uploadTempFile, "w");
            if (!uploadFile) {
                fileError = "could not create file";
            }
        }
    } else if (fileAction != "delete") {
        fileError = "unknown action";
    }
}

void MQTTBroker::publishFileStatus() {
    JsonDocument doc;

    doc["action"] = fileAction;
    doc["set"] = fileDir;
    doc["name"] = fileName;
    doc["status"] = fileError == 0 ? "ok" : fileError;

    String payload;
    serializeJson(doc, payload);
    client.publish(fileStatusTopic, 1, false, payload.c_str());
}

bool MQTTBroker::init(const String& id) {
    this->id = id;

    client.disconnect();

    IPAddress ipAddress;
    if (ipAddress.fromString(getHost().value.c_str())) {
        sprintf(home, "clock/%s", id.c_str());
        sprintf(volatileStateTopic, "clock/%s/volatile/state", id.c_str());
        sprintf(persistentStateTopic, "clock/%s/persistent/state", id.c_str());
        sprintf(availabilityTopic, "clock/%s/availability", id.c_str());
        sprintf(fileStatusTopic, "clock/%s/files/status", id.c_str());
        sprintf(otaStateTopic, "clock/%s/ota/state", id.c_str());

        otaVersion[0] = 0;
        otaUrl[0] = 0;
        otaMd5[0] = 0;
        otaReleaseUrl[0] = 0;
        otaTitle[0] = 0;
        otaLatestReceived = false;

        client.setServer(getHost().value.c_str(), getPort());
        client.setCredentials(getUser().value.c_str(),getPassword().value.c_str());
        client.setWill(availabilityTopic, 2, true, "offline");
        screenSaver->setChangeCallback([this](bool isOff) { requestMqttPublish(); });
        
        reconnect = true;
    }

    return reconnect;
}

void MQTTBroker::connect() {
    if (WiFi.isConnected() && !wifiManager->isAP()) {
//        Serial.println("Connecting to MQTT...");
        client.connect();
    }
}

void MQTTBroker::checkConnection() {
    if (reconnect && (millis() - lastReconnect >= 2000)) {
        lastReconnect = millis();
        connect();
    }

    // The clock task turns a panel tube black when it needs a picture, and the web server task clears
    // the flag when one arrives over /panel, so watch the masks here and publish again when either
    // changes. Called every loop, so the state goes out within about a second.
    if (client.connected()) {
        uint8_t panelTubes = tfts->getPanelTubes();
        uint8_t panelsNeeded = tfts->getPanelsNeeded();
        if (panelTubes != lastPanelTubes || panelsNeeded != lastPanelsNeeded) {
            lastPanelTubes = panelTubes;
            lastPanelsNeeded = panelsNeeded;
            requestMqttPublish();
        }

        // The update's percentage changes while it runs, so publish the OTA state again, at most
        // once a second like the rest of the state
        if (OTA::otaBusy() && millis() - lastOtaStatePublish >= 1000) {
            lastOtaStatePublish = millis();
            publishOtaState();
        }
    }
}

// Returns false if the state couldn't be published right now and should be tried again later
bool MQTTBroker::publishState() {
    if (client.connected()) {
        // Takes a lot of memory, so try again later if the weather is being fetched
        if (xSemaphoreTake(memMutex, 0) == pdTRUE)
        {
            // AsyncMqttClient aborts if it can't allocate a packet, so leave plenty of room
            if (ESP.getMaxAllocHeap() < 12 * 1024) {
                xSemaphoreGive(memMutex);
                return false;
            }

            JsonDocument volatileState;
            volatileState["screen_saver_on"] = screenSaver->isOn() ? "ON" : "OFF";
            volatileState["brightness"] = IPSClock::getBrightnessConfig().value;
            volatileState["custom"] = IPSClock::getCustomData().value;
            volatileState["display"] = IPSClock::getTimeOrDate().value;
            // Bit i set when tube i (left to right) shows a streamed picture, and when it needs one
            volatileState["panel_tubes"] = tfts->getPanelTubes();
            volatileState["panels_needed"] = tfts->getPanelsNeeded();

            volatileState["backlight_state"] = Backlights::backlightState ? "ON" : "OFF";
            JsonArray blArray = volatileState["backlight_hs"].to<JsonArray>();
            blArray.add(360.0 * Backlights::backlightHue / 255.0);
            blArray.add(100.0 * Backlights::backlightSaturation / 255.0);
            volatileState["backlight_brightness"] = Backlights::backlightBrightness;
#if (NUM_LEDS > 6)
            volatileState["underlight_state"] = Backlights::backlightState ? "ON" : "OFF";  // backlight and underlight share the same state
            JsonArray ulArray = volatileState["underlight_hs"].to<JsonArray>();
            ulArray.add(360.0 * Backlights::underlightHue / 255.0);
            ulArray.add(100.0 * Backlights::underlightSaturation / 255.0);
            volatileState["underlight_brightness"] = Backlights::underlightBrightness;
#endif
            char buffer[384];
            serializeJson(volatileState, buffer);
            client.publish(volatileStateTopic, 1, false, buffer);

            // Leave out the MQTT credentials, the weather API key, the web UI login and the access point password, anyone subscribed to the broker can read this
            const char *excludes[] = { "mqtt", "weather", "web_login", "access_point", 0 };
            const char *weatherExcludes[] = { "weather_token", 0 };
            String persistentState;
            persistentState.reserve(1400);
            persistentState += "{";
            persistentState += rootConfig.toJSON(true, excludes);
#ifndef HA_ONLY
            persistentState += ",\"weather\":";
            persistentState += rootConfig.get("weather")->toJSON(false, weatherExcludes);
#else
            (void)weatherExcludes;	// No weather settings in this build
#endif
            persistentState += "}";
            client.publish(persistentStateTopic, 1, false, persistentState.c_str());

            xSemaphoreGive(memMutex);
        } else {
            // Serial.println("Gave up waiting for memory semaphore");
            return false;
        }

        publishOtaState();
    }

    return true;
}

/*
 * Called by the clock task. Building and queueing all the discovery messages takes a lot of
 * memory, so this returns false without doing anything if the weather is being fetched or
 * images are being unpacked, and should be tried again later.
 */
bool MQTTBroker::sendHADiscoveryMessage() {
    if (xSemaphoreTake(memMutex, 0) != pdTRUE) {
        return false;
    }

    deviceDescribed = false;

    JsonDocument doc;

    doc["name"] = "Screen Saver";
    doc["icon"] = "mdi:sleep";
    doc["stat_t"] = volatileStateTopic;
    doc["cmd_t"] = screenSaverTopic;
    doc["val_tpl"] = "{{value_json.screen_saver_on}}";

    publishDiscovery("switch", "screen_saver", doc, "screen_saver_");

    doc["name"] = "Screen Saver Mode";
    doc["icon"] = "mdi:matrix";
    doc["stat_t"] = "~/persistent/state";
    doc["cmd_t"] = screenSaverModeTopic;
    // The clock task treats anything other than 0 (ScreenSaver::BLANK) as Matrix
    doc["val_tpl"] = "{{ 'Blank' if value_json.matrix.screen_saver == 0 else 'Matrix' }}";
    doc["command_template"] = "{{ 0 if value == 'Blank' else 1 }}";

    JsonArray modeArray = doc["options"].to<JsonArray>();
    modeArray.add("Blank");
    modeArray.add("Matrix");

    publishDiscovery("select", "screen_saver_mode", doc);

    doc["name"] = "Brightness";
    doc["icon"] = "mdi:brightness-6";
    doc["stat_t"] = volatileStateTopic;
    doc["cmd_t"] = brightnessTopic;
    doc["min"] = 10;
    doc["max"] = 255;
    doc["val_tpl"] = "{{value_json.brightness}}";

    publishDiscovery("number", "brightness", doc);

    doc["name"] = "Custom Data";
    doc["icon"] = "mdi:clock-edit";
    doc["stat_t"] = volatileStateTopic;
    doc["cmd_t"] = customDataTopic;
    doc["val_tpl"] = "{{value_json.custom}}";
    doc["max"] = NUM_DIGITS;	// The clock ignores anything longer, so Home Assistant should refuse it

    publishDiscovery("text", "custom", doc, "custom-data");

    doc["name"] = "Backlight";
    doc["icon"] = "mdi:television-ambient-light";	// The LEDs behind the displays

    doc["cmd_t"] = backlightStateTopic;
    doc["stat_t"] = volatileStateTopic;
    doc["stat_val_tpl"] = "{{value_json.backlight_state}}";

    doc["bri_stat_t"] = volatileStateTopic;
    doc["bri_cmd_t"] = backlightBrightnessTopic;
    doc["bri_val_tpl"] = "{{value_json.backlight_brightness}}";
    doc["hs_stat_t"] = volatileStateTopic;
    doc["hs_cmd_t"] = backlightHSTopic;
    doc["hs_val_tpl"] = "{{value_json.backlight_hs | join(',')}}";
    doc["clrm"] = true;
    JsonArray blArray = doc["sup_clrm"].to<JsonArray>();
    blArray.add("hs");

    publishDiscovery("light", "backlight", doc);

#if (NUM_LEDS > 6)
    doc["name"] = "Underlight";
    doc["icon"] = "mdi:led-strip-variant";

    doc["cmd_t"] = underlightStateTopic;
    doc["stat_t"] = volatileStateTopic;
    doc["stat_val_tpl"] = "{{value_json.underlight_state}}";

    doc["bri_stat_t"] = volatileStateTopic;
    doc["bri_cmd_t"] = underlightBrightnessTopic;
    doc["bri_val_tpl"] = "{{value_json.underlight_brightness}}";
    doc["hs_stat_t"] = volatileStateTopic;
    doc["hs_cmd_t"] = underlightHSTopic;
    doc["hs_val_tpl"] = "{{value_json.underlight_hs | join(',')}}";
    doc["clrm"] = true;
    JsonArray ulArray = doc["sup_clrm"].to<JsonArray>();
    ulArray.add("hs");

    publishDiscovery("light", "underlight", doc);
#endif

    doc["name"] = "Display";
    doc["icon"] = "mdi:view-carousel-outline";

    doc["cmd_t"] = displayTopic;
    doc["stat_t"] = volatileStateTopic;
#ifndef HA_ONLY
    doc["val_tpl"] = "{% if value_json.display == 0 %}Time{% elif value_json.display == 1 %}Date{% elif value_json.display == 2 %}Weather{% elif value_json.display == 3 %}Slideshow{% elif value_json.display == 4 %}Panels{% endif %}";
    doc["command_template"] = "{% if value == 'Time' %}0{% elif value == 'Date' %}1{% elif value == 'Weather' %}2{% elif value == 'Slideshow' %}3{% elif value == 'Panels' %}4{% endif %}";
#else
    doc["val_tpl"] = "{% if value_json.display == 0 %}Time{% elif value_json.display == 1 %}Date{% elif value_json.display == 3 %}Slideshow{% elif value_json.display == 4 %}Panels{% endif %}";
    doc["command_template"] = "{% if value == 'Time' %}0{% elif value == 'Date' %}1{% elif value == 'Slideshow' %}3{% elif value == 'Panels' %}4{% endif %}";
#endif

    JsonArray optArray = doc["options"].to<JsonArray>();
    for (int i = 0; displayStates[i] != 0; i++) {
        optArray.add(displayStates[i]);
    }

    publishDiscovery("select", "display", doc);

    for (const Setting &setting : settings) {
        sendSettingDiscovery(setting, doc);
    }
#ifdef HA_ONLY
    doc.clear();
    for (const auto &entity : removedWeatherEntities) {
        publishDiscovery(entity[0], entity[1], doc);	// An empty config removes the entity
    }
#endif

    doc["name"] = "Restart";
    doc["cmd_t"] = restartTopic;
    doc["dev_cla"] = "restart";
    doc["ent_cat"] = "config";

    publishDiscovery("button", "restart", doc);

    // Home Assistant's update entity, fed by clock/<id>/ota/state and clock/<id>/set/ota_install
    doc["name"] = "Firmware";
    doc["cmd_t"] = otaInstallTopicName;
    doc["stat_t"] = otaStateTopic;
    doc["payload_install"] = "install";
    doc["dev_cla"] = "firmware";
    doc["ent_cat"] = "config";
    // No templates: Home Assistant reads installed_version, latest_version, title, release_url,
    // in_progress and update_percentage straight from the JSON on the state topic

    publishDiscovery("update", "firmware", doc, "firmware_");

    for (const FileSet &fileSet : fileSets) {
        sendFileDiscovery(fileSet.dir);
    }

    client.publish(availabilityTopic, 2, true, "online");

    xSemaphoreGive(memMutex);

    requestMqttPublish();

    return true;
}

void MQTTBroker::sendSettingDiscovery(const Setting &setting, JsonDocument &doc) {
    String value = String("value_json.") + setting.group + "." + setting.item;
    const char *component = "number";

    doc["name"] = setting.name;
    doc["icon"] = setting.icon;
    doc["stat_t"] = "~/persistent/state";
    doc["cmd_t"] = String("~/set/") + setting.item;
    if (setting.config) {
        doc["ent_cat"] = "config";
    }

    if (setting.type == NUMBER || setting.type == COORDINATE) {
        doc["min"] = setting.min;
        doc["max"] = setting.max;
        if (setting.unit != 0) {
            doc["unit_of_meas"] = setting.unit;
        }
        if (setting.type == COORDINATE) {
            // Home Assistant ignores the whole entity if the step is smaller than 0.001
            doc["step"] = 0.001;
            doc["mode"] = "box";
        }
        doc["val_tpl"] = String("{{") + value + "}}";
    } else if (setting.type == SWITCH) {
        component = "switch";
        doc["pl_on"] = "true";
        doc["pl_off"] = "false";
        doc["val_tpl"] = String("{{ 'true' if ") + value + " else 'false' }}";
    } else if (setting.type == TEXT_SELECT) {
        // The options are single capitalized words, so capitalize turns "metric" back into "Metric"
        component = "select";
        JsonArray optArray = doc["options"].to<JsonArray>();
        for (int i = 0; setting.options[i] != 0; i++) {
            optArray.add(setting.options[i]);
        }
        doc["val_tpl"] = String("{{ ") + value + " | capitalize }}";
        doc["command_template"] = "{{ value | lower }}";
    } else {
        // e.g. {{ ['Blank','Dim','Matrix'][value_json.clock.dimming] }} and {{ {'Blank':0,'Dim':1,'Matrix':2}[value] }}
        component = "select";
        String names;
        String values;
        JsonArray optArray = doc["options"].to<JsonArray>();
        for (int i = 0; setting.options[i] != 0; i++) {
            const char *option = setting.options[i];
            names += String(i > 0 ? "," : "") + "'" + option + "'";
            if (*option != 0) {
                optArray.add(option);
                values += String(values.length() > 0 ? "," : "") + "'" + option + "':" + i;
            }
        }
        doc["val_tpl"] = String("{{ [") + names + "][" + value + "] }}";
        doc["command_template"] = String("{{ {") + values + "}[value] }}";
    }

    publishDiscovery(component, setting.item, doc);
}

/*
 * Sends the discovery message for the select that picks a file from /ips/<dir>. Its options
 * are the files in that directory, so it is sent again whenever they change.
 */
void MQTTBroker::sendFileDiscovery(const String& dir) {
    const FileSet *fileSet = findFileSet(dir);

    if (fileSet == 0 || !client.connected()) {
        return;
    }

    JsonDocument doc;
    JsonArray optArray = doc["options"].to<JsonArray>();

    String dirName = "/ips/" + dir;
    fs::File dirFile = LittleFS.open(dirName);
    String name = dirFile.getNextFileName();
    while (name.length() > 0) {
        if (name.endsWith(".tar.gz")) {
            optArray.add(name.substring(dirName.length() + 1, name.length() - 7));
        }
        name = dirFile.getNextFileName();
    }
    dirFile.close();

    if (optArray.size() > 0) {
        doc["name"] = fileSet->name;
        doc["icon"] = fileSet->icon;
        doc["cmd_t"] = String("~/set/") + fileSet->item;
        doc["stat_t"] = "~/persistent/state";
        doc["val_tpl"] = String("{{value_json.faces.") + fileSet->item + "}}";
    } else {
        // A select has to have options, so remove it until there is something to select
        doc.clear();
    }

    publishDiscovery("select", fileSet->item, doc);
}

/*
 * Adds the fields every entity shares to doc, publishes it as the entity's discovery message
 * and clears doc ready for the next entity. An empty doc removes the entity.
 */
void MQTTBroker::publishDiscovery(const char *component, const char *objectId, JsonDocument &doc, const char *uniqueId) {
    char discoveryTopic[128];
    sprintf(discoveryTopic, "homeassistant/%s/%s/%s/config", component, id.c_str(), objectId);

    String payload;
    if (doc.size() > 0) {
        doc["~"] = home;
        doc["unique_id"] = String(uniqueId != 0 ? uniqueId : objectId) + id;
        doc["avty_t"] = availabilityTopic;
        doc["dev"]["identifiers"][0] = WiFi.macAddress();
        // Home Assistant only needs the device details once, the rest just say which device they belong to
        if (!deviceDescribed) {
            deviceDescribed = true;
            doc["dev"]["configuration_url"] = "http://" + WiFi.localIP().toString() + "/";
            doc["dev"]["name"] = manifest[3];
            doc["dev"]["model"] = manifest[0];
            doc["dev"]["sw_version"] = manifest[1];
        }

        serializeJson(doc, payload);
        doc.clear();
    }

    // QoS 0 because AsyncMqttClient only sends one QoS 1 message at a time, often waiting for its
    // next poll half a second later. All of the discovery messages would take many seconds to go,
    // holding their memory while the weather is fetched, and if the client runs out of memory part
    // way through a QoS 1 message it stops sending altogether. Discovery is sent again on every
    // connect and whenever Home Assistant comes online, so nothing is lost if one goes missing.
    //
    // Retained so Home Assistant has the entities as soon as it connects to the broker. It only says
    // it is online once it has started, too late for automations that run at startup. An empty
    // retained message removes the entity from the broker as well.
    client.publish(discoveryTopic, 0, true, payload.c_str());
}

// Copies a JSON string into a fixed buffer, ignoring it when it doesn't fit
static void copyOtaField(char *dest, size_t destSize, JsonVariant value) {
    if (value.isNull()) {
        dest[0] = 0;
        return;
    }
    const char *s = value.as<const char*>();
    if (s == 0 || strlen(s) >= destSize) {
        dest[0] = 0;
        return;
    }
    strcpy(dest, s);
}

/*
 * A retained message on clock/<id>/ota/latest, published by a Home Assistant app, says what the
 * newest release is:
 * {"version":"2.2.0-nt1","url":"http://.../firmware-ipstube.bin","md5":"<32 hex>","release_url":"...","title":"..."}
 * Only http:// URLs are accepted, TLS needs more memory than the clock has free.
 */
void MQTTBroker::onOtaLatest(const char *payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        return;
    }

    char url[128];
    copyOtaField(url, sizeof(url), doc["url"]);
    bool urlOk = strncmp(url, "http://", 7) == 0;

    copyOtaField(otaVersion, sizeof(otaVersion), doc["version"]);
    copyOtaField(otaUrl, sizeof(otaUrl), urlOk ? doc["url"] : doc["no_such_field"]);
    copyOtaField(otaMd5, sizeof(otaMd5), doc["md5"]);
    copyOtaField(otaReleaseUrl, sizeof(otaReleaseUrl), doc["release_url"]);
    copyOtaField(otaTitle, sizeof(otaTitle), doc["title"]);
    otaLatestReceived = true;

    publishOtaState();
    requestMqttPublish();
}

bool MQTTBroker::publishPanelButton() {
    if (!client.connected()) {
        return false;
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/button", home);
    client.publish(topic, 0, false, "next");
    return true;
}

// Publishes clock/<id>/ota/state, retained, for Home Assistant's update entity
void MQTTBroker::publishOtaState() {
    if (!client.connected()) {
        return;
    }

    JsonDocument doc;
    doc["installed_version"] = manifest[1];	// FIRMWARE_VERSION
    doc["latest_version"] = otaLatestReceived && otaVersion[0] != 0 ? otaVersion : manifest[1];
    // Home Assistant rejects the whole message if release_url isn't a valid URL, so leave out empty ones
    if (otaTitle[0] != 0) {
        doc["title"] = otaTitle;
    }
    if (otaReleaseUrl[0] != 0) {
        doc["release_url"] = otaReleaseUrl;
    }
    doc["in_progress"] = OTA::otaBusy() || OTA::otaRestartPending();
    int percent = OTA::otaPercentage();
    if (OTA::otaBusy() && percent >= 0) {
        doc["update_percentage"] = percent;
    } else {
        doc["update_percentage"] = nullptr;
    }

    char buffer[384];
    serializeJson(doc, buffer);
    client.publish(otaStateTopic, 1, true, buffer);
}

// The firmware image for startOtaInstall(), copied before the task starts
struct OtaDownload {
    char url[128];
    char md5[33];
};

// Downloads the firmware over HTTP and streams it into the other app slot. Runs on its own task
// because the download takes minutes and the MQTT callback must return quickly.
static void otaDownloadTask(void *param) {
    OtaDownload *download = (OtaDownload*)param;

    WiFiClient wifiClient;
    HTTPClient http;
    http.begin(wifiClient, download->url);
    http.setTimeout(15000);	// ms, for each read
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        OTA::otaAbort(httpCode == HTTP_CODE_NOT_FOUND ? "download: not found" : "download failed");
        http.end();
        delete download;
        vTaskDelete(NULL);
        return;
    }

    int size = http.getSize();	// -1 when the server didn't say
    if (!OTA::otaBegin(size > 0 ? (size_t)size : 0, download->md5)) {
        http.end();
        delete download;
        vTaskDelete(NULL);
        return;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[1024];
    uint32_t lastData = millis();
    size_t remaining = size > 0 ? (size_t)size : SIZE_MAX;	// SIZE_MAX: read until the server closes
    bool failed = false;
    while (remaining > 0 && !failed) {
        size_t available = stream->available();
        if (available > 0) {
            size_t toRead = available < sizeof(buffer) ? available : sizeof(buffer);
            if (toRead > remaining) {
                toRead = remaining;
            }
            size_t read = stream->readBytes(buffer, toRead);
            if (read == 0 || !OTA::otaWrite(buffer, read)) {
                failed = true;	// otaWrite() has already recorded the error
                if (read == 0) {
                    OTA::otaAbort("download: read failed");
                }
            } else {
                lastData = millis();
                if (remaining != SIZE_MAX) {
                    remaining -= read;
                }
            }
        } else if (!stream->connected()) {
            break;	// Closed by the server: complete when the size was unknown, else otaEnd() fails it
        } else if (millis() - lastData >= 15000) {
            OTA::otaAbort("download stalled");
            failed = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (!failed) {
        OTA::otaEnd();
    }
    http.end();
    delete download;
    vTaskDelete(NULL);
}

// Starts the download of the latest release on its own task, so the MQTT callback can return
void MQTTBroker::startOtaInstall() {
    if (otaUrl[0] == 0) {
        return;	// No ota/latest message with a URL yet
    }
    if (!OTA::otaAvailable() || OTA::otaBusy()) {
        return;
    }

    OtaDownload *download = new OtaDownload();
    if (download == NULL) {
        return;
    }
    strncpy(download->url, otaUrl, sizeof(download->url) - 1);
    download->url[sizeof(download->url) - 1] = 0;
    strncpy(download->md5, otaMd5, sizeof(download->md5) - 1);
    download->md5[sizeof(download->md5) - 1] = 0;

    if (xTaskCreate(otaDownloadTask, "OTA download", 8192, download, 1, NULL) != pdPASS) {
        delete download;
    }
}
