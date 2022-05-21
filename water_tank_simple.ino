#define DYNAMIC_JSON_DOCUMENT_SIZE 16384
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include "AsyncJson.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <StreamUtils.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>

#define FIRMWARE_VERSION 1 // firmware version
#define ULTRASOUND_ECHOPIN 13 // D7 blue  Pin to receive echo pulse TX
#define ULTRASOUND_TRIGPIN 12 // D6 green Pin to send trigger pulse RX
#define MEASUREMENT_COUNT 168
#define CONFIG_SIZE 256

AsyncWebServer server(80);

char wifi_ssid[32];
char wifi_password[32];
unsigned long current_distance_cm = 0;
float average_distance_cm = 0;
byte measurements[MEASUREMENT_COUNT] = {};
int loop_count = 0;
int lastTimeStep = 0;
char sensor_name[32] = {"watertankmeter"};

int sensor_height_cm = 0;
int tank_diameter_cm = 0;
bool connection_success = false;
int current_hour = 0;
int lastWificheck = 0;

unsigned long last_distance_cm = 0;
uint32_t last_freeheap = 0;


time_t getNow() {
  return millis();
}

void calc_average_distance() {
  if (average_distance_cm == 0) {
    average_distance_cm = float(current_distance_cm);
  } else {
    average_distance_cm = (average_distance_cm * 59 + current_distance_cm) / 60.0;
  }
}

int timeStepHour() {
  return getNow() / 3600000;
}

void storeMeasurement() {
  int ts = timeStepHour();
  if (ts > lastTimeStep) {
    lastTimeStep = ts;
    for (int i = MEASUREMENT_COUNT - 1; i > 0; i--) {
      measurements[i] = measurements[i - 1];
    }
  }
  measurements[0] = round(average_distance_cm);
}

int remainingDepth(int distance_cm) {
  if (sensor_height_cm == 0) {
    return 0;
  }
  return sensor_height_cm - distance_cm;
}

int calcPercentFull(int distance_cm) {
  if (distance_cm == 0) {
    return 0;
  }
  return (remainingDepth(distance_cm) / (sensor_height_cm - 20.0)) * 100;
}

float waterVolumeM3(int distance_cm) {
  if (distance_cm == 0 || sensor_height_cm == 0 || tank_diameter_cm == 0) {
    return 0;
  }
  return 3.14 * (tank_diameter_cm / 2.0) * (tank_diameter_cm / 2.0) * remainingDepth(distance_cm) / (100 * 100 * 100);
}

unsigned long rawDistance() {
  delay(100);
  unsigned long distance_cm;
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Set the trigger pin to low for 2uS
  delayMicroseconds(2);
  digitalWrite(ULTRASOUND_TRIGPIN, HIGH); // Send a 10uS high to trigger ranging
  delayMicroseconds(20);
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Send pin low again
  distance_cm = pulseIn(ULTRASOUND_ECHOPIN, HIGH, 26000) / 58; // Read in times pulse
  return distance_cm;
}


int measureDistance() {
  int raw_sum = 0;
  int raw_best = 0;
  int raw[5] = {};
  for (int i = 0; i < 5; i++) {
    raw[i] = rawDistance();
    raw_sum += raw[i];
    delay(50);
  }
  raw_best = raw[0];
  for (int i = 0; i < 5; i++) {
    if (abs(raw_best - (raw_sum / 6.0)) > abs(raw[i] - (raw_sum / 6.0))) {
      raw_best = raw[i];
    }
  }
  return raw_best;
}


