#ifdef STICKC
  #include <M5StickC.h>
#else
  #include <M5StickCPlus.h>
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <stdlib.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include "MPU6886s.h"
#include "config.h"
#include "util.h"
#include "mqtt.h"
#include "stalta.h"
#include "screen.h"
#include "notify.h"
#include "udplog.h"
#include "events.h"

float tilt_ref_pga = 0;
unsigned long tilt_steady_since = 0;

uint8_t eq_level = SEISMO_LEVEL_NONE;  // highest level reached by the current event
unsigned long eq_started_ms = 0;
float pga_raw = 0;  // against the calibration only, before drift removal

// Slow moving average of the acceleration; pga is measured against it
float ref_x = 0, ref_y = 0, ref_z = 0;
bool ref_ready = false;

float dynamic_pga(float x, float y, float z) {
  if (!ref_ready) {
    ref_x = x; ref_y = y; ref_z = z;
    ref_ready = true;
  }
  float dx = x - ref_x, dy = y - ref_y, dz = z - ref_z;
  const float a = 1.0f / DYN_TAU_SAMPLES;
  ref_x += a * dx; ref_y += a * dy; ref_z += a * dz;
  return sqrt(dx * dx + dy * dy + dz * dz);
}

// Ring buffer for the sustained-shaking levels: bit 0 = above SUSTAIN_LEVEL,
// bit 1 = above pga_trigger
uint8_t shake_flags[SUSTAIN_WINDOW] = {0};
int shake_idx = 0;
int weak_count = 0;
int strong_count = 0;

uint8_t shaking_level(float pga) {
  uint8_t f = (pga >= SUSTAIN_LEVEL ? 1 : 0) | (pga >= pga_trigger ? 2 : 0);
  uint8_t old = shake_flags[shake_idx];
  weak_count += (f & 1) - (old & 1);
  strong_count += ((f >> 1) & 1) - ((old >> 1) & 1);
  shake_flags[shake_idx] = f;
  shake_idx = (shake_idx + 1) % SUSTAIN_WINDOW;
  if (strong_count >= STRONG_COUNT) return SEISMO_LEVEL_STRONG;
  if (SUSTAIN_ENABLE && weak_count >= SUSTAIN_COUNT) return SEISMO_LEVEL_WEAK;
  return SEISMO_LEVEL_NONE;
}

void sustained_reset() {
  memset(shake_flags, 0, sizeof(shake_flags));
  shake_idx = 0;
  weak_count = 0;
  strong_count = 0;
  ref_ready = false;
}

AsyncWebServer server(80);

MPU6886s seismo;

STA_LTA staLta(trigger_threshold,detrigger_threshold);

screen lcd;

void updateState(const char* state) {
  publish_mqtt(MQTT_PUB_STATE, state, 1, true);
}

String updateEvent(int16_t _x, int16_t _y, int16_t _z, float _pga) {
  String msg = "{\"x\":\""+ String(_x) + "\",\"y\":\"" + String(_y) + "\",\"z\":\"" + String(_z) + "\",\"pga\":\""+ String(_pga,4).c_str() +"\"}";
  return msg;
}

void handleReset(AsyncWebServerRequest *request) {
  request->send(200, "text/html", "Reset OK");    
  logln("MQTT Restart Request Received");
  ESP.restart();
}

void calibrate_MPU() {
  updateState("INIT_MPU");
  seismo.Init();
  if (!calibrating) {
    updateState("WAIT");
    lcd.show_wait();
  }

  lcd.show_calibration();
  updateState("CALIBRATION");

  seismo.calibrateAccel(buffersize,acel_deadzone);

  sustained_reset(); // old samples are from before the new offsets
  calibrating = false;
  delay(100);
  seismo.getAccelAdc(&ax, &ay, &az);
  publish_mqtt(MQTT_PUB_EVENT, updateEvent(ax,ay,az,pga).c_str(), 0, true);
  updateState("LISTENING");
  lcd.clear_screen();
  lcd.set_brightness(lcd_brightness);
  lcd.draw_axis();
  lcd.show_title();
}

