#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#define ULTRASOUND_ECHOPIN D6// Pin to receive echo pulse
#define ULTRASOUND_TRIGPIN D7// Pin to send trigger pulse
#define MEASUREMENT_COUNT 168

int sensor_height_cm = 0;
const String CONFIG_PATH = "/config_v3.json";
unsigned long current_distance_cm = 0;
float average_distance_cm = 0;
float last_average_distance_cm = 0;
unsigned long tank_diameter_cm = 0;
byte measurements[MEASUREMENT_COUNT] = {}; 
unsigned long loop_count = 0;
const char *myname   = "watertankmeter";
String wifi_ssid = "";
String wifi_password = "";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
ESP8266WebServer server(80);

void calc_average_distance() {
  if (average_distance_cm == 0) {
    average_distance_cm = float(current_distance_cm);
  } else {
    average_distance_cm = (average_distance_cm * 59 + current_distance_cm) / 60.0;
  }
}

int timeStep() {
  return timeClient.getEpochTime() % 604800L / 3600;
}

int remainingDepth(int distance_cm) {
  return sensor_height_cm - distance_cm;
}

float waterVolumeM3(int distance_cm) {
  if (distance_cm == 0) {
    return 0;
  }
  return 3.14 * (tank_diameter_cm/2.0) * (tank_diameter_cm/2.0) * remainingDepth(distance_cm) / (100 * 100 * 100);
}

String waterVolumeSanitizedM3(int distance_cm) {
  float val = waterVolumeM3(distance_cm);
  if (val != 0) {
    return String(val) + "m<sup>3</sup>";
  }
  return "no value";
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

unsigned long measureDistance() {
  int distance_cm;
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Set the trigger pin to low for 2uS
  delayMicroseconds(2);
  digitalWrite(ULTRASOUND_TRIGPIN, HIGH); // Send a 10uS high to trigger ranging
  delayMicroseconds(20);
  digitalWrite(ULTRASOUND_TRIGPIN, LOW); // Send pin low again
  distance_cm = pulseIn(ULTRASOUND_ECHOPIN, HIGH, 26000)/58; // Read in times pulse
  return distance_cm;
}


// prepare a web page to be send to a client (web browser)
String genHomePage()
{
  String htmlPage;
  htmlPage.reserve(20480);               // prevent ram fragmentation
  htmlPage = F("<!DOCTYPE HTML>"
               "<html><head>"
               "<style>"
               "html {"
                  "font-size: 200%;"
               "}"
               "table, th, td {"
                  "border: 1px solid black;"
               "}"
               ".volumetextcolor {"
                  "fill: black;"
               "}"
               "</style>"
               "<meta http-equiv=\"refresh\" content=\"60\">"
               "</head>"
               "<body>");

  htmlPage += "<form method=\"get\" action=\"/setup/\">\
    <input type=\"submit\" value=\"Setup\">\
  </form>";    
    
  htmlPage += "<table>";
  htmlPage += "<tr><td>volume</td>";
  htmlPage += "<td>" + waterVolumeSanitizedM3(average_distance_cm) + "</td></tr>";
  htmlPage += "<tr><td>average distance</td>";
  htmlPage += "<td>" + String(int(average_distance_cm)) + "cm</td></tr>";
  htmlPage += "<tr><td>current distance</td>";
  htmlPage += "<td>" + String(current_distance_cm) + "cm</td></tr>";
  htmlPage += "<tr><td>time</td>";
  htmlPage += "<td>" + formatTime(timeClient.getEpochTime()) + "</td></tr>";
  htmlPage += "<tr><td>sensor height</td>";
  htmlPage += "<td>" + String(sensor_height_cm) + "cm</td></tr>";
  htmlPage += "<tr><td>tank diameter</td>";
  htmlPage += "<td>" + String(tank_diameter_cm) + "cm</td></tr>";
  htmlPage += "<tr><td>WIFI</td>";
  htmlPage += "<td>" + ((wifi_get_opmode() == 2) ? ("AP: " + String(myname)) : ("Station: " + wifi_ssid)) + "</td></tr>";
  htmlPage += "<tr><td>your IP</td>";
  htmlPage += "<td>" + server.client().remoteIP().toString() + "</td></tr>";
  htmlPage += "<tr><td>my IP</td>";
  htmlPage += "<td>" + WiFi.localIP().toString() + "</td></tr>";
  htmlPage += "<tr><td>timestamp</td>";
  htmlPage += "<td>" + String(timeClient.getEpochTime()) + "</td></tr>";
  htmlPage += "<tr><td>loop number</td>";
  htmlPage += "<td>" + String(loop_count) + "</td></tr>";
  htmlPage += "</table>";
  
  htmlPage += "<table>";
  htmlPage += "<tr><td><b>time</b></td><td><b>volume</b></td><td><b>distance</b></td></tr>";
  int current_hour = timeStep();
  int nowtime = timeClient.getEpochTime();
  
  for(int i = current_hour; i > -1; i--) {
    htmlPage += "<tr><td>" + formatTime(subtractTime(nowtime, calculateTimeDiff(current_hour, i)) / 3600 * 3600) + "</td>";
    htmlPage += "<td>" + waterVolumeSanitizedM3(measurements[i]) + "</td>";
    htmlPage += "<td>" + ((measurements[i] == 0) ? "no value" : (String(measurements[i]) + "cm")) + "</td></tr>";
  }

  for(int i = MEASUREMENT_COUNT-1; i > current_hour; i--) {
    htmlPage += "<tr><td>" + formatTime(subtractTime(nowtime, calculateTimeDiff(current_hour, i)) / 3600 * 3600) + "</td>";
    htmlPage += "<td>" + waterVolumeSanitizedM3(measurements[i]) + "</td>";
    htmlPage += "<td>" + ((measurements[i] == 0) ? "no value" : (String(measurements[i]) + "cm")) + "</td></tr>";
  }
  
  htmlPage += "</table>";
  htmlPage += F("</body></html>"
                "\r\n");
  return htmlPage;
}

void handleRoot() {
  server.send(200, "text/html", genHomePage());
}

void handleSetup() {
  String htmlPage = "<form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"/postform/\">\
    <label for=\"fname\">wifi name:</label>\
    <input type=\"text\" name=\"wifi name\" value=\"" + wifi_ssid + "\">\
    <label for=\"fname\">wifi password:</label>\
    <input type=\"text\" name=\"wifi password\" value=\"" + wifi_password + "\">\
    <label for=\"fname\">senzor distance from bottom level:</label>\
    <input type=\"text\" name=\"sensor distance\" value=\"" + sensor_height_cm + "\">\
    <label for=\"fname\">tank diameter:</label>\
    <input type=\"text\" name=\"tank diameter\" value=\"" + tank_diameter_cm + "\">\
    <input type=\"submit\" value=\"Submit\">\
  </form>";
  server.send(200, "text/html", htmlPage);
}

void handleForm() {
  String message = F("<html><head>"
                    "<meta http-equiv=\"refresh\" content=\"10; URL=http://watertankmeter.local/\">"
                    "</head>"
                    "<body>");
  
  message += "Setup data:<br>";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "<br>";
    if (server.argName(i) == "wifi name") {
      wifi_ssid = server.arg(i); 
    } else if (server.argName(i) == "wifi password") {
      wifi_password = server.arg(i); 
    } else if (server.argName(i) == "sensor distance") {
      sensor_height_cm = server.arg(i).toInt();
    } else if (server.argName(i) == "tank diameter") {
      tank_diameter_cm = server.arg(i).toInt();
    }
  }
  saveConfig();
  message += "<br> <form method=\"get\" action=\"/\">\
    <input type=\"submit\" value=\"Home\">\
    </form></body></html>";
  server.send(200, "text/html", message);
  delay(2000);
  connectWifi();
  setupMDNS();
  setupNTP();
}


