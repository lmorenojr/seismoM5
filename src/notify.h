#ifndef _notify_H_
#define _notify_H_

#include "config.h"
#include "util.h"
#include "espnow_msg.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>

static const uint8_t espnow_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool espnow_ready = false;
unsigned long espnow_last_heartbeat = 0;
// Broadcasts have no delivery receipt: repeat the event state so one lost packet can't hide it
uint8_t espnow_eq_level = SEISMO_LEVEL_NONE;  // level of the event in progress, NONE when idle
uint8_t espnow_end_repeats = 0;
uint8_t espnow_end_level = SEISMO_LEVEL_NONE;
float espnow_trigger = 0;
float eq_pga_peak = 0;

void espnow_init() {
  if (!ESPNOW_ENABLE || espnow_ready) return;
  if (esp_now_init() != ESP_OK) {
    logln("[ESP-NOW] init failed");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, espnow_broadcast, 6);
  peer.channel = 0;          // follow the WiFi STA channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    logln("[ESP-NOW] add peer failed");
    return;
  }
  espnow_ready = true;
  logln("[ESP-NOW] ready");
}

void espnow_send(SeismoMsgType type, float pga, float pga_peak, float trigger, uint8_t level = SEISMO_LEVEL_NONE) {
  if (!espnow_ready) return;
  SeismoMsg msg = {SEISMO_MSG_MAGIC, type, pga, pga_peak, trigger, (uint32_t)(millis() / 1000), level};
  esp_now_send(espnow_broadcast, (const uint8_t*)&msg, sizeof(msg));
}

// One worker task sends queued messages one at a time: two HTTPS requests
// at once run the ESP32 out of memory for TLS
#define NTFY_QUEUE_LEN 8
#define NTFY_MSG_MAX 160
#define NTFY_RETRIES 3
QueueHandle_t ntfy_queue = NULL;

struct NtfyItem {
  char title[32];
  char priority[10];
  char tags[32];
  char body[NTFY_MSG_MAX];
};

void ntfy_worker(void* arg) {
  NtfyItem m;
  for (;;) {
    if (xQueueReceive(ntfy_queue, &m, portMAX_DELAY) != pdTRUE) continue;
    for (int attempt = 1; attempt <= NTFY_RETRIES; attempt++) {
      if (!WiFi.isConnected()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      int code = -1;
      if (http.begin(client, "https://ntfy.sh/" NTFY_TOPIC)) {
        http.addHeader("Title", m.title);
        http.addHeader("Priority", m.priority);
        http.addHeader("Tags", m.tags);
        code = http.POST((uint8_t*)m.body, strlen(m.body));
        http.end();
      }
      logln("[ntfy] attempt " + String(attempt) + ", HTTP " + String(code) + ", heap " + String(ESP.getFreeHeap()));
      if (code >= 200 && code < 300) break;
      vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
    }
  }
}

void ntfy_send(const String& text, const char* title = "SeismoM5", const char* priority = "urgent", const char* tags = "warning") {
  if (!NTFY_ENABLE) return;
  if (ntfy_queue == NULL) {
    ntfy_queue = xQueueCreate(NTFY_QUEUE_LEN, sizeof(NtfyItem));
    xTaskCreate(ntfy_worker, "ntfy", 8192, NULL, 1, NULL);
  }
  NtfyItem m;
  strlcpy(m.title, title, sizeof(m.title));
  strlcpy(m.priority, priority, sizeof(m.priority));
  strlcpy(m.tags, tags, sizeof(m.tags));
  strlcpy(m.body, text.c_str(), sizeof(m.body));
  if (xQueueSend(ntfy_queue, &m, 0) != pdTRUE) logln("[ntfy] queue full, dropped");
}

// Called when an event starts and again whenever its level goes up
void notify_eq_start(float pga, float trigger, uint8_t level, bool escalated) {
  if (!escalated) eq_pga_peak = pga;
  else if (pga > eq_pga_peak) eq_pga_peak = pga;
  espnow_send(SEISMO_EQ_START, pga, eq_pga_peak, trigger, level);
  espnow_eq_level = level;
  espnow_end_repeats = 0;
  espnow_trigger = trigger;
  if (level >= SEISMO_LEVEL_STRONG)
    ntfy_send("Sismo FUERTE! PGA " + String(pga, 4) + " g sostenido sobre " + String(trigger, 3) + " g",
              "SeismoM5 - FUERTE", "urgent", "red_circle,warning");
  else
    ntfy_send("Vibracion debil sostenida (posible replica, o movimiento en casa). PGA " + String(pga, 4) + " g",
              "SeismoM5 - posible replica", "default", "large_blue_circle");
}

void notify_eq_update(float pga) {
  if (pga > eq_pga_peak) eq_pga_peak = pga;
}

void notify_eq_end(float pga, float trigger, uint8_t level, float duration_s) {
  espnow_send(SEISMO_EQ_END, pga, eq_pga_peak, trigger, level);
  espnow_eq_level = SEISMO_LEVEL_NONE;
  espnow_end_level = level;
  espnow_end_repeats = 3;
  logln("[event] end level=" + String(level) + " peak=" + String(eq_pga_peak, 4) + " dur=" + String(duration_s, 1));
  if (level >= SEISMO_LEVEL_STRONG)  // weak events get one push only
    ntfy_send("Sismo terminado. PGA pico " + String(eq_pga_peak, 4) + " g, " + String(duration_s, 0) + " s",
              "SeismoM5 - FUERTE", "high", "red_circle");
}

// The sensor was moved: cancel the alarm everywhere and say why
void notify_tilt_recal(float pga, float trigger) {
  espnow_send(SEISMO_EQ_END, pga, eq_pga_peak, trigger);
  espnow_eq_level = SEISMO_LEVEL_NONE;
  espnow_end_level = SEISMO_LEVEL_NONE;
  espnow_end_repeats = 3;
  ntfy_send("Sensor movido (lectura fija " + String(pga, 4) + " g). Alarma cancelada, recalibrando...");
}

void notify_loop(float pga, float trigger) {
  static unsigned long last_repeat = 0;
  if (millis() - last_repeat >= 1000) {
    last_repeat = millis();
    if (espnow_eq_level != SEISMO_LEVEL_NONE) {
      espnow_send(SEISMO_EQ_START, pga, eq_pga_peak, espnow_trigger, espnow_eq_level);
    } else if (espnow_end_repeats) {
      espnow_end_repeats--;
      espnow_send(SEISMO_EQ_END, pga, eq_pga_peak, espnow_trigger, espnow_end_level);
    }
  }
  if (millis() - espnow_last_heartbeat >= ESPNOW_HEARTBEAT_MS) {
    espnow_last_heartbeat = millis();
    espnow_send(SEISMO_HEARTBEAT, pga, 0, trigger);
  }
}

#endif
