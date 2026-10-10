# seismoM5: aftershock monitor fork

*[Leer en español](README.es.md)*

This is a fork of [febalci/seismoM5](https://github.com/febalci/seismoM5), the M5StickC earthquake sensor by **febalci**. The original repository is archived, so these changes live here. All the credit for the sensor itself goes to febalci: the MPU6886 calibration, the PGA and STA/LTA triggers, the web settings page and the MQTT integration all come from the original project, and they were what made it possible to build a working setup in a single night.

## Why this fork exists

This fork was built during the aftershock sequence of the M7.7 earthquake off Panama on October 9, 2026. Starting from a sensor that already calibrated and measured made it possible to have something working that same night, and most of what was added came from trial and error with real aftershocks:

- **Two sticks, two jobs.** The sensor stays fixed and only measures; a second stick (an M5StickC Plus) is the notifier and can sit wherever people will see it.
- **The alarm levels came from the data.** At 150-200 km, an aftershock is weaker than a bump on the desk but keeps going for several seconds, so both levels are based on duration rather than peak alone.
- **First result:** an M5.1 aftershock 155 km away was detected (EMSC confirmed); little below M4.5 is visible at that distance.

## How it works

```mermaid
flowchart LR
  S["Sensor<br/>M5StickC on a wall"] -- "ESP-NOW, instant" --> N["Notifier<br/>M5StickC Plus"]
  S -- "push over WiFi" --> T["ntfy.sh"]
  C["USGS + EMSC catalogs<br/>5-10 min delay"] -- "every 60 s" --> N
  N -- "M4.0+ in the region" --> T
  T --> P["Phone<br/>ntfy app"]
  S -- "UDP log, 10 samples/s" --> M["Computer<br/>tools/udplog.py"]
  M -- "tools/xcheck.py" --> E["events.csv<br/>sensor vs catalog"]
```

The sensor tells the notifier through ESP-NOW, a direct ESP32-to-ESP32 radio link that does not go through the internet, and sends a push to the phone at the same time. The notifier also polls the USGS and EMSC catalogs every minute. Catalogs arrive 5 to 10 minutes after the shaking, so their events are shown on screen and pushed to the phone silently.

## What's new in this fork

### Sensor (`src/`)

- **Builds again today.** Pinned `espressif32@6.10.0`, ESPAsyncWebServer at commit `f71e3d4` and WebSerial `~1.4.0`; newer versions no longer compile this code.
- **Drift-free PGA.** PGA is measured against a 30-second moving average of the acceleration instead of the boot calibration. On the original code the noise floor tripled overnight (0.0008 g to 0.0024 g), likely from temperature; now it stays at about 0.0006 g.
- **Two alarm levels, both based on duration.** A bump is a short spike; a quake keeps going.

  | Level | Rule (10 samples/s, 5 s window) | Notifier | Phone |
  | --- | --- | --- | --- |
  | Blue: possible aftershock | 12 of 50 samples above 0.006 g | Blue screen, slow LED, one "tu tu tu tu" | Normal push |
  | Red: strong shaking | 10 of 50 samples above `pga_trigger` (0.015 g) | Red screen, full alarm | Urgent push |

  A blue event that grows turns red on the spot. Replayed over a full day of data, the red rule fired zero times on desk bumps up to 0.05 g. Blue still fires when people move near the sensor; that is the price of catching distant aftershocks.
- **Tilt auto-recalibration.** A reading stuck above the threshold for 60 s is a moved sensor, not a quake: it recalibrates itself and tells the phone.
- **ntfy push notifications** with a priority per level, sent one at a time from a queue (parallel TLS runs the ESP32 out of memory).
- **ESP-NOW broadcast** to the notifier: event start, level changes, end, and a heartbeat every 10 s.
- **Detections page** at `http://seismom5.local/events`: the last 20 events with time, level, peak and duration, kept across reboots.
- **UDP logging**: every sample and log line goes to a computer over WiFi, so the sensor needs no cable to the computer.

### Notifier (`notifier/`)

A separate PlatformIO project for a second stick (an M5StickC Plus here):

- Receives the sensor over ESP-NOW and shows blue or red alarms, with sensor status and the last local peak.
- Polls USGS and EMSC every 60 s for the region, deduplicates the same quake across both feeds (±60 s, 100 km), and pushes M4.0+ silently.
- Battery percentage, charging indicator, and a push when USB power is lost or restored.
- **Mute** with a long press on button A, saved across reboots: alarms still show on screen and LED.
- Updates over WiFi (ArduinoOTA).

### Tools (`tools/`)

- `udplog.py` receives the UDP log and writes `logs/pga_<date>.csv`.
- `xcheck.py` cross-checks a day of sensor data against the EMSC catalog. For each quake it computes when the S wave should arrive and what the sensor measured then. It writes `logs/events.csv` and raw samples for every event, and merges your own notes from `logs/labels.csv` ("someone sat down", "door closed") so false triggers become labelled data.

## Setup

1. Copy `src/secrets.example.h` to `src/secrets.h` and fill in WiFi, the ntfy topic (make it hard to guess: anyone who knows it can read and send your alerts) and the name of the logging computer. `secrets.h` is ignored by git.
2. Flash the sensor: `pio run -e m5stick-c -t upload` (or `m5stick-c-plus`). Add `PLATFORMIO_BUILD_FLAGS="-DPGA_STREAM"` to also stream every sample over serial.
3. Mount it firmly and leave it still for about 30 s while it calibrates. Any orientation works. Then set the PGA trigger to 0.015 g at `http://seismom5.local`. To recalibrate later without touching it, open `http://seismom5.local/recal`.
4. Flash the notifier: `cd notifier && pio run -e m5stick-c-plus -t upload`; later updates go over WiFi with `pio run -e ota -t upload`. Set your region in `notifier/src/notifier_config.h`.
5. On the phone, install [ntfy](https://ntfy.sh) and subscribe to your topic.
6. Optional logging: `python3 tools/udplog.py logs`, then `python3 tools/xcheck.py logs/pga_<date>.csv`. Set your location with `SEISMO_HOME=lat,lon`.

**Notifier buttons:** A closes an alarm, a long press on A toggles mute, B plays a 3-second test of the red alarm.

## Lessons learned

- **Placement matters more than the algorithm.** On a desk, people make more vibration than an M5 aftershock. On a wall, the first try failed because the tape crept under the cable's weight until the stick fell; tape the cable to the wall, not just the stick. A USB cable to the computer also carries desk vibration to the sensor, which is why logging moved to WiFi.
- **Calibrate where it stays.** Half a degree of tilt already reads as 0.008 g.
- **Opening the serial port resets these sticks**, and the sensor recalibrates. Prefer OTA and UDP logging.
- **Test rules on recorded data before flashing.** Every trigger rule here was replayed over the full day's log first.
- **It is an amateur device.** At 150-200 km it sees roughly M5 and up. It does not replace a seismic network, but it warns minutes before the catalogs publish.

---

## Original README by febalci

*Kept as written for the original project. In this fork, WiFi and MQTT settings live in `src/secrets.h` (see Setup above).*
<p align="center">
<img src="pics/S5_logo.jpg" width="150" height="125">
</p>
SeismoM5 is an Earthquake Sensor Implementation on M5StickC, using its own MPU6886 accelerometer. Although MPU6886 in M5StickC is a bit on the noisy side; disabling Gyro and using an accel calibration helped a little. The properties of this project is:

- Calibrate MPU6886 accelerometer and use calibration results as offsets.
- Use DLPF - 5 Hz.
- Use MQTT and send data, only when an earthquake happens.
- Draw X,Y,Z accel results on M5stickC screen as graph, only when an earthquake happens.
- Show PGA always.
- Emergency alerts; Red Led and SPK HAT sound warning during an earthquake.

<p align="center">
<img src="pics/IMG_4267a.jpg" width="382" height="200">
</p>
  
### Seismology

I am nowhere near being a Seismologist or I understand anything about. It is just that I live in an overly active Seismic zone. The idea came up for adding an earthquake sensor to the smart home; to shut gas valves off and open some rolling shutters for an escape route during an earthquake.

So i tried some signal algorithms to get the most effective earthquake trigger mechanism possible. There are 2 triggering methods; PGA threshold trigger and STA/LTA trigger method.

``
pga = sqrt(x_vector_mag * x_vector_mag + y_vector_mag * y_vector_mag + z_vector_mag * z_vector_mag)*scale_factor;
``

PGA gives intensity too, but i do not know how reliable that is.

### Usage

This is a Platformio code. First of all, the user needs to fill WiFi and MQTT server details in main.cpp. Other parameters like Seismic, MPU calibration or screen properties can be changed for best fit after some tests and trials. Also for M5stickC select **env:M5stick-c** and for M5StickC-Plus select **env:M5stick-c-plus** in Platformio.

Button A of M5stickC toggles MQTT server connection. If bottom right corner of the screen shows MQTT, the server is connected.

Button B of M5stickC resets M5StickC for recalibration purposes.

<img src="pics/IMG_3999.jpg" width="245" height="150"> <img src="pics/IMG_4001.jpg" width="254" height="150"> <img src="pics/IMG_4003.jpg" width="250" height="150">


### MQTT

#### Availability: seismoM5/status

"**online**" or "**offline**"


#### State: seismoM5/state

**INIT_MPU:** Init MPU6886.

**WAIT:** Wait for 10 seconds, to get ready to keep M5StickC steady.

**CALIBRATION:** MPU6886 Accelerator find offsets and send to MPU registers for calibration. Might take up to 1 minute.

**LISTENING:** Everything is ready and listening for earthquakes.

**EARTHQUAKE:** Earthquake is happening


#### Events: seismoM5/event

**{"x":"-85","y":"-1097","z":"16305","pga":"0.0007"}**   <sup>Sample</sup>

Momentary x,y,z parameters and PGA in (g). Only send while an earthquake occurs.

PGA results can be compared with the values in:

[PGA Correlation with the Mercalli scale](https://en.wikipedia.org/wiki/Peak_ground_acceleration#Correlation_with_the_Mercalli_scale)

or

[Japan Meteorological Agency seismic intensity scale](https://en.wikipedia.org/wiki/Japan_Meteorological_Agency_seismic_intensity_scale#Scale_overview)


#### PGA Trigger: seismoM5/pga_trigger

Displays PGA trigger value for the earthquake alert. The first time SeismoM5 works after the first upload, the default PGA Trigger value will be 0.025 g. On screen, top right corner shows the PGA Trigger value.


#### Commands: seismoM5/command

Should be send in JSON format as follows: 
```
{
    "pga_trigger": 0.0150,
    "update": false,
    "reset": false,
    "speaker_enable" : true,
    "lcd_brightness" : 7,
    "continuous_graph" : false,
    "update_period" : 30
}
```
There is no need to send this full message, any item can be send standalone or together with any other item.

**"pga_trigger" : *float***  - Changes the PGA Trigger. If the PGA Trigger is ever changed, SeismoM5 saves this value permanently and use it from then on even after you reset SeismoM5.

**"reset" : *bool***  - If true, resets M5StickC for recalibration purposes.

**"update" : *bool***  - If true, sends a one time event message update request.

**"speaker_enable" : *bool***  - If true, enables the SPK HAT Speaker, false disables it.

**"lcd_brightness" : *int***  - Sets the standby LCD brightness, should be between 7-15, 15 being the brightest.

**"continuous_graph" : *bool***  - If true, draws the graph continuously, even when there is no earthquake.

**"update period" : *int***  - Sets the update period of event mqtt message to be send, even there is no earthquake, in seconds.


### SeismoM5 Web Page

<p align="center">
<img src="pics/Screenshot 2023-01-29 at 21.01.02.jpg" width="245" height="371">
</p>

You can connect to SeismoM5 either with http://seismom5.local address or its IP address to monitor or change:

- Change PGA Trigger Value

- Change standby LCD Brightness

- Change the update period of event mqtt message to be send, even there is no earthquake, in seconds

- Use STA/LTA Method for triggering, this disables PGA Triggering.

- Enable/Disable Speaker

- Enable/Disable continuous graph

- Change Logging Type (Serial, WebSerial, Both or None)

- WebSerial Output

- Recalibrate MPU

- Restart SeismoM5 ESP32

### OTA Firmware Upload

The first upload should be a Serial connection upload. After that, these 2 lines can be uncommented (delete ";" characters) from platformio.ini file for OTA firmware uploading.

```
;upload_port = seismoM5.local
;upload_protocol = espota
```

### Master / Slave Configuration for more reliable triggers

Although MPU6886 on M5StickC is not very much noisy, once in a while, the pga jumps for 5-10 seconds above trigger (frequency is like once or twice every 1-2 days, up to 0.0150 g according to my results). So i also added a Master/Slave Configuration which requires two M5Sticks. If the Master stick triggers an earthquake, it also checks the Slave Stick and if it also has a trigger, then the Master changes its state to "EARTHQUAKE" only then...

**Configuration:**

1. In order to upload "SLAVE" first, uncomment "//#define SLAVE" and make it "**#define SLAVE**" in config.h file.

2. Upload the SLAVE M5StickC. The SLAVE is ready.

3. Comment the "#define SLAVE" line back to "**//#define SLAVE**" in config.h file.

4. Upload the MASTER M5StickC.

5. Go to the Web Page of MASTER M5StickC.

6. Check "Master" checkbox and save it. If you don't check the "Master" checkbox on web page, this stick will still act as a standalone SeismoM5.

7. Just check the /state of MASTER Stick for earthquakes, not the SLAVE!


### Mounting

Seismologists mount accelerometer based seismic sensors to the lowest point of the structure, close to the ground as possible. However, since this is an amateur earthquake sensor, i mount it as high as possible in the house, on a wall. You can use the in-built magnets of M5StickC to attach it to a metal surface but as i experienced they are not so strong and may fall off during an earthquake. Double sided adhesive mounting tapes can also work, but make sure the wall paint is a stickable one. All in all, it should be mounted very firm and be careful about the x,y,z axis of the accelerometer; use a carpenters level if required. 

** Due to the screen position, x axis is used as z axis and z axis is used as x axis within the code.

### Installation

1. Download VSCode from https://code.visualstudio.com/download and install it.
2. Open VSCode and press "Extensions" button (which has small squares icon) which is on the left vertical bar of the screen.
3. In "Search extensions in Marketplace"  edit box, write "Platformio" and find and install Platformio IDE on VSCode.
4. Wait for some time for platformio to be installed completely.

This was the Compiler installation part. Now we will open and upload SeismoM5 to our stick.
1. From https://github.com/febalci/seismoM5 address Press the Green "Code" Button on this page and select "Download ZIP". Download and unzip this into a folder.
2. From VSCode, Press the Alien head on the left vertical bar on the screen. This is PIO (PlatformIO).
3. Select "Open". PIO Home Page will come up. Press "Open Project" button from this page. Select the Folder of unzipped SeismoM5 and press open.
4. Wait for some time for PIO to open and set everything up for this project.
5. From the bottom blue bar of VSCode, click on where it writes "Default (seismom5)" and select "env:m5stick-c-plus" or "env:m5stick-c" from there.
6. Now we will set the config parameters. Click "Explorer" on the left vertical band and open the "config.h" file under src folder.
7. In this file you need to enter these parameters:
```
//WiFi Parameters
#define WIFI_SSID "XXXXXXXX"
#define WIFI_PASS "XXXXXXXX"

#define MQTT_SERVER "XXXXXXXX"
#define MQTT_PORT 1883
#define MQTT_USER "XXXXXXXX"
#define MQTT_PASS "XXXXXXXX"
```
8. Everything is ready to upload now. Connect your M5stick to a usb port on your computer. On the bottom blue bar find the arrow icon pointing right (Tooltip: PlatformIO Upload). Click it and wait for it to upload to M5Stick.

### Home Assistant Integration

<p align="center">
<img src="pics/S5_graph.jpg" width="382" height="200">
</p>

Here is the configuration that you should add to configuration.yaml:

```
mqtt:
  sensor:
    - name: "seismoM5_online"
      state_topic: "seismoM5/status"
      icon: mdi:pulse
    - name: "seismoM5_state"
      state_topic: "seismoM5/state"
      availability:
        - topic: "seismoM5/status"
          payload_available: "online"
          payload_not_available: "offline"
      icon: mdi:pulse
    - name: "seismoM5_pga"
      availability:
        - topic: "seismoM5/status"
          payload_available: "online"
          payload_not_available: "offline"
      state_topic: "seismoM5/event"
      value_template: "{{ value_json.pga }}"
      icon: mdi:pulse
      unit_of_measurement: "g"
    - name: "seismoM5_x"
      availability:
        - topic: "seismoM5/status"
          payload_available: "online"
          payload_not_available: "offline"
      state_topic: "seismoM5/event"
      value_template: "{{ value_json.x }}"
      icon: mdi:pulse
      unit_of_measurement: "g"
    - name: "seismoM5_y"
      availability:
        - topic: "seismoM5/status"
          payload_available: "online"
          payload_not_available: "offline"
      state_topic: "seismoM5/event"
      value_template: "{{ value_json.y }}"
      icon: mdi:pulse
      unit_of_measurement: "g"
    - name: "seismoM5_z"
      availability:
        - topic: "seismoM5/status"
          payload_available: "online"
          payload_not_available: "offline"
      state_topic: "seismoM5/event"
      value_template: "{{ value_json.z }}"
      icon: mdi:pulse
      unit_of_measurement: "g"
  button:
    - unique_id: seismom5_reset_btn
      name: "Restart SeismoM5"
      command_topic: "seismoM5/command"
      payload_press: "{\"reset\": true }"
      availability:
        - topic: "seismoM5/status"
      qos: 0
      retain: false
      entity_category: "config"
      device_class: "restart"
```

and the earthquake warning automation for a "pushover" app on smart phone notification:

```
- alias: Earthquake
  trigger:
  - platform: state
    entity_id: sensor.seismom5_state
    from: "LISTENING"
    to: "EARTHQUAKE"
  action:
  - service: notify.pushover
    data_template:
      message: >
        DEPREM !!!
```

offline warning automation for a "pushover" app on smart phone notification:

```
- alias: SeismoM5 Online Check
  trigger:
  - platform: state
    entity_id: sensor.seismom5_state
    to: "unavailable"
  condition:
  - condition: template
    value_template: '{{(as_timestamp(now()) - as_timestamp(state_attr("automation.seismom5_online_check", "last_triggered") | default(0)) | int > 600 )}}'
  action:
  - service: notify.pushover
    data_template:
      message: >
        SeismoM5 Offline...
  id: "seismom5_online_check"
```

Lovelace cards for the sensors
```
type: grid
square: false
columns: 1
cards:
  - type: entity
    entity: sensor.seismom5_pga
  - type: entity
    entity: sensor.seismom5_state
    name: SeismoM5 State
  - type: entity
    entity: sensor.seismom5_online
  - type: logbook
    entities:
      - sensor.seismom5_state
  - show_name: true
    show_icon: true
    type: button
    tap_action:
      action: toggle
    entity: button.restart_seismom5
    icon_height: 30px
```
