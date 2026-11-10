#pragma once

#include <stdint.h>

namespace BoilerTelemetry {
constexpr uint32_t MAGIC = 0x324C4F42; // "BOL2" on ESP32 (little-endian).
constexpr uint8_t VERSION = 1;
constexpr uint16_t UDP_PORT = 4210;
constexpr uint32_t TIMEOUT_MS = 2000;

constexpr uint8_t BOILER_VALID = 1u << 0;
constexpr uint8_t INLET_VALID = 1u << 1;
constexpr uint8_t OUTLET_VALID = 1u << 2;

struct __attribute__((packed)) Frame {
  uint32_t magic;
  uint8_t version;
  uint8_t validMask;
  uint32_t sequence;
  uint32_t bootId;
  int16_t temperatureCentiC[3];
  uint16_t millivolts[3];
  uint64_t inletPulses;
  uint64_t outletPulses;
  uint16_t crc;
};

static_assert(sizeof(Frame) == 44, "Unexpected telemetry frame layout");

inline uint16_t crc16(const uint8_t *data, uint32_t length) {
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

inline bool valid(const Frame &frame) {
  return frame.magic == MAGIC && frame.version == VERSION
      && frame.crc == crc16(reinterpret_cast<const uint8_t *>(&frame),
                            sizeof(Frame) - sizeof(frame.crc));
}

inline bool newer(uint32_t candidate, uint32_t previous) {
  return static_cast<int32_t>(candidate - previous) > 0;
}
}
