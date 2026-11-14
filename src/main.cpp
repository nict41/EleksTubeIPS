#include <Arduino.h>
#include <new>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <AsyncWiFiManager.h>
#include <EspSNTPTimeSync.h>
#include <sntp_pt.h>
#include <lwip/tcpip.h>
#include <mbedtls/sha256.h>
#include <bootloader_random.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#ifndef DS1302
#include <EspRTCTimeSync.h>
#else
#include <EspDS1302TimeSync.h>
#endif
#include <ConfigItem.h>
#include <EEPROMConfig.h>
#include <ImprovWiFi.h>
#include <eSPI_Menu.h>

#include "TFTs.h"
#include "IPSClock.h"
#include "Backlights.h"
#include "GPIOButton.h"
#include "WSHandler.h"
#include "WSMenuHandler.h"
#include "WSConfigHandler.h"
#include "WSInfoHandler.h"
#include "OpenWeatherMapWeatherService.h"
#include "weather.h"
#include "ScreenSaver.h"
#include "mqttBroker.h"
#include "OTA.h"
#include "IRAMPtrArray.h"
#include "Uptime.h"
#include "TLSArena.h"
#include "WiFiWatchdog.h"
#include "QoiStream.h"
#include "JpegPanel.h"
#include "PanelDraw.h"
#include "GifPlayer.h"

//#define DEBUG(...) { Serial.println(__VA_ARGS__); }
#ifndef DEBUG
#define DEBUG(...) {  }
#endif

#define SMALL_FONT  2        // font for menus
#define ITEM_FONT   2        // font for menus
#define TITLE_FONT  4        // font for all headings

// The firmware name should match "name" in web/esp-web-tools-manifest-*.json, which the web installer
// compares with what the clock reports to offer an update rather than a fresh install. The version
// comes from version.py: bump it there for a release.
IRAMPtrArray<const char*> manifest {
	// Firmware name
#if defined(HARDWARE_PunkCyber_CLOCK)
	"HA Tubes Firmware for PCBWay RGB Glow Tube Clock",
#elif defined(HARDWARE_Elekstube_CLOCK)
	"HA Tubes Firmware for EleksTube IPS V1",
#elif defined(HARDWARE_Elekstube_CLOCK_V2)
	"HA Tubes Firmware for EleksTube IPS V2",
#elif defined(HARDWARE_NovelLife_SE_CLOCK)
	"HA Tubes Firmware for NovelLife SE",
#elif defined(HARDWARE_SI_HAI_CLOCK)
	"HA Tubes Firmware for Si Hai Clock",
#elif defined(HARDWARE_IPSTube_CLOCK)
#if defined(HA_ONLY)
	"HA Tubes Firmware for IPSTube Clock (Home Assistant only)",
#else
	"HA Tubes Firmware for IPSTube Clock",
#endif
#elif defined(HARDWARE_IPSTube_DIM_CLOCK)
	"HA Tubes Firmware for IPSTube Clock With Hardware Dimming",
#else
	"Unknown clock hardware",
#endif
	// Firmware version, from version.py
	FIRMWARE_VERSION,
	// Hardware chip/variant
	"ESP32",
	// Device name
	"IPS Clock"
};

String getChipId(void);
void setWiFiAP(bool on);
void infoCallback();
String wifiCallback();
String clockFacesCallback();
String clockCallback();
String mqttCallback();
String weatherCallback();
void broadcastUpdate(String msg);
void broadcastUpdate(const BaseConfigItem& item);
void broadcastToWebUI(const BaseConfigItem& item);
void setFace(const char *menuLabel);
void initFacesMenu();
void redrawMenu();

TFTs *tfts = NULL;
eSPIMenu::Menu *menu;
Backlights *backlights = NULL;
IPSClock *ipsClock = NULL;
Weather *weather = NULL;
WeatherService *weatherService = NULL;
ImageUnpacker *imageUnpacker = NULL;

ScreenSaver *screenSaver;

#if defined(BUTTON_MODE_PIN) && defined(BUTTON_RIGHT_PIN) && defined(BUTTON_LEFT_PIN)
#define BUTTON_MENU_PINS
#endif

#ifdef BUTTON_MENU_PINS
GPIOButton *modeButton;	// open == high, closed == low
GPIOButton *rightButton;	// open == high, closed == low
GPIOButton *leftButton;	// open == high, closed == low
#endif
#ifdef BUTTON_POWER_PIN
GPIOButton *powerButton;	// open == high, closed == low
#endif

AsyncWebServer *server = new AsyncWebServer(80);
AsyncWebSocket *ws = new AsyncWebSocket("/ws"); // access at ws://[esp ip]/ws
DNSServer *dns = new DNSServer();
AsyncWiFiManager *wifiManager = new AsyncWiFiManager(server, dns);
TimeSync *timeSync;
#ifndef DS1302
RTCTimeSync *rtcTimeSync;
#else
DS1302TimeSync *rtcTimeSync;
#endif
MQTTBroker *mqttBroker;

Uptime uptime;

TaskHandle_t wifiManagerTask;
TaskHandle_t clockTask;
TaskHandle_t improvTask;
TaskHandle_t ledTask;
TaskHandle_t commitEEPROMTask;
TaskHandle_t weatherTask;

SemaphoreHandle_t wsMutex;
SemaphoreHandle_t memMutex;
QueueHandle_t weatherQueue;
QueueHandle_t wsPendingQueue;	// Web UI updates from tasks other than async_tcp, see broadcastUpdate()

AsyncWiFiManagerParameter *hostnameParam;
String ssid("HATubes");
String mqttId;	// What upstream used as the access point's name, see setup()
String chipId = getChipId();

// Persistent Configuration
#if defined(HARDWARE_PunkCyber_CLOCK)
StringConfigItem hostName("hostname", 63, "punkcyber");
#elif defined(HARDWARE_Elekstube_CLOCK)
StringConfigItem hostName("hostname", 63, "elekstubeips");
#elif defined(HARDWARE_Elekstube_CLOCK_V2)
StringConfigItem hostName("hostname", 63, "elekstubeipsv2");
#elif defined(HARDWARE_NovelLife_SE_CLOCK)
StringConfigItem hostName("hostname", 63, "novellifese");
#elif defined(HARDWARE_SI_HAI_CLOCK)
StringConfigItem hostName("hostname", 63, "sihai");
#elif defined(HARDWARE_IPSTube_CLOCK) || defined (HARDWARE_IPSTube_DIM_CLOCK)
StringConfigItem hostName("hostname", 63, "ipstube");
#else
StringConfigItem hostName("hostname", 63, "ipsclock");
#endif

// Set on the Wi-Fi portal's Configure Static WiFi page, see captureStaticAddress(). An empty address means DHCP.
StringConfigItem staticIP("static_ip", 15, "");
StringConfigItem staticGateway("static_gateway", 15, "");
StringConfigItem staticSubnet("static_subnet", 15, "");
StringConfigItem staticDNS1("static_dns1", 15, "");
StringConfigItem staticDNS2("static_dns2", 15, "");
bool staticAddressChanged = false;

StringConfigItem timeServer("time_server", 63, "pool.ntp.org");	// A name or an IP address

// The web UI login, see WebLoginHandler. The clock only asks for it when both are set.
StringConfigItem webUser("web_user", 31, "");
StringConfigItem webPassword("web_password", 63, "");

// Each clock makes up its own, see initFromEEPROM(). Upstream used "secretsauce" on every clock.
StringConfigItem apPassword("ap_password", 63, "");

enum WEATHER_MESSAGE {
	WEATHER_UPDATE = 1,
	COORDINATES_UPDATE = 2
};

// Set by any task, acted on by the clock task. Flags rather than a queue so nothing ever waits.
volatile bool mqttPublishRequested = false;
volatile bool mqttDiscoveryRequested = false;

void requestMqttPublish() {
	mqttPublishRequested = true;
}

void requestMqttDiscovery() {
	mqttDiscoveryRequested = true;
}

// LittleFS works out the space in use by walking the whole filesystem, which takes about
// a quarter of a second and holds the filesystem lock. So keep the answer until something
// changes. Anything that writes or removes files must call fsChanged() once it has done so.
static SemaphoreHandle_t fsSpaceMutex;
static volatile bool fsSpaceStale = true;
static size_t fsTotal = 0;
static size_t fsFree = 0;

void fsChanged() {
	fsSpaceStale = true;
}

size_t fsTotalBytes() {
	xSemaphoreTake(fsSpaceMutex, portMAX_DELAY);
	if (fsTotal == 0) {
		fsTotal = LittleFS.totalBytes();	// Never changes
	}
	xSemaphoreGive(fsSpaceMutex);

	return fsTotal;
}

size_t fsFreeBytes() {
	size_t total = fsTotalBytes();

	xSemaphoreTake(fsSpaceMutex, portMAX_DELAY);
	if (fsSpaceStale) {
		fsSpaceStale = false;	// Before the walk, so a change made during it isn't lost
		fsFree = total - LittleFS.usedBytes();
	}
	size_t free = fsFree;
	xSemaphoreGive(fsSpaceMutex);

	return free;
}

// Clock config
IRAMPtrArray<BaseConfigItem*> clockSet {
	// Clock
	&IPSClock::getDateFormat(),
	&IPSClock::getTimeOrDate(),
	&IPSClock::getSlideTransition(),
	&IPSClock::getHourFormat(),
	&IPSClock::getLeadingZero(),
	&IPSClock::getFourDigitDisplay(),
	&IPSClock::getDisplayOn(),
	&IPSClock::getDisplayOff(),
	&IPSClock::getClockFace(),
	&IPSClock::getDimming(),
	&IPSClock::getBrightnessConfig(),
	&IPSClock::getTimeZone(),
	0
};
CompositeConfigItem clockConfig("clock", 0, clockSet);

IRAMPtrArray<BaseConfigItem*> ledSet {
    // LEDs
    &Backlights::getLEDPattern(),
    &Backlights::getLEDHue(),
    &Backlights::getLEDSaturation(),
    &Backlights::getLEDValue(),
    &Backlights::getBreathPerMin(),
	&Backlights::getHuePerLed(),
    0
};
CompositeConfigItem ledConfig("leds", 0, ledSet);

// Allocate these on the heap to save some dram space
StringConfigItem *fileSet = new StringConfigItem("file_set", 10, "faces");
StringConfigItem *slidesSet = new StringConfigItem("slide_show", 25, "anime_female");
String *oldSlidesSet = new String("anime_female");
bool reloadSlides = false;	// Unpack the slide show again, e.g. after it was replaced

IRAMPtrArray<BaseConfigItem*> faceSet {
	// Faces
	&IPSClock::getClockFace(),
	&Weather::getIconPack(),
	slidesSet,
	fileSet,
	0
};
CompositeConfigItem facesConfig("faces", 0, faceSet);

IRAMPtrArray<BaseConfigItem*> weatherSet {
    // Weather service
    &WeatherService::getWeatherToken(),
    &WeatherService::getLatitude(),
    &WeatherService::getLongitude(),
    &WeatherService::getUnits(),
	&Weather::getWeatherHue(),
	&Weather::getWeatherSaturation(),
	&Weather::getWeatherValue(),
    0
};
CompositeConfigItem weatherConfig("weather", 0, weatherSet);

IRAMPtrArray<BaseConfigItem*> matrixSet {
    // Screensaver config
	&DigitalRainAnimation::getMatrixSpeed(),
	&DigitalRainAnimation::getMatrixHue(),
	&DigitalRainAnimation::getMatrixSaturation(),
	&DigitalRainAnimation::getMatrixValue(),
	&DigitalRainAnimation::getMatrixHueCycling(),
	&DigitalRainAnimation::getMatrixHueCycleTime(),
	&ScreenSaver::getScreenSaver(),
	&ScreenSaver::getScreenSaverDelay(),
	0
};
CompositeConfigItem matrixConfig("matrix", 0, matrixSet);

IRAMPtrArray<BaseConfigItem*> mqttSet {
    // MQTT service
	&MQTTBroker::getHost(),
	&MQTTBroker::getPort(),
	&MQTTBroker::getUser(),
	&MQTTBroker::getPassword(),
	0
};

CompositeConfigItem mqttConfig("mqtt", 0, mqttSet);

// Global configuration
IRAMPtrArray<BaseConfigItem*> configSetGlobal = {
	&hostName,
	0
};

CompositeConfigItem globalConfig("global", 0, configSetGlobal);

// Settings added in 2.0.3. They are stored after the earlier ones, see upgradeEEPROM().
IRAMPtrArray<BaseConfigItem*> networkSet {
	&staticIP,
	&staticGateway,
	&staticSubnet,
	&staticDNS1,
	&staticDNS2,
	&timeServer,
	0
};

CompositeConfigItem networkConfig("network", 0, networkSet);

IRAMPtrArray<BaseConfigItem*> cycleSet {
	&IPSClock::getDisplayCycle(),
	0
};

CompositeConfigItem cycleConfig("cycle", 0, cycleSet);

// Settings added after 2.0.5, stored after all of those. The web UI and MQTT are never sent the password.
IRAMPtrArray<BaseConfigItem*> webLoginSet {
	&webUser,
	&webPassword,
	0
};

CompositeConfigItem webLoginConfig("web_login", 0, webLoginSet);

// Settings added after 2.0.7. MQTT is never sent the access point's password.
IRAMPtrArray<BaseConfigItem*> accessPointSet {
	&apPassword,
	0
};

CompositeConfigItem accessPointConfig("access_point", 0, accessPointSet);

// All configurations
IRAMPtrArray<BaseConfigItem*> configSetRoot {
	&globalConfig,
	&clockConfig,
	&ledConfig,
	&facesConfig,
	&weatherConfig,
	&matrixConfig,
	&mqttConfig,
	&networkConfig,
	&cycleConfig,
	&webLoginConfig,
	&accessPointConfig,
	0
};

CompositeConfigItem rootConfig("root", 0, configSetRoot);

// The configurations from upstream 1.9.4 to 2.0.2, to recognise them in EEPROM
IRAMPtrArray<BaseConfigItem*> setRoot202 {
	&globalConfig,
	&clockConfig,
	&ledConfig,
	&facesConfig,
	&weatherConfig,
	&matrixConfig,
	&mqttConfig,
	0
};

CompositeConfigItem rootConfig202("root", 0, setRoot202);

// And from 2.0.3 to 2.0.5
IRAMPtrArray<BaseConfigItem*> setRoot205 {
	&globalConfig,
	&clockConfig,
	&ledConfig,
	&facesConfig,
	&weatherConfig,
	&matrixConfig,
	&mqttConfig,
	&networkConfig,
	&cycleConfig,
	0
};

CompositeConfigItem rootConfig205("root", 0, setRoot205);

// And from 2.0.6 to 2.0.7
IRAMPtrArray<BaseConfigItem*> setRoot207 {
	&globalConfig,
	&clockConfig,
	&ledConfig,
	&facesConfig,
	&weatherConfig,
	&matrixConfig,
	&mqttConfig,
	&networkConfig,
	&cycleConfig,
	&webLoginConfig,
	0
};

CompositeConfigItem rootConfig207("root", 0, setRoot207);

// Store the configurations in EEPROM
EEPROMConfig config(rootConfig);

void asyncTimeSetCallback(String time) {
	DEBUG(time);
	tfts->setStatus("NTP time received...");
#ifndef DS1302
	rtcTimeSync->enabled(false);
	rtcTimeSync->setDevice();
#else
	rtcTimeSync->enabled(false);
	rtcTimeSync->setDevice();
#endif
}

void asyncTimeErrorCallback(String msg) {
	DEBUG(msg);
#ifndef DS1302
	rtcTimeSync->enabled(true);
#else
	rtcTimeSync->enabled(true);
#endif
}

template<class T>
void onMqttParamsChanged(ConfigItem<T> &item) {
	mqttBroker->init(mqttId);
}

void onTimezoneChanged(ConfigItem<String> &tzItem) {
	timeSync->setTz(tzItem);
	timeSync->sync();
}

