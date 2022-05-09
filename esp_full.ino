#define DYNAMIC_JSON_DOCUMENT_SIZE 16384
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include "AsyncJson.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <StreamUtils.h>
#include <AsyncElegantOTA.h>
#include <EEPROM.h>
#include <StreamUtils.h>
#include <Timezone.h>

#define ULTRASOUND_ECHOPIN D6// Pin to receive echo pulse
#define ULTRASOUND_TRIGPIN D7// Pin to send trigger pulse
#define MEASUREMENT_COUNT 168

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
AsyncWebServer server(80);

unsigned long current_distance_cm = 0;
float last_average_distance_cm = 0;
float average_distance_cm = 0;
byte measurements[MEASUREMENT_COUNT] = {}; 
unsigned long loop_count = 0;
const char *myname   = "watertankmeter";

String wifi_ssid = "";
String wifi_password = "";
int sensor_height_cm = 0;
unsigned long tank_diameter_cm = 0;
bool connection_success = false;
bool should_reconnect = false;

int current_hour = 0;
int lastWificheck = 0;
int nowtime = 0;
long rssi = 0;

// Central European Time (Frankfurt, Prague)
TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 27, 120};     // Central European Summer Time
TimeChangeRule CET = {"CET ", Last, Sun, Oct, 30, 60};       // Central European Standard Time
Timezone CE(CEST, CET);

int getNow() {
  return CE.toLocal(timeClient.getEpochTime());
}

void calc_average_distance() {
  if (average_distance_cm == 0) {
    average_distance_cm = float(current_distance_cm);
  } else {
    average_distance_cm = (average_distance_cm * 59 + current_distance_cm) / 60.0;
  }
}

int timeStep() {
  return getNow() % 604800L / 3600;
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
  return 3.14 * (tank_diameter_cm/2.0) * (tank_diameter_cm/2.0) * remainingDepth(distance_cm) / (100 * 100 * 100);
}

String prependDigits(int digits) {
  if(digits < 10)
    return "0" + String(digits);
  return String(digits);
}

String formatTime(int time_stamp) {
  return String(year(time_stamp)) + "-" + prependDigits(month(time_stamp)) + "-" + prependDigits(day(time_stamp)) + " " + prependDigits(hour(time_stamp)) + ":" + prependDigits(minute(time_stamp)) + ":" + prependDigits(second(time_stamp));
}

int subtractTime(int ts, int diff_hours) {
  return ts + (3600 * diff_hours);
}

int calculateTimeDiff(int current, int tstep) {
  if (tstep <= current) {
    return -(current - tstep);
  } else {
    return -(current + MEASUREMENT_COUNT - tstep);
  }
}

unsigned long rawDistance() {
  int distance_cm;
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Set the trigger pin to low for 2uS
  delayMicroseconds(2);
  digitalWrite(ULTRASOUND_TRIGPIN, HIGH); // Send a 10uS high to trigger ranging
  delayMicroseconds(20);
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Send pin low again
  distance_cm = pulseIn(ULTRASOUND_ECHOPIN, HIGH, 26000)/58; // Read in times pulse
  return distance_cm;
}


int measureDistance() {
  int raw_sum = 0;
  int raw_best = 0;
  int raw[5] = {}; 
  for (int i = 0; i < 5; i++) {
    raw[i] = rawDistance();
    raw_sum += raw[i];
//    Serial.print(String(raw[i]) + ",");
    delay(100);
  }
  raw_best = raw[0];
  for (int i = 0; i < 5; i++) {
    if (abs(raw_best - (raw_sum / 6.0)) > abs(raw[i] - (raw_sum / 6.0))) {
      raw_best = raw[i];
    }
  }
//  Serial.println();
//  Serial.println(raw_best);
  return raw_best;
}


