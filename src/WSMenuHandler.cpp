#include <WSMenuHandler.h>

const char* WSMenuHandler::clockMenu = "{\"1\": { \"url\" : \"clock.html\", \"title\" : \"Clock\", \"group\" : \"Display\" }}";
const char* WSMenuHandler::ledsMenu = "{\"2\": { \"url\" : \"leds.html\", \"title\" : \"LEDs\", \"group\" : \"Display\" }}";
const char* WSMenuHandler::facesMenu = "{\"3\": { \"url\" : \"faces.html\", \"title\" : \"Files\", \"group\" : \"Content\" }}";
const char* WSMenuHandler::mqttMenu = "{\"4\": { \"url\" : \"mqtt.html\", \"title\" : \"MQTT\", \"group\" : \"Connections\" }}";
const char* WSMenuHandler::infoMenu = "{\"5\": { \"url\" : \"info.html\", \"title\" : \"Info\", \"group\" : \"System\" }}";
const char* WSMenuHandler::networkMenu = "{\"6\": { \"url\" : \"network.html\", \"title\" : \"Network\", \"group\" : \"Connections\" }}";
const char* WSMenuHandler::weatherMenu = "{\"7\": { \"url\" : \"weather.html\", \"title\" : \"Weather\", \"group\" : \"Content\" }}";
const char* WSMenuHandler::matrixMenu = "{\"8\": { \"url\" : \"matrix.html\", \"title\" : \"Screen Saver\", \"group\" : \"Display\" }}";
const char* WSMenuHandler::panelsMenu = "{\"9\": { \"url\" : \"panels.html\", \"title\" : \"Panels\", \"group\" : \"Display\" }}";
const char* WSMenuHandler::aboutMenu = "{\"10\": { \"url\" : \"about.html\", \"title\" : \"About\", \"group\" : \"System\" }}";

void WSMenuHandler::handle(AsyncWebSocketClient *client, char *data) {
	String json("{\"type\":\"sv.init.menu\", \"value\":[");
	const char *sep = "";
	for (int i=0; items[i] != 0; i++) {
		json.concat(sep);json.concat(items[i]);sep=",";
	}
	json.concat("]}");
	client->text(json);
}

void WSMenuHandler::setItems(const char **items) {
	this->items = items;
}

