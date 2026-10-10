#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "notifier_config.h"
#include "espnow_msg.h"

// ---- Sensor state (written from the ESP-NOW callback) ----
QueueHandle_t sensor_queue = NULL;  // ESP-NOW messages, handled in order
unsigned long sensor_last_seen = 0;
float sensor_pga = 0;
float sensor_trigger = 0;
float sensor_peak = 0;
bool sensor_eq = false;
unsigned long sensor_eq_seen = 0;  // last START while in an event
uint8_t sensor_eq_level = SEISMO_LEVEL_NONE;  // level of the event in progress
uint8_t last_event_level = SEISMO_LEVEL_NONE;  // of the last local event
String last_event_time = "";

// ---- Alarm ----
// Red: strong shaking, beeps (unless muted). Blue: weak sustained shaking, screen + LED only.
enum AlarmKind { ALARM_NONE, ALARM_BLUE, ALARM_RED };
AlarmKind alarm_kind = ALARM_NONE;
unsigned long alarm_started = 0;
bool muted = false;  // long press A; saved across reboots
Preferences prefs;

// ---- Earthquake feeds (USGS + EMSC) ----
enum Feed { FEED_USGS = 0, FEED_EMSC = 1 };
const char* FEED_NAME[] = {"USGS", "EMSC"};
bool feed_ok[2] = {false, false};
bool feed_initialized[2] = {false, false};  // first poll only sets the baseline

// Recently seen events, so the same quake from both feeds alerts once
struct SeenQuake { int64_t t_ms; float lat, lon; };
#define SEEN_MAX 64
SeenQuake seen[SEEN_MAX];
int seen_count = 0, seen_next = 0;

float last_mag = 0;
String last_place = "";
String last_source = "";
int64_t last_time_ms = 0;

bool screen_dirty = true;
volatile bool usgs_new_event = false;  // highlight the USGS line until a button press

String agoText(int64_t t_ms);
void pollFeeds();
void drawScreenLocked();
SemaphoreHandle_t data_mutex;  // feed task writes, display reads

void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != sizeof(SeismoMsg)) return;
  SeismoMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != SEISMO_MSG_MAGIC) return;
  if (sensor_queue) xQueueSend(sensor_queue, &m, 0);
}

// One worker sends pushes one at a time (parallel TLS runs out of memory)
struct NtfyMsg {
  char title[32];
  char priority[10];
  char tags[24];
  char body[160];
};
QueueHandle_t ntfy_queue = NULL;

void ntfy_worker(void* arg) {
  NtfyMsg m;
  for (;;) {
    if (xQueueReceive(ntfy_queue, &m, portMAX_DELAY) != pdTRUE) continue;
    for (int attempt = 1; attempt <= 3; attempt++) {
      int code = -1;
      if (WiFi.isConnected()) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        if (http.begin(client, "https://ntfy.sh/" NTFY_TOPIC)) {
          http.addHeader("Title", m.title);
          http.addHeader("Priority", m.priority);
          http.addHeader("Tags", m.tags);
          code = http.POST((uint8_t*)m.body, strlen(m.body));
          http.end();
        }
      }
      Serial.printf("[ntfy] attempt %d HTTP %d: %s\n", attempt, code, m.body);
      if (code >= 200 && code < 300) break;
      vTaskDelay(pdMS_TO_TICKS(3000 * attempt));
    }
  }
}

void ntfy_push(const char* title, const char* priority, const char* tags, const String& body) {
  if (!ntfy_queue) return;
  NtfyMsg m;
  strlcpy(m.title, title, sizeof(m.title));
  strlcpy(m.priority, priority, sizeof(m.priority));
  strlcpy(m.tags, tags, sizeof(m.tags));
  strlcpy(m.body, body.c_str(), sizeof(m.body));
  if (xQueueSend(ntfy_queue, &m, 0) != pdTRUE) Serial.println("[ntfy] queue full");
}

void ntfy_send(const String& text) {
  if (NTFY_USGS) ntfy_push("USGS - Panama", "default", "earth_americas", text);
}

// ---- Power (AXP192) ----
bool on_usb = true;            // debounced state
bool on_usb_raw = true;
unsigned long usb_change_at = 0;
float bat_v = 0;
int bat_pct = 0;
bool low_bat_sent = false;
bool charging = false;
float charge_ma = 0;