void handleData(AsyncWebServerRequest *request){
  AsyncJsonResponse * response = new AsyncJsonResponse();
  response->addHeader("Server","ESP Async Web Server");
  JsonObject jDoc = response->getRoot();
  JsonObject info = jDoc.createNestedObject("info");
  info["volume"] = waterVolumeM3(average_distance_cm);
  info["percent full"] = calcPercentFull(average_distance_cm);
  info["average distance"] = int(average_distance_cm);
  info["current distance"] = current_distance_cm;
  info["time"] = formatTime(getNow());
  info["sensor height"] = sensor_height_cm;
  info["tank diameter"] = tank_diameter_cm;
  info["WIFI"] = ((wifi_get_opmode() == 2) ? ("AP: " + String(myname)) : ("Station: " + wifi_ssid));
  info["your IP"] = request->client()->remoteIP().toString();
  info["my IP"] =  WiFi.localIP().toString();
  info["timestamp"] = getNow();
  info["loop number"] = loop_count;
  info["wifi name"] = wifi_ssid;
  info["wifi password"] = wifi_password;
  info["signal strength dBi"] = rssi;
 
  JsonArray data = jDoc.createNestedArray("data");
  for(int i = current_hour; i > -1; i--) {
    JsonObject measurement = data.createNestedObject();
    measurement["time"] = formatTime(subtractTime(nowtime, calculateTimeDiff(current_hour, i)) / 3600 * 3600);
    measurement["volume"] = waterVolumeM3(measurements[i]);
    measurement["distance"] = measurements[i];
  }

  for(int i = MEASUREMENT_COUNT-1; i > current_hour; i--) {
    JsonObject measurement = data.createNestedObject();
    measurement["time"] = formatTime(subtractTime(nowtime, calculateTimeDiff(current_hour, i)) / 3600 * 3600);
    measurement["volume"] = waterVolumeM3(measurements[i]);
    measurement["distance"] = measurements[i];
  }
  response->setLength();
  request->send(response);
}


void handleSave(AsyncWebServerRequest *request) {
  String response;
  DynamicJsonDocument jDoc(256);
  bool wifi_unchanged = true;
  for (uint8_t i = 0; i < request->args(); i++) {
    Serial.println("Save param:" + request->argName(i) + " - " + request->arg(i));
    jDoc[request->argName(i)] = request->arg(i);
    if (request->argName(i) == "wifi name") {
      wifi_unchanged = wifi_unchanged && wifi_ssid == request->arg(i);
      wifi_ssid = request->arg(i); 
    } else if (request->argName(i) == "wifi password") {
      wifi_unchanged = wifi_unchanged && wifi_password == request->arg(i);
      wifi_password = request->arg(i); 
    } else if (request->argName(i) == "sensor distance") {
      sensor_height_cm = request->arg(i).toInt();
    } else if (request->argName(i) == "tank diameter") {
      tank_diameter_cm = request->arg(i).toInt();
    }
  }
  connection_success = wifi_unchanged;
  should_reconnect = !wifi_unchanged;
  saveConfig();
  Serial.println("sending form data back");
  serializeJson(jDoc, response);
  Serial.println("form data serialized");
  request->send(200, "application/json", response);
  Serial.println("form data ssend back to client");
}


void loadConfig() {
  Serial.println("load config:");
  StaticJsonDocument<256> doc;
  EepromStream eepromStream(0, 256);
  deserializeJson(doc, eepromStream);
  if (doc != NULL) {
    Serial.println("found config file");
    wifi_ssid = String(doc["wifi ssid"]) ? String(doc["wifi ssid"]) : "";
    wifi_password = String(doc["wifi password"]) ? String(doc["wifi password"]) : "";
    sensor_height_cm = doc["sensor height cm"] ? doc["sensor height cm"] : 0;
    tank_diameter_cm = doc["tank diameter cm"] ? doc["tank diameter cm"] : 0;
    connection_success = doc["connection success"] ? doc["connection success"] : false;
    Serial.println("loaded config");
    serializeJson(doc, Serial);
    Serial.println();
  } else {
    Serial.println("not found config file, using dummy defaults");
    sensor_height_cm = 0;
    tank_diameter_cm = 0;
    wifi_ssid = "";
    wifi_password = "";
  }
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["wifi ssid"] = wifi_ssid;
  doc["wifi password"] = wifi_password;
  doc["sensor height cm"] = sensor_height_cm;
  doc["tank diameter cm"] = tank_diameter_cm;
  doc["connection success"] = connection_success;
  Serial.println("save config:");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.println("saving to EEPROM");
  EepromStream eepromStream(0, 256);
  serializeJson(doc, eepromStream);
  eepromStream.flush(); 
  Serial.println("config saved");
}

