#ifndef _espnow_msg_H_
#define _espnow_msg_H_

#include <stdint.h>

// Shared between the sensor (seismoM5) and the notifier stick
#define SEISMO_MSG_MAGIC 0x534D3502 // "SM5" v2

enum SeismoMsgType : uint8_t {
  SEISMO_HEARTBEAT = 0,
  SEISMO_EQ_START  = 1,
  SEISMO_EQ_END    = 2,  // EQ_START is sent again when the level goes up
};

enum SeismoLevel : uint8_t {
  SEISMO_LEVEL_NONE   = 0,
  SEISMO_LEVEL_WEAK   = 1,  // blue: weak sustained shaking
  SEISMO_LEVEL_STRONG = 2,  // red: strong sustained shaking
};

struct __attribute__((packed)) SeismoMsg {
  uint32_t magic;
  uint8_t  type;
  float    pga;        // current pga (g)
  float    pga_peak;   // peak pga of the event (g), EQ_END only
  float    pga_trigger;
  uint32_t uptime_s;
  uint8_t  level;      // SeismoLevel of the event so far
};

#endif