// EspSNTPTimeSync::setTz() copies a new time zone over the TZ environment variable, which is only as
// long as the time zone the clock started with. A longer one overwrote the heap, which could restart
// the clock before the new time zone was saved. Make room for the longest the setting holds.
void makeRoomForTimeZone() {
	char room[64];
	memset(room, ' ', sizeof(room) - 1);
	room[sizeof(room) - 1] = 0;
	setenv("TZ", room, 1);
	timeSync->setTz(IPSClock::getTimeZone().value);
}

// EspSNTPTimeSync always asks pool.ntp.org. This switches to the time server setting.
// The SNTP module keeps a pointer to the name, not a copy.
static char sntpServer[64];

void useTimeServer(void *arg) {
	String name = timeServer.value.length() > 0 ? timeServer.value : String("pool.ntp.org");
	pftime_sntp::stop();
	strlcpy(sntpServer, name.c_str(), sizeof(sntpServer));
	pftime_sntp::setservername(0, sntpServer);
	pftime_sntp::init();	// Asks the new server straight away
}

void onTimeServerChanged(ConfigItem<String> &item) {
	tcpip_callback(useTimeServer, NULL);	// The SNTP module must only be used from lwIP's thread once the network is up
}

void onWeatherConfigChanged(ConfigItem<String> &item) {
	uint32_t value = COORDINATES_UPDATE;
	xQueueSend(weatherQueue, &value, 0);
}

void broadcastFSChange() {
	String freeSpace = String(fsFreeBytes());
	String msg = "{\"type\":\"sv.update\",\"value\":{\"fs_free\":" + freeSpace + ",\"fs_size\":" + String(fsTotalBytes()) + "}}";
	broadcastUpdate(msg);
}

void onFileSetChanged(ConfigItem<String> &item) {
	String msg = "{\"type\":\"sv.update\",\"value\":{\"clock_face\":\""
		 + IPSClock::getClockFace().value
		 + "\""
		 + ",\"weather_icons\":\""
		 + Weather::getIconPack()
		 + "\""
		 + ",\"slide_show\":\""
		 + slidesSet->value
		 + "\","
		 + clockFacesCallback()
		 + "}}";

	broadcastUpdate(msg);
}

// Also called every second while the hue cycles. Only the web UI follows that, publishing the whole
// state to MQTT every second is a lot of work for the clock and fills Home Assistant's history.
// Changes made from the web UI or MQTT are published by the code that makes them.
void onMatrixHueChanged(ConfigItem<int> &item) {
	broadcastToWebUI(item);
}

unsigned long lastDisplayChange = 0;

#ifdef HA_ONLY
// This build has no weather and no slide show (Home Assistant shows both), so a display or number of
// digits that would show them (saved by an earlier build, its default, or set over MQTT) falls back to
// one that doesn't, and so does the web UI's Files list. The web UI hears of each change.
void haOnlyFallbacks() {
	IntConfigItem &display = IPSClock::getTimeOrDate();
	if (display.value == IPSClock::WEATHER || display.value == IPSClock::SLIDE_SHOW) {
		display.value = IPSClock::TIME;
		broadcastUpdate(display);
		display.notify();
	}
	ByteConfigItem &digits = IPSClock::getFourDigitDisplay();
	if (digits.value == IPSClock::FOUR_WITH_WEATHER || digits.value == IPSClock::FOUR_WITH_SLIDESHOW) {
		digits.value = IPSClock::SIX;
		broadcastUpdate(digits);
		digits.notify();
	}
	if (fileSet->value != "faces") {
		fileSet->value = "faces";
		broadcastUpdate(*fileSet);
	}
}
#endif

#ifdef HA_ONLY
// Deletes the slide show pictures the file system image brings (or an earlier build unpacked): this
// build has no slide show, so they'd only take up room that GIFs could use. A file system update
// brings them back, so this runs at every start. The folders themselves stay.
static void removeSlideFiles() {
	bool removed = false;
	for (const char *path : { "/ips/slides", "/ips/slides_cache" }) {
		fs::File dir = LittleFS.open(path);
		if (!dir || !dir.isDirectory()) {
			continue;
		}
		String name = dir.getNextFileName();
		while (name.length() > 0) {
			removed = LittleFS.remove(name) || removed;
			name = dir.getNextFileName();
		}
		dir.close();
	}
	if (removed) {
		fsChanged();
	}
}
#endif

void onDisplayChanged(ConfigItem<int> &item) {
	lastDisplayChange = millis();	// However it changed, show it for a whole cycle
	tfts->invalidateAllDigits();

	weather->redraw();
}

// Moves on to the next of time, date and weather once the display has been shown for the cycle time.
// Weather is skipped without an API key. The display isn't saved, so the flash isn't written every
// few seconds, and after a restart the clock starts on the one chosen last.
void cycleDisplays() {
	uint8_t seconds = IPSClock::getDisplayCycle().value;
	if (seconds == 0 || millis() - lastDisplayChange < seconds * 1000UL) {
		return;
	}

	IntConfigItem &display = IPSClock::getTimeOrDate();
	if (display.value == IPSClock::TIME) {
		display.value = IPSClock::DATE;
#ifndef HA_ONLY
	} else if (display.value == IPSClock::DATE && WeatherService::getWeatherToken().value.length() > 0) {
		display.value = IPSClock::WEATHER;
#endif
	} else {
		display.value = IPSClock::TIME;	// After the weather, or from the slide show
	}
	broadcastUpdate(display);
	display.notify();
}

void onBrightnessChanged(ConfigItem<byte> &item) {
	weather->redraw();
}

template <class T>
void onWeatherColorChanged(ConfigItem<T> &item) {
	weather->redraw();
}

bool menuDrawn = false;

// The display the clock's button moves on to: time, date, weather, slide show, then the time again.
// PANELS is only chosen from the web UI or MQTT, so it's skipped. The Home Assistant-only build has
// no weather or slide show, so its button just switches between the time and the date.
int nextButtonDisplay(int display) {
#ifdef HA_ONLY
	return display == IPSClock::TIME ? IPSClock::DATE : IPSClock::TIME;
#else
	display = (display + 1) % (IPSClock::PANELS + 1);
	if (display == IPSClock::PANELS) {
		display = IPSClock::TIME;
	}
	return display;
#endif
}

void onButtonEvent(const Button *button, Button::Event evt) {
	// Any event will reset the screensaver timer, which will turn it off if it was visible
	bool screenSaverWasOn = screenSaver->reset();

#ifdef BUTTON_POWER_PIN
	if (screenSaverWasOn && ipsClock->clockOn()) {
		// If the screen saver was running (and visible), any button any event cancels it and that is all
		return;
	}

	if (button == powerButton && evt == Button::long_press) {
		// Screensaver isn't running or clock is off, set on/off override and exit
		ipsClock->overrideUntilNextChange();
		return;
	}

	if (!ipsClock->clockOn()) {
		// Otherwise, if the clock is off, any event turns it on for 10 seconds
		ipsClock->setOnOverride();
		return;
	}
#endif
	// Now we know the clock is on so we can do more interactive stuff...

	// ... like the menu
#ifdef BUTTON_MENU_PINS
	if (button == rightButton && evt == Button::button_clicked && menuDrawn) {
		menu->down();
		tfts->getSprite().pushSprite(0, 0);
	}

	if (button == leftButton && evt == Button::button_clicked  && menuDrawn) {
		menu->up();
		tfts->getSprite().pushSprite(0, 0);
	}

	if (button == modeButton) {
		if (evt == Button::long_press) {
			if (!menuDrawn) {
				tfts->clear();
				initFacesMenu();
				menu->show();
				tfts->getSprite().pushSprite(0, 0);
				menuDrawn = true;
			} else {
				tfts->invalidateAllDigits();
				menuDrawn = false;
				weather->redraw();
			}
		}

		if (evt == Button::button_clicked) {
			if (menuDrawn) {
				setFace(menu->getSelectedText());
				tfts->fillScreen(TFT_BLACK);
				tfts->invalidateAllDigits();
				menuDrawn = false;
				weather->redraw();
			} else {
				// ... or switching display modes. This *is* the mode button after all
				IntConfigItem &dateOrTime = IPSClock::getTimeOrDate();

				// Showing panels: the Home Assistant app moves on to its next screen instead, as long as MQTT
				// is up to tell it. Its Display select (or the web UI) switches back to the clock.
				if (dateOrTime.value == IPSClock::PANELS && mqttBroker->publishPanelButton()) {
					return;
				}

				dateOrTime.value = nextButtonDisplay(dateOrTime.value);
				dateOrTime.put();
				broadcastUpdate(dateOrTime);
				dateOrTime.notify();
				tfts->invalidateAllDigits();
			}
		}
	}
#endif

#ifdef BUTTON_POWER_PIN
	if (button == powerButton) {
#ifdef BUTTON_MENU_PINS
		// If we got here, the screen saver wasn't on, clicking the power button turns it on
		if (!screenSaverWasOn) {
			screenSaver->start();
		}
#else
		// If we only have the power button, it is more useful to cycle through the display modes
		IntConfigItem &dateOrTime = IPSClock::getTimeOrDate();

		// Showing panels: the Home Assistant app moves on to its next screen instead, as long as MQTT
		// is up to tell it. Its Display select (or the web UI) switches back to the clock.
		if (dateOrTime.value == IPSClock::PANELS && mqttBroker->publishPanelButton()) {
			return;
		}

		dateOrTime.value = nextButtonDisplay(dateOrTime.value);
		dateOrTime.put();
		broadcastUpdate(dateOrTime);
		dateOrTime.notify();
		tfts->invalidateAllDigits();
#endif
	}
#endif
}

// The buttons are read even while the weather is fetched and the displays can't be drawn, see
// clockTaskFn(), and what was pressed is acted on afterwards, so presses aren't lost
struct ButtonEvent {
	const Button *button;
	Button::Event event;
};

ButtonEvent buttonEvents[8];
uint8_t buttonEventCount = 0;

void queueButtonEvent(const Button *button, Button::Event evt) {
	if (buttonEventCount < sizeof(buttonEvents) / sizeof(buttonEvents[0])) {
		buttonEvents[buttonEventCount++] = { button, evt };
	}
}

void readButtons() {
#ifdef BUTTON_MENU_PINS
	leftButton->getEvent();
	rightButton->getEvent();
	modeButton->getEvent();
#endif
#ifdef BUTTON_POWER_PIN
	powerButton->getEvent();
#endif
}

// Fast enough for the buttons and the matrix animation, which runs at up to 50 frames a second
#define DEFAULT_MAIN_SLEEP (pdMS_TO_TICKS(10))

// Only sends commands when the state needs to change. Claims the displays because other tasks can show status messages.
void setDisplaysEnabled(bool enabled) {
	if (tfts->isEnabled() != enabled) {
		tfts->claim();
		if (enabled) {
			tfts->enableAllDisplays();
		} else {
			tfts->disableAllDisplays();
		}
		tfts->release();
	}
}

void runMatrixAnimation() {
	setDisplaysEnabled(true);	// e.g. the screen saver was just switched from blank to matrix
	tfts->setShowDigits(IPSClock::getTimeOrDate());
	tfts->setDimming(ipsClock->getBrightness());
	tfts->animateRain();
	tfts->invalidateAllDigits();
}

// Set by the weather task when TLS has had the display buffer, see weatherTaskFn(), and when unpacking
// images has, see ImageUnpacker
volatile bool displayBufferLent = false;

// Everything that draws, which uses the display buffer. Only call it holding memMutex.
void updateDisplays() {
	// GIF players keep their decoders in the display buffer; it's needed again outside panel mode
	if (IPSClock::getTimeOrDate().value != IPSClock::PANELS || screenSaver->isOn()) {
		GifPlayer::stopAllForDisplay();
	}
	if (displayBufferLent) {
		displayBufferLent = false;
		tfts->invalidateAllDigits();
		weather->redraw();
		if (menuDrawn) {
			redrawMenu();
		}
	}

	for (uint8_t i = 0; i < buttonEventCount; i++) {
		onButtonEvent(buttonEvents[i].button, buttonEvents[i].event);
	}
	buttonEventCount = 0;

	if (tfts->checkStatus()) {
		weather->redraw();	// To get rid of the status message
	}

	if (menuDrawn) {
		return;
	}

	ipsClock->setBrightness(ipsClock->getBrightnessConfig());

	if ((ipsClock->getDimming() == IPSClock::MATRIX) && !ipsClock->clockOn()) {
		runMatrixAnimation();
	} else if (ipsClock->clockOn() && screenSaver->isOn()) {
		switch(ScreenSaver::getScreenSaver()) {
			case ScreenSaver::BLANK:
				setDisplaysEnabled(false);
				break;
			default:
				runMatrixAnimation();
				break;
		}
	} else {
		// Dim is still showing, so keeps cycling. Blank isn't, so waits for the displays to come on
		if (ipsClock->clockOn() || (ipsClock->getDimming() == IPSClock::DIM)) {
			cycleDisplays();
		}

		tfts->setShowDigits(IPSClock::getTimeOrDate());
#ifndef HA_ONLY	// No slide show in the Home Assistant-only build, so its pictures aren't unpacked
		if (reloadSlides || slidesSet->value != *oldSlidesSet) {
			reloadSlides = false;
			slidesSet->value = imageUnpacker->unpackImages("/ips/slides/", "/ips/slides_cache", *slidesSet, *oldSlidesSet);
			slidesSet->put();
			broadcastUpdate(*slidesSet);
			broadcastFSChange();
			*oldSlidesSet = slidesSet->value;
			tfts->claim();
			tfts->invalidateAllDigits();
			tfts->release();
		}
#endif
		weather->checkIconPack();
		ipsClock->checkIconPack();
#ifdef HA_ONLY
		haOnlyFallbacks();
#endif

		switch (IPSClock::getTimeOrDate().value) {
			case IPSClock::WEATHER:
				if (ipsClock->clockOn() || (ipsClock->getDimming() == IPSClock::DIM)) {
					weather->loop(ipsClock->getBrightness());
				} else {
					setDisplaysEnabled(false);
				}
				break;
			case IPSClock::SLIDE_SHOW:
			// drop through
			case IPSClock::PANELS:
			// drop through, the panel tubes are drawn by ipsClock->loop() and fed over HTTP
			default:
				if (timeSync->initialized() || rtcTimeSync->initialized()) {
					ipsClock->loop();
					if (ipsClock->getFourDigitDisplay() == IPSClock::FOUR_WITH_WEATHER && IPSClock::getTimeOrDate().value == IPSClock::TIME) {
						weather->drawSingleDay(ipsClock->getBrightness(), 0, 0);
					}
				}
				break;
		}
	}
}

// While the access point is on, its name and password take turns in the status line, so whoever can
// see the clock can join it, along with why the clock can't join a network if it tried and failed.
// Each is shown before the last expires, so the digits aren't redrawn.
void showAccessPoint() {
	static unsigned long lastShown = 0;
	static uint8_t showing = 0;

	// Less than the 6 s a status with a label lasts, so it's replaced before it goes
	if (wifiManager->isAP() && millis() - lastShown >= 5000) {
		lastShown = millis();
		const char *problem = wifiManager->connectProblem();
		if (showing == 2 && problem == NULL) {
			showing = 0;
		}

		if (showing == 0) {
			// The ID from createSSID() on a line of its own, so the name fits the bigger text
			String name = ssid.substring(0, ssid.length() - 4) + "\n" + ssid.substring(ssid.length() - 4);
			tfts->setStatus("WiFi network", name.c_str());
		} else if (showing == 1) {
			tfts->setStatus("WiFi password", apPassword.value.c_str(), true);
		} else {
			// Split at the first space, so the words stay whole in the bigger text
			String text(problem);
			text.setCharAt(text.indexOf(' '), '\n');
			tfts->setStatus("WiFi problem", text.c_str());
		}
		showing = (showing + 1) % 3;
	}
}