String config_processor(const String& var){
  String server_details = "";
  if (var == "PGAPLACEHOLDER"){
    server_details = String(pga_trigger,4);
  } else if (var == "BRIPLACEHOLDER"){
    server_details = String(lcd_brightness);
  } else if (var == "PERPLACEHOLDER"){
    server_details = String(flush_period);
  } else if (var == "SPKPLACEHOLDER"){
    server_details = SPK_HAT;
  } else if (var == "CONPLACEHOLDER"){
    server_details = continuous_graph;
  } else if (var == "LOGPLACEHOLDER"){
    server_details = logging;
  } else if (var == "SLTAPLACEHOLDER"){
    server_details = slta;
  } else if (var == "MASTERPLACEHOLDER"){
    server_details = master;
  } else if (var == "SLAVEPLACEHOLDER"){
    server_details = slave;
  } 
  return server_details;
}

void eq_happening(uint8_t level = SEISMO_LEVEL_STRONG) {
  if (!eq_status || level > eq_level) {
    bool escalated = eq_status;
    if (!eq_status) {
      eq_started_ms = millis();
      events_start();
    }
    eq_level = level;
    logln(String(escalated ? "[trigger] escalated" : "[trigger]") + (level >= SEISMO_LEVEL_STRONG ? " strong (red)" : " weak (blue)") +
          " pga=" + String(pga, 4));
    notify_eq_start(pga, pga_trigger, level, escalated);
  }
  else notify_eq_update(pga);
  eq_status = true;
  eq_time_count = 0;
  lcd.set_brightness(15); // Full Brightness
  updateState("EARTHQUAKE");
  digitalWrite(M5_LED, LOW);
  if (SPK_HAT) ledcWriteTone(spkChannel, 800);
}

void connectToWifi() {
  logln("Connecting to Wi-Fi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void onWifiEvent(WiFiEvent_t event) {
}

void checkConnection() {
  if (!WiFi.isConnected() && (conn_stat != 1)) { conn_stat = 0; }
  if (MQTT_active) {
    if (WiFi.isConnected() && !mqttClient.connected() && (conn_stat != 3))  { conn_stat = 2; }
    if (WiFi.isConnected() && mqttClient.connected() && (conn_stat != 5)) { conn_stat = 4;}
  } else {
    if (WiFi.isConnected() && (conn_stat == 1))  { conn_stat = 4; }
  }
  switch (conn_stat) {
    case 0:                                                       // MQTT and WiFi down: start WiFi
      logln("[MQTT] and [WiFi] down: start [WiFi]");
      connectToWifi();
      conn_stat = 1;
      waitCount = 0;
      break;
    case 1:                                                       // WiFi starting, do nothing here
      logln("[WiFi] starting, wait : "+ String(waitCount));
      waitCount++;
      if (waitCount > WIFI_RECONNECT_TIMER) {
        ESP.restart();
      }
      break;
    case 2:                                                       // WiFi up, MQTT down: start MQTT
      logln("[WiFi] up, [MQTT] down: start [MQTT]");
      if (MQTT_active) connectToMqtt();
      conn_stat = 3;
      waitCount = 0;
      break;
    case 3:                                                       // WiFi up, MQTT starting, do nothing here
      logln("[WiFi] up, [MQTT] starting, wait : "+ String(waitCount));
      waitCount++;
      break;
    case 4:                                                       // WiFi up, MQTT up: finish MQTT configuration
      if (MQTT_active) {
        logln("[WiFi] up, [MQTT] up: Connected");
      } else {
        logln("[WiFi] up, [MQTT] button disabled: Connected");
      }
      conn_stat = 5;  
      waitCount = 0;                  
      break;
  }
}

void setup() {
  Serial.begin(115200);   // Initialize serial communication
  M5.begin();

#ifdef SLAVE
  slave = true;
#else
  slave = false;
#endif

  pref_init(); // Read Parameters
  pinMode(M5_LED, OUTPUT); // Setup M5 Red LED
  digitalWrite(M5_LED, HIGH);

  ledcSetup(spkChannel, spkFreq, spkResolution); // Setup SPK_HAT
  ledcAttachPin(SPK_pin, spkChannel);
  ledcWriteTone(spkChannel, 0);
  lcd.show_wifi_connect();  
  initMqtt(); //Initialize MQTT Server Parameters

  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWifiEvent);
  WiFi.setHostname(WIFI_HOSTNAME);
  connectToWifi();

  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    waitCount++;
    if (waitCount > (WIFI_RECONNECT_TIMER)) {
      ESP.restart();
    }
  }
  
  espnow_init();
  events_begin();
  if (MQTT_active) connectToMqtt();

  WebSerial.begin(&server);
  webSerialEnabled = true;

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);

  pga_trigger_changed = pga_trigger;

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {request->send_P(200, "text/html", index_html,config_processor);});
  server.on("/events", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", events_page(pga, pga_trigger, eq_status));
  });
  server.on("/event_del", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("t")) events_delete(strtoul(request->getParam("t")->value().c_str(), nullptr, 10));
    request->redirect("/events");
  });
  server.on("/rt", HTTP_GET, [](AsyncWebServerRequest *request){handleReset(request);});
  server.on("/recal", HTTP_GET, [](AsyncWebServerRequest *request){
    calibrating = true;
    request->send(200, "text/html", "Recalibrating MPU");
    });
  server.on("/sv", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("new_pga") && (request->getParam("new_pga")->value() != "")) {
      pga_trigger_changed = request->getParam("new_pga")->value().toFloat();
    }
    if (request->hasParam("new_bri") && (request->getParam("new_bri")->value() != "")) {
      lcd_brightness = request->getParam("new_bri")->value().toInt();
      lcd.set_brightness(lcd_brightness);
    }
    if (request->hasParam("new_per") && (request->getParam("new_per")->value() != "")) {
      flush_period = request->getParam("new_per")->value().toInt();
    }
    if (request->hasParam("spk")) {
      if (request->getParam("spk")->value() == "on") SPK_HAT = true; else SPK_HAT = false;
    } else SPK_HAT = false;
    if (request->hasParam("slta")) {
      if (request->getParam("slta")->value() == "on") slta = true; else slta = false;
    } else slta = false;
    if (request->hasParam("con")) {
      if (request->getParam("con")->value() == "on") continuous_graph = true; else continuous_graph = false;
    } else continuous_graph = false;
    if (request->hasParam("lg") && (request->getParam("lg")->value() != "")) {
      logging = (request->getParam("lg")->value().toInt());
    }
    if (request->hasParam("master")) {
      if (request->getParam("master")->value() == "on") master = true; else master = false;
    } else master = false;
    pref_update();
    request->send(200, "text/html", "Settings Changed");
    request->redirect("/");
  });
  server.onNotFound([](AsyncWebServerRequest *request){request->send(404);});

  server.begin();
  calibrate_MPU();
}

