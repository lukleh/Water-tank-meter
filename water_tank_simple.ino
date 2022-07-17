#define DYNAMIC_JSON_DOCUMENT_SIZE 16384

#include <AsyncJson.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <StreamUtils.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>

#define FIRMWARE_VERSION 1.8 // firmware version
#define ULTRASOUND_ECHOPIN 13 // D7 blue  Pin to receive echo pulse TX
#define ULTRASOUND_TRIGPIN 12 // D6 green Pin to send trigger pulse RX
#define MEASUREMENT_COUNT 168
#define CONFIG_SIZE 512
#define SENSOR_MEASUREMENT_TRESHOLD 20


AsyncWebServer server(80);

char wifi_ssid[32];
char wifi_password[32];
double current_distance_cm = 0;
double average_distance_cm = 0;
float sensor_measurement_threshold = SENSOR_MEASUREMENT_TRESHOLD;
double measurements[MEASUREMENT_COUNT] = {};
int loop_count = 0;
int lastTimeStep = 0;
char sensor_name[32] = {"watertankmeter"};

int sensor_distance_empty_cm = 0;
int sensor_distance_full_cm = sensor_measurement_threshold;
int tank_diameter_cm = 0;
int current_hour = 0;
int lastWificheck = 0;

double last_distance_cm = 0;
uint32_t last_freeheap = 0;


time_t getNow() {
  return millis();
}

void calc_average_distance() {
  if (average_distance_cm == 0) {
    average_distance_cm = current_distance_cm;
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
  measurements[0] = average_distance_cm;
}

double remainingDepth(double distance_cm) {
  if (distance_cm == 0) {
    return 0;
  }
  return sensor_distance_empty_cm - distance_cm;
}

double calcPercentFull(double distance_cm) {
  if (distance_cm == 0 || (sensor_distance_empty_cm == 0 && sensor_distance_full_cm == 0)) {
    return 0;
  }
  return (remainingDepth(distance_cm) / (double)(sensor_distance_empty_cm - sensor_distance_full_cm)) * 100;
}

int calcPercentFullCapped(double percent) {
  return max(0, min(100, (int)round(percent)));
}

double waterVolumeM3(double distance_cm) {
  if (distance_cm == 0 || sensor_distance_empty_cm == 0 || tank_diameter_cm == 0) {
    return 0;
  }
  return 3.14 * (tank_diameter_cm / 2.0) * (tank_diameter_cm / 2.0) * remainingDepth(distance_cm) / (100 * 100 * 100);
}

double rawDistance() {
  delay(100);
  double distance_cm;
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Set the trigger pin to low for 2uS
  delayMicroseconds(2);
  digitalWrite(ULTRASOUND_TRIGPIN, HIGH); // Send a 10uS high to trigger ranging
  delayMicroseconds(20);
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Send pin low again
  distance_cm = pulseIn(ULTRASOUND_ECHOPIN, HIGH, 26000) / 57.5; // Read in times pulse
  return distance_cm;
}


double measureDistance() {
  double raw_sum = 0;
  double raw_best = 0;
  double raw[5] = {};
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
  Serial.println("HTTP GET /data");
  AsyncJsonResponse * response = new AsyncJsonResponse();
  response->addHeader("Server", "ESP Async Web Server");
  JsonObject jDoc = response->getRoot();
  JsonObject info = jDoc.createNestedObject("info");
  info["volume"] = waterVolumeM3(average_distance_cm);
  info["percent full"] = calcPercentFullCapped(calcPercentFull(average_distance_cm));
  info["percent full unfiltered"] = calcPercentFull(average_distance_cm);
  info["average distance"] = average_distance_cm;
  info["current distance"] = current_distance_cm;
  info["sensor distance empty cm"] = sensor_distance_empty_cm;
  info["sensor distance full cm"] = sensor_distance_full_cm;
  info["tank diameter"] = tank_diameter_cm;
  info["firmware version"] = FIRMWARE_VERSION;
  info["board name"] = ARDUINO_BOARD;
  info["sensor name"] = sensor_name;
  info["WIFI"] = (wifi_get_opmode() == 2) ? "AP" : "Station";
  info["WIFI mode"] = wifi_get_opmode();
  info["your IP"] = request->client()->remoteIP().toString();
  info["my IP"] =  WiFi.localIP().toString();
  info["MAC address"] =  WiFi.macAddress();
  info["uptime"] = getNow();
  info["loop number"] = loop_count;
  info["wifi name"] = wifi_ssid;
  info["wifi password"] = wifi_password;
  if (wifi_get_opmode() == 1) {
    info["signal strength dBi"] = WiFi.RSSI();
  }
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
    measurement["p"] = calcPercentFullCapped(calcPercentFull(measurements[i]));
    measurement["v"] = round(waterVolumeM3(measurements[i]) * 1000) / 1000.0;
    measurement["d"] = round(remainingDepth(measurements[i]) * 10) / 10.0;
  }
  response->setLength();
  request->send(response);
}


