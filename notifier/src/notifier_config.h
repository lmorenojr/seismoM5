#ifndef _notifier_config_H_
#define _notifier_config_H_

// WiFi and NTFY_TOPIC come from the sensor's config.h (../src)
#include "config.h"

// USGS region filter (Panama and surroundings)
#define REGION_MIN_LAT 6.0
#define REGION_MAX_LAT 10.5
#define REGION_MIN_LON -83.5
#define REGION_MAX_LON -77.0
#define QUAKE_MIN_MAG 4.0           // push to phone for feed events at or above this
#define USGS_FEED "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_hour.geojson"
#define EMSC_QUERY "https://www.seismicportal.eu/fdsnws/event/1/query?format=json&orderby=time&limit=30&minmag=2.5"
#define EMSC_WINDOW_S (3 * 3600)    // EMSC query looks back this far
#define FEED_POLL_MS 60000
#define DEDUP_SECONDS 60            // same quake if both feeds agree within this time...
#define DEDUP_KM 100                // ...and this distance
#define MAX_ALERT_AGE_S (3 * 3600)  // never push events older than this

#define SENSOR_TIMEOUT_MS 30000     // sensor shown offline after this without heartbeat
#define ALARM_MAX_MS 60000          // sensor alarm beeps at most this long
#define CHIME_DUTY 512              // blue chime loudness, of 1023; 512 (50%) is the buzzer's loudest, same as red
#define NTFY_USGS true              // also push USGS events to the phone

// Battery (AXP192 reports volts)
#define BAT_FULL_V 4.15
#define BAT_EMPTY_V 3.30
#define LOW_BAT_PCT 20
#define CHARGING_MIN_MA 5.0         // charge current above this = charging
#define POWER_DEBOUNCE_MS 10000     // USB state must hold this long before alerting

#define TZ_OFFSET_S (-5 * 3600)     // Panama, UTC-5

#endif