void clockTaskFn(void *pArg) {
	TickType_t toSleep = DEFAULT_MAIN_SLEEP;

	imageUnpacker = new ImageUnpacker();

	weatherService = new OpenWeatherMapWeatherService();
	WeatherService::getLatitude().setCallback(onWeatherConfigChanged);
	WeatherService::getLongitude().setCallback(onWeatherConfigChanged);
	WeatherService::getWeatherToken().setCallback(onWeatherConfigChanged);
	WeatherService::getUnits().setCallback(onWeatherConfigChanged);

	weather = new Weather(weatherService);
	weather->setImageUnpacker(imageUnpacker);
	weather->setTimeSync(timeSync);
	Weather::getWeatherHue().setCallback(onWeatherColorChanged);
	Weather::getWeatherSaturation().setCallback(onWeatherColorChanged);
	Weather::getWeatherValue().setCallback(onWeatherColorChanged);

	fileSet->setCallback(onFileSetChanged);

	DigitalRainAnimation::getMatrixHue().setCallback(onMatrixHueChanged);

	ipsClock = new IPSClock();
	ipsClock->init();
	ipsClock->setImageUnpacker(imageUnpacker);
	ipsClock->setTimeSync(timeSync);
	ipsClock->getTimeOrDate().setCallback(onDisplayChanged);
	ipsClock->getBrightnessConfig().setCallback(onBrightnessChanged);

	*oldSlidesSet = slidesSet->value;

	screenSaver = new ScreenSaver();

#ifdef BUTTON_MENU_PINS
	modeButton = new GPIOButton(BUTTON_MODE_PIN, false);
	rightButton = new GPIOButton(BUTTON_RIGHT_PIN, false);
	leftButton = new GPIOButton(BUTTON_LEFT_PIN, false);

	leftButton->setCallback(queueButtonEvent);
	modeButton->setCallback(queueButtonEvent);
	rightButton->setCallback(queueButtonEvent);
#endif
#ifdef BUTTON_POWER_PIN
	powerButton = new GPIOButton(BUTTON_POWER_PIN, false);

	powerButton->setCallback(queueButtonEvent);
#endif

	screenSaver->reset();

	mqttBroker->init(mqttId);

	uint32_t lastMqttPublish = 0;

	while (true) {
		vTaskDelay(toSleep);

		// Discovery and publishing the state take a few K, so they are tried again later if the weather
		// is being fetched. Publish at most once a second so that something that changes quickly, like
		// matrix hue cycling, can't run the heap out.
		if (mqttDiscoveryRequested) {
			mqttDiscoveryRequested = false;
			if (!mqttBroker->sendHADiscoveryMessage()) {
				mqttDiscoveryRequested = true;
			}
		}

		if (mqttPublishRequested && millis() - lastMqttPublish >= 1000) {
			mqttPublishRequested = false;
			lastMqttPublish = millis();
			if (!mqttBroker->publishState()) {
				mqttPublishRequested = true;
			}
		}

		uptime.loop();
		ipsClock->updateSchedule();
		showAccessPoint();
		readButtons();
		OTA::otaLoop();	// Restarts the clock once a firmware update has succeeded

		// Memory is an issue if the displays need a .gz.tar file unpacked and the weather task
		// retrieves the forecast at the same time. Also reading images from flash while the weather
		// is fetched over TLS slows both down so much that the task watchdog fires. And while it's
		// fetched, TLS has the display buffer. So the displays wait until it's done, but the buttons
		// and MQTT carry on.
		if (xSemaphoreTake(memMutex, 0) == pdTRUE) {
			updateDisplays();
			xSemaphoreGive(memMutex);
		}

		mqttBroker->checkConnection();
#ifdef HA_ONLY
		ws->cleanupClients(1);	// One web UI browser at a time: each keeps a queue of up to 32 messages
#endif
	}
}

#define DEFAULT_WEATHER_SLEEP (pdMS_TO_TICKS(15 * 60 * 1000))
#define FIRST_WEATHER_UPTIME (30 * 1000UL)	// ms

#ifdef USE_DMA
#error "Wait for the last DMA transfer before lending the display buffer to TLS in weatherTaskFn()"
#endif

void weatherTaskFn(void *pArg) {
	TickType_t toSleep = DEFAULT_WEATHER_SLEEP;
	bool lendDisplayBuffer = true;
	while (true) {
		// Read from weatherQueue. Wait at most 'toSleep' ticks.
		uint32_t value;
		BaseType_t result = xQueueReceive(weatherQueue, &value, toSleep);

		if (result == pdTRUE) {
			if (value == COORDINATES_UPDATE) {	// Location coordinates changed
				DEBUG("Weather task sleeping 30 seconds");
				value = 0;
				delay(30000);	// In case user is changing latitude and longitude, i.e. wait for both to possibly change
			} else if (value == WEATHER_UPDATE) {
				value = 0;
				delay(5000);	// Give memory some time to free
			}
			DEBUG("Weather task getting right to it");
		}

		// Drain the queue of any pending messages
		while (xQueueReceive(weatherQueue, &value, 0) == pdTRUE);

		DEBUG("Trying to get weather info");

		toSleep = DEFAULT_WEATHER_SLEEP;
		if ((WiFi.status() == WL_CONNECTED) && !wifiManager->isAP()) {
			if (WeatherService::getWeatherToken().value.length() == 0) {
				continue;	// Setting a key wakes this task, see onWeatherConfigChanged()
			}

			// Not on top of starting up, or of browsers reconnecting after a restart. Read once, since if it
			// passed 30 s between two reads, the subtraction would wrap round to over an hour's sleep.
			unsigned long uptime = millis();
			if (uptime < FIRST_WEATHER_UPTIME) {
				toSleep = pdMS_TO_TICKS(FIRST_WEATHER_UPTIME - uptime);
				continue;
			}

			if (!WeatherService::timeIsSet()) {
				// NTP usually answers within a few seconds of connecting
				toSleep = pdMS_TO_TICKS(10000);
				continue;
			}

			// Memory is an issue if the clock task decides to unpack a .gz.tar file
			// and this task tries to retrieve the forecast at the same time.
			xSemaphoreTake(memMutex, portMAX_DELAY);
			// The displays aren't drawn while this holds memMutex, so TLS can have their buffer, the
			// biggest block of memory the clock has. Transfers to the displays aren't DMA, so they've finished.
			bool lent = lendDisplayBuffer && !GifPlayer::active();	// GIF decoders live in that buffer
			if (lent) {
				TLSArena::begin(StaticSprite::output_buffer, sizeof(StaticSprite::output_buffer));
			}
			bool gotWeather = weatherService->getWeatherInfo();
			if (lent) {
				// Never seen, but if TLS kept some of the buffer, it mustn't end up in a later loan too
				if (!TLSArena::end()) {
					Serial.println("TLS didn't give back all of the display buffer, so it won't be lent again");
					lendDisplayBuffer = false;
				}
				displayBufferLent = true;
			}
			xSemaphoreGive(memMutex);
			weather->redraw();	// Even a failed update can have changed part of the forecast
			if (!gotWeather) {
				DEBUG("Failed to get weather");
				toSleep = pdMS_TO_TICKS(180000);	// Try again in 3 minutes
			}
		}

//		Serial.printf("Weather task going back to sleep for %d ticks\n", toSleep);
	}
}

String getChipId(void)
{
  uint8_t macid[6];
  esp_efuse_mac_get_default(macid);
  String chipId = String((uint32_t)(macid[5] + (((uint32_t)(macid[4])) << 8) + (((uint32_t)(macid[3])) << 16)), HEX);
  chipId.toUpperCase();
  return chipId;
}

void createSSID() {
	// Unique enough from the end of the MAC address, and short enough to read off the displays
	uint8_t macid[6];
	esp_efuse_mac_get_default(macid);
	char name[16];
	snprintf(name, sizeof(name), "HATubes%02X%02X", macid[4], macid[5]);
	ssid = name;
}

IRAMPtrArray<const char*> items {
	WSMenuHandler::clockMenu,
	WSMenuHandler::panelsMenu,
	WSMenuHandler::matrixMenu,
	WSMenuHandler::ledsMenu,
#ifndef HA_ONLY	// Home Assistant shows the weather, the clock doesn't fetch it
	WSMenuHandler::weatherMenu,
#endif
	WSMenuHandler::facesMenu,
	WSMenuHandler::networkMenu,
	WSMenuHandler::mqttMenu,
	WSMenuHandler::infoMenu,
	WSMenuHandler::aboutMenu,
	0
};

WSMenuHandler wsMenuHandler(items);
WSConfigHandler wsClockHandler(rootConfig, "clock", clockCallback);
WSConfigHandler wsLEDHandler(rootConfig, "leds");
WSConfigHandler wsFacesHandler(rootConfig, "faces", clockFacesCallback);
#ifndef HA_ONLY
const char *weatherExcludes[] = { "weather_token", 0 };	// See weatherCallback()
WSConfigHandler wsWeatherHandler(rootConfig, "weather", weatherCallback, weatherExcludes);
#endif
WSConfigHandler wsMatrixHandler(rootConfig, "matrix");
const char *mqttExcludes[] = { "mqtt_password", 0 };	// See mqttCallback()
WSConfigHandler wsMqttHandler(rootConfig, "mqtt", mqttCallback, mqttExcludes);
WSConfigHandler wsNetworkHandler(rootConfig, "network", wifiCallback);
WSInfoHandler wsInfoHandler(infoCallback);

// Order of this needs to match the numbers in WSMenuHandler.cpp
IRAMPtrArray<WSHandler*> wsHandlers {
	&wsMenuHandler,
	&wsClockHandler,
	&wsLEDHandler,
	&wsFacesHandler,
	&wsMqttHandler,
	&wsInfoHandler,
	&wsNetworkHandler,
#ifndef HA_ONLY
	&wsWeatherHandler,
#else
	NULL,	// The weather page's number: no weather in this build
#endif
	&wsMatrixHandler,
	NULL
};


void infoCallback() {
	wsInfoHandler.setSsid(ssid);
	wsInfoHandler.setMqttId(mqttId);
	// wsInfoHandler.setBlankingMonitor(&blankingMonitor);
	wsInfoHandler.setDescription(manifest[0]);
	wsInfoHandler.setRevision(manifest[1]);
	wsInfoHandler.setUptime(uptime.uptime());

	wsInfoHandler.setFSSize(String(fsTotalBytes()));
	wsInfoHandler.setFSFree(String(fsFreeBytes()));
	TimeSync::SyncStats &syncStats = timeSync->getStats();

	wsInfoHandler.setFailedCount(syncStats.failedCount);
	wsInfoHandler.setLastFailedMessage(syncStats.lastFailedMessage);
	wsInfoHandler.setLastUpdateTime(syncStats.lastUpdateTime);
	wsInfoHandler.setHostname(hostName);
}

/*
 * The web socket library isn't thread safe. If another task sends while async_tcp is handling an
 * ack for the same client, the client's message queue gets corrupted and the clock crashes. So
 * messages from other tasks, e.g. the clock task, are queued and sent by async_tcp, see wsHandler().
 */
void broadcastUpdate(String msg) {
	if (strcmp(pcTaskGetName(NULL), "async_tcp") != 0) {
		String *pending = new (std::nothrow) String(msg);
		if (pending != NULL && xQueueSend(wsPendingQueue, &pending, 0) != pdTRUE) {
			delete pending;	// Queue is full
		}
		return;
	}

	xSemaphoreTake(wsMutex, portMAX_DELAY);

    ws->textAll(msg);

	xSemaphoreGive(wsMutex);
}

// Only call this on async_tcp
void sendPendingUpdates() {
	String *pending;

	while (xQueueReceive(wsPendingQueue, &pending, 0) == pdTRUE) {
		broadcastUpdate(*pending);
		delete pending;
	}
}

void broadcastUpdate(const BaseConfigItem& item) {
	broadcastToWebUI(item);

	requestMqttPublish();
}

void broadcastToWebUI(const BaseConfigItem& item) {
	JsonDocument doc;
	JsonObject root = doc.to<JsonObject>();

	root["type"] = "sv.update";

	JsonVariant value = root["value"].to<JsonObject>();
	String rawJSON = item.toJSON();	// This object needs to hang around until we are done serializing.
	value[item.name] = serialized(rawJSON.c_str());

	String json;
	serializeJson(root, json);
	broadcastUpdate(json);
}

// The web UI is only told whether there's a password, never what it is
const char *passwordPlaceholder = "********";

String passwordJSON(const StringConfigItem &password) {
	return String("\"") + password.name + "\":\"" + (password.value.length() > 0 ? passwordPlaceholder : "") + "\"";
}

String webLoginJSON() {
	return "\"web_user\":" + webUser.toJSON() + "," + passwordJSON(webPassword);
}

// Refuses anything a login couldn't match. Stored settings stop at the first character that isn't
// ASCII, so a password would stop matching after a restart, and the web UI's user goes in the
// browser's login header, which can't carry every character.
bool validLogin(const StringConfigItem &item, const String &value) {
	if (value.length() > item.maxSize) {
		return false;
	}

	for (int i = 0; i < value.length(); i++) {
		char c = value[i];
		if (&item == &webUser ? !isalnum((unsigned char)c) && strchr("._-@", c) == NULL : c < ' ' || c > '~') {
			return false;
		}
	}

	return true;
}

void setWebLogin(StringConfigItem &item, const String &value) {
	bool changed = validLogin(item, value) && value != item.value;
	if (changed) {
		item = value;
		item.put();
	}

	broadcastUpdate("{\"type\":\"sv.update\",\"value\":{" + webLoginJSON() + "}}");	// Also puts back anything refused

	// Web sockets are only checked when they connect, so ones opened before the login was set or
	// changed must connect again with it
	if (changed && webUser.value.length() > 0 && webPassword.value.length() > 0) {
		xSemaphoreTake(wsMutex, portMAX_DELAY);
		ws->closeAll();
		xSemaphoreGive(wsMutex);
	}
}

// For the MQTT password and the weather API key, which the web UI is only told are set. A page
// loaded before they were hidden sends the placeholder back when it loses focus, so that's ignored.
void setSecret(StringConfigItem &item, const String &value) {
	if (validLogin(item, value) && value != item.value && value != passwordPlaceholder) {
		item = value;
		item.put();
		item.notify();	// Connects to the broker again, or fetches the weather, with it
	}

	broadcastUpdate("{\"type\":\"sv.update\",\"value\":{" + passwordJSON(item) + "}}");	// Also puts back anything refused
}

String randomAPPassword();

// Unlike the other passwords it's shown, on the Network page and on the displays, since it's there so
// that whoever can see the clock can join its access point. Clearing it makes a new random one. It's
// used the next time the access point starts.
void setAPPassword(const String &value) {
	String password = value.length() > 0 ? value : randomAPPassword();
	if (password.length() >= 8 && validLogin(apPassword, password) && password != apPassword.value) {
		apPassword = password;
		apPassword.put();
		xSemaphoreTake(wsMutex, portMAX_DELAY);	// Held while the Wi-Fi manager runs, see wifiManagerTaskFn()
		wifiManager->setAPCredentials(ssid.c_str(), apPassword.value.c_str());
		xSemaphoreGive(wsMutex);
	}

	broadcastToWebUI(apPassword);	// Also puts back anything refused
}

// A name that works everywhere the hostname is used: DHCP, mDNS and the web UI's JSON. Letters, digits
// and hyphens, without a hyphen at either end.
bool validHostname(const String &name) {
	if (name.length() == 0 || name.length() > hostName.maxSize || name[0] == '-' || name[name.length() - 1] == '-') {
		return false;
	}

	for (int i = 0; i < name.length(); i++) {
		if (!isalnum((unsigned char)name[i]) && name[i] != '-') {
			return false;
		}
	}

	return true;
}

// A group of settings, like "web_login". Setting one does nothing, but would send what's in it to the
// web UI, passwords too.
bool isSettingsGroup(const BaseConfigItem *item) {
	if (item == &rootConfig) {
		return true;
	}

	for (int i = 0; configSetRoot[i] != 0; i++) {
		if (item == configSetRoot[i]) {
			return true;
		}
	}

	return false;
}

