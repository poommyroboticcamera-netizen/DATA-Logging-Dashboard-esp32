#pragma once
#include <stdint.h>
// Unused GPIOs in the current dashboard map. VERIFY the transceiver wiring.
// Do not use GPIO21/22: those already belong to the I2C sensor bus.
constexpr int CAN_TX_PIN=25, CAN_RX_PIN=26;
constexpr uint32_t CAN_BITRATE=500000;
constexpr int SD_SCK_PIN=18, SD_MISO_PIN=19, SD_MOSI_PIN=23, SD_CS_PIN=5;
// Bounded web + existing sensor profile. SD captures untracked IDs too.
constexpr unsigned MAX_IDS=16, SAMPLE_COUNT=32;
constexpr unsigned RX_QUEUE_DEPTH=128, LOG_QUEUE_DEPTH=256, DRIVER_RX_DEPTH=128;
constexpr unsigned DEFAULT_CAPTURE_SECONDS=10, MAX_CAPTURE_SECONDS=3600, MIN_EVIDENCE=16;
static_assert(SAMPLE_COUNT>=MIN_EVIDENCE && SAMPLE_COUNT<=64,"sample bounds");