void handleData(AsyncWebServerRequest *request) {
  Serial.println("Handle data");
  AsyncJsonResponse * response = new AsyncJsonResponse();
  response->addHeader("Server", "ESP Async Web Server");
  JsonObject jDoc = response->getRoot();
  JsonObject info = jDoc.createNestedObject("info");
  info["volume"] = waterVolumeM3(average_distance_cm);
  info["percent full"] = calcPercentFull(average_distance_cm);
  info["average distance"] = int(average_distance_cm);
  info["current distance"] = current_distance_cm;
  info["sensor height"] = sensor_height_cm;
  info["tank diameter"] = tank_diameter_cm;
  info["firmware version"] = FIRMWARE_VERSION;
  info["sensor name"] = sensor_name;
  info["WIFI"] = (wifi_get_opmode() == 2) ? "AP" : "Station";
  info["your IP"] = request->client()->remoteIP().toString();
  info["my IP"] =  WiFi.localIP().toString();
  info["uptime"] = getNow();
  info["loop number"] = loop_count;
  info["wifi name"] = wifi_ssid;
  info["wifi password"] = wifi_password;
  if (wifi_get_opmode() == 1) {
    info["signal strength dBi"] = WiFi.RSSI();
  }
  info["connection success"] = connection_success;
  info["wifi opmode"] = wifi_get_opmode();
  info["free heap"] = ESP.getFreeHeap();
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    info["FILE " + dir.fileName()] = dir.fileSize();
  }

  JsonArray data = jDoc.createNestedArray("data");
  for (int i = 0; i < MEASUREMENT_COUNT; i++) {
    if (measurements[i] == 0) {
      continue;
    }
    JsonObject measurement = data.createNestedObject();
    measurement["i"] = i;
    measurement["v"] = round(waterVolumeM3(measurements[i]) * 100) / 100.0;
    measurement["d"] = round(measurements[i] * 100) / 100.0;
  }
  response->setLength();
  request->send(response);
}


void handleSave(AsyncWebServerRequest *request) {
  Serial.println("Handle save");
  String response;
  bool wifi_unchanged = true;
  DynamicJsonDocument jDoc(256);
  for (uint8_t i = 0; i < request->args(); i++) {
    Serial.printf("Save param:%s - %s\n", request->argName(i).c_str(), request->arg(i).c_str());
    jDoc[request->argName(i)] = request->arg(i);
    if (request->argName(i) == "wifi name") {
      wifi_unchanged = wifi_unchanged && strcmp(wifi_ssid, request->arg(i).c_str()) == 0;
      strcpy(wifi_ssid, request->arg(i).c_str());
    } else if (request->argName(i) == "wifi password") {
      wifi_unchanged = wifi_unchanged && strcmp(wifi_password, request->arg(i).c_str()) == 0;
      strcpy(wifi_password, request->arg(i).c_str());
    } else if (request->argName(i) == "sensor name") {
      wifi_unchanged = wifi_unchanged && strcmp(sensor_name, request->arg(i).c_str()) == 0;
      strcpy(sensor_name, request->arg(i).c_str());
    } else if (request->argName(i) == "sensor distance") {
      sensor_height_cm = request->arg(i).toInt();
    } else if (request->argName(i) == "tank diameter") {
      tank_diameter_cm = request->arg(i).toInt();
    }
  }

  saveConfig();
  serializeJson(jDoc, response);
  Serial.print("response ");
  Serial.println(response);
  request->send(200, "application/json", response);
  if (!wifi_unchanged) {
    Serial.printf("Network settings changed, restarting");
    ESP.restart();
  }
}

void handleRestart(AsyncWebServerRequest *request) {
  Serial.println("Handle restart");
  request->send(200, "application/json", "restarting");
  ESP.restart();
}


void loadConfig() {
  Serial.println("loading config");
  StaticJsonDocument<CONFIG_SIZE> doc;
  EepromStream eepromStream(0, CONFIG_SIZE);
  deserializeJson(doc, eepromStream);
  if (!doc.isNull()) {
    Serial.println("found config file");
    strcpy(wifi_ssid, doc["wifi ssid"]);
    strcpy(wifi_password, doc["wifi password"]);
    if (!doc["sensor name"].isNull()) {
      strcpy(sensor_name, doc["sensor name"]);
    }
    sensor_height_cm = doc["sensor height cm"];
    tank_diameter_cm = doc["tank diameter cm"];
    connection_success = doc["connection success"];
    Serial.print("loaded config: ");
    serializeJson(doc, Serial);
    Serial.println();
  } else {
    Serial.println("not found config file, using dummy defaults");
    sensor_height_cm = 0;
    tank_diameter_cm = 0;
    strcpy(wifi_ssid, "");
    strcpy(wifi_password, "");
  }
}

void saveConfig() {
  StaticJsonDocument<CONFIG_SIZE> doc;
  doc["wifi ssid"] = wifi_ssid;
  doc["wifi password"] = wifi_password;
  doc["sensor name"] = sensor_name;
  doc["sensor height cm"] = sensor_height_cm;
  doc["tank diameter cm"] = tank_diameter_cm;
  doc["connection success"] = connection_success;
  Serial.print("save config:");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.println("saving to EEPROM");
  EepromStream eepromStream(0, CONFIG_SIZE);
  serializeJson(doc, eepromStream);
  eepromStream.flush();
  Serial.println("config saved");
}

