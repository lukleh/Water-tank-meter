# Water tank meter

<img src="imgs/screenshot_mobile.jp" width=50% height=50%>
![Mobile screenshot](imgs/screenshot_mobile.jpg)

ESP8266 based project for measuring water level in tank using ultrasonic sensor complete with 3D printed enclosure accesible through web interface

## Dependencies
* https://arduino-esp8266.readthedocs.io/en/latest/installing.html
* ArduinoJson
* StreamUtils
* https://github.com/me-no-dev/ESPAsyncTCP/archive/refs/heads/master.zip
* https://github.com/me-no-dev/ESPAsyncWebServer/archive/refs/heads/master.zip

## Wiring diagram

![Wiring diagram](imgs/wiring.png)


## Electronics parts
* Wemos D1 mini pro


## Setup

* compile and upload firmware + static files ([how to](https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html#uploading-files-to-file-system))
* on first boot, the board sets up WIFI AP with  default "watertankmeter" SSID and "watertankmeter.local" address
* connect and setup WIFI and tank dimensions and sensor distances
* NOTICE: minimum full distance is 20cm - which is the minimum distance the sensor can measure
* Better to keep the sensor at least 30cm above the maximum water level


![Distances logic](imgs/logic.png)