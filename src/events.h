#ifndef SEISMO_EVENTS_H
#define SEISMO_EVENTS_H

// Last detections, kept across reboots and shown at /events.
// Included only from main.cpp, like udplog.h.

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include "config.h"
#include "espnow_msg.h"

struct SeismoEvent {
  uint32_t start;   // unix time, 0 if the clock was not set yet
  float peak;       // g
  float duration;   // s
  uint8_t level;    // SeismoLevel
  uint8_t moved;    // cancelled by the tilt check
};

SeismoEvent events_ring[EVENTS_KEEP];
uint8_t events_head = 0;   // next slot to write
uint8_t events_count = 0;
uint32_t events_pending_start = 0;
Preferences events_prefs;

void events_begin() {
  configTzTime(LOCAL_TZ, "pool.ntp.org", "time.google.com");
  events_prefs.begin("events", false);
  if (events_prefs.getBytesLength("ring") == sizeof(events_ring)) {
    events_prefs.getBytes("ring", events_ring, sizeof(events_ring));
    events_head = events_prefs.getUChar("head", 0) % EVENTS_KEEP;
    events_count = min((int)events_prefs.getUChar("count", 0), EVENTS_KEEP);
  }
}

uint32_t events_now() {
  time_t now = time(nullptr);
  return now > 1700000000 ? (uint32_t)now : 0;  // before NTP the clock reads 1970
}

void events_start() {
  events_pending_start = events_now();
}

void events_save() {
  events_prefs.putBytes("ring", events_ring, sizeof(events_ring));
  events_prefs.putUChar("head", events_head);
  events_prefs.putUChar("count", events_count);
}

void events_end(uint8_t level, float peak, float duration, bool moved = false) {
  events_ring[events_head] = {events_pending_start, peak, duration, level, (uint8_t)moved};
  events_head = (events_head + 1) % EVENTS_KEEP;
  if (events_count < EVENTS_KEEP) events_count++;
  events_save();
}

// Remove the event that started at `start` (a test, someone bumping the sensor...)
bool events_delete(uint32_t start) {
  SeismoEvent kept[EVENTS_KEEP];
  int n = 0;
  for (int i = events_count - 1; i >= 0; i--) {  // oldest first
    const SeismoEvent &e = events_ring[(events_head + EVENTS_KEEP - 1 - i) % EVENTS_KEEP];
    if (e.start != start) kept[n++] = e;
  }
  if (n == events_count) return false;
  memset(events_ring, 0, sizeof(events_ring));
  memcpy(events_ring, kept, n * sizeof(SeismoEvent));
  events_count = n;
  events_head = n % EVENTS_KEEP;
  events_save();
  return true;
}

String events_time(uint32_t t, const char *fmt) {
  if (!t) return "sin hora";
  time_t tt = t;
  struct tm tm;
  localtime_r(&tt, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), fmt, &tm);
  return String(buf);
}

String events_page(float pga, float trigger, bool shaking) {
  String h;
  h.reserve(4000);
  h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<meta http-equiv='refresh' content='10'><title>SeismoM5 detecciones</title><style>"
         "body{font-family:verdana;margin:0;background:#f4f6f8;color:#222}"
         ".top{background:#1fa3ec;color:#fff;padding:12px;text-align:center}"
         ".top h1{margin:0;font-size:1.4rem}.box{max-width:640px;margin:12px auto;padding:0 12px}"
         ".now{background:#fff;border-radius:8px;padding:12px;margin-bottom:12px}"
         "table{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden}"
         "th,td{padding:8px;text-align:left;border-bottom:1px solid #eee;font-size:.9rem}"
         ".b{color:#fff;border-radius:4px;padding:2px 6px;font-size:.8rem}"
         ".blue{background:#1e6fd9}.red{background:#d32f2f}.grey{background:#888}"
         "a{color:#1fa3ec}</style></head><body><div class='top'><h1>Detecciones</h1></div><div class='box'>");
  h += "<div class='now'><b>Ahora:</b> " + events_time(events_now(), "%d/%m %H:%M:%S") +
       " &middot; PGA " + String(pga, 4) + " g &middot; WiFi canal " + String(WiFi.channel()) +
       " (AP " + WiFi.BSSIDstr() + ") &middot; " +
       (shaking ? "<span class='b red'>vibrando</span>" : "escuchando") +
       "<br><small>Azul: " + String(SUSTAIN_COUNT) + " de " + String(SUSTAIN_WINDOW) + " muestras &ge; " +
       String(SUSTAIN_LEVEL, 3) + " g &middot; Rojo: " + String(STRONG_COUNT) + " de " + String(SUSTAIN_WINDOW) +
       " &ge; " + String(trigger, 3) + " g</small></div>";
  if (!events_count) {
    h += F("<p>Sin detecciones guardadas.</p>");
  } else {
    h += F("<table><tr><th>Fecha y hora</th><th>Nivel</th><th>Pico</th><th>Duraci&oacute;n</th><th></th></tr>");
    for (int i = 0; i < events_count; i++) {  // newest first
      const SeismoEvent &e = events_ring[(events_head + EVENTS_KEEP - 1 - i) % EVENTS_KEEP];
      String badge = e.moved ? "<span class='b grey'>sensor movido</span>"
                   : e.level >= SEISMO_LEVEL_STRONG ? "<span class='b red'>rojo</span>"
                   : "<span class='b blue'>azul</span>";
      h += "<tr><td>" + events_time(e.start, "%d/%m %H:%M:%S") + "</td><td>" + badge + "</td><td>" +
           String(e.peak, 4) + " g</td><td>" + String(e.duration, 0) + " s</td><td>" +
           (e.start ? "<a href='/event_del?t=" + String(e.start) +
                      "' onclick='return confirm(\"Borrar este evento?\")'>borrar</a>" : String("")) +
           "</td></tr>";
    }
    h += F("</table>");
  }
  h += F("<p><a href='/'>Ajustes</a> &middot; <a href='/webserial'>Web Serial</a></p></div></body></html>");
  return h;
}

#endif