int batteryPercent(float v) {
  int pct = (int)((v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f);
  return constrain(pct, 0, 100);
}

void updatePower() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  bat_v = M5.Axp.GetBatVoltage();
  bat_pct = batteryPercent(bat_v);
  bool usb = M5.Axp.GetVBusVoltage() > 4.0f;
  charge_ma = M5.Axp.GetBatChargeCurrent();
  bool was_charging = charging;
  charging = usb && charge_ma > CHARGING_MIN_MA;
  if (charging != was_charging) screen_dirty = true;
  static int n = 0;
  if (n++ % 30 == 0)
    Serial.printf("[power] usb=%d bat=%.2fV %d%% charge=%.0fmA\n", usb, bat_v, bat_pct, charge_ma);
  if (usb != on_usb_raw) {
    on_usb_raw = usb;
    usb_change_at = millis();
  }
  // Only act once the state has held, so a wiggly cable doesn't spam the phone
  if (on_usb_raw != on_usb && millis() - usb_change_at > POWER_DEBOUNCE_MS) {
    on_usb = on_usb_raw;
    if (on_usb) {
      low_bat_sent = false;
      ntfy_push("Notificador", "default", "electric_plug", "Notificador conectado a USB otra vez (" + String(bat_pct) + "%)");
    } else {
      ntfy_push("Notificador", "high", "battery", "Notificador sin USB, en bateria (" + String(bat_pct) + "%, ~1 h)");
    }
    screen_dirty = true;
  }
  if (!on_usb && !low_bat_sent && bat_pct <= LOW_BAT_PCT) {
    low_bat_sent = true;
    ntfy_push("Notificador", "high", "warning", "Bateria baja del notificador: " + String(bat_pct) + "%");
  }
}

void startAlarm(AlarmKind kind) {
  alarm_kind = kind;
  alarm_started = millis();
  M5.Axp.ScreenBreath(100);
  screen_dirty = true;
}

void stopAlarm() {
  alarm_kind = ALARM_NONE;
  M5.Beep.mute();
  digitalWrite(M5_LED, HIGH);
  M5.Axp.ScreenBreath(40);
  screen_dirty = true;
}

// Red: fast beep-beep (150 ms on/off) and LED. Blue: slow LED blink with a soft chime.
void updateAlarm() {
  if (alarm_kind == ALARM_NONE) return;
  unsigned long t = millis() - alarm_started;
  if (t > ALARM_MAX_MS) { stopAlarm(); return; }
  if (alarm_kind == ALARM_RED) {
    bool on = (t / 150) % 2 == 0;
    if (on && !muted) M5.Beep.tone(3000); else M5.Beep.mute();
    digitalWrite(M5_LED, on ? LOW : HIGH);
  } else {
    digitalWrite(M5_LED, (t % 2000) < 300 ? LOW : HIGH);
    // "tu tu tu tu" once at the start: four short beeps, 80 ms on / 80 ms off
    static int note_playing = -1;
    int slot = t / 160;
    int note = (slot < 4 && t % 160 < 80 && !muted) ? slot : -1;
    if (note != note_playing) {
      note_playing = note;
      if (note < 0) {
        M5.Beep.mute();
      } else {
        M5.Beep.tone(1568);                       // G6
        ledcWrite(TONE_PIN_CHANNEL, CHIME_DUTY);
      }
    }
  }
}

String clockText() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm t;
  localtime_r(&now, &t);
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return buf;
}