void updateValue(int screen, String pair) {
	if (screen != 8) {
		screenSaver->reset();
	}

	int index = pair.indexOf(':');
	DEBUG(pair)
	// _key has to hang around because key points to an internal data structure
	String _key = pair.substring(0, index);
	const char* key = _key.c_str();
	String value = pair.substring(index+1);
	BaseConfigItem *item = rootConfig.get(key);
	if (item == &webUser || item == &webPassword) {
		setWebLogin(*(StringConfigItem*)item, value);
	} else if (item == &MQTTBroker::getPassword() || item == &WeatherService::getWeatherToken()) {
		setSecret(*(StringConfigItem*)item, value);
	} else if (item == &apPassword) {
		setAPPassword(value);
	} else if (item == &hostName && !validHostname(value)) {
		broadcastToWebUI(hostName);	// Puts back what was refused
	} else if (item != 0 && !isSettingsGroup(item)) {
		item->fromString(value);
		item->put();
		// Order of below is important to maintain external consistency
		broadcastUpdate(*item);
		item->notify();
		if (_key == "hostname") {
			config.commit();
			ESP.restart();
		}
	} else if (_key == "get_weather") {
		uint32_t value = WEATHER_UPDATE;
		xQueueSend(weatherQueue, &value, 0);
	} else if (_key == "wifi_ap") {
		setWiFiAP(value == "true" ? true : false);
	}
}

/*
 * Handle application protocol
 */
void handleWSMsg(AsyncWebSocketClient *client, char *data) {
	String wholeMsg(data);
	int code = wholeMsg.substring(0, wholeMsg.indexOf(':')).toInt();

	if (code < 9) {
		if (code >= 0 && code < wsHandlers.length()) {
			if (wsHandlers[code] != NULL) {
				wsHandlers[code]->handle(client, data);
			}
		}
	} else {
		String message = wholeMsg.substring(wholeMsg.indexOf(':')+1);
		int screen = message.substring(0, message.indexOf(':')).toInt();
		String pair = message.substring(message.indexOf(':')+1);
		updateValue(screen, pair);
	}
}

void wsHandler(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
	//Handle WebSocket event
	switch (type) {
	case WS_EVT_CONNECT:
		DEBUG("WS connected")
		;
		// Anything queued while nobody was connected is out of date. Send it now, before the
		// client asks for the current state, rather than after.
		sendPendingUpdates();

		// Send updates queued by other tasks whenever async_tcp polls the client, see broadcastUpdate()
		client->client()->onPoll([](void *arg, AsyncClient *c) {
			sendPendingUpdates();
			((AsyncWebSocketClient*)arg)->_onPoll();	// What the web socket library registered
		}, client);
		break;
	case WS_EVT_DISCONNECT:
		DEBUG("WS disconnected")
		;
		break;
	case WS_EVT_ERROR:
		DEBUG("WS error")
		;
		DEBUG((char* )data)
		;
		break;
	case WS_EVT_PONG:
		DEBUG("WS pong")
		;
		break;
	case WS_EVT_DATA:	// Yay we got something!
		DEBUG("WS data")
		;
		AwsFrameInfo * info = (AwsFrameInfo*) arg;
		if (info->final && info->index == 0 && info->len == len) {
			//the whole message is in a single frame and we got all of it's data
			if (info->opcode == WS_TEXT) {
				DEBUG("WS text data");
				data[len] = 0;
				handleWSMsg(client, (char *) data);
			} else {
				DEBUG("WS binary data");
			}
		} else {
			DEBUG("WS data was split up!");
		}
		break;
	}
}

void mainHandler(AsyncWebServerRequest *request) {
	DEBUG("Got request")
	request->redirect("/app.html");	// Straight there, rather than through index.html and its refresh
}

void timeHandler(AsyncWebServerRequest *request) {
	DEBUG("Got time request")
	String wifiTime = request->getParam("time", true, false)->value();

	DEBUG(String("Setting time from wifi manager") + wifiTime);

	timeSync->setTime(wifiTime);
#ifndef DS1302
	rtcTimeSync->setTime(wifiTime);
#else
	rtcTimeSync->setTime(wifiTime);
#endif

	request->send(LittleFS, "/time.html");
}

/*
 * Called when <name>.tar.gz has been uploaded to, or deleted from, /ips/<dir> by
 * either the web UI or MQTT
 */
void filesChanged(const String &dir, const String &name, bool uploaded) {
	// If the file in use was replaced, unpack the new version
	if (uploaded) {
		if (dir == "faces" && name == IPSClock::getClockFace().value) {
			ipsClock->reloadClockFace();
		} else if (dir == "weather" && name == Weather::getIconPack().value) {
			weather->reloadIconPack();
		} else if (dir == "slides" && name == slidesSet->value) {
			reloadSlides = true;
		}
	}

	wsFacesHandler.broadcast(*ws, 0);
	mqttBroker->sendFileDiscovery(dir);
}

#define DELETE_FACE_URL "/delete_face/"

void handleDelete(AsyncWebServerRequest *request) {
	DEBUG("Got delete request");

	String filename = request->url().substring(strlen(DELETE_FACE_URL));

	DEBUG(filename);

	// No '/' so it can't reach outside the current file set
	if (filename.length() > 0 && filename.endsWith(".tar.gz") && filename.indexOf('/') < 0) {
		if (LittleFS.remove("/ips/" + fileSet->value + "/" + filename)) {
			fsChanged();
			request->send(200, "text/plain", "File deleted");

			filesChanged(fileSet->value, filename.substring(0, filename.lastIndexOf(".tar.gz")), false);
			return;
		}
	}

	request->send(500, "text/plain", "Delete failed");
}

// Outcome of a web upload, kept in request->_tempObject (which the request frees) until handleUploadDone() responds
struct UploadResult {
	int code;
	const char *message;
};

// Uploads are written here first so a failed upload doesn't destroy the file it was replacing
static const char *const webUploadTempFile = "/ips/web_upload.tmp";

// Called once the whole request has been received. Only respond here, a request can only have one response.
void handleUploadDone(AsyncWebServerRequest *request) {
	UploadResult *result = (UploadResult*)request->_tempObject;

	if (result == NULL) {
		request->send(400, "text/plain", "No file");
	} else {
		request->send(result->code, "text/plain", result->message);
	}
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
	String name = filename.substring(0, filename.lastIndexOf(".tar.gz"));

	if (!index && request->_tempObject == NULL)
	{
		DEBUG((String) "UploadStart: " + filename);
		UploadResult *result = (UploadResult*)malloc(sizeof(UploadResult));
		if (result == NULL) {
			return;
		}
		*result = { 200, "File uploaded" };
		request->_tempObject = result;

		LittleFS.remove(webUploadTempFile);	// In case an earlier upload was cut short by a reset
		fsChanged();	// Also means the space check below uses an up to date figure

		if (!filename.endsWith(".tar.gz") || !ImageUnpacker::isValidName(name) || len < 2 || data[0] != 0x1f || data[1] != 0x8b) {
			*result = { 415, "Invalid file name or type" };
		} else if (request->contentLength() + 8192 > fsFreeBytes()) {
			*result = { 507, "Not enough space" };
		} else {
			// open the file on first call and store the file handle in the request object
			request->_tempFile = LittleFS.open(webUploadTempFile, "w");
			if (!request->_tempFile) {
				*result = { 500, "Could not create file" };
			}
		}
	}

	UploadResult *result = (UploadResult*)request->_tempObject;
	if (result == NULL || !request->_tempFile) {
		return;	// Failed already
	}

	if (len)
	{
		// stream the incoming chunk to the opened file
		DEBUG((String) "Writing: " + len + " byted");
		if (request->_tempFile.write(data, len) != len) {
			*result = { 500, "Write failed" };
			request->_tempFile.close();
			LittleFS.remove(webUploadTempFile);
			fsChanged();
			return;
		}
		fsChanged();	// Every chunk, in case the upload is abandoned part way through
	}
	if (final)
	{
		DEBUG((String) "UploadEnd: " + filename);
		// close the file handle as the upload is now done, and swap it in
		request->_tempFile.close();
		String path = "/ips/" + fileSet->value + "/" + filename;
		LittleFS.remove(path);
		bool renamed = LittleFS.rename(webUploadTempFile, path);
		fsChanged();
		if (renamed) {
			filesChanged(fileSet->value, name, true);
		} else {
			*result = { 500, "Could not save file" };
		}
	}
}

// Shows text instead of the time, like ~/set/custom over MQTT, e.g. curl -d value=12:34 http://<clock>/custom.
// Replies with the text being shown, so a GET just reads it.
void customHandler(AsyncWebServerRequest *request) {
	if (request->method() == HTTP_POST && request->hasArg("value")) {
		if (!IPSClock::setCustomData(request->arg("value"))) {
			request->send(400, "text/plain", "The value can be at most " + String(NUM_DIGITS) + " characters\n");
			return;
		}
		requestMqttPublish();
	}

	request->send(200, "text/plain", IPSClock::getCustomData().value + "\n");
}

// Rows drawn per drawPanelRows() call while streaming a picture to a panel tube, so the buffer
// stays small and the whole picture is never held in memory
#define PANEL_ROWS_PER_CALL 4
#define PANEL_JPEG_MAX (48 * 1024)			// Largest JPEG body accepted
#define PANEL_JPEG_HEADROOM (16 * 1024)		// Heap left over after taking the body: decoder + everything else
static_assert(PANEL_JPEG_MAX <= sizeof(StaticSprite::output_buffer), "A JPEG must fit the display buffer");
#define PANEL_BATCH_HEADER 14				// Bytes of a /panel/batch record header

// The /panel upload in progress. ESPAsyncWebServer calls the body handler as the body arrives and the
// request handler only once all of it has, so the body handler checks the request and draws, and the
// request handler sends the reply it left in result. Both run on the web server's task, and only one
// upload is allowed at a time, so this needs no lock of its own.
struct PanelUpload {
	AsyncWebServerRequest *request = NULL;	// The upload being drawn
	bool active = false;
	uint8_t tube = 0;
	int16_t x = 0, y = 0, w = TFT_WIDTH, h = TFT_HEIGHT;
	size_t expected = 0;	// w * h * 2, the body length that was asked for
	size_t received = 0;	// Body bytes taken so far
	uint16_t rowBytes = 0;	// Bytes of the current row already in row[]
	int16_t drawnRows = 0;	// Rows already sent to the display
	uint8_t rowCount = 0;	// Complete rows waiting in rows[]
	uint8_t row[TFT_WIDTH * 2];
	uint8_t rows[PANEL_ROWS_PER_CALL * TFT_WIDTH * 2];
	// format=rgb565 (raw, the default), qoi (decoded as it arrives) or jpeg (decoded once all of it
	// is in jpeg[], which is malloc'd for the upload and freed when it ends)
	enum Format : uint8_t { RGB565, QOI, JPEG } format = RGB565;
	QoiStream qoi;
	uint8_t *jpeg = NULL;
	size_t jpegLen = 0;
	bool jpegInDisplayBuffer = false;	// jpeg points into the display buffer, holding memMutex

	// POST /panel/batch: the body is a run of records, each a 14-byte header and then its picture,
	// and the fields above describe the record being drawn
	bool batch = false;
	uint8_t recordHeader[PANEL_BATCH_HEADER];
	uint8_t recordHeaderBytes = 0;	// Header bytes of the next record taken so far
	bool inRecord = false;			// The header is in and the picture is arriving
	uint32_t recordLeft = 0;		// Picture bytes of the current record still to come
	uint16_t records = 0;			// Records drawn so far
	bool replacedWhole = false;		// A record replaced a whole tube, so MQTT needs telling
	// A batch held in memory as it arrives, then drawn by PanelDraw's task (with its transition)
	uint8_t *stage = NULL;
	size_t stageLen = 0;
	PanelDraw::Transition transition;

	AsyncWebServerRequest *resultRequest = NULL;	// The request the reply below is for
	int resultCode = 0;
	const char *resultMessage = "";
	uint32_t resultRetryMs = 0;		// With a 503: when to try again, sent as X-Retry-After-Ms
};
static PanelUpload panelUpload;

// The JPEG body is only held for the upload it belongs to
static void panelUploadFreeJpeg() {
	if (panelUpload.jpegInDisplayBuffer) {
		panelUpload.jpegInDisplayBuffer = false;
		xSemaphoreGive(memMutex);
	} else {
		free(panelUpload.jpeg);
	}
	panelUpload.jpeg = NULL;
	panelUpload.jpegLen = 0;
}

// A batch's memory is only held until it's handed to PanelDraw, or the upload fails
static void panelUploadFreeStage() {
	if (panelUpload.stage != NULL) {
		PanelDraw::freeBuffer(panelUpload.stage);
		panelUpload.stage = NULL;
	}
}

// Leaves the reply for the request handler and makes the state ready for the next upload
static void panelUploadFinish(AsyncWebServerRequest *request, int code, const char *message) {
	if (panelUpload.request == request) {
		panelUpload.active = false;
		panelUpload.request = NULL;
		panelUploadFreeJpeg();
		panelUploadFreeStage();
	}
	panelUpload.resultRequest = request;
	panelUpload.resultCode = code;
	panelUpload.resultMessage = message;
	panelUpload.resultRetryMs = 0;
}

// A 503 because a batch is still being drawn: says when to try again
static void panelUploadBusyDrawing(AsyncWebServerRequest *request, uint32_t retryMs) {
	panelUploadFinish(request, 503, "busy drawing");
	panelUpload.resultRetryMs = retryMs;
}

// Sends the complete rows waiting in panelUpload.rows[] to the tube
static void panelUploadFlushRows() {
	if (panelUpload.rowCount == 0) {
		return;
	}

	tfts->drawPanelRows(panelUpload.tube, panelUpload.x, panelUpload.y + panelUpload.drawnRows,
		panelUpload.w, panelUpload.rowCount, panelUpload.rows);
	panelUpload.drawnRows += panelUpload.rowCount;
	panelUpload.rowCount = 0;
}

// Forgets the upload if its client goes away part way through (the tube then still needs a picture)
static void panelUploadWatchDisconnect(AsyncWebServerRequest *request) {
	request->onDisconnect([request]() {
		if (panelUpload.request == request) {
			panelUpload.active = false;
			panelUpload.request = NULL;
			panelUploadFreeJpeg();
			panelUploadFreeStage();
		}
		if (panelUpload.resultRequest == request) {
			panelUpload.resultRequest = NULL;
		}
	});
}

// Checks a new upload's arguments when its first body bytes arrive. Returns false, with the reply
// left for the request handler, if it can't be drawn.
static bool panelUploadStart(AsyncWebServerRequest *request, size_t total) {
	// Defaults: the whole tube
	int tube = request->hasArg("tube") ? request->arg("tube").toInt() : 0;
	int x = request->hasArg("x") ? request->arg("x").toInt() : 0;
	int y = request->hasArg("y") ? request->arg("y").toInt() : 0;
	int w = request->hasArg("w") ? request->arg("w").toInt() : TFT_WIDTH;
	int h = request->hasArg("h") ? request->arg("h").toInt() : TFT_HEIGHT;

	if (tube < 0 || tube >= NUM_DIGITS) {
		panelUploadFinish(request, 400, "tube must be 0 to 5");
		return false;
	}
	if (w <= 0 || h <= 0 || x < 0 || y < 0 || w > TFT_WIDTH || h > TFT_HEIGHT || x > TFT_WIDTH - w || y > TFT_HEIGHT - h) {
		panelUploadFinish(request, 400, "rectangle must be inside the tube");
		return false;
	}
	PanelUpload::Format format = PanelUpload::RGB565;
	if (request->hasArg("format")) {
		String f = request->arg("format");
		if (f == "qoi") {
			format = PanelUpload::QOI;
		} else if (f == "jpeg" || f == "jpg") {
			format = PanelUpload::JPEG;
		} else if (f != "rgb565") {
			panelUploadFinish(request, 400, "format must be rgb565, qoi or jpeg");
			return false;
		}
	}
	size_t expected = (size_t)w * h * 2;
	if (format == PanelUpload::RGB565 && total != expected) {
		panelUploadFinish(request, 400, "body length must be w*h*2");
		return false;
	}
	if (format == PanelUpload::QOI && total < 14 + 8) {
		panelUploadFinish(request, 400, "not a QOI file");
		return false;
	}
	if (format == PanelUpload::JPEG && (total < 4 || total > PANEL_JPEG_MAX)) {
		panelUploadFinish(request, 413, "a JPEG must be at most 48 KB");
		return false;
	}
	if (!tfts->isPanelTube((uint8_t)tube)) {
		panelUploadFinish(request, 409, "not a panel tube");
		return false;
	}
	if (panelUpload.active) {
		panelUploadFinish(request, 503, "another panel upload is in progress");
		return false;
	}
	if (OTA::otaBusy()) {
		panelUploadFinish(request, 503, "a firmware update is in progress");
		return false;
	}
	if (PanelDraw::busy()) {
		panelUploadBusyDrawing(request, PanelDraw::msLeft());	// Pictures are drawn in the order they come
		return false;
	}

	if (format == PanelUpload::JPEG) {
		// The decoder needs all of it. The display buffer is idle in panel mode unless GIFs live in
		// it, so it holds the JPEG then (no heap needed); otherwise it comes from the heap, leaving room
		// for the decoder and everything else.
		if (!GifPlayer::active() && xSemaphoreTake(memMutex, 0) == pdTRUE) {
			if (GifPlayer::active()) {	// One started meanwhile
				xSemaphoreGive(memMutex);
			} else {
				panelUpload.jpeg = StaticSprite::output_buffer;
				panelUpload.jpegInDisplayBuffer = true;
			}
		}
		if (panelUpload.jpeg == NULL
				&& (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < total + PANEL_JPEG_HEADROOM
					|| (panelUpload.jpeg = (uint8_t*)malloc(total)) == NULL)) {
			panelUploadFinish(request, 503, "not enough memory for this JPEG");
			return false;
		}
		panelUpload.jpegLen = total;
	}

	GifPlayer::stop((uint8_t)tube);	// A picture replaces whatever GIF played on the tube

	panelUpload.request = request;
	panelUpload.active = true;
	panelUpload.format = format;
	panelUpload.tube = (uint8_t)tube;
	panelUpload.x = (int16_t)x;
	panelUpload.y = (int16_t)y;
	panelUpload.w = (int16_t)w;
	panelUpload.h = (int16_t)h;
	panelUpload.expected = expected;
	panelUpload.received = 0;
	panelUpload.rowBytes = 0;
	panelUpload.drawnRows = 0;
	panelUpload.rowCount = 0;
	panelUpload.batch = false;
	if (format == PanelUpload::QOI) {
		panelUpload.qoi.begin((uint16_t)w, (uint16_t)h);
	}
	panelUploadWatchDisconnect(request);
	return true;
}