void loop() {
  checkConnection();
  notify_loop(pga, pga_trigger);
  udplog_loop();
  ArduinoOTA.handle();
  if (!calibrating) {
    M5.update();
    seismo.getAccelAdc(&ax, &ay, &az);

    if (slta) {
      lcd.show_method(slta, true);
    } else {
      if (pga_trigger_changed != pga_trigger) {
        pga_trigger = pga_trigger_changed;
        lcd.show_method(false, true);
        updateState("CHANGED_PGA_TRIGGER");
        publish_mqtt(MQTT_PUB_PGA_TRIGGER, String(pga_trigger,4).c_str(), 1, true);
        updateState("LISTENING");
      }
      lcd.show_method(false,false,pga_trigger);
    }

    lcd.show_mqtt(mqttClient.connected());
//    lcd.draw_axis();

    x_vector_mag = ax / gravity - 1.0;
    y_vector_mag = ay / gravity;
    z_vector_mag = az / gravity;

    pga_raw = sqrt(x_vector_mag * x_vector_mag + y_vector_mag * y_vector_mag + z_vector_mag * z_vector_mag)*scale_factor;
    pga = dynamic_pga(x_vector_mag, y_vector_mag, z_vector_mag)*scale_factor;

// For Helicorder:
// Serial Studio
//    String msg = "/*"+ String(ax) + "," + String(ay) +"," + String(az) + ","+ String(pga,4).c_str() +"*/"; 
// Jamasesis - Select ASCII 4
//    String msg = String(ax) + " , " + String(ay) +" , " + String(az) + " , " + String(int(pga*10000)).c_str() ;
//    Serial.println(msg);
    char pga_line[64];
    snprintf(pga_line, sizeof(pga_line), "PGA,%lu,%d,%d,%d,%.5f,%.5f", millis(), ax, ay, az, pga, pga_raw);
#ifdef PGA_STREAM
    Serial.println(pga_line);
#endif
    udplog_write(pga_line, true);

    if ((continuous_graph) || (not continuous_graph && eq_status)){
      if (slta) {
        lcd.draw_slta_graph(staLta.getSTA(), staLta.getLTA(), pga);
      } else {
        lcd.draw_acc_graph(x_vector_mag,y_vector_mag,z_vector_mag);
      }
    } 
//    if ((continuous_graph) || (not continuous_graph && eq_status)) lcd.draw_pga_graph(pga);

    if (slta) {
      staLta.updateData(pga);
      lcd.show_stalta(staLta.getSTA(),staLta.getLTA());
      if (staLta.checkTrigger()) {
        if ((master && slave_eq_state) || !master) {
          eq_happening();
        }
      }
    } else {
      lcd.show_pga(pga);
      uint8_t level = shaking_level(pga);
      if (level != SEISMO_LEVEL_NONE) {
        if ((master && slave_eq_state) || !master) {
          eq_happening(level);
        }
      }
    }

    // A real quake keeps changing; a reading stuck above the trigger without
    // moving means the stick was tilted or shifted. Recalibrate instead of
    // alarming forever.
    if (!eq_status || fabs(pga - tilt_ref_pga) > TILT_STEADY_BAND) {
      tilt_ref_pga = pga;
      tilt_steady_since = millis();
    } else if (millis() - tilt_steady_since > TILT_STUCK_MS) {
      logln("[tilt] steady " + String(pga, 4) + " g for " + String(TILT_STUCK_MS / 1000) + " s, recalibrating");
      notify_tilt_recal(pga, pga_trigger);
      events_end(eq_level, eq_pga_peak, (millis() - eq_started_ms) / 1000.0f, true);
      eq_status = false;
      eq_level = SEISMO_LEVEL_NONE;
      eq_time_count = 0;
      digitalWrite(M5_LED, HIGH);
      if (SPK_HAT) ledcWriteTone(spkChannel, 0);
      lcd.set_brightness(lcd_brightness);
      tilt_steady_since = millis();
      calibrating = true;
    }

    if (eq_status) { // Earthquake still happening
      eq_time_count++ ;

      if (((slta && !staLta.checkTrigger()) || (!slta && eq_time_count > eq_pet)) || (master && !slave_eq_state)) { // EQ Ends...              
          eq_status = false;
          eq_time_count = 0;

          updateState("LISTENING");
          notify_eq_end(pga, pga_trigger, eq_level, (millis() - eq_started_ms) / 1000.0f);
          events_end(eq_level, eq_pga_peak, (millis() - eq_started_ms) / 1000.0f);
          eq_level = SEISMO_LEVEL_NONE;

          publish_mqtt(MQTT_PUB_EVENT, updateEvent(ax,ay,az,pga).c_str(), 0, true);

          digitalWrite(M5_LED, HIGH);
          if (SPK_HAT) ledcWriteTone(spkChannel, 0);
          lcd.set_brightness(lcd_brightness);
      } else {
        if (eq_time_count%2) publish_mqtt(MQTT_PUB_EVENT, updateEvent(ax,ay,az,pga).c_str(), 0, true);
      }
    } else {
        if (flush_update) { // Send periodic event update message
        flush_event++;
        if (flush_event > (flush_period*10)) {
          publish_mqtt(MQTT_PUB_EVENT, updateEvent(ax,ay,az,pga).c_str(), 0, true);
          flush_event = 0;
        }
      }
    }

    if (M5.BtnA.wasPressed()) {
      if (mqttClient.connected()) {
        updateState("MQTT_BUTTON_DISCONNECT");
        MQTT_active = false;
        mqttClient.disconnect();
      } else {
        updateState("MQTT_BUTTON_CONNECT");
        MQTT_active = true;
      }
    }

    if (M5.BtnB.wasPressed()) {
      ESP.restart();
    }

    delay(100);    
  } else {
    calibrate_MPU();
  }
}