void handleSensorMsg() {
  SeismoMsg m;
  while (sensor_queue && xQueueReceive(sensor_queue, &m, 0) == pdTRUE) {
  sensor_last_seen = millis();
  static uint32_t rx_count = 0;
  rx_count++;
  if (m.type != SEISMO_HEARTBEAT || rx_count % 6 == 1)
    Serial.printf("[espnow] rx #%u type=%u pga=%.4f\n", rx_count, m.type, m.pga);
  sensor_pga = m.pga;
  sensor_trigger = m.pga_trigger;
  if (m.type == SEISMO_EQ_START) {  // also sent when an event goes from blue to red
    // The sensor repeats START every second during an event; act on new or higher levels only
    sensor_eq_seen = millis();
    sensor_peak = m.pga_peak;
    if (!sensor_eq || m.level > sensor_eq_level) {
      Serial.printf("[sensor] EQ START level=%u pga=%.4f\n", m.level, m.pga);
      if (!sensor_eq) last_event_time = clockText();
      sensor_eq = true;
      sensor_eq_level = m.level;
      last_event_level = m.level;
      if (m.level >= SEISMO_LEVEL_STRONG) {
        if (alarm_kind != ALARM_RED) startAlarm(ALARM_RED);
      } else if (alarm_kind == ALARM_NONE) {
        startAlarm(ALARM_BLUE);
      }
    }
  } else if (m.type == SEISMO_EQ_END) {
    static unsigned long last_end = 0;
    if (!sensor_eq && last_end && millis() - last_end < 10000) continue;  // repeated END
    last_end = millis();
    Serial.printf("[sensor] EQ END level=%u peak=%.4f\n", m.level, m.pga_peak);
    if (!sensor_eq && m.level > SEISMO_LEVEL_NONE) {  // START was lost
      last_event_time = clockText();
      last_event_level = m.level;
    }
    sensor_eq = false;
    sensor_eq_level = SEISMO_LEVEL_NONE;
    sensor_peak = m.pga_peak;
    if (m.level > last_event_level) last_event_level = m.level;
    if (alarm_kind != ALARM_NONE) stopAlarm();
  }
  screen_dirty = true;
  }
  if (sensor_eq && millis() - sensor_eq_seen > 5000) {  // END lost: the repeats stopped
    sensor_eq = false;
    sensor_eq_level = SEISMO_LEVEL_NONE;
    if (alarm_kind != ALARM_NONE) stopAlarm();
    screen_dirty = true;
  }
  // The first NTP request can go out before WiFi is up; ask again until the clock is set
  static unsigned long ntp_retry = 0;
  if (time(nullptr) < 1700000000 && WiFi.isConnected() && millis() - ntp_retry > 30000) {
    ntp_retry = millis();
    configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.nist.gov");
  }
}

bool inRegion(float lat, float lon) {
  return lat >= REGION_MIN_LAT && lat <= REGION_MAX_LAT &&
         lon >= REGION_MIN_LON && lon <= REGION_MAX_LON;
}

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f, D = PI / 180.0f;
  float dlat = (lat2 - lat1) * D, dlon = (lon2 - lon1) * D;
  float h = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * D) * cos(lat2 * D) * sin(dlon / 2) * sin(dlon / 2);
  return 2 * R * asin(sqrt(h));
}

bool alreadySeen(int64_t t_ms, float lat, float lon) {
  for (int i = 0; i < seen_count; i++) {
    if (llabs(seen[i].t_ms - t_ms) <= DEDUP_SECONDS * 1000LL &&
        distanceKm(seen[i].lat, seen[i].lon, lat, lon) <= DEDUP_KM) return true;
  }
  return false;
}

void rememberQuake(int64_t t_ms, float lat, float lon) {
  seen[seen_next] = {t_ms, lat, lon};
  seen_next = (seen_next + 1) % SEEN_MAX;
  if (seen_count < SEEN_MAX) seen_count++;
}

// Every event from either feed goes through here (caller holds data_mutex)
void handleQuake(Feed feed, int64_t t_ms, float lat, float lon, float mag, const String& place) {
  if (!inRegion(lat, lon)) return;
  if (alreadySeen(t_ms, lat, lon)) return;  // already reported by this or the other feed
  rememberQuake(t_ms, lat, lon);
  if (t_ms > last_time_ms) {
    last_time_ms = t_ms;
    last_mag = mag;
    last_place = place;
    last_source = FEED_NAME[feed];
    screen_dirty = true;
  }
  if (!feed_initialized[feed]) return;  // baseline: events from before boot don't alert
  time_t now = time(nullptr);
  if (now > 1700000000 && (int64_t)now * 1000 - t_ms > MAX_ALERT_AGE_S * 1000LL) return;
  Serial.printf("[%s] new M%.1f %s\n", FEED_NAME[feed], mag, place.c_str());
  if (mag >= QUAKE_MIN_MAG) {
    usgs_new_event = true;  // screen only, no beep: catalogs lag the shaking by minutes
    ntfy_send("M" + String(mag, 1) + " - " + place + " (" + agoText(t_ms) + ", " + FEED_NAME[feed] + ")");
  }
}

// Open an HTTPS GET; returns the HTTP code (stream ready on 200)
int httpsGet(HTTPClient& http, WiFiClientSecure& client, const String& url) {
  client.setInsecure();
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be parsed as a stream
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -1;
  return http.GET();
}