// One more decoded pixel (RGB565, big-endian) for the row being assembled; complete rows are drawn
// PANEL_ROWS_PER_CALL at a time like the raw upload's
static void panelPutPixel(uint8_t hi, uint8_t lo) {
	size_t rowLen = (size_t)panelUpload.w * 2;
	panelUpload.row[panelUpload.rowBytes++] = hi;
	panelUpload.row[panelUpload.rowBytes++] = lo;
	if (panelUpload.rowBytes == rowLen) {
		memcpy(panelUpload.rows + (size_t)panelUpload.rowCount * rowLen, panelUpload.row, rowLen);
		panelUpload.rowBytes = 0;
		panelUpload.rowCount++;
		if (panelUpload.rowCount == PANEL_ROWS_PER_CALL) {
			panelUploadFlushRows();
		}
	}
}

static void panelQoiPixel(void *ctx, uint8_t r, uint8_t g, uint8_t b) {
	(void)ctx;
	uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	panelPutPixel((uint8_t)(c >> 8), (uint8_t)(c & 0xFF));
}

// Takes raw RGB565 bytes of the rectangle being drawn, at most up to its end, and draws complete rows
// PANEL_ROWS_PER_CALL at a time. Returns how many bytes it took.
static size_t panelRawFeed(const uint8_t *data, size_t len) {
	size_t rowLen = (size_t)panelUpload.w * 2;
	size_t offset = 0;
	while (offset < len && panelUpload.received < panelUpload.expected) {
		size_t take = rowLen - panelUpload.rowBytes;
		if (take > len - offset) {
			take = len - offset;
		}
		memcpy(panelUpload.row + panelUpload.rowBytes, data + offset, take);
		panelUpload.rowBytes += take;
		panelUpload.received += take;
		offset += take;

		if (panelUpload.rowBytes == rowLen) {
			// A complete row: keep it with the others until there are enough to draw
			memcpy(panelUpload.rows + (size_t)panelUpload.rowCount * rowLen, panelUpload.row, rowLen);
			panelUpload.rowBytes = 0;
			panelUpload.rowCount++;
			if (panelUpload.rowCount == PANEL_ROWS_PER_CALL) {
				panelUploadFlushRows();
			}
		}
	}
	return offset;
}

// A whole tube was replaced: put the status message back on top of it and mark it as done
static void panelWholeTubeReplaced(uint8_t tube) {
	PanelDraw::wholeTubeReplaced(tube);
}

// Done once the whole body is in, whatever its format: the bookkeeping for a replaced tube and the reply
static void panelUploadComplete(AsyncWebServerRequest *request) {
	if (panelUpload.x == 0 && panelUpload.y == 0 && panelUpload.w == TFT_WIDTH && panelUpload.h == TFT_HEIGHT) {
		// The whole tube was replaced, so tell Home Assistant the tube has its picture now
		panelWholeTubeReplaced(panelUpload.tube);
		requestMqttPublish();
	}
	panelUploadFinish(request, 200, "ok");
}

/*
 * Streams the body of POST /panel?tube=N[&x=X&y=Y&w=W&h=H] to a panel tube, row by row, so a
 * whole-tube picture (64 KB) is never held in memory. The pixels are RGB565, big-endian, row by
 * row, and the body must be exactly w*h*2 bytes. panelHandler() sends the reply.
 */
void panelBodyHandler(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
	if (index == 0 && !panelUploadStart(request, total)) {
		return;
	}
	if (!panelUpload.active || panelUpload.request != request) {
		return;	// The rest of a request that was rejected
	}

	if (panelUpload.format == PanelUpload::QOI) {
		QoiStream::Result r = panelUpload.qoi.feed(data, len, panelQoiPixel, NULL);
		if (r == QoiStream::ERROR) {
			panelUploadFinish(request, 400, panelUpload.qoi.error());
			return;
		}
		if (index + len >= total) {
			if (!panelUpload.qoi.complete()) {
				panelUploadFinish(request, 400, "QOI image incomplete");
				return;
			}
			panelUploadFlushRows();
			panelUploadComplete(request);
		}
		return;
	}

	if (panelUpload.format == PanelUpload::JPEG) {
		if (index + len > panelUpload.jpegLen) {
			panelUploadFinish(request, 400, "JPEG body longer than announced");
			return;
		}
		memcpy(panelUpload.jpeg + index, data, len);
		if (index + len >= total) {
			const char *err = jpegDrawPanel(panelUpload.jpeg, panelUpload.jpegLen, panelUpload.tube,
				panelUpload.x, panelUpload.y, panelUpload.w, panelUpload.h);
			panelUploadFreeJpeg();
			if (err != NULL) {
				panelUploadFinish(request, 400, err);
				return;
			}
			panelUploadComplete(request);
		}
		return;
	}

	if (panelRawFeed(data, len) < len) {
		panelUploadFinish(request, 400, "body length must be w*h*2");
		return;
	}

	if (index + len >= total) {
		if (panelUpload.received != panelUpload.expected) {
			panelUploadFinish(request, 400, "body length must be w*h*2");
			return;
		}
		panelUploadFlushRows();
		panelUploadComplete(request);
	}
}

#define PANEL_BATCH_STREAM_MAX 4096	// Largest batch drawn as it arrives when it can't be held in memory

// Reads transition=none|wipe|cascade, duration (ms, a wipe's length), gap (ms between a cascade's
// tubes) and order (tubes, e.g. 2,3,1,4,0,5). Returns NULL or what's wrong.
static const char *panelParseTransition(AsyncWebServerRequest *request, PanelDraw::Transition &t) {
	t = PanelDraw::Transition();
	if (!request->hasArg("transition")) {
		return NULL;
	}
	String kind = request->arg("transition");
	if (kind == "wipe") {
		t.kind = PanelDraw::WIPE;
	} else if (kind == "cascade") {
		t.kind = PanelDraw::CASCADE;
	} else if (kind == "flip") {
		t.kind = PanelDraw::FLIP;
	} else if (kind != "none") {
		return "transition must be none, wipe, cascade or flip";
	}
	if (request->hasArg("duration")) {
		long v = request->arg("duration").toInt();
		if (v < 0 || v > 10000) {
			return "duration must be 0 to 10000 ms";
		}
		t.durationMs = (uint16_t)v;
	}
	if (request->hasArg("gap")) {
		long v = request->arg("gap").toInt();
		if (v < 0 || v > 5000) {
			return "gap must be 0 to 5000 ms";
		}
		t.gapMs = (uint16_t)v;
	}
	if (request->hasArg("order")) {
		String order = request->arg("order");
		uint8_t seen = 0;
		t.orderLen = 0;
		for (size_t i = 0; i < order.length(); i++) {
			char c = order[i];
			if (c == ',') {
				continue;
			}
			if (c < '0' || c > '5' || (seen & (1 << (c - '0'))) || t.orderLen == 6) {
				return "order must be tubes 0 to 5, each at most once, e.g. 2,3,1,4,0,5";
			}
			seen |= (uint8_t)(1 << (c - '0'));
			t.order[t.orderLen++] = (uint8_t)(c - '0');
		}
	}
	return NULL;
}

static uint16_t panelLe16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

// Checks a POST /panel/batch when its first body bytes arrive. Returns false, with the reply left
// for the request handler, if it can't be taken.
static bool panelBatchStart(AsyncWebServerRequest *request, size_t total) {
	if (total < PANEL_BATCH_HEADER) {
		panelUploadFinish(request, 400, "a batch needs at least one record");
		return false;
	}
	if (panelUpload.active) {
		panelUploadFinish(request, 503, "another panel upload is in progress");
		return false;
	}
	if (OTA::otaBusy()) {
		panelUploadFinish(request, 503, "a firmware update is in progress");
		return false;
	}
	PanelDraw::Transition transition;
	const char *err = panelParseTransition(request, transition);
	if (err != NULL) {
		panelUploadFinish(request, 400, err);
		return false;
	}
	if (PanelDraw::busy()) {
		// interrupt: this batch replaces the one being drawn (a new screen), so stop that one
		if (request->hasArg("interrupt")) {
			PanelDraw::interrupt();
			panelUploadBusyDrawing(request, 20);
		} else {
			panelUploadBusyDrawing(request, PanelDraw::msLeft());
		}
		return false;
	}
	bool retrySoon = false;
	uint8_t *stage = PanelDraw::takeBuffer(total, &retrySoon);
	if (stage == NULL && retrySoon) {
		panelUploadBusyDrawing(request, 30);	// The display buffer is free again in a moment
		return false;
	}
	if (stage == NULL && (transition.kind != PanelDraw::NONE || total > PANEL_BATCH_STREAM_MAX)) {
		// Drawn as it arrives, a big batch would lose packets and stall, see PanelDraw.h
		panelUploadFinish(request, 413, "batch too big to hold in memory now, see batch_buffer in GET /panel");
		return false;
	}

	panelUpload.request = request;
	panelUpload.active = true;
	panelUpload.batch = true;
	panelUpload.recordHeaderBytes = 0;
	panelUpload.inRecord = false;
	panelUpload.recordLeft = 0;
	panelUpload.records = 0;
	panelUpload.replacedWhole = false;
	panelUpload.stage = stage;
	panelUpload.stageLen = total;
	panelUpload.transition = transition;
	panelUploadWatchDisconnect(request);
	return true;
}

// The header of the next record is in: check it and get ready to draw its picture
static bool panelBatchRecordStart(AsyncWebServerRequest *request) {
	const uint8_t *hd = panelUpload.recordHeader;
	uint8_t tube = hd[0];
	uint8_t fmt = hd[1];
	int x = panelLe16(hd + 2), y = panelLe16(hd + 4), w = panelLe16(hd + 6), h = panelLe16(hd + 8);
	uint32_t len = (uint32_t)hd[10] | ((uint32_t)hd[11] << 8) | ((uint32_t)hd[12] << 16) | ((uint32_t)hd[13] << 24);

	if (tube >= NUM_DIGITS) {
		panelUploadFinish(request, 400, "batch record: tube must be 0 to 5");
		return false;
	}
	if (w <= 0 || h <= 0 || w > TFT_WIDTH || h > TFT_HEIGHT || x > TFT_WIDTH - w || y > TFT_HEIGHT - h) {
		panelUploadFinish(request, 400, "batch record: rectangle must be inside the tube");
		return false;
	}
	size_t expected = (size_t)w * h * 2;
	if (fmt == 0) {
		if (len != expected) {
			panelUploadFinish(request, 400, "batch record: rgb565 length must be w*h*2");
			return false;
		}
	} else if (fmt == 1) {
		if (len < 14 + 8) {
			panelUploadFinish(request, 400, "batch record: not a QOI image");
			return false;
		}
	} else {
		panelUploadFinish(request, 400, "batch record: format must be 0 (rgb565) or 1 (qoi)");
		return false;
	}
	if (!tfts->isPanelTube(tube)) {
		panelUploadFinish(request, 409, "not a panel tube");
		return false;
	}

	GifPlayer::stop(tube);	// A picture replaces whatever GIF played on the tube

	panelUpload.format = fmt == 1 ? PanelUpload::QOI : PanelUpload::RGB565;
	panelUpload.tube = tube;
	panelUpload.x = (int16_t)x;
	panelUpload.y = (int16_t)y;
	panelUpload.w = (int16_t)w;
	panelUpload.h = (int16_t)h;
	panelUpload.expected = expected;
	panelUpload.received = 0;
	panelUpload.rowBytes = 0;
	panelUpload.drawnRows = 0;
	panelUpload.rowCount = 0;
	if (fmt == 1) {
		panelUpload.qoi.begin((uint16_t)w, (uint16_t)h);
	}
	panelUpload.recordLeft = len;
	panelUpload.inRecord = true;
	return true;
}

// All of a record's bytes are in: draw what's left of it and do a replaced tube's bookkeeping
static bool panelBatchRecordEnd(AsyncWebServerRequest *request) {
	if (panelUpload.format == PanelUpload::QOI ? !panelUpload.qoi.complete()
			: panelUpload.received != panelUpload.expected) {
		panelUploadFinish(request, 400, "batch record: picture incomplete");
		return false;
	}
	panelUploadFlushRows();
	if (panelUpload.x == 0 && panelUpload.y == 0 && panelUpload.w == TFT_WIDTH && panelUpload.h == TFT_HEIGHT) {
		panelWholeTubeReplaced(panelUpload.tube);
		panelUpload.replacedWhole = true;
	}
	panelUpload.records++;
	panelUpload.inRecord = false;
	return true;
}

// Feeds batch bytes that are drawn as they arrive (a batch that isn't held in memory). Returns false,
// with the reply left for the request handler, if a record is bad.
static bool panelBatchFeed(AsyncWebServerRequest *request, const uint8_t *data, size_t len) {
	size_t offset = 0;
	while (offset < len) {
		if (!panelUpload.inRecord) {
			size_t take = PANEL_BATCH_HEADER - panelUpload.recordHeaderBytes;
			if (take > len - offset) {
				take = len - offset;
			}
			memcpy(panelUpload.recordHeader + panelUpload.recordHeaderBytes, data + offset, take);
			panelUpload.recordHeaderBytes += take;
			offset += take;
			if (panelUpload.recordHeaderBytes == PANEL_BATCH_HEADER) {
				panelUpload.recordHeaderBytes = 0;
				if (!panelBatchRecordStart(request)) {
					return false;
				}
			}
			continue;
		}

		size_t take = panelUpload.recordLeft;
		if (take > len - offset) {
			take = len - offset;
		}
		if (panelUpload.format == PanelUpload::QOI) {
			if (panelUpload.qoi.feed(data + offset, take, panelQoiPixel, NULL) == QoiStream::ERROR) {
				panelUploadFinish(request, 400, panelUpload.qoi.error());
				return false;
			}
		} else {
			panelRawFeed(data + offset, take);	// The header's length is w*h*2, so it takes all of it
		}
		offset += take;
		panelUpload.recordLeft -= take;
		if (panelUpload.recordLeft == 0 && !panelBatchRecordEnd(request)) {
			return false;
		}
	}

	return true;
}