void makeSoftAP() {
  Serial.print("Wi-Fi mode set to WIFI_AP ... ");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(sensor_name);
}

void connectWifi() {
  if (strcmp(wifi_ssid, "") == 0 || !wifi_ssid || strcmp(wifi_ssid, "null") == 0 || strcmp(wifi_password, "") == 0 || !wifi_password || strcmp(wifi_password, "null") == 0) {
    Serial.println("empty wifi credentials, turning to softAP");
    makeSoftAP();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.waitForConnectResult(10000) != WL_CONNECTED) {
    Serial.println("WiFi failed, switching to AP");
    break;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connection_success = false;
    makeSoftAP();
    return;
  }
  if (!connection_success) {
    connection_success = true;
    saveConfig();
  }
}

void checkWifi() {
  /*
     if in AP mode, and last WIFI connection was successful, try to reconnect
  */
  if ((wifi_get_opmode() == 2) && connection_success) {
    if (getNow() - lastWificheck > 60000) {
      Serial.println("Trying to reconnect to the last known WIFI network");
      lastWificheck = getNow();
      connectWifi();
    }
  }
}

void flashInfo() {
  uint32_t realSize = ESP.getFlashChipRealSize();
  uint32_t ideSize = ESP.getFlashChipSize();
  FlashMode_t ideMode = ESP.getFlashChipMode();
  Serial.printf("Flash real id:   %08X\n", ESP.getFlashChipId());
  Serial.printf("Flash real size: %u bytes\n\n", realSize);
  Serial.printf("Flash ide  size: %u bytes\n", ideSize);
  Serial.printf("Flash ide speed: %u Hz\n", ESP.getFlashChipSpeed());
  Serial.printf("Flash ide mode:  %s\n", (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT"
                                          : ideMode == FM_DIO  ? "DIO"
                                          : ideMode == FM_DOUT ? "DOUT"
                                          : "UNKNOWN"));
  if (ideSize != realSize) {
    Serial.println("Flash Chip configuration wrong!\n");
  } else {
    Serial.println("Flash Chip configuration ok.\n");
  }
}

void setup()
{
  Serial.begin(9600);
  Serial.setDebugOutput(true);
  for (int i = 0; i < 10; i++) {
    Serial.printf("Forcing serial to get through %d\n", i);
  }
  Serial.println("Starting setup");
  Serial.println("Initializing EEPROM...");
  EEPROM.begin(256);

  pinMode(LED_BUILTIN, OUTPUT);  // Initialize the LED_BUILTIN pin as an output
  pinMode(ULTRASOUND_ECHOPIN, INPUT);
  pinMode(ULTRASOUND_TRIGPIN, OUTPUT);

  loadConfig();
  connectWifi();

  MDNS.begin(sensor_name);
  if (LittleFS.begin()) {
    Serial.println("Filesystem mounted");
  } else {
    Serial.println("Filesystem NOT mounted");
  }

  server.serveStatic("/", LittleFS, "/").setDefaultFile("/index.html");
  server.serveStatic("/static", LittleFS, "/");
  server.on("/data", HTTP_GET, handleData);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/restart", HTTP_GET, handleRestart);
  AsyncElegantOTA.begin(&server);
  server.begin();
  Serial.println("Web server started");

  Serial.println("List filesystem:");
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    Serial.printf("%s - %d\n", dir.fileName().c_str(), dir.fileSize());
  }

  Serial.println("Setup finished");

  flashInfo();
}

void loop()
{
  MDNS.update();
  checkWifi();
  digitalWrite(LED_BUILTIN, HIGH);
  current_distance_cm = measureDistance();
  digitalWrite(LED_BUILTIN, LOW);
  calc_average_distance();
  if (loop_count % 100 == 0 || abs((int)current_distance_cm - (int)last_distance_cm) > 10 || ( abs((int)last_freeheap - (int)ESP.getFreeHeap()) > 1000)) {
    last_distance_cm = current_distance_cm;
    last_freeheap = ESP.getFreeHeap();
    Serial.printf("ts: %lld distance: %lucm avg distance: %.2fcm free heap: %d\n", getNow(), current_distance_cm, average_distance_cm, ESP.getFreeHeap());
  }
  storeMeasurement();
  loop_count++;
}
