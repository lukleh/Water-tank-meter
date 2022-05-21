#define DYNAMIC_JSON_DOCUMENT_SIZE 8192
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <StreamUtils.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>

//#define ULTRASOUND_ECHOPIN D8// Pin to receive echo pulse TX
//#define ULTRASOUND_TRIGPIN D7// Pin to send trigger pulse RX
#define ULTRASOUND_ECHOPIN 15 // Pin to receive echo pulse TX
#define ULTRASOUND_TRIGPIN 13 // Pin to send trigger pulse RX
#define MEASUREMENT_COUNT 168
#define CONFIG_SIZE 256

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

char wifi_ssid[32];
char wifi_password[32];
unsigned long current_distance_cm = 0;
float average_distance_cm = 0;
byte measurements[MEASUREMENT_COUNT] = {};
int loop_count = 0;
int lastTimeStep = 0;
const char *myname = "watertankmeter";

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
  if (sensor_height_cm == 0) {
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
  return rawDistance();
  //  int raw_sum = 0;
  //  int raw_best = 0;
  //  int raw[5] = {};
  //  for (int i = 0; i < 5; i++) {
  //    raw[i] = rawDistance();
  //    raw_sum += raw[i];
  //    delay(100);
  //  }
  //  raw_best = raw[0];
  //  for (int i = 0; i < 5; i++) {
  //    if (abs(raw_best - (raw_sum / 6.0)) > abs(raw[i] - (raw_sum / 6.0))) {
  //      raw_best = raw[i];
  //    }
  //  }
  //  return raw_best;
}


void handleData() {
  Serial.println("Handle data");
  DynamicJsonDocument jDoc(4096);
  JsonObject info = jDoc.createNestedObject("info");
  info["volume"] = waterVolumeM3(average_distance_cm);
  info["percent full"] = calcPercentFull(average_distance_cm);
  info["average distance"] = int(average_distance_cm);
  info["current distance"] = current_distance_cm;
  info["sensor height"] = sensor_height_cm;
  info["tank diameter"] = tank_diameter_cm;
  info["WIFI"] = (wifi_get_opmode() == 2) ? "AP" : "Station";
  info["your IP"] = server.client().remoteIP().toString();
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
  String response;
  serializeJson(jDoc, response);
  server.send(200, "application/json", response);
}


void handleSave() {
  Serial.println("Handle save");
  String response;
  bool wifi_unchanged = true;
  DynamicJsonDocument jDoc(128);
  for (uint8_t i = 0; i < server.args(); i++) {
    Serial.printf("Save param:%s - %s\n", server.argName(i).c_str(), server.arg(i).c_str());
    jDoc[server.argName(i)] = server.arg(i);
    if (server.argName(i) == "wifi name") {
      wifi_unchanged = wifi_unchanged && strcmp(wifi_ssid, server.arg(i).c_str()) == 0;
      strcpy(wifi_ssid, server.arg(i).c_str());
    } else if (server.argName(i) == "wifi password") {
      wifi_unchanged = wifi_unchanged && strcmp(wifi_password, server.arg(i).c_str()) == 0;
      strcpy(wifi_password, server.arg(i).c_str());
    } else if (server.argName(i) == "sensor distance") {
      sensor_height_cm = server.arg(i).toInt();
    } else if (server.argName(i) == "tank diameter") {
      tank_diameter_cm = server.arg(i).toInt();
    }
  }
  saveConfig();
  serializeJson(jDoc, response);
  Serial.println(response);
  server.send(200, "application/json", response);
  if (!wifi_unchanged) {
    Serial.printf("WIFI credentials changed, restarting");
    ESP.restart();
  }
}

void handleRoot() {
  Serial.println("Handle root");
  File file = LittleFS.open("index.html", "r");
  if (!file) {
    server.send(200, "text/plain", "no index.html file found");
  } else {
    server.streamFile(file, "text/html");
    file.close();
  }
}

void handleRestart() {
  Serial.println("Handle restart");
  server.send(200, "text/plain", "restarting");
  ESP.restart();
}


void loadConfig() {
  Serial.println("load config:");
  StaticJsonDocument<CONFIG_SIZE> doc;
  EepromStream eepromStream(0, CONFIG_SIZE);
  deserializeJson(doc, eepromStream);
  if (!doc.isNull()) {
    Serial.println("found config file");
    strcpy(wifi_ssid, doc["wifi ssid"]);
    strcpy(wifi_password, doc["wifi password"]);
    sensor_height_cm = doc["sensor height cm"];
    tank_diameter_cm = doc["tank diameter cm"];
    connection_success = doc["connection success"];
    Serial.println("loaded config");
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
  doc["sensor height cm"] = sensor_height_cm;
  doc["tank diameter cm"] = tank_diameter_cm;
  doc["connection success"] = connection_success;
  Serial.println("save config:");
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
  WiFi.softAP(myname);
}

void connectWifi() {
  if (strcmp(wifi_ssid, "") == 0 || !wifi_ssid || strcmp(wifi_ssid, "null") == 0 || strcmp(wifi_password, "") == 0 || !wifi_password || strcmp(wifi_password, "null") == 0) {
    Serial.println("empty wifi credentials, turning to softAP");
    makeSoftAP();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
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

  MDNS.begin(myname);
  if (LittleFS.begin()) {
    Serial.println("Filesystem mounted");
  } else {
    Serial.println("Filesystem NOT mounted");
  }

  server.on("/", handleRoot);
  server.serveStatic("/static", LittleFS, "/");
  server.on("/data", HTTP_GET, handleData);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/restart", HTTP_GET, handleRestart);
  httpUpdater.setup(&server);
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
  server.handleClient();
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