/*
 * The body of POST /panel/batch[?transition=wipe|cascade&duration=&gap=&order=&interrupt=1]: several
 * pictures in one request, each record a 14-byte header (little-endian: tube, format 0 = rgb565 /
 * 1 = qoi / 2 = jpeg, x, y, w, h, length) and then the picture as POST /panel takes it.
 *
 * Normally the body is only copied into memory as it arrives and PanelDraw's task draws it once it's
 * all in, playing the transition if one was asked for, see PanelDraw.h. When there isn't the memory
 * to hold it, a batch of at most PANEL_BATCH_STREAM_MAX bytes (raw or QOI, no transition) is drawn as
 * it arrives instead, and a bigger one is refused with 413. The records before a bad one are drawn;
 * panelHandler() sends the reply.
 */
void panelBatchBodyHandler(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
	if (index == 0 && !panelBatchStart(request, total)) {
		return;
	}
	if (!panelUpload.active || panelUpload.request != request || !panelUpload.batch) {
		return;	// The rest of a request that was rejected
	}

	if (panelUpload.stage != NULL) {
		// Held in memory as it arrives, then drawn by PanelDraw's task, see PanelDraw.h
		if (index + len > panelUpload.stageLen) {
			panelUploadFinish(request, 400, "batch longer than announced");
			return;
		}
		memcpy(panelUpload.stage + index, data, len);
		if (index + len < total) {
			return;
		}
		int code = 200;
		const char *err = PanelDraw::start(panelUpload.stage, total, panelUpload.transition, &code);
		if (err != NULL) {
			panelUploadFinish(request, code, err);	// Frees the memory
			return;
		}
		panelUpload.stage = NULL;	// PanelDraw's now
		panelUploadFinish(request, 200, "ok");
		return;
	}

	if (!panelBatchFeed(request, data, len)) {
		return;
	}

	if (index + len >= total) {
		bool replaced = panelUpload.replacedWhole;
		if (panelUpload.inRecord || panelUpload.recordHeaderBytes != 0) {
			panelUploadFinish(request, 400, "batch ends part way through a record");
		} else {
			panelUploadFinish(request, 200, "ok");
		}
		if (replaced) {
			requestMqttPublish();	// Home Assistant hears which tubes have their pictures now
		}
	}
}

/*
 * POST /panel/test[?transition=wipe|cascade&duration=&gap=&order=]: a test pattern on every panel
 * tube, made on the clock, played with the transition. For the Panels page.
 */
void panelTestHandler(AsyncWebServerRequest *request) {
	if (OTA::otaBusy()) {
		request->send(503, "text/plain", "a firmware update is in progress");
		return;
	}
	PanelDraw::Transition transition;
	const char *err = panelParseTransition(request, transition);
	if (err != NULL) {
		request->send(400, "text/plain", err);
		return;
	}
	if (PanelDraw::busy()) {
		AsyncWebServerResponse *response = request->beginResponse(503, "text/plain", "busy drawing");
		response->addHeader("X-Retry-After-Ms", String((unsigned long)PanelDraw::msLeft()));
		request->send(response);
		return;
	}
	int code = 200;
	err = PanelDraw::startTest(transition, &code);
	request->send(code, "text/plain", err != NULL ? err : "ok");
}

/*
 * GET /panel replies which tubes are panel tubes and which of them need a fresh picture:
 * {"tubes":[0,1,2],"needed":[3],"width":135,"height":240}
 * POST /panel sends the reply panelBodyHandler() left, once the whole body has arrived. The web UI
 * login applies to it like any other page, see WebLoginHandler.
 */
void panelHandler(AsyncWebServerRequest *request) {
	if (request->method() == HTTP_POST) {
		if (panelUpload.resultRequest == request) {
			panelUpload.resultRequest = NULL;
			AsyncWebServerResponse *response = request->beginResponse(panelUpload.resultCode, "text/plain",
				panelUpload.resultMessage);
			if (panelUpload.resultCode == 503 && panelUpload.resultRetryMs > 0) {
				response->addHeader("X-Retry-After-Ms", String((unsigned long)panelUpload.resultRetryMs));
			}
			request->send(response);
		} else {
			// No body, so the body handler never saw it
			if (panelUpload.request == request) {
				panelUpload.active = false;
				panelUpload.request = NULL;
			}
			request->send(400, "text/plain", "body length must be w*h*2");
		}
		return;
	}

	uint8_t tubes = tfts->getPanelTubes();
	uint8_t needed = tfts->getPanelsNeeded();
	String json = "{\"tubes\":[";
	String sep = "";
	for (uint8_t i = 0; i < NUM_DIGITS; i++) {
		if (tubes & (1 << i)) {
			json += sep + String(i);
			sep = ",";
		}
	}
	json += "],\"needed\":[";
	sep = "";
	for (uint8_t i = 0; i < NUM_DIGITS; i++) {
		if (needed & (1 << i)) {
			json += sep + String(i);
			sep = ",";
		}
	}
	json += "],\"playing\":[";
	sep = "";
	uint8_t playing = GifPlayer::playingMask();
	for (uint8_t i = 0; i < NUM_DIGITS; i++) {
		if (playing & (1 << i)) {
			json += sep + String(i);
			sep = ",";
		}
	}
	json += "],\"width\":" + String(TFT_WIDTH) + ",\"height\":" + String(TFT_HEIGHT)
		+ ",\"formats\":[\"rgb565\",\"qoi\",\"jpeg\"],\"batch\":true,\"batch_buffer\":"
		+ String((unsigned long)PanelDraw::bufferFree())
		+ ",\"transitions\":[\"wipe\",\"cascade\",\"flip\"],\"flip_memory\":"
		+ String((unsigned long)PanelDraw::flipMemory())
		+ ",\"last_flip\":{\"folded\":" + String(PanelDraw::lastFlipFoldedTubes())
		+ ",\"frames\":" + String(PanelDraw::lastFlipFrameCount()) + "}"
		+ ",\"drawing\":" + String(PanelDraw::busy() ? "true" : "false")
		+ ",\"last_draw_ms\":" + String((unsigned long)PanelDraw::lastDurationMs())
#ifdef HA_ONLY
		+ ",\"weather\":false,\"slides\":false"	// For the web UI, which hides those settings then
#endif
		+ ",\"gif\":true,\"gif_free\":"
		+ String((unsigned long)GifPlayer::freeBytes())
		+ ",\"firmware\":\"" FIRMWARE_VERSION "\"}";
	request->send(200, "application/json", json);
}

/*
 * GIFs stored on the clock and played on panel tubes, see GifPlayer.h.
 *   GET    /gif                      {"files":[{"name","size"}],"free":bytes}
 *   POST   /gif?name=<name>.gif      body = the GIF file (at most 600 KB and 135x240)
 *   DELETE /gif?name=<name>.gif      (or POST /gif/delete?name=...)
 *   POST   /gif/play?tube=N&name=<name>.gif[&x=X&y=Y][&loop=0|1][&bg=RRGGBB]
 *   POST   /gif/stop[?tube=N]        the last frame stays; no tube = all
 * Like /panel, the body callback does the work and the request handler only replies.
 */
static AsyncWebServerRequest *gifUploadRequest = NULL;
static const char *gifUploadError = NULL;

void gifBodyHandler(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
	if (index == 0) {
		gifUploadRequest = request;
		gifUploadError = GifPlayer::uploadBegin(request->arg("name").c_str(), total);
		request->onDisconnect([request]() {
			if (gifUploadRequest == request) {
				GifPlayer::uploadAbort();
				gifUploadRequest = NULL;
			}
		});
	}
	if (gifUploadRequest != request || gifUploadError != NULL) {
		return;
	}
	gifUploadError = GifPlayer::uploadWrite(data, len);
	if (gifUploadError == NULL && index + len >= total) {
		gifUploadError = GifPlayer::uploadEnd();
	}
}

void gifHandler(AsyncWebServerRequest *request) {
	if (request->method() == HTTP_GET) {
		request->send(200, "application/json", GifPlayer::listJson());
		return;
	}
	if (request->method() == HTTP_DELETE) {
		String name = request->arg("name");
		request->send(GifPlayer::remove(name.c_str()) ? 200 : 404, "text/plain", "ok");
		return;
	}
	// POST: the upload
	if (gifUploadRequest != request) {
		request->send(400, "text/plain", "the body must be the GIF file");
		return;
	}
	gifUploadRequest = NULL;
	if (gifUploadError != NULL) {
		request->send(400, "text/plain", gifUploadError);
	} else {
		request->send(200, "text/plain", "ok");
	}
}

void gifDeleteHandler(AsyncWebServerRequest *request) {
	String name = request->arg("name");
	request->send(GifPlayer::remove(name.c_str()) ? 200 : 404, "text/plain", "ok");
}

void gifPlayHandler(AsyncWebServerRequest *request) {
	int tube = request->hasArg("tube") ? request->arg("tube").toInt() : -1;
	if (tube < 0 || tube >= NUM_DIGITS) {
		request->send(400, "text/plain", "tube must be 0 to 5");
		return;
	}
	int x = request->hasArg("x") ? request->arg("x").toInt() : -1;
	int y = request->hasArg("y") ? request->arg("y").toInt() : -1;
	bool loop = !request->hasArg("loop") || request->arg("loop") != "0";
	uint16_t bg = 0;
	if (request->hasArg("bg")) {
		uint32_t rgb = (uint32_t)strtoul(request->arg("bg").c_str(), NULL, 16);
		bg = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
	}
	String name = request->arg("name");
	const char *err = GifPlayer::play((uint8_t)tube, name.c_str(), (int16_t)x, (int16_t)y, loop, bg);
	if (err == NULL) {
		request->send(200, "text/plain", "ok");
	} else {
		int code = strstr(err, "already playing") != NULL || strstr(err, "busy") != NULL ? 503
			: strstr(err, "panel tube") != NULL ? 409 : strstr(err, "no such") != NULL ? 404 : 400;
		request->send(code, "text/plain", err);
	}
}

void gifStopHandler(AsyncWebServerRequest *request) {
	if (request->hasArg("tube")) {
		int tube = request->arg("tube").toInt();
		if (tube >= 0 && tube < NUM_DIGITS) {
			GifPlayer::stop((uint8_t)tube);
		}
	} else {
		GifPlayer::stopAll();
	}
	request->send(200, "text/plain", "ok");
}

/*
 * The firmware update in progress, like PanelUpload above. ESPAsyncWebServer calls the upload and
 * body callbacks while the data arrives and the request handler only once all of it has, so the
 * checks and the writing happen there and updateHandler() sends the reply left in result. Both run
 * on the web server's task, and OTA only allows one update at a time, so this needs no lock.
 */
struct UpdateUpload {
	AsyncWebServerRequest *request = NULL;	// The upload being written
	bool active = false;

	AsyncWebServerRequest *resultRequest = NULL;	// The request the reply below is for
	int resultCode = 0;
	const char *resultMessage = "";
};
static UpdateUpload updateUpload;

// Leaves the reply for the request handler and makes the state ready for the next upload
static void updateUploadFinish(AsyncWebServerRequest *request, int code, const char *message) {
	if (updateUpload.request == request) {
		updateUpload.active = false;
		updateUpload.request = NULL;
	}
	updateUpload.resultRequest = request;
	updateUpload.resultCode = code;
	updateUpload.resultMessage = message;
}

// Checks a new upload when its first bytes arrive. Returns false, with the reply left for the
// request handler, if it can't be written. size is the firmware's size when it's known: the raw
// body's length, or 0 for a multipart upload, whose content length includes the boundaries.
static bool updateUploadStart(AsyncWebServerRequest *request, size_t size) {
	// ?target=fs writes the file system image (the web UI, faces, icons) instead of the firmware
	bool filesystem = request->hasArg("target") && request->arg("target") == "fs";
	if (!filesystem && !OTA::otaAvailable()) {
		updateUploadFinish(request, 501, "This clock has no room for updates over Wi-Fi");
		return false;
	}
	if (OTA::otaBusy()) {
		updateUploadFinish(request, 409, "An update is already running");
		return false;
	}

	String md5 = request->hasArg("md5") ? request->arg("md5") : "";
	if (!OTA::otaBegin(size, md5.c_str(), filesystem)) {
		updateUploadFinish(request, 500, OTA::otaError());
		return false;
	}

	updateUpload.request = request;
	updateUpload.active = true;
	request->onDisconnect([request]() {
		// Aborted part way through
		if (updateUpload.request == request) {
			updateUpload.active = false;
			updateUpload.request = NULL;
			OTA::otaAbort("connection lost");
		}
		if (updateUpload.resultRequest == request) {
			updateUpload.resultRequest = NULL;
		}
	});
	return true;
}

// Writes one chunk of the firmware to the other app slot. For a raw body, done is index + len >= total
// and total is the firmware's size; for a multipart file part, the upload callback's final flag says
// done and the size isn't known, so 0 is passed to otaBegin().
static void updateUploadWrite(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total, bool done) {
	if (index == 0 && !updateUploadStart(request, total)) {
		return;
	}
	if (!updateUpload.active || updateUpload.request != request) {
		return;	// The rest of a request that was rejected
	}

	if (len > 0 && !OTA::otaWrite(data, len)) {
		updateUploadFinish(request, 500, OTA::otaError());
		return;
	}

	if (done) {
		if (OTA::otaEnd()) {
			updateUploadFinish(request, 200, "Updated, restarting");
		} else {
			updateUploadFinish(request, 500, OTA::otaError());
		}
	}
}

// The multipart file field "firmware" from the web UI's Info page. Its size isn't known until the
// part ends, so otaBegin() gets 0 (unknown) and the md5 query argument is the only check.
static void updateUploadHandler(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
	(void)filename;
	updateUploadWrite(request, data, len, index, 0, final);
}

// A raw body, e.g. curl --data-binary @firmware.bin -H 'Content-Type: application/octet-stream'
static void updateBodyHandler(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
	updateUploadWrite(request, data, len, index, total, index + len >= total);
}

/*
 * POST /update sends the reply updateUploadWrite() left, once the whole body has arrived. The web UI
 * login applies to it like any other page, see WebLoginHandler.
 */
static void updateHandler(AsyncWebServerRequest *request) {
	if (updateUpload.resultRequest == request) {
		updateUpload.resultRequest = NULL;
		request->send(updateUpload.resultCode, "text/plain", updateUpload.resultMessage);
	} else {
		// No body, so the body handler never saw it
		if (updateUpload.request == request) {
			updateUpload.active = false;
			updateUpload.request = NULL;
			OTA::otaAbort("connection lost");
		}
		request->send(400, "text/plain", "no firmware in the request body");
	}
}

static const char *const webLoginRealm = "IPS Clock";

// Never through the clock's own access point, which has a password of its own. That's how a forgotten
// login can be changed: the access point comes on when the clock can't join Wi-Fi at startup.
bool webLoginRequired(AsyncWebServerRequest *request) {
	return webUser.value.length() > 0 && webPassword.value.length() > 0 && !ON_AP_FILTER(request);
}

// Not every browser sends the login with the web socket's handshake, so the pages set this cookie
// and the web socket checks it instead. It changes when the login does.
String webLoginCookie() {
	String login = chipId + ":" + webUser.value + ":" + webPassword.value;
	uint8_t hash[32];
	mbedtls_sha256_ret((const unsigned char*)login.c_str(), login.length(), hash, 0);

	String cookie = "ips_login=";
	for (uint8_t b : hash) {
		cookie += "0123456789abcdef"[b >> 4];
		cookie += "0123456789abcdef"[b & 15];
	}

	return cookie;
}

void addWebLoginCookie(AsyncWebServerRequest *request, AsyncWebServerResponse *response) {
	if (webLoginRequired(request)) {
		response->addHeader("Set-Cookie", webLoginCookie() + "; Path=/; HttpOnly; SameSite=Strict");
	}
}

// Answers every request that needs the login and doesn't have it, before any other handler sees it
class WebLoginHandler : public AsyncWebHandler {
public:
	bool canHandle(AsyncWebServerRequest *request) override {
		if (!webLoginRequired(request)) {
			return false;
		}

		if (request->url() == ws->url()) {
			AsyncWebHeader *cookies = request->getHeader("Cookie");
			return cookies == NULL || ("; " + cookies->value() + ";").indexOf("; " + webLoginCookie() + ";") < 0;
		}

		return !request->authenticate(webUser.value.c_str(), webPassword.value.c_str(), webLoginRealm);
	}

