#ifndef _secrets_H_
#define _secrets_H_

// Private settings: WiFi, MQTT, ntfy topic, logging computer.
// secrets.h is not committed (see .gitignore). To set up, copy
// secrets.example.h to secrets.h and fill in your values.

#define WIFI_SSID "XXXXXXX"
#define WIFI_PASS "XXXXXXX"

#define MQTT_SERVER "XXXXXXX"
#define MQTT_USER "XXXXXXX"
#define MQTT_PASS "XXXXXXX"

// Anyone who knows the topic can read and send your alerts: make it hard to guess
#define NTFY_TOPIC "seismo-change-me-1234"

// mDNS name of the computer running tools/udplog.py, without .local
#define UDP_LOG_HOST "my-computer"

#endif