void makeSoftAP() {
  IPAddress local_IP(192,168,4,1);
  IPAddress gateway(192,168,4,1);
  IPAddress subnet(255,255,0,0);
  Serial.print("Setting soft-AP configuration ... ");
  Serial.println(WiFi.softAPConfig(local_IP, gateway, subnet) ? "Ready" : "Failed!");
  
  Serial.print("Wi-Fi mode set to WIFI_AP ... ");
  Serial.println(WiFi.mode(WIFI_AP) ? " Ready" : " Failed!");
  
  Serial.print("Setting soft-AP ... " + String(myname));
  Serial.println(WiFi.softAP(myname) ? " Ready" : " Failed!");
  
  Serial.print("Soft-AP IP address = ");
  Serial.println(WiFi.softAPIP());
}

void connectWifi() {
  if (wifi_ssid == "" || wifi_ssid == NULL) {
    Serial.println("empty ssid, turning to softAP");
    makeSoftAP();
    return;
  }
  if (wifi_password == "" || wifi_password == NULL) {
    Serial.println("empty wifi password, turning to softAP");
    makeSoftAP();
    return;
  }
  int i = 0;
  Serial.print("Wi-Fi mode set to WIFI_STA ... ");
  Serial.println(WiFi.mode(WIFI_STA) ? "Ready" : "Failed!");
  
  Serial.println("Connecting to wifi:" + wifi_ssid + " password:" + wifi_password);
  WiFi.begin(wifi_ssid, wifi_password);

  Serial.println("Connecting");
  while ((WiFi.status() != WL_CONNECTED) && (i < 20))
  {
    delay(500);
    Serial.print(".");
    i++;
  }
  Serial.println();
  WiFi.hostname(myname);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Could not connect " + String(WiFi.status()) + " to " + wifi_ssid);
    makeSoftAP();
    return;
  }
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
  if (!connection_success) {
    connection_success = true;
    saveConfig();
  }
}

void checkWifi() {
  /*
   * if in AP mode, and last WIFI connection was successful, try to reconnect
   */
  if ((wifi_get_opmode() == 2) && connection_success) {
    if (nowtime - lastWificheck > 60) {
      lastWificheck = nowtime;
      connectWifi();
    }
  }
}

void setupNTP() {
  timeClient.begin();
//  timeClient.setTimeOffset(3600);
  timeClient.update();
}

void setupMDNS() {
  if (!MDNS.begin(myname)) {
    Serial.println("Error setting up MDNS responder!");
  } else {
    Serial.println("mDNS responder started");
  }
}

void setup()
{
  Serial.begin(9600);
  Serial.println("Starting setup");
  Serial.flush();
  Serial.println("Initializing EEPROM...");
  EEPROM.begin(256);
  
  pinMode(LED_BUILTIN, OUTPUT);  // Initialize the LED_BUILTIN pin as an output
  pinMode(ULTRASOUND_ECHOPIN, INPUT);
  pinMode(ULTRASOUND_TRIGPIN, OUTPUT);

  LittleFS.begin();
  
  loadConfig();
  connectWifi();
  setupMDNS();
  setupNTP();
  server.serveStatic("/", LittleFS, "/").setDefaultFile("/index.html");
  server.on("/data", HTTP_GET, handleData);
  server.on("/save", HTTP_POST, handleSave);
  AsyncElegantOTA.begin(&server);
  server.begin();
  Serial.println("Web server started");

  MDNS.addService("http", "tcp", 80);
  
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    Serial.println(dir.fileName() + " - " + dir.fileSize());
  }
}

void loop()
{
  digitalWrite(LED_BUILTIN, HIGH);
  MDNS.update();
  timeClient.update();

  if (should_reconnect) {
    connectWifi();
    should_reconnect = false;
  }
  nowtime = getNow();
  current_hour = timeStep();
  checkWifi();
  rssi = WiFi.RSSI();
  current_distance_cm = measureDistance(); 
  calc_average_distance();
  if (round(last_average_distance_cm) != round(average_distance_cm)) {
    last_average_distance_cm = average_distance_cm;
    Serial.println(formatTime(nowtime) + " @ " + String(average_distance_cm) + "cm " + String(current_distance_cm) + "cm " + ESP.getFreeHeap());
  }
  if (measurements[current_hour] == 0) {
    Serial.println("timestep " + String(current_hour));
  }
  measurements[timeStep()] = average_distance_cm;

  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
  loop_count++;
}
