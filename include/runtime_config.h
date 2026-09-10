#pragma once
// Full LED cycle: ON half / OFF half. Dashboard setting overrides this default.
constexpr uint32_t DEFAULT_DEBUG_BLINK_MS = 100;
constexpr int RECORD_START_PIN = 36; // Current START wiring; earlier schematic SW1 used GPIO34.
constexpr int RECORD_STOP_PIN = 39;  // Schematic SW2, external pull-up, active LOW.
constexpr uint32_t SWITCH_DEBOUNCE_MS = 40;
// ESP32 dual-core task placement. Keep values 0 or 1.
constexpr BaseType_t WEB_CORE = 0;
constexpr BaseType_t SENSOR_CORE = 1;

// SD card installed. This firmware requires a FAT16/FAT32 filesystem.
constexpr bool ENABLE_SD_LOGGING = true;