	void handleRequest(AsyncWebServerRequest *request) override {
		request->requestAuthentication(webLoginRealm);	// Digest, so the browser doesn't send the password itself
	}
};

// The files in board_build.embed_files
#define SETTINGS_PAGE(name) \
	extern const uint8_t name##PageStart[] asm("_binary_data_" #name "_html_gz_start"); \
	extern const uint8_t name##PageEnd[] asm("_binary_data_" #name "_html_gz_end");

SETTINGS_PAGE(clock)
SETTINGS_PAGE(leds)
SETTINGS_PAGE(faces)
#ifndef HA_ONLY
SETTINGS_PAGE(weather)
#endif
SETTINGS_PAGE(matrix)
SETTINGS_PAGE(mqtt)
SETTINGS_PAGE(network)
SETTINGS_PAGE(info)
SETTINGS_PAGE(panels)
SETTINGS_PAGE(about)

struct SettingsPage {
	const char *url;
	const uint8_t *start;
	const uint8_t *end;
};

// Served from the firmware rather than the filesystem, so they always match it, even when only the
// firmware was installed. The filesystem image has copies, for anything older that looks for them there.
static const SettingsPage settingsPages[] = {
	{ "/clock.html", clockPageStart, clockPageEnd },
	{ "/leds.html", ledsPageStart, ledsPageEnd },
	{ "/faces.html", facesPageStart, facesPageEnd },
#ifndef HA_ONLY
	{ "/weather.html", weatherPageStart, weatherPageEnd },
#endif
	{ "/matrix.html", matrixPageStart, matrixPageEnd },
	{ "/mqtt.html", mqttPageStart, mqttPageEnd },
	{ "/network.html", networkPageStart, networkPageEnd },
	{ "/info.html", infoPageStart, infoPageEnd },
	{ "/panels.html", panelsPageStart, panelsPageEnd },
	{ "/about.html", aboutPageStart, aboutPageEnd }
};

#define EMBEDDED_FILE(name, file) \
	extern const uint8_t name##Start[] asm("_binary_data_" file "_gz_start"); \
	extern const uint8_t name##End[] asm("_binary_data_" file "_gz_end");

EMBEDDED_FILE(theme, "theme_css")
EMBEDDED_FILE(faviconSvg, "favicon_svg")
EMBEDDED_FILE(faviconPng, "favicon_png")
EMBEDDED_FILE(touchIcon, "apple_touch_icon_png")

struct EmbeddedFile {
	const char *url;
	const char *type;
	const uint8_t *start;
	const uint8_t *end;
};

// The web UI's look and its icons. The settings pages link the stylesheet as well as app.html, and theme.js
// swaps in the icon, so a clock with an older filesystem image, and so an older app.html, gets them too.
static const EmbeddedFile embeddedFiles[] = {
	{ "/theme.css", "text/css", themeStart, themeEnd },
	{ "/favicon.svg", "image/svg+xml", faviconSvgStart, faviconSvgEnd },
	{ "/favicon.png", "image/png", faviconPngStart, faviconPngEnd },
	{ "/apple-touch-icon.png", "image/png", touchIconStart, touchIconEnd }
};

// Browsers keep a page sent with an ETag and ask each time whether it has changed. When it hasn't, the
// clock answers with a few hundred bytes instead of the page. That matters most for app.html, 147 KB that
// was sent every time the web UI was opened, which left the clock short of memory while it did.

// For the files built into the firmware, which only change with it
static const String &firmwareEtag() {
	static String etag;

	if (etag.length() == 0) {
		char hex[17];
		const uint8_t *sha = esp_ota_get_app_description()->app_elf_sha256;
		snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x", sha[0], sha[1], sha[2], sha[3], sha[4], sha[5], sha[6], sha[7]);
		etag = String("\"") + hex + "\"";
	}

	return etag;
}

// For app.html, which only changes with a new filesystem image, so only after a restart. Empty if it's missing.
static const String &appEtag() {
	static String etag;

	if (etag.length() == 0) {
		// Whichever the server sends, see AsyncFileResponse
		fs::File file = LittleFS.exists("/app.html") ? LittleFS.open("/app.html") : LittleFS.open("/app.html.gz");
		if (file) {
			uint8_t buffer[512];
			uint32_t crc = 0;
			size_t n;
			while ((n = file.read(buffer, sizeof(buffer))) > 0) {
				crc = esp_rom_crc32_le(crc, buffer, n);
			}
			etag = "\"" + String(crc, HEX) + "-" + String(file.size()) + "\"";
		}
	}

	return etag;
}

// Sending app.html takes several KB for the few seconds it lasts. Several at once left Wi-Fi so little
// memory that it could stop sending for good. A browser only needs one, so more than two wait.
static uint8_t appSends = 0;	// Only used on async_tcp
static const uint8_t maxAppSends = 2;

// Answers the request if the browser's copy is current. Otherwise returns NULL.
static AsyncWebServerResponse *notModified(AsyncWebServerRequest *request, const String &etag) {
	AsyncWebHeader *match = request->getHeader("If-None-Match");
	return etag.length() > 0 && match != NULL && match->value() == etag ? request->beginResponse(304) : NULL;
}

static void addEtag(AsyncWebServerResponse *response, const String &etag) {
	if (etag.length() > 0) {
		response->addHeader("ETag", etag);
		response->addHeader("Cache-Control", "no-cache");	// Keep it, but check before using it
	}
}

void configureWebServer() {
	// Before serveStatic(), which would find the copies in the filesystem
	for (const SettingsPage &page : settingsPages) {
		server->on(page.url, HTTP_GET, [&page](AsyncWebServerRequest *request) {
			AsyncWebServerResponse *response = notModified(request, firmwareEtag());
			if (response == NULL) {
				response = request->beginResponse_P(200, "text/html", page.start, page.end - page.start);
				response->addHeader("Content-Encoding", "gzip");
			}
			addEtag(response, firmwareEtag());
			addWebLoginCookie(request, response);	// So the web socket can reconnect after the login has changed
			request->send(response);
		});
	}

	for (const EmbeddedFile &file : embeddedFiles) {
		server->on(file.url, HTTP_GET, [&file](AsyncWebServerRequest *request) {
			AsyncWebServerResponse *response = notModified(request, firmwareEtag());
			if (response == NULL) {
				response = request->beginResponse_P(200, file.type, file.start, file.end - file.start);
				response->addHeader("Content-Encoding", "gzip");
			}
			addEtag(response, firmwareEtag());
			request->send(response);
		});
	}

	// The page that opens the web socket. It's in the filesystem, but served here to set the login cookie.
	server->on("/app.html", HTTP_GET, [](AsyncWebServerRequest *request) {
		AsyncWebServerResponse *response = notModified(request, appEtag());
		if (response == NULL) {
			if (appSends >= maxAppSends) {
				response = request->beginResponse(503);
				response->addHeader("Retry-After", "1");
				request->send(response);
				return;
			}
			response = request->beginResponse(LittleFS, "/app.html", "text/html");
			if (response == NULL) {
				request->send(404);	// The filesystem image is missing
				return;
			}
			appSends++;
			request->onDisconnect([]() { appSends--; });
		}
		addEtag(response, appEtag());
		addWebLoginCookie(request, response);
		request->send(response);
	});

#ifdef HA_ONLY
	// The file system is shared with the standard build, so the weather page is in it: not here
	server->on("/weather.html", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(404, "text/plain", "this build has no weather, Home Assistant shows it");
	});
#endif
	server->serveStatic("/", LittleFS, "/");
	server->on("/", HTTP_GET, mainHandler).setFilter(ON_STA_FILTER);
	server->on("/custom", HTTP_GET | HTTP_POST, customHandler);
	// /panel/batch first: a handler for /panel would also take /panel/<anything>
	server->on("/panel/batch", HTTP_POST, panelHandler, NULL, panelBatchBodyHandler);
	server->on("/panel/test", HTTP_POST, panelTestHandler);
	server->on("/panel", HTTP_GET | HTTP_POST, panelHandler, NULL, panelBodyHandler);
	// The specific /gif/... routes first: a handler for /gif would also take /gif/<anything>
	server->on("/gif/play", HTTP_POST, gifPlayHandler);
	server->on("/gif/stop", HTTP_POST, gifStopHandler);
	server->on("/gif/delete", HTTP_POST, gifDeleteHandler);
	server->on("/gif", HTTP_GET | HTTP_POST | HTTP_DELETE, gifHandler, NULL, gifBodyHandler);
	server->on("/update", HTTP_POST, updateHandler, updateUploadHandler, updateBodyHandler);
	server->on("/t", HTTP_POST, timeHandler).setFilter(ON_AP_FILTER);
	server->on("/upload_face", HTTP_POST, handleUploadDone, handleUpload);
	// A wildcard rather than a regex because std::regex adds well over 100K to the image
	server->on(DELETE_FACE_URL "*", HTTP_DELETE, handleDelete);
	server->serveStatic("/assets", LittleFS, "/assets");

	// attach AsyncWebSocket
	ws->onEvent(wsHandler);
	server->addHandler(ws);
	server->begin();
	ws->enable(true);
}


void setFace(const char *menuLabel) {
	if (IPSClock::getTimeOrDate() == IPSClock::WEATHER) {
		weather->getIconPack().fromString(menuLabel);
	} else if (IPSClock::getTimeOrDate() == IPSClock::SLIDE_SHOW) {
		slidesSet->fromString(menuLabel);
	} else if (IPSClock::getTimeOrDate() == IPSClock::PANELS) {
		// Panels have no file set, so there is nothing to choose
	} else {
		ipsClock->getClockFace().fromString(menuLabel);
	}
}

/*
Menu title background: 0x0A2D
Hilight background: 0x0926
Item background: 0x09A9
Disabled text: 0x74F7
text: 0xFFFF
*/
#define MENU_TEXT_TITLE 0xDF1C
#define MENU_TEXT_ITEM 0xA5B7
#define MENU_TEXT_HILIGHT 0xDF1C
#define MENU_TEXT_DISABLED 0x3B92
#define MENU_ITEM_BACKGROUND 0x00E5
#define MENU_TITLE_BACKGROUND 0x0a2d
#define MENU_ITEM_HILIGHT_BACKGROUND 0x0062
#define ITEM_BORDER_COLOR 0xFFFF
#define TITLE_BORDER_COLOR 0xFFFF

void initFacesMenu() {
	String postfix(".tar.gz");

	int display = IPSClock::getTimeOrDate();

	uint8_t font = TITLE_FONT;

	menu->reset();
	const char *displayTitle = "Clock Face";
	if (display == IPSClock::WEATHER)
		displayTitle = "Icons";
	else if (display == IPSClock::SLIDE_SHOW)
		displayTitle = "Slide Show";
	else if (display == IPSClock::PANELS)
		displayTitle = "Panels";

	menu->setTitle(displayTitle);
	eSPIMenu::Spec& itemSpec = menu->getItemSpec();
	itemSpec.setFont(TITLE_FONT);
	itemSpec.setItemColors(MENU_ITEM_BACKGROUND, MENU_TEXT_ITEM, MENU_ITEM_HILIGHT_BACKGROUND, MENU_TEXT_HILIGHT, MENU_ITEM_BACKGROUND, MENU_TEXT_DISABLED);
	itemSpec.setMargins(1, 1, 2, 2);
	itemSpec.setBorder(0, 2, 0, 0);
	itemSpec.setBorderColors(MENU_ITEM_BACKGROUND, ITEM_BORDER_COLOR, MENU_ITEM_BACKGROUND);

	eSPIMenu::Spec& titleSpec = menu->getTitleSpec();
	titleSpec.setFont(TITLE_FONT);
	titleSpec.setItemColors(MENU_TITLE_BACKGROUND, MENU_TEXT_TITLE, MENU_ITEM_HILIGHT_BACKGROUND, MENU_TEXT_HILIGHT, MENU_ITEM_BACKGROUND, MENU_TEXT_DISABLED);
	titleSpec.setMargins(2, 2, 2, 2);
	titleSpec.setBorder(0, 0, 1, 0);
	titleSpec.setBorderColors(TITLE_BORDER_COLOR, TITLE_BORDER_COLOR, TITLE_BORDER_COLOR);

	String dirName("/ips/");
	if (display == IPSClock::WEATHER)
		dirName += "weather";
	else if (display == IPSClock::SLIDE_SHOW)
		dirName += "slides";
	else
		dirName += "faces";	// Also for PANELS, which has no file set of its own, so the menu does nothing

	String current = ipsClock->getClockFace();
	if (display == IPSClock::WEATHER)
		current = weather->getIconPack();
	else if (display == IPSClock::SLIDE_SHOW)
		current = slidesSet->value;

	fs::File dir = LittleFS.open(dirName);
    String name = dir.getNextFileName();
	int optIndex = 0;
	bool selected = false;
    while(name.length() > 0 && optIndex < ESPI_MENU_MAX_ITEMS){
		String fileName = name.substring(dirName.length() + 1, name.length());
		String option = fileName.substring(0, fileName.lastIndexOf(postfix));
		eSPIMenu::State state = option == current ? eSPIMenu::selected : eSPIMenu::none;
		menu->addItem(option.c_str(), state);
        name = dir.getNextFileName();
		optIndex++;
	}

	tfts->fillScreen(MENU_ITEM_BACKGROUND);
}

// After TLS has had the display buffer, see updateDisplays()
void redrawMenu() {
	tfts->getSprite().fillSprite(MENU_ITEM_BACKGROUND);
	menu->show();
	tfts->getSprite().pushSprite(0, 0);
}

// The display cycle is shown on the clock page, but stored apart from the other clock settings
String clockCallback() {
	return "\"display_cycle\":" + IPSClock::getDisplayCycle().toJSON();
}

// The MQTT page is sent everything but the password, and only whether there is one
String mqttCallback() {
	return passwordJSON(MQTTBroker::getPassword());
}

// The same for the weather page's API key
String weatherCallback() {
	return passwordJSON(WeatherService::getWeatherToken());
}

String wifiCallback() {
	String wifiStatus = "\"wifi_ap\":";

	if ((WiFi.getMode() & WIFI_MODE_AP) != 0) {
		wifiStatus += "true";
	} else {
		wifiStatus += "false";
	}

	wifiStatus += ",";
	wifiStatus += "\"hostname\":";
	wifiStatus += hostName.toJSON();	// Escaped, as one saved before names were checked could have anything in it
	wifiStatus += ",";
	wifiStatus += webLoginJSON();
	wifiStatus += ",\"ap_password\":";
	wifiStatus += apPassword.toJSON();

	return wifiStatus;
}

String clockFacesCallback() {
	const String postfix(".tar.gz");
	const String quote("\"");
	const String quoteColonQuote("\":\"");
	const String comma_quote(",\"");

	String freeSpace = String(fsFreeBytes());
	String options = "\"fs_free\":" + freeSpace + comma_quote
#ifdef notdef
	 + "fs_size\":" + String(fsTotalBytes()) + comma_quote
#endif
	 ;
	options += "face_files\":{";
	String sep = quote;
	String dirName = "/ips/" + fileSet->value;
	fs::File dir = LittleFS.open(dirName);
    String name = dir.getNextFileName();
    while(name.length() > 0){
		String fileName = name.substring(dirName.length() + 1, name.length());
		String option = fileName.substring(0, fileName.lastIndexOf(postfix));
		options += sep;
		options += option;
		options += quoteColonQuote;
		options += fileName;
		options += quote;
		sep = comma_quote;
        name = dir.getNextFileName();
    }
	dir.close();

	options += "}";

	return options;
}

void ledTaskFn(void *pArg) {
	backlights = new Backlights();
	backlights->begin();

	while (true) {
		if (ipsClock != NULL) {
			if (ipsClock->clockOn()) {
				backlights->setOn(true);
			} else {
				if (IPSClock::getDimming() == IPSClock::DIM) {
					backlights->setOn(true);
				} else {
					backlights->setOn(false);
				}
			}
			backlights->setBrightness(ipsClock->getBrightness());
			backlights->loop();
		}
		delay(16);
	}
}