void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }
  Serial.println("load config:");
  StaticJsonDocument<256> doc;
  if (LittleFS.exists(CONFIG_PATH)) {
    Serial.println("found config file");
    File file = LittleFS.open(CONFIG_PATH, "r");
    deserializeJson(doc, file);
    file.close();
    wifi_ssid = String(doc["wifi ssid"]);
    wifi_password = String(doc["wifi password"]);
    sensor_height_cm = doc["sensor height cm"];
    tank_diameter_cm = doc["tank diameter cm"];
    Serial.println("loaded config");
    serializeJson(doc, Serial);
    Serial.println();
  } else {
    LittleFS.format();
    Serial.println("not found config file, using dummy defaults");
    sensor_height_cm = 200;
    tank_diameter_cm = 100;
    wifi_ssid = "";
    wifi_password = "";
  }
  LittleFS.end();
  Serial.flush(); 
}

void saveConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }
  if (LittleFS.exists(CONFIG_PATH)) {
    LittleFS.remove(CONFIG_PATH);
  }
  StaticJsonDocument<256> doc;
  doc["wifi ssid"] = wifi_ssid;
  doc["wifi password"] = wifi_password;
  doc["sensor height cm"] = sensor_height_cm;
  doc["tank diameter cm"] = tank_diameter_cm;
  File file = LittleFS.open(CONFIG_PATH, "w");
  Serial.println("save config:");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.println("saving to file");
  serializeJson(doc, file);
  file.close();
  LittleFS.end();
  Serial.flush();
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
  int i = 0;
  Serial.print("Wi-Fi mode set to WIFI_STA ... ");
  Serial.println(WiFi.mode(WIFI_STA) ? "Ready" : "Failed!");
  
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
}

void setupNTP() {
  timeClient.begin();
  timeClient.setTimeOffset(3600);
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
  
  pinMode(LED_BUILTIN, OUTPUT);  // Initialize the LED_BUILTIN pin as an output
  pinMode(ULTRASOUND_ECHOPIN, INPUT);
  pinMode(ULTRASOUND_TRIGPIN, OUTPUT);

//  LittleFS.begin();
  
  loadConfig();
  connectWifi();
  setupMDNS();
  setupNTP();
  
  server.begin();
  Serial.println("Web server started");
  server.on("/", handleRoot);
  server.on("/setup/", handleSetup);
  server.on("/postform/", HTTP_POST, handleForm);
  MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(myname);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }
//    LittleFS.end();
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
//    LittleFS.begin();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
}

void loop()
{
  digitalWrite(LED_BUILTIN, HIGH);
  MDNS.update();
  timeClient.update();
  ArduinoOTA.handle();

  int current_hour = timeStep();

  current_distance_cm = measureDistance(); 
  calc_average_distance();
  if (int(last_average_distance_cm) != int(average_distance_cm)) {
    last_average_distance_cm = average_distance_cm;
    Serial.println(formatTime(timeClient.getEpochTime()) + " @ " + String(average_distance_cm) + "cm " + String(current_distance_cm) + "cm");
  }
  if (measurements[current_hour] == 0) {
    Serial.println("timestep " + String(current_hour));
  }
  measurements[timeStep()] = average_distance_cm;

  server.handleClient();
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
  loop_count++;
}