void handleSave(AsyncWebServerRequest *request) {
  Serial.println("HTTP POST /save");
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
    } else if (request->argName(i) == "sensor distance full cm") {
      sensor_distance_full_cm = request->arg(i).toInt();
    } else if (request->argName(i) == "sensor distance empty cm") {
      sensor_distance_empty_cm = request->arg(i).toInt();
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
  Serial.println("HTTP GET /restart");
  request->send(200, "application/json", "restarting");
  ESP.restart();
}

void handleRoot(AsyncWebServerRequest *request) {
  /*
     need to an ugly hack to select index.html
     templating breaks the file (maybe a network issue)
  */
  Serial.println("HTTP GET /");
  if (wifi_get_opmode() == WIFI_STA) {
    request->send(LittleFS, "/index_internet.html", "text/html");
  } else {
    request->send(LittleFS, "/index_local.html", "text/html");
  }
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
    if (!doc["sensor distance empty cm"].isNull()) {
      sensor_distance_empty_cm = doc["sensor distance empty cm"];
    }
    if (!doc["sensor distance full cm"].isNull()) {
      sensor_distance_full_cm = doc["sensor distance full cm"];
    }
    tank_diameter_cm = doc["tank diameter cm"];
    Serial.print("loaded config: ");
    serializeJson(doc, Serial);
    Serial.println();
  } else {
    Serial.println("config file not found");
  }
}

void saveConfig() {
  StaticJsonDocument<CONFIG_SIZE> doc;
  doc["wifi ssid"] = wifi_ssid;
  doc["wifi password"] = wifi_password;
  doc["sensor name"] = sensor_name;
  doc["sensor distance empty cm"] = sensor_distance_empty_cm;
  doc["sensor distance full cm"] = sensor_distance_full_cm;
  doc["tank diameter cm"] = tank_diameter_cm;
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

boolean haveWifiCredentials() {
  return !(strcmp(wifi_ssid, "") == 0 || !wifi_ssid || strcmp(wifi_ssid, "null") == 0 || strcmp(wifi_password, "") == 0 || !wifi_password || strcmp(wifi_password, "null") == 0);
}

void connectWifi() {
  if (!haveWifiCredentials()) {
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
    makeSoftAP();
    return;
  }
}

void checkWifi() {
  /*
     if in AP mode, and last WIFI connection was successful, try to reconnect
  */
  if (getNow() - lastWificheck > 300000) {
    lastWificheck = getNow();
    if (wifi_get_opmode() == 2 && haveWifiCredentials()) {
      Serial.println("Trying to reconnect to the last known WIFI network");
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
  Serial.print("ESP Board MAC Address:  ");
  Serial.println(WiFi.macAddress());
  MDNS.begin(sensor_name);
  Serial.println("mDNS responder started");
  if (LittleFS.begin()) {
    Serial.println("Filesystem mounted");
  } else {
    Serial.println("Filesystem NOT mounted");
  }

  server.on("/data", HTTP_GET, handleData);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/restart", HTTP_GET, handleRestart);
  server.serveStatic("/static", LittleFS, "/");
  server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");
  server.on("/", HTTP_GET, handleRoot);

  AsyncElegantOTA.begin(&server);
  server.begin();
  Serial.println("Web server started");

  Serial.println("List filesystem:");
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    Serial.printf("%s - %d\n", dir.fileName().c_str(), dir.fileSize());
  }
  MDNS.addService("http", "tcp", 80);
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
    Serial.printf("ts: %lld distance: %fcm avg distance: %.2fcm free heap: %d\n", getNow(), current_distance_cm, average_distance_cm, ESP.getFreeHeap());
  }
  storeMeasurement();
  loop_count++;
}