void pollUsgs() {
  WiFiClientSecure client;
  HTTPClient http;
  int code = httpsGet(http, client, USGS_FEED);
  if (code != 200) {
    Serial.printf("[USGS] HTTP %d\n", code);
    feed_ok[FEED_USGS] = false;
    http.end();
    return;
  }
  StaticJsonDocument<256> filter;
  JsonObject f = filter["features"].createNestedObject();
  f["properties"]["mag"] = true;
  f["properties"]["place"] = true;
  f["properties"]["time"] = true;
  f["geometry"]["coordinates"] = true;
  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[USGS] JSON error: %s\n", err.c_str());
    feed_ok[FEED_USGS] = false;
    return;
  }
  feed_ok[FEED_USGS] = true;
  JsonArray features = doc["features"].as<JsonArray>();
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  for (int i = features.size() - 1; i >= 0; i--) {  // oldest first
    JsonObject p = features[i]["properties"];
    JsonArray c = features[i]["geometry"]["coordinates"];
    handleQuake(FEED_USGS, p["time"].as<int64_t>(), c[1], c[0], p["mag"] | 0.0f, p["place"].as<String>());
  }
  feed_initialized[FEED_USGS] = true;
  xSemaphoreGive(data_mutex);
  Serial.printf("[USGS] ok, %d events\n", (int)features.size());
}

// days_from_civil (Howard Hinnant): UTC calendar date -> days since 1970-01-01
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// "2026-10-10T06:04:20.5Z" -> epoch ms (UTC)
int64_t parseIsoUtcMs(const char* s) {
  int y, mo, d, h, mi;
  float sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%fZ", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  int64_t days = daysFromCivil(y, mo, d);
  return ((days * 86400 + h * 3600 + mi * 60) * 1000) + (int64_t)(sec * 1000);
}

void pollEmsc() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;  // need NTP time for the query window
  time_t start = now - EMSC_WINDOW_S;
  struct tm t;
  gmtime_r(&start, &t);
  char start_iso[24];
  strftime(start_iso, sizeof(start_iso), "%Y-%m-%dT%H:%M:%S", &t);
  String url = String(EMSC_QUERY) + "&start=" + start_iso +
               "&minlat=" + String(REGION_MIN_LAT, 1) + "&maxlat=" + String(REGION_MAX_LAT, 1) +
               "&minlon=" + String(REGION_MIN_LON, 1) + "&maxlon=" + String(REGION_MAX_LON, 1);
  WiFiClientSecure client;
  HTTPClient http;
  int code = httpsGet(http, client, url);
  if (code == 204) {  // FDSN: no events in the window
    http.end();
    feed_ok[FEED_EMSC] = true;
    feed_initialized[FEED_EMSC] = true;
    Serial.println("[EMSC] ok, 0 events");
    return;
  }
  if (code != 200) {
    Serial.printf("[EMSC] HTTP %d\n", code);
    feed_ok[FEED_EMSC] = false;
    http.end();
    return;
  }
  StaticJsonDocument<256> filter;
  JsonObject f = filter["features"].createNestedObject();
  f["properties"]["mag"] = true;
  f["properties"]["flynn_region"] = true;
  f["properties"]["time"] = true;
  f["properties"]["lat"] = true;
  f["properties"]["lon"] = true;
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[EMSC] JSON error: %s\n", err.c_str());
    feed_ok[FEED_EMSC] = false;
    return;
  }
  feed_ok[FEED_EMSC] = true;
  JsonArray features = doc["features"].as<JsonArray>();
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  for (int i = features.size() - 1; i >= 0; i--) {  // newest first in the response
    JsonObject p = features[i]["properties"];
    String region = p["flynn_region"].as<String>();
    region.toLowerCase();
    if (region.length()) region[0] = toupper(region[0]);
    handleQuake(FEED_EMSC, parseIsoUtcMs(p["time"]), p["lat"], p["lon"], p["mag"] | 0.0f, region);
  }
  feed_initialized[FEED_EMSC] = true;
  xSemaphoreGive(data_mutex);
  Serial.printf("[EMSC] ok, %d events\n", (int)features.size());
}

void pollFeeds() {
  if (!WiFi.isConnected()) return;
  pollEmsc();  // usually publishes first
  pollUsgs();
  screen_dirty = true;
}

String agoText(int64_t t_ms) {
  time_t now = time(nullptr);
  if (now < 1700000000 || t_ms == 0) return "";
  long mins = (long)(now - t_ms / 1000) / 60;
  if (mins < 1) return "ahora";
  if (mins < 60) return "hace " + String(mins) + " min";
  return "hace " + String(mins / 60) + " h " + String(mins % 60) + " m";
}

void drawScreen() {
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  drawScreenLocked();
  xSemaphoreGive(data_mutex);
}