void setWiFiCredentials(const char *ssid, const char *password)
{
	WiFi.disconnect();
	wifiManager->setRouterCredentials(ssid, password);
	wifiManager->connect();
}

void improvTaskFn(void *pArg) {
	ImprovWiFi improvWiFi(
        manifest[0],
        manifest[1],
        manifest[2],
        manifest[3]
	);

	improvWiFi.setInfoCallback([](const char *msg) {tfts->setStatus(msg);});
	improvWiFi.setWiFiCallback(setWiFiCredentials);

	DEBUG("Running improv");

	while (true) {
		xSemaphoreTake(wsMutex, portMAX_DELAY);
		improvWiFi.loop();
		xSemaphoreGive(wsMutex);
		
		delay(10);
	}
}

void commitEEPROMTaskFn(void *pArg) {
	while(true) {
		delay(60000);
//		DEBUG("Committing config");
		config.commit();
	}
}

// config.init() resets every setting to its default if the set of settings has changed, which it
// recognises by a checksum of their names. Settings added since then are stored after the others,
// so if EEPROM holds an earlier set, keep it and just add the new settings.
void upgradeEEPROM() {
	unsigned int storedChecksum = 0;
	EEPROM.get(0, storedChecksum);
	// The new settings' defaults, nothing has been read yet
	if (storedChecksum == rootConfig202.getChecksum(0)) {
		networkConfig.put();
		cycleConfig.put();
		webLoginConfig.put();
		accessPointConfig.put();
	} else if (storedChecksum == rootConfig205.getChecksum(0)) {
		webLoginConfig.put();
		accessPointConfig.put();
	} else if (storedChecksum == rootConfig207.getChecksum(0)) {
		accessPointConfig.put();
	} else {
		return;
	}
	EEPROM.put(0, rootConfig.getChecksum(0));
	config.commit();
}

// From letters and digits that can't be mistaken for each other, so it's easy to copy off the displays
String randomAPPassword() {
	// None that look alike, and none as wide as m and w, so that 8 always fit on a line of the
	// displays in the bigger text. The fewest WPA2 allows, but it's only on while setting up.
	const char *characters = "abcdefghijknpqrstuvxyz23456789";	// 30
	String password;

	// esp_random() is only really random while Wi-Fi is on, or with the bootloader's source, which
	// mustn't be used while Wi-Fi is on
	bool wifiOff = WiFi.getMode() == WIFI_OFF;
	if (wifiOff) {
		bootloader_random_enable();
	}
	for (int i = 0; i < 8; i++) {
		password += characters[esp_random() % 30];
	}
	if (wifiOff) {
		bootloader_random_disable();
	}

	return password;
}

void initFromEEPROM() {
//	config.setDebugPrint(debugPrint);
	upgradeEEPROM();
	config.init();
//	rootConfig.debug(debugPrint);
	rootConfig.get();	// Read all of the config values from EEPROM

	if (apPassword.value.length() < 8) {	// The first start, or the first since upgrading
		apPassword = randomAPPassword();
		apPassword.put();
		config.commit();
	}

	// The browser's check is for the message. SetupServer() checks again.
	hostnameParam = new AsyncWiFiManagerParameter("Hostname", "Hostname", hostName.value.c_str(), 63,
		"required pattern='[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?' title='Letters, digits and hyphens, without a hyphen at either end'");
}

void connectedHandler() {
	tfts->setStatus(WiFi.localIP().toString());
	DEBUG("connectedHandler");
	MDNS.end();
	MDNS.begin(hostName.value.c_str());
	MDNS.addService("http", "tcp", 80);

	if (!wifiManager->isAP()) {
		uint32_t value = WEATHER_UPDATE;
		xQueueSend(weatherQueue, &value, 0);	// May not work if AP is active
	}
}

void apChange(AsyncWiFiManager *wifiManager) {
	DEBUG("apChange()");
	DEBUG(wifiManager->isAP());
	if (wifiManager->isAP()) {
		tfts->setStatus(ssid);
	} else {
		tfts->setStatus("AP Destroyed...");
		uint32_t value = WEATHER_UPDATE;
		xQueueSend(weatherQueue, &value, 0);	// Not enough memory to make an HTTPS request while AP is active
	}

	// So the Network page's switch doesn't go on saying otherwise when the access point closes by itself
	broadcastUpdate(String("{\"type\":\"sv.update\",\"value\":{\"wifi_ap\":") + (wifiManager->isAP() ? "true" : "false") + "}}");
}

// An access point that's forgotten about keeps the weather from updating and its name and password on
// the displays, so it closes when nobody has been on it for a while. Not if the clock isn't on a
// network, when the access point is the only way to reach it.
void closeIdleAccessPoint() {
	static const unsigned long CHECK_MS = 5000;
	static const unsigned long IDLE_MS = 10 * 60 * 1000;
	static unsigned long lastChecked = 0;
	static unsigned long lastInUse = 0;

	if (!wifiManager->isAP() || !WiFi.isConnected()) {
		lastInUse = millis();
		return;
	}

	if (millis() - lastChecked < CHECK_MS) {
		return;
	}
	lastChecked = millis();

	if (WiFi.softAPgetStationNum() > 0) {
		lastInUse = millis();
	} else if (millis() - lastInUse >= IDLE_MS) {
		lastInUse = millis();
		wifiManager->stopConfigPortal();
	}
}

void setWiFiAP(bool on) {
	if (on) {
		wifiManager->startConfigPortal(ssid.c_str(), apPassword.value.c_str());
	} else {
		wifiManager->stopConfigPortal();
	}
}

// Saves an address from the portal's form in its canonical form, or empty if it isn't one
void setAddress(StringConfigItem &item, const String &value) {
	IPAddress address;
	String newValue = address.fromString(value) && (uint32_t)address != 0 ? address.toString() : "";
	if (newValue != item.value) {
		item = newValue;
		staticAddressChanged = true;
	}
}

// The Wi-Fi portal keeps the static address it is given to itself, and forgets it when the clock
// restarts. This sees the portal's form before the portal does, so SetupServer() can save it with
// the other settings.
void captureStaticAddress(AsyncWebServerRequest *request) {
	// Only the Configure Static WiFi page has the address fields
	if (request->hasArg("ip")) {
		setAddress(staticIP, request->arg("ip"));
		setAddress(staticGateway, request->arg("gw"));
		setAddress(staticSubnet, request->arg("sn"));
		setAddress(staticDNS1, request->arg("dns1"));
		setAddress(staticDNS2, request->arg("dns2"));
	}
}

// Gives the Wi-Fi portal the saved static address, if there is one. The portal uses it to connect
// and shows it on the Configure Static WiFi page.
void setStaticAddress() {
	wifiManager->setSaveFormCallback(captureStaticAddress);

	IPAddress ip, gateway, subnet, dns1, dns2;
	if (ip.fromString(staticIP.value)) {
		gateway.fromString(staticGateway.value);
		if (!subnet.fromString(staticSubnet.value)) {
			subnet = IPAddress(255, 255, 255, 0);
		}
		if (!dns1.fromString(staticDNS1.value)) {
			dns1 = gateway;	// Routers usually answer DNS queries, and the clock needs DNS for NTP and the weather
		}
		dns2.fromString(staticDNS2.value);
		wifiManager->setSTAStaticIPConfig(ip, gateway, subnet, dns1, dns2);
	}
}

void SetupServer() {
	DEBUG("SetupServer()");
	String name(hostnameParam->getValue());
	if (validHostname(name)) {
		hostName = name;
		hostName.put();
	}
	if (staticAddressChanged) {
		// The portal has already reconnected with the old address, and it can't go back to DHCP.
		// Restart so the new address is used.
		networkConfig.put();
		config.commit();
		delay(500);	// Let the reply to the form go
		ESP.restart();
	}
	config.commit();
	createSSID();
	wifiManager->setAPCredentials(ssid.c_str(), apPassword.value.c_str());
	DEBUG(hostName.value.c_str());
	MDNS.begin(hostName.value.c_str());
	MDNS.addService("http", "tcp", 80);
//	getTime();
}

void wifiManagerTaskFn(void *pArg) {
	while(true) {
		xSemaphoreTake(wsMutex, portMAX_DELAY);
		wifiManager->loop();
		xSemaphoreGive(wsMutex);

		closeIdleAccessPoint();
		WiFiWatchdog::loop();

		delay(50);
	}
}

void setup() {
	/*
	* setup() runs on core 1
	*/
	Serial.begin(115200);
	Serial.setDebugOutput(false);
	TLSArena::init();	// Before the weather task can use TLS

	DEBUG("Setup...");

	wsMutex = xSemaphoreCreateMutex();
	memMutex = xSemaphoreCreateMutex();
	fsSpaceMutex = xSemaphoreCreateMutex();
    weatherQueue = xQueueCreate(5, sizeof(uint32_t));
    wsPendingQueue = xQueueCreate(8, sizeof(String*));
	tfts = new TFTs();

	LittleFS.begin();
#ifdef HA_ONLY
	removeSlideFiles();
#endif

	// Setup TFTs
	tfts->begin(LittleFS);
	GifPlayer::begin();
	PanelDraw::begin();
	tfts->fillScreen(TFT_BLACK);
	tfts->setTextColor(TFT_WHITE, TFT_BLACK);
	tfts->setCursor(0, 0, 2);
	tfts->setStatus("setup...");

	menu = new eSPIMenu::Menu(&tfts->getSprite());

	createSSID();

	// MQTT topics and Home Assistant's unique IDs are made from this. It's what upstream used as the
	// access point's name, which was made before the settings were read, so it has the default
	// hostname rather than the clock's own. It stays the same so entities keep their IDs. Upstream
	// made it again with the clock's own hostname when the Wi-Fi portal was saved, until a restart.
	mqttId = (chipId + hostName).substring(0, 31);

	EEPROM.begin(2048);
	initFromEEPROM();

	timeSync = new EspSNTPTimeSync(IPSClock::getTimeZone().value, asyncTimeSetCallback, asyncTimeErrorCallback);
	timeSync->init();
	makeRoomForTimeZone();
	useTimeServer(NULL);	// Before the network is up, like init() itself
	timeServer.setCallback(onTimeServerChanged);

#ifndef DS1302
	rtcTimeSync = new EspRTCTimeSync(RTC_SDA_PIN, RTC_SCL_PIN);
	rtcTimeSync->init();
	rtcTimeSync->enabled(true);
#else
	rtcTimeSync = new EspDS1302TimeSync(DS1302_IO, DS1302_SCLK, DS1302_CE);
	rtcTimeSync->init();
	rtcTimeSync->enabled(true);
#endif

	mqttBroker = new MQTTBroker();
	MQTTBroker::getHost().setCallback(onMqttParamsChanged);
	MQTTBroker::getPort().setCallback(onMqttParamsChanged);
	MQTTBroker::getUser().setCallback(onMqttParamsChanged);
	MQTTBroker::getPassword().setCallback(onMqttParamsChanged);

IPSClock::getTimeZone().setCallback(onTimezoneChanged);

    xTaskCreatePinnedToCore(
          commitEEPROMTaskFn, /* Function to implement the task */
          "Commit EEPROM task", /* Name of the task */
		  3072,  /* Stack size in bytes. Writing the settings to flash used over 1.5K of 2K */
          NULL,  /* Task input parameter */
		  tskIDLE_PRIORITY,  /* More than background tasks */
          &commitEEPROMTask,  /* Task handle. */
		  xPortGetCoreID()
		  );

    xTaskCreatePinnedToCore(
		ledTaskFn, /* Function to implement the task */
		"led task", /* Name of the task */
		1500,  /* Stack size in words */
		NULL,  /* Task input parameter */
		tskIDLE_PRIORITY + 2,  /* Priority of the task (idle) */
		&ledTask,  /* Task handle. */
		1	/*  */
	);

	// Not on core 0 with Wi-Fi and lwIP, which take it from this task whenever they need it. Drawing and
	// unpacking images can keep this task busy for many seconds, and when that also kept core 0's idle
	// task from running for 5 seconds, the task watchdog restarted the clock.
	xTaskCreatePinnedToCore(
		clockTaskFn, /* Function to implement the task */
		"Clock task", /* Name of the task */
		5000,  /* Stack size in words */
		NULL,  /* Task input parameter */
		tskIDLE_PRIORITY + 1,  /* More than background tasks */
		&clockTask,  /* Task handle. */
		1
	);

	xTaskCreatePinnedToCore(
		improvTaskFn, /* Function to implement the task */
		"Improv task", /* Name of the task */
		2048,  /* Stack size in words */
		NULL,  /* Task input parameter */
		tskIDLE_PRIORITY + 1,  /* More than background tasks */
		&improvTask,  /* Task handle. */
		0
	);

	tfts->setStatus("Connecting...");

	wifiManager->setDebugOutput(false);
	wifiManager->setHostname(hostName.value.c_str());	// name router associates DNS entry with
	wifiManager->setCustomOptionsHTML("<br><form action='/t' name='time_form' method='post'><button name='time' onClick=\"{var now=new Date();this.value=now.getFullYear()+','+(now.getMonth()+1)+','+now.getDate()+','+now.getHours()+','+now.getMinutes()+','+now.getSeconds();} return true;\">Set Clock Time</button></form><br><form action=\"/app.html\" method=\"get\"><button>Configure Clock</button></form>");
	wifiManager->addParameter(hostnameParam);
	wifiManager->setSaveConfigCallback(SetupServer);
	wifiManager->setConnectedCallback(connectedHandler);
	wifiManager->setAPCallback(apChange);
	wifiManager->setAPCredentials(ssid.c_str(), apPassword.value.c_str());
	server->addHandler(new WebLoginHandler());	// Before any other handler, so it sees every request first
	setStaticAddress();

	// After a cold boot some clocks transmit far more weakly than the power they report, so the
	// router can barely hear them and connecting fails. Setting the power again fixes it. Seen on an
	// ESP32-PICO-D4, see https://github.com/judge2005/EleksTubeIPS/issues/93. Not WiFi.setTxPower(),
	// which does nothing until Wi-Fi's start event has been handled.
	WiFi.mode(WIFI_STA);
	esp_wifi_set_max_tx_power(WIFI_POWER_19_5dBm);
	// When this runs out, opening the access point cancels the connection and starts it again, so allow
	// for a slow router. A clock with no network saved doesn't wait.
	wifiManager->setConnectTimeout(10000);	// milliseconds
	wifiManager->start();
	wifiManager->setConnectTimeout(5000);	// Later ones hold up the Wi-Fi manager's task, and the web UI with it
	esp_wifi_set_max_tx_power(WIFI_POWER_19_5dBm);

	configureWebServer();

	/*
	 * Very occasionally the wifi stack will close with a beacon timeout error.
	 *
	 * This is apparently related to it not being able to allocate a buffer at
	 * some point because of a transitory memory issue.
	 *
	 * In older versions this issue is not well handled so the wifi stack never
	 * recovers. The work-around is to not allow the wifi radio to sleep.
	 *
	 * https://github.com/espressif/esp-idf/issues/11615
	 */
	esp_wifi_set_ps(WIFI_PS_NONE);

	xTaskCreatePinnedToCore(
		wifiManagerTaskFn,    /* Function to implement the task */
		"WiFi Manager task",  /* Name of the task */
		3000,                 /* Stack size in words */
		NULL,                 /* Task input parameter */
		tskIDLE_PRIORITY + 2, /* Priority of the task (idle) */
		&wifiManagerTask,     /* Task handle. */
		0
	);

#ifndef HA_ONLY	// Home Assistant shows the weather; the task's stack and its TLS memory peaks are saved
    xTaskCreatePinnedToCore(
		weatherTaskFn, /* Function to implement the task */
		"Weather client task", /* Name of the task */
		6144,  /* Stack size in words */
		NULL,  /* Task input parameter */
		tskIDLE_PRIORITY,  /* More than background tasks */
		&weatherTask,  /* Task handle. */
		xPortGetCoreID()
	);
#endif

    Serial.print("setup() running on core ");
    Serial.println(xPortGetCoreID());

	Serial.println(getCpuFrequencyMhz());

    vTaskDelete(NULL);	// Delete this task (so loop() won't be called)
}

void loop() {
	
}
