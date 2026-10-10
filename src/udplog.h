#ifndef _udplog_H_
#define _udplog_H_

#include "config.h"
#include "util.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>

// Lines are batched and sent every 500 ms. The computer is found by its mDNS
// name; until then (or if it never resolves) packets go to the LAN broadcast.
static WiFiUDP udplog_udp;
static SemaphoreHandle_t udplog_mutex = NULL;
static char udplog_buf[1400];
static size_t udplog_len = 0;
static unsigned long udplog_last_send = 0;
static IPAddress udplog_ip(255, 255, 255, 255);
static bool udplog_resolver_started = false;

static void udplog_flush_locked() {
  if (udplog_len == 0) return;
  if (WiFi.isConnected()) {
    udplog_udp.beginPacket(udplog_ip, UDP_LOG_PORT);
    udplog_udp.write((const uint8_t*)udplog_buf, udplog_len);
    udplog_udp.endPacket();
  }
  udplog_len = 0;
  udplog_last_send = millis();
}

static void udplog_resolver(void* arg) {
  vTaskDelay(pdMS_TO_TICKS(30000));  // let WiFi and mDNS (started by OTA) come up
  for (;;) {
    bool found = false;
    if (WiFi.isConnected()) {
      IPAddress ip = MDNS.queryHost(UDP_LOG_HOST, 2000);
      if (ip != IPAddress(0, 0, 0, 0)) {
        xSemaphoreTake(udplog_mutex, portMAX_DELAY);
        udplog_ip = ip;
        xSemaphoreGive(udplog_mutex);
        found = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(found ? 300000 : 15000));
  }
}

void udplog_write(const String& text, bool newline) {
  if (!UDP_LOG_ENABLE) return;
  if (udplog_mutex == NULL) udplog_mutex = xSemaphoreCreateMutex();
  xSemaphoreTake(udplog_mutex, portMAX_DELAY);
  size_t n = text.length();
  if (udplog_len + n + 1 > sizeof(udplog_buf)) udplog_flush_locked();
  if (n + 1 <= sizeof(udplog_buf)) {
    memcpy(udplog_buf + udplog_len, text.c_str(), n);
    udplog_len += n;
    if (newline) udplog_buf[udplog_len++] = '\n';
  }
  xSemaphoreGive(udplog_mutex);
}

void udplog_loop() {
  if (!UDP_LOG_ENABLE || udplog_mutex == NULL) return;
  if (!udplog_resolver_started && WiFi.isConnected()) {
    udplog_resolver_started = true;
    xTaskCreate(udplog_resolver, "udplog", 4096, NULL, 1, NULL);
  }
  if (millis() - udplog_last_send < 500) return;
  xSemaphoreTake(udplog_mutex, portMAX_DELAY);
  udplog_flush_locked();
  xSemaphoreGive(udplog_mutex);
}

#endif