void drawScreenLocked() {
  bool sensor_online = sensor_last_seen && (millis() - sensor_last_seen < SENSOR_TIMEOUT_MS);
  uint16_t bg = (alarm_kind == ALARM_RED) ? RED : (alarm_kind == ALARM_BLUE ? BLUE : BLACK);
  M5.Lcd.fillScreen(bg);
  M5.Lcd.setTextColor(WHITE, bg);
  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextSize(1);

  M5.Lcd.setCursor(4, 2);
  // ESP-NOW only hears the sensor on the same channel: show it to compare with the sensor's /events page
  if (WiFi.isConnected()) M5.Lcd.printf("WiFi ch%d", WiFi.channel()); else M5.Lcd.print("WiFi --");
  if (muted) {
    M5.Lcd.setCursor(82, 2);
    M5.Lcd.setTextColor(YELLOW, bg);
    M5.Lcd.print("MUTE");
    M5.Lcd.setTextColor(WHITE, bg);
  }
  M5.Lcd.setCursor(150, 2);
  if (on_usb && charging) {
    M5.Lcd.setTextColor(GREEN, bg);
    M5.Lcd.printf("CARGA %d%%", bat_pct);
    M5.Lcd.setTextColor(WHITE, bg);
  } else if (on_usb) {
    M5.Lcd.printf("USB %d%%", bat_pct);  // plugged in, not charging (full)
  } else {
    M5.Lcd.setTextColor(bat_pct <= LOW_BAT_PCT ? RED : YELLOW, bg);
    M5.Lcd.printf("BAT %d%%", bat_pct);
    M5.Lcd.setTextColor(WHITE, bg);
  }

  if (alarm_kind != ALARM_NONE) {
    bool red = alarm_kind == ALARM_RED;
    M5.Lcd.setTextFont(4);
    M5.Lcd.setCursor(12, 36);
    M5.Lcd.print(red ? "SISMO FUERTE!" : "Posible replica");
    M5.Lcd.setTextFont(2);
    M5.Lcd.setCursor(12, 70);
    M5.Lcd.print(red ? "Vibracion fuerte sostenida" : "Debil: replica o alguien cerca");
    M5.Lcd.setCursor(12, 90);
    M5.Lcd.printf("PGA %.4f g  pico %.4f g", sensor_pga, sensor_peak);
    M5.Lcd.setCursor(12, 112);
    M5.Lcd.print("[A] cerrar");
    return;
  }

  M5.Lcd.setCursor(4, 26);
  if (sensor_online) {
    M5.Lcd.setTextColor(GREEN, bg);
    M5.Lcd.printf("Sensor OK  PGA %.4f", sensor_pga);
  } else {
    M5.Lcd.setTextColor(YELLOW, bg);
    M5.Lcd.print("Sensor sin senal");
  }
  M5.Lcd.setTextColor(WHITE, bg);
  if (sensor_peak > 0) {
    M5.Lcd.setCursor(4, 44);
    M5.Lcd.print("Local " + last_event_time + ": ");
    M5.Lcd.setTextColor(last_event_level >= SEISMO_LEVEL_STRONG ? RED : CYAN, bg);
    M5.Lcd.printf("%.4f g %s", sensor_peak, last_event_level >= SEISMO_LEVEL_STRONG ? "rojo" : "azul");
    M5.Lcd.setTextColor(WHITE, bg);
  }

  M5.Lcd.setCursor(4, 66);
  bool any_feed = feed_ok[FEED_USGS] || feed_ok[FEED_EMSC];
  M5.Lcd.setTextColor(any_feed ? CYAN : YELLOW, bg);
  if (!any_feed) M5.Lcd.print("Feeds: sin conexion");
  else if (last_time_ms) M5.Lcd.print("Ultimo sismo (" + last_source + "):");
  else M5.Lcd.print("USGS + EMSC:");
  if (usgs_new_event) {
    M5.Lcd.setTextColor(ORANGE, bg);
    M5.Lcd.print(" NUEVO");
  }
  M5.Lcd.setTextColor(WHITE, bg);
  if (last_time_ms) {
    M5.Lcd.setTextFont(4);
    M5.Lcd.setCursor(4, 84);
    if (last_mag >= 5.0) M5.Lcd.setTextColor(RED, bg);
    M5.Lcd.printf("M%.1f", last_mag);
    M5.Lcd.setTextColor(WHITE, bg);
    M5.Lcd.setTextFont(2);
    M5.Lcd.setCursor(80, 90);
    M5.Lcd.print(agoText(last_time_ms));
    M5.Lcd.setCursor(4, 114);
    M5.Lcd.print(last_place.substring(0, 34));
  } else if (any_feed) {
    M5.Lcd.setCursor(4, 88);
    M5.Lcd.print("Sin sismos en la region");
  }
}

void feeds_task(void* arg) {
  for (;;) {
    pollFeeds();
    vTaskDelay(pdMS_TO_TICKS(FEED_POLL_MS));
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(40);
  pinMode(M5_LED, OUTPUT);
  digitalWrite(M5_LED, HIGH);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextFont(2);
  M5.Lcd.setCursor(4, 4);
  M5.Lcd.print("Conectando WiFi...");
  prefs.begin("notifier", false);
  muted = prefs.getBool("muted", false);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("seismoNotifier");
  WiFi.setSleep(false);  // modem sleep drops ESP-NOW broadcasts between beacons
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(200);
  Serial.printf("WiFi %s, channel %d\n", WiFi.isConnected() ? "connected" : "FAILED", WiFi.channel());

  sensor_queue = xQueueCreate(16, sizeof(SeismoMsg));
  ntfy_queue = xQueueCreate(8, sizeof(NtfyMsg));
  xTaskCreatePinnedToCore(ntfy_worker, "ntfy", 8192, NULL, 1, NULL, 0);
  data_mutex = xSemaphoreCreateMutex();

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[ESP-NOW] listening");
  } else {
    Serial.println("[ESP-NOW] init failed");
  }

  configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.nist.gov");

  // Startup chirp so you know the buzzer works
  if (!muted) {
    M5.Beep.tone(2500);
    delay(80);
    M5.Beep.mute();
  }

  ArduinoOTA.setHostname("seismoNotifier");
  ArduinoOTA.onStart([]() {
    M5.Lcd.fillScreen(BLUE);
    M5.Lcd.setCursor(4, 50);
    M5.Lcd.print("Actualizando firmware...");
  });
  ArduinoOTA.begin();

  xTaskCreatePinnedToCore(feeds_task, "feeds", 12288, NULL, 1, NULL, 0);
}

// With several access points the notifier can join one on a different channel from the
// sensor's, and ESP-NOW then hears nothing. After a minute without the sensor, move to the
// strongest access point of the same network that is on another channel.
void findSensorChannel() {
  static unsigned long last_hop = 0;
  if (!WiFi.isConnected()) return;
  unsigned long quiet = millis() - sensor_last_seen;  // since boot when never seen
  if (quiet < 60000 || millis() - last_hop < 60000) return;
  last_hop = millis();
  int n = WiFi.scanNetworks();
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != WIFI_SSID || WiFi.channel(i) == WiFi.channel()) continue;
    if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
  }
  if (best >= 0) {
    Serial.printf("[wifi] no sensor on ch%d, trying %s ch%d\n", WiFi.channel(), WiFi.BSSIDstr(best).c_str(), WiFi.channel(best));
    WiFi.begin(WIFI_SSID, WIFI_PASS, WiFi.channel(best), WiFi.BSSID(best));
    screen_dirty = true;
  }
  WiFi.scanDelete();
}

void loop() {
  ArduinoOTA.handle();
  M5.update();
  handleSensorMsg();
  updatePower();
  updateAlarm();
  findSensorChannel();

  if (M5.BtnA.wasReleasefor(1000)) {  // long press: mute / unmute
    muted = !muted;
    prefs.putBool("muted", muted);
    M5.Beep.mute();
    if (!muted) {  // short chirp: sound is back
      M5.Beep.tone(2500);
      delay(80);
      M5.Beep.mute();
    }
    Serial.printf("[button] muted=%d\n", muted);
    screen_dirty = true;
  } else if (M5.BtnA.wasReleased()) {
    if (alarm_kind != ALARM_NONE) stopAlarm();
    usgs_new_event = false;
    screen_dirty = true;
  }
  if (M5.BtnB.wasPressed()) {  // 3 s test of the red alarm (silent when muted)
    startAlarm(ALARM_RED);
    alarm_started = millis() - (ALARM_MAX_MS - 3000);
  }

  if (!WiFi.isConnected()) {
    static unsigned long last_try = 0;
    if (millis() - last_try > 10000) {
      last_try = millis();
      WiFi.reconnect();
    }
  }


  static unsigned long last_draw = 0;
  if (screen_dirty || millis() - last_draw > 5000) {
    if (alarm_kind == ALARM_NONE || screen_dirty) drawScreen();
    screen_dirty = false;
    last_draw = millis();
  }
  delay(20);
}
