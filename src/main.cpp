#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <driver/rmt.h>
#include <driver/gpio.h>
#include <Wire.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <atomic>
#include <SPI.h>
#include <SD.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Encoder.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include "wifi_config.h"
#include "runtime_config.h"
#include "dashboard_page.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// Each device has one owning task. Only the bus and dashboard snapshots
// are shared. Never hold the bus mutex while waiting for a conversion.
std::atomic<bool> supplyBusEnabled{false};
std::atomic<uint32_t> supplyGeneration{0};
SemaphoreHandle_t i2cMutex = nullptr;
SemaphoreHandle_t stateMutex = nullptr;
QueueHandle_t imuCommands = nullptr, mcpCommands = nullptr;
QueueHandle_t sensorCommands = nullptr, logQueue = nullptr;

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_EXT: return "EXTERNAL_RESET";
    case ESP_RST_SW: return "SOFTWARE_RESET";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO_RESET";
    default: return "UNKNOWN";
  }
}

struct LogMessage { char text[224]; };
void logf(const char *format, ...) {
  if (SERIAL_IP_ONLY) return;
  LogMessage message{};
  va_list args;
  va_start(args, format);
  vsnprintf(message.text, sizeof(message.text), format, args);
  va_end(args);
  // Logging never blocks acquisition. Console is the sole UART writer
  // after startup; excess diagnostic messages may be dropped.
  if (logQueue) xQueueSend(logQueue, &message, 0);
  else Serial.print(message.text);
}
void logLine(const char *text) { logf("%s\n", text); }

class Lock {
  SemaphoreHandle_t mutex;
  bool held;
public:
  explicit Lock(SemaphoreHandle_t m, TickType_t timeout = portMAX_DELAY)
    : mutex(m), held(m && xSemaphoreTake(m, timeout) == pdTRUE) {}
  ~Lock() { if (held) xSemaphoreGive(mutex); }
  explicit operator bool() const { return held; }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
};

struct InaReading {
  bool valid = false;
  uint32_t updatedMs = 0;
  float voltageV = 0, shuntMv = 0, currentA = 0, powerW = 0;
  float rshuntMohm = 0; // Resistance actually used for this reading, in milliohms.
};
struct ImuReading {
  bool valid = false, tiltValid = false, rollValid = false;
  bool yawValid = false, calibrating = true;
  uint32_t updatedMs = 0;
  uint8_t identity = 0;
  unsigned calibrationSamples = 0;
  uint32_t yawReference = 0;
  float ax = 0, ay = 0, az = 0, temperatureC = 0;
  float rollDeg = 0, pitchDeg = 0, yawDeg = 0;
};
struct McpReading {
  bool valid = false, chase = false;
  uint32_t updatedMs = 0;
  uint8_t portA = 0, portB = 0, commandedOutputs = 0;
};
struct RtcReading {
  bool valid = false, running = false, batteryEnabled = false, powerFail = false;
  uint32_t updatedMs = 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};
struct SupplyReading {
  bool valid = false, present = false, overRange = false;
  uint16_t rawMin = 4095, rawMax = 0;
  unsigned overRangeSamples = 0;
  uint32_t updatedMs = 0;
  float adcMv = 0, voltageV = 0;
};
struct DhtReading {
  bool valid = false;
  uint32_t updatedMs = 0;
  float temperatureC = 0, humidityPct = 0;
};
constexpr int DS_PINS[4] = {27, 16, 13, 14}; // One sensor per independent DATA pin.
enum DsStatus : uint8_t { DS_WAITING, DS_READY, DS_DISABLED, DS_SUPPLY_PAUSED,
                         DS_NOT_FOUND, DS_CONFIG_FAILED, DS_START_FAILED, DS_TIMEOUT, DS_READ_FAILED };
const char *dsStatusName(DsStatus status) {
  const char *names[] = {"WAITING", "READY", "DISABLED", "SUPPLY_PAUSED", "NOT_FOUND",
                         "CONFIG_FAILED", "START_FAILED", "CONVERSION_TIMEOUT", "READ_FAILED"};
  return unsigned(status) < sizeof(names) / sizeof(names[0]) ? names[status] : "UNKNOWN";
}
struct DsReading {
  DsStatus status = DS_WAITING;
  bool parasite = false;
  bool valid = false;
  uint32_t updatedMs = 0;
  float temperatureC = 0;
};
struct EncoderReading {
  bool valid = false, rpmValid = false, speedValid = false;
  uint32_t updatedMs = 0;
  int64_t count = 0;
  float countsPerSecond = 0, shaftRpm = 0, wheelRpm = 0, speedKmh = 0;
};
struct DashboardState {
  InaReading ina[3];
  ImuReading imu;
  McpReading mcp;
  RtcReading rtc;
  SupplyReading supply12v;
  DhtReading dht;
  DsReading ds[4];
  EncoderReading encoder;
};
DashboardState dashboard;

// These runtime switches pause sampling, not electrical power to the devices.
enum DeviceId { DEV_INA1, DEV_INA2, DEV_INA3, DEV_RTC, DEV_IMU, DEV_MCP,
                DEV_DHT, DEV_DS1, DEV_DS2, DEV_DS3, DEV_DS4, DEV_ENCODER, DEV_SD, DEVICE_COUNT };
const char *const deviceNames[] = {"ina1", "ina2", "ina3", "rtc", "imu", "mcp",
                                   "dht", "ds1", "ds2", "ds3", "ds4", "encoder", "sd"};
constexpr uint32_t ALL_DEVICES_MASK = (1U << DEVICE_COUNT) - 1;
std::atomic<uint32_t> enabledDevices{ALL_DEVICES_MASK};
uint32_t deviceControlRevision = 0; // Wi-Fi task owns revision and switch writes.
bool deviceEnabled(unsigned id) { return enabledDevices.load() & (1U << id); }
struct EncoderConfig { float ppr = 360, wheelMm = 100, ratio = 1; };
EncoderConfig encoderConfig; // Protected by stateMutex after setup.
struct ShuntConfig { float mohm[3] = {1.0f, 1.0f, 1.0f}; };
ShuntConfig shuntConfig; // 1 milliohm = 0.001 ohm. Protected by stateMutex.
bool validShuntMohm(float value) { return isfinite(value) && value >= 0.01f && value <= 1000.0f; }
ShuntConfig getShuntConfig() { Lock lock(stateMutex); return shuntConfig; }
EncoderConfig getEncoderConfig() { Lock lock(stateMutex); return encoderConfig; }
constexpr int DEBUG_LED_PIN = 2;
constexpr int DEBUG_LED_ON = HIGH, DEBUG_LED_OFF = LOW;
std::atomic<bool> mcpStopFailed{false};


// Future HTTP/WebSocket/UI task reads a copy; never reads driver globals.
DashboardState getDashboardSnapshot() {
  Lock lock(stateMutex);
  DashboardState copy = dashboard;
  if (!supplyBusEnabled.load()) {
    for (auto &ina : copy.ina) ina.valid = false;
    copy.imu.valid = copy.imu.yawValid = false;
    copy.mcp.valid = false;
    copy.rtc.valid = false;
    copy.dht.valid = false;
    copy.encoder.valid = copy.encoder.rpmValid = copy.encoder.speedValid = false;
    for (auto &ds : copy.ds) { ds.valid = false; ds.status = DS_SUPPLY_PAUSED; }
  }
  for (unsigned i = 0; i < 3; ++i) if (!deviceEnabled(DEV_INA1 + i)) copy.ina[i].valid = false;
  for (unsigned i = 0; i < 4; ++i) if (!deviceEnabled(DEV_DS1 + i)) { copy.ds[i].valid = false; copy.ds[i].status = DS_DISABLED; }
  if (!deviceEnabled(DEV_IMU)) copy.imu.valid = copy.imu.yawValid = false;
  if (!deviceEnabled(DEV_RTC)) copy.rtc.valid = false;
  if (!deviceEnabled(DEV_MCP)) copy.mcp.valid = false;
  if (!deviceEnabled(DEV_DHT)) copy.dht.valid = false;
  if (!deviceEnabled(DEV_ENCODER)) copy.encoder.valid = copy.encoder.rpmValid = copy.encoder.speedValid = false;
  return copy;
}

uint8_t probeAddress(uint8_t address) {
  Lock lock(i2cMutex, pdMS_TO_TICKS(60));
  if (!lock || !supplyBusEnabled.load()) return 5;
  Wire.beginTransmission(address);
  return Wire.endTransmission(true);
}

// ============================================================
// Settings
// ============================================================
constexpr int SDA_PIN = 21;
constexpr int SCL_PIN = 22;
constexpr uint32_t I2C_SPEED = 100000;
// Temporarily disabled: keep sensor tasks running without an ADC power gate.
constexpr bool SUPPLY_MONITOR_ENABLED = false;
constexpr int VC12V_PIN = 15; // ADC2 on ESP32; dedicated voltage sense input
constexpr float SUPPLY_DIVIDER_RATIO = (100000.0f + 10000.0f) / 10000.0f;
constexpr float SUPPLY_CALIBRATION = 1.0f; // Adjust against a multimeter
constexpr float SUPPLY_ON_V = 10.0f;
constexpr float SUPPLY_OFF_V = 3.5f;
constexpr uint32_t SUPPLY_ADC_MAX_MV = 3300;
constexpr uint16_t SUPPLY_ADC_SATURATION_RAW = 4095;
static_assert(SUPPLY_ON_V > SUPPLY_OFF_V, "Supply thresholds need hysteresis");

constexpr uint8_t MCP_ADDR = 0x27;
constexpr uint8_t RTC_ADDR = 0x6F;

// CH1 = U60, CH2 = U65, CH3 = U66
const uint8_t INA_ADDR[3] = {0x40, 0x45, 0x44};
// Shunt resistance is configured per INA channel from the dashboard.

bool inaReady[3] = {false, false, false};
bool mcpReady = false;
bool imuReady = false;

uint8_t outputMask = 0;
constexpr uint32_t DEFAULT_GA_STEP_MS = 500;
std::atomic<uint32_t> gaStepMs{DEFAULT_GA_STEP_MS};
uint32_t gaPeriodForHz(float hz) {
  return isfinite(hz) && hz >= 0.2f && hz <= 20.0f ? uint32_t(lroundf(1000.0f / hz)) : 0;
}
bool lightChaseRunning = false;
uint8_t lightChasePin = 3;
uint32_t lightChaseLastMs = 0;
void updateLightChase();
uint8_t imuAddress = 0;
uint8_t imuIdentity = 0;
uint8_t imuSample[14] = {};
bool imuSampleValid = false;
unsigned imuReadFailures = 0;
float imuFilteredAccel[3] = {};
bool imuFilterReady = false;
uint32_t imuSampleUs = 0;
float yawDeg = 0.0f, yawBias = 0.0f, yawSum = 0.0f, yawSumSq = 0.0f;
float previousYawRate = 0.0f;
unsigned yawCalibrationCount = 0;
uint32_t yawReference = 0;
bool yawCalibrated = false, yawContinuous = false;

void updateIMU();

void calibrateYaw() {
  yawCalibrationCount = 0;
  yawSum = yawSumSq = 0.0f;
  yawCalibrated = yawContinuous = false;
  yawDeg = 0.0f;
  imuSampleUs = 0;
  logLine("Yaw: keep board STILL for about 4 seconds to calibrate.");
}

const char *imuModel() {
  return imuIdentity == 0x70 ? "MPU6500" :
         imuIdentity == 0x68 ? "MPU6050" : "UNKNOWN IMU";
}

// ============================================================
// I2C helpers
// ============================================================
bool ping(uint8_t address) {
  return probeAddress(address) == 0;
}

bool readBytes(uint8_t address, uint8_t reg,
               uint8_t *data, size_t count) {
  Lock lock(i2cMutex, pdMS_TO_TICKS(60));
  if (!lock || !supplyBusEnabled.load()) return false;
  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom(address, count, true) != count) {
    while (Wire.available()) Wire.read();
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    data[i] = Wire.read();
  }
  return true;
}

bool writeBytes(uint8_t address, uint8_t reg,
                const uint8_t *data, size_t count) {
  Lock lock(i2cMutex, pdMS_TO_TICKS(60));
  if (!lock || !supplyBusEnabled.load()) return false;
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(data, count);
  return Wire.endTransmission(true) == 0;
}

bool read8(uint8_t address, uint8_t reg, uint8_t &value) {
  return readBytes(address, reg, &value, 1);
}

bool write8(uint8_t address, uint8_t reg, uint8_t value) {
  return writeBytes(address, reg, &value, 1);
}

bool read16(uint8_t address, uint8_t reg, uint16_t &value) {
  uint8_t data[2];

  if (!readBytes(address, reg, data, 2)) return false;

  value = (uint16_t(data[0]) << 8) | data[1];
  return true;
}

bool write16(uint8_t address, uint8_t reg, uint16_t value) {
  uint8_t data[2] = {
    uint8_t(value >> 8),
    uint8_t(value & 0xFF)
  };

  return writeBytes(address, reg, data, 2);
}

int16_t signed16(const uint8_t *data) {
  return static_cast<int16_t>(
    (uint16_t(data[0]) << 8) | data[1]
  );
}

void scanI2C() {
  unsigned found = 0;
  unsigned errors = 0;

  logLine("\n--- I2C SCAN ---");

  for (uint8_t address = 0x08; address <= 0x77; address++) {
    uint8_t result = probeAddress(address);

    if (result == 0) {
      logf("Found device at 0x%02X\n", address);
      found++;
    } else if (result != 2) {
      logf("Bus error %u at 0x%02X\n", result, address);
      errors++;
    }

    delay(1);
  }

  logf("Found %u device(s), %u bus error(s)\n",
                found, errors);
}

// ============================================================
// INA226
// ============================================================
bool initINA(uint8_t index) {
  const uint8_t address = INA_ADDR[index];
  uint16_t manufacturer, die;

  if (!read16(address, 0xFE, manufacturer) ||
      !read16(address, 0xFF, die)) {
    logf("INA CH%u [0x%02X]: I2C ERROR\n",
                  unsigned(index + 1), address);
    return false;
  }

  bool valid = manufacturer == 0x5449 &&
               (die == 0x2260 || die == 0x2261);

  logf(
    "INA CH%u [0x%02X]: manufacturer=%04X die=%04X %s\n",
    unsigned(index + 1), address, manufacturer, die,
    valid ? "ID OK" : "UNEXPECTED ID"
  );

  return valid;
}

void sampleINA(uint8_t index) {
  // Keep the previous fresh sample visible while the next conversion runs.
  // Only failed conversions invalidate it; updatedMs advances on success only.
  const auto invalidateReading = [index]() {
    Lock lock(stateMutex);
    dashboard.ina[index].valid = false;
  };
  const uint8_t address = INA_ADDR[index];

  if (!inaReady[index]) {
    inaReady[index] = initINA(index);
    if (!inaReady[index]) { invalidateReading(); return; }
  }

  // AVG=16, bus/shunt conversion time=1.1 ms each.
  // Trigger one complete shunt + bus measurement.
  if (!write16(address, 0x00, 0x4523)) {
    inaReady[index] = false;
    logf("INA CH%u: trigger ERROR\n",
                  unsigned(index + 1));
    invalidateReading(); return;
  }

  bool converted = false;
  uint32_t started = millis();

  while (millis() - started < 250) {
    uint16_t status;

    if (!read16(address, 0x06, status)) {
      inaReady[index] = false;
      logf("INA CH%u: status read ERROR\n",
                    unsigned(index + 1));
      invalidateReading(); return;
    }

    if (status & 0x0008) {
      converted = true;
      break;
    }

    delay(2);
  }

  if (!converted) {
    logf("INA CH%u: conversion TIMEOUT\n",
                  unsigned(index + 1));
    invalidateReading(); return;
  }

  uint16_t rawBus, rawShunt;

  if (!read16(address, 0x02, rawBus) ||
      !read16(address, 0x01, rawShunt)) {
    inaReady[index] = false;
    logf("INA CH%u: measurement read ERROR\n",
                  unsigned(index + 1));
    invalidateReading(); return;
  }

  const int16_t shuntRaw = static_cast<int16_t>(rawShunt);
  const float busV = rawBus * 0.00125f;
  const float shuntV = shuntRaw * 0.0000025f;
  const float rshuntMohm = getShuntConfig().mohm[index];
  const float shuntOhms = rshuntMohm * 0.001f;

  if (shuntRaw == INT16_MAX || shuntRaw == INT16_MIN ||
      busV > 36.0f || !validShuntMohm(rshuntMohm)) {
    logf("INA CH%u: OUT OF RANGE / INVALID SHUNT\n",
                  unsigned(index + 1));
    invalidateReading(); return;
  }

  // Calculate from shunt voltage; calibration register unused.
  const float currentA = shuntV / shuntOhms;
  const float powerW = busV * currentA;

  InaReading value;
  value.valid = true;
  value.updatedMs = millis();
  value.voltageV = busV;
  value.shuntMv = shuntV * 1000.0f;
  value.currentA = currentA;
  value.powerW = powerW;
  value.rshuntMohm = rshuntMohm;
  Lock lock(stateMutex);
  // A setting change during this sample must not publish old calibration as new.
  if (shuntConfig.mohm[index] != rshuntMohm || !deviceEnabled(DEV_INA1 + index)) value.valid = false;
  dashboard.ina[index] = value;
}

// ============================================================
// MCP23017
//
// GPA0 -> U45 -> OP3 / CN15
// GPA1 -> U44 -> OP2 / CN16
// GPA2 -> U43 -> OP1 / CN17
// GPA3 -> U42 -> OP0 / CN18
//
// GPB0..5 -> H1 inputs with internal pull-ups.
// Unused GPA7 and GPB7 are configured as output LOW.
// ============================================================
bool initMCP() {
  lightChaseRunning = false;
  mcpReady = false;

  if (!ping(MCP_ADDR)) return false;

  // Normalize BANK=0.
  // In BANK=1, 0x05 is IOCON.
  // In BANK=0, 0x05 is GPINTENB.
  if (!write8(MCP_ADDR, 0x05, 0x00) ||
      !write8(MCP_ADDR, 0x0A, 0x00)) return false;

  // Disable interrupts and polarity inversion.
  if (!write8(MCP_ADDR, 0x04, 0x00) ||
      !write8(MCP_ADDR, 0x05, 0x00) ||
      !write8(MCP_ADDR, 0x02, 0x00) ||
      !write8(MCP_ADDR, 0x03, 0x00)) return false;

  // Set output latches LOW before enabling output pins.
  if (!write8(MCP_ADDR, 0x14, 0x00) ||
      !write8(MCP_ADDR, 0x15, 0x00)) return false;

  // A0..3 outputs, A4..6 inputs, A7 output.
  // B0..6 inputs, B7 output.
  if (!write8(MCP_ADDR, 0x00, 0x70) ||
      !write8(MCP_ADDR, 0x01, 0x7F) ||
      !write8(MCP_ADDR, 0x0C, 0x70) ||
      !write8(MCP_ADDR, 0x0D, 0x7F)) return false;

  uint8_t dirA, dirB;

  if (!read8(MCP_ADDR, 0x00, dirA) ||
      !read8(MCP_ADDR, 0x01, dirB)) return false;

  outputMask = 0;
  mcpReady = dirA == 0x70 && dirB == 0x7F;
  return mcpReady;
}

void setOutputs(uint8_t mask) {
  if (!mcpReady) {
    logLine("MCP: NOT READY; send r to initialize");
    return;
  }

  mask &= 0x0F;

  if (!write8(MCP_ADDR, 0x14, mask)) {
    lightChaseRunning = false;
    mcpReady = false;
    logLine("MCP: output write ERROR");
    return;
  }

  outputMask = mask;
  logf("MCP output command = 0x%02X\n", outputMask);
}

void startLightChase() {
  if (!mcpReady) {
    logLine("Light chase: MCP not ready; send r to initialize");
    return;
  }
  lightChasePin = 3;
  setOutputs(uint8_t(1U << lightChasePin));
  lightChaseRunning = mcpReady;
  lightChaseLastMs = millis();
  if (lightChaseRunning) {
    logLine("Light chase ON: GA3 -> GA2 -> GA1 -> GA0; 0 stops");
  }
}

void updateLightChase() {
  if (!lightChaseRunning || !mcpReady) return;
  const uint32_t now = millis();
  if (uint32_t(now - lightChaseLastMs) < gaStepMs.load()) return;

  // One output HIGH at a time. No blocking delay or catch-up flashes.
  lightChasePin = lightChasePin == 0 ? 3 : lightChasePin - 1;
  setOutputs(uint8_t(1U << lightChasePin));
  lightChaseLastMs = millis();
}

void sampleMCP() {
  McpReading value;
  value.chase = lightChaseRunning;
  value.commandedOutputs = outputMask;
  value.valid = mcpReady && read8(MCP_ADDR, 0x12, value.portA) &&
                read8(MCP_ADDR, 0x13, value.portB);
  if (!value.valid) { mcpReady = false; lightChaseRunning = false; value.chase = false; }
  value.updatedMs = millis();
  Lock lock(stateMutex);
  dashboard.mcp = value;
}

// ============================================================
// MCP7940N RTC
// ============================================================
uint8_t toBCD(int value) {
  return uint8_t((value / 10) * 16 + value % 10);
}

int fromBCD(uint8_t value) {
  return (value >> 4) * 10 + (value & 0x0F);
}

bool validBCD(uint8_t value, int minimum, int maximum) {
  int decoded = fromBCD(value);

  return (value & 0x0F) <= 9 &&
         (value >> 4) <= 9 &&
         decoded >= minimum &&
         decoded <= maximum;
}

bool setRTCFromCompileTime() {
  char monthName[4];
  int day, year, hour, minute, second;

  if (sscanf(__DATE__, "%3s %d %d",
             monthName, &day, &year) != 3 ||
      sscanf(__TIME__, "%d:%d:%d",
             &hour, &minute, &second) != 3) {
    return false;
  }

  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *position = strstr(months, monthName);

  if (!position || year < 2000 || year > 2099) return false;

  const int month = (position - months) / 3 + 1;

  // Sunday=1 ... Saturday=7.
  const int offsets[] = {
    0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
  };

  int y = year - (month < 3);

  int weekday =
    (y + y / 4 - y / 100 + y / 400 +
     offsets[month - 1] + day) % 7 + 1;

  uint8_t control, secondsReg, weekdayReg;

  if (!read8(RTC_ADDR, 0x07, control) ||
      !read8(RTC_ADDR, 0x00, secondsReg)) return false;

  // Select crystal operation and stop oscillator.
  if (!write8(RTC_ADDR, 0x07, control & ~0x08) ||
      !write8(RTC_ADDR, 0x00, secondsReg & 0x7F)) {
    return false;
  }

  uint32_t started = millis();

  while (true) {
    if (!read8(RTC_ADDR, 0x03, weekdayReg)) return false;
    if (!(weekdayReg & 0x20)) break;

    if (millis() - started > 1000) return false;
    delay(2);
  }

  // 24-hour mode, battery backup enabled.
  // Setting the time explicitly clears the old PWRFAIL flag.
  uint8_t data[7] = {
    toBCD(second),
    toBCD(minute),
    toBCD(hour),
    uint8_t(weekday | 0x08),
    toBCD(day),
    toBCD(month),
    toBCD(year % 100)
  };

  if (!writeBytes(RTC_ADDR, 0x00, data, sizeof(data))) {
    return false;
  }

  // ST=1: start oscillator.
  if (!write8(RTC_ADDR, 0x00, toBCD(second) | 0x80)) {
    return false;
  }

  started = millis();

  while (millis() - started < 2000) {
    if (!read8(RTC_ADDR, 0x03, weekdayReg)) return false;

    if (weekdayReg & 0x20) {
      logf("RTC set to compile time: %s %s\n",
                    __DATE__, __TIME__);
      return true;
    }

    delay(10);
  }

  return false;
}

void sampleRTC() {
  const auto invalidate = []() { Lock lock(stateMutex); dashboard.rtc.valid = false; };
  uint8_t data[7], secondAgain;
  bool stable = false;

  // Avoid displaying a mixed timestamp across a second rollover.
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!readBytes(RTC_ADDR, 0x00, data, sizeof(data)) ||
        !read8(RTC_ADDR, 0x00, secondAgain)) {
      logLine("RTC: I2C read ERROR");
      invalidate(); return;
    }

    if ((data[0] & 0x7F) == (secondAgain & 0x7F)) {
      stable = true;
      break;
    }
  }

  if (!stable) {
    logLine("RTC: unstable time read");
    invalidate(); return;
  }

  const bool mode12 = (data[2] & 0x40) != 0;
  const uint8_t hourBCD = data[2] & (mode12 ? 0x1F : 0x3F);

  bool valid =
    validBCD(data[0] & 0x7F, 0, 59) &&
    validBCD(data[1] & 0x7F, 0, 59) &&
    validBCD(hourBCD, mode12 ? 1 : 0, mode12 ? 12 : 23) &&
    validBCD(data[4] & 0x3F, 1, 31) &&
    validBCD(data[5] & 0x1F, 1, 12) &&
    validBCD(data[6], 0, 99);

  if (!valid) {
    logLine("RTC: invalid/unset date; send t to set time");
    invalidate(); return;
  }

  int year = 2000 + fromBCD(data[6]);
  int month = fromBCD(data[5] & 0x1F);
  int day = fromBCD(data[4] & 0x3F);

  const uint8_t monthDays[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };

  int maxDay = monthDays[month - 1];
  if (month == 2 && year % 4 == 0) maxDay = 29;

  if (day > maxDay) {
    logLine("RTC: invalid calendar date; send t");
    invalidate(); return;
  }

  int hour = fromBCD(hourBCD);

  if (mode12) {
    hour %= 12;
    if (data[2] & 0x20) hour += 12;
  }

  RtcReading value;
  value.valid = true;
  value.updatedMs = millis();
  value.year = year; value.month = month; value.day = day; value.hour = hour;
  value.minute = fromBCD(data[1] & 0x7F);
  value.second = fromBCD(data[0] & 0x7F);
  value.running = (data[3] & 0x20) != 0;
  value.batteryEnabled = (data[3] & 0x08) != 0;
  value.powerFail = (data[3] & 0x10) != 0;
  Lock lock(stateMutex);
  dashboard.rtc = value;
}

// ============================================================
// MPU6050 / MPU6500 (identified by WHO_AM_I, not I2C address)
// ============================================================
// Report transport failures separately from a responding, unsupported chip.
bool probeIMU(uint8_t address, bool &acknowledged) {
  const uint8_t result = probeAddress(address);
  acknowledged = result == 0;

  if (!acknowledged) {
    logf("IMU [0x%02X]: %s (Wire error=%u)\n", address,
                  result == 2 ? "NO ACK" : "BUS ERROR", result);
    return false;
  }

  uint8_t id;
  if (!read8(address, 0x75, id)) {
    logf("IMU [0x%02X]: ACK, but WHO_AM_I read FAILED\n", address);
    return false;
  }

  logf("IMU [0x%02X]: ACK, WHO_AM_I=0x%02X (supported: 0x68 / 0x70)\n",
                address, id);
  if (id != 0x68 && id != 0x70) {
    logLine("IMU: unsupported identity; check module marking/model.");
    return false;
  }
  imuIdentity = id;
  logf("IMU: identified as %s\n", imuModel());
  return true;
}

bool initIMU() {
  imuReadFailures = 0;
  imuFilterReady = false;
  imuReady = false;
  imuSampleValid = false;
  imuAddress = 0;
  imuIdentity = 0;

  const uint8_t candidates[] = {0x68, 0x69};
  bool anyAcknowledged = false;

  for (uint8_t address : candidates) {
    bool acknowledged = false;
    bool supported = probeIMU(address, acknowledged);
    anyAcknowledged |= acknowledged;
    if (supported) {
      imuAddress = address;
      break;
    }
  }

  if (imuAddress == 0) {
    if (anyAcknowledged) {
      logLine("IMU: device responds, but no supported identity verified.");
    } else {
      logLine("IMU: neither 0x68 nor 0x69 acknowledged.");
      logLine("Check module power/GND, SDA/SCL (not XDA/XCL), and AD0.");
    }
    return false;
  }

  if (!write8(imuAddress, 0x6B, 0x80)) {
    logLine("IMU: reset write ERROR");
    return false;
  }

  delay(100);

  // Wake up, CLKSEL=1 (6050: X gyro PLL; 6500: auto clock), all axes.
  if (!write8(imuAddress, 0x6B, 0x01) ||
      !write8(imuAddress, 0x6C, 0x00)) {
    logLine("IMU: wake-up ERROR");
    return false;
  }

  delay(100);

  // Disable FIFO/internal I2C master.
  // DLPF=3; sample rate=50 Hz.
  // Gyro=+/-1000 dps; accelerometer=+/-8 g for vibration headroom.
  if (!write8(imuAddress, 0x6A, 0x00) ||
      !write8(imuAddress, 0x23, 0x00) ||
      !write8(imuAddress, 0x1A, 0x03) ||
      !write8(imuAddress, 0x19, 0x13) ||
      !write8(imuAddress, 0x1B, 0x10) ||
      !write8(imuAddress, 0x1C, 0x10)) {
    logLine("IMU: configuration write ERROR");
    return false;
  }

  uint8_t power, gyroConfig, accelConfig;

  // MPU6500 has a separate accelerometer filter register.
  // ACCEL_FCHOICE_B=0, A_DLPFCFG=3; do not write this on MPU6050.
  if (imuIdentity == 0x70) {
    uint8_t accelFilter;
    if (!write8(imuAddress, 0x1D, 0x03) ||
        !read8(imuAddress, 0x1D, accelFilter)) {
      logLine("MPU6500: accelerometer filter I2C ERROR");
      return false;
    }
    if ((accelFilter & 0x0F) != 0x03) {
      logf("MPU6500: ACCEL_CONFIG2=0x%02X; expected low bits 0x03\n",
                    accelFilter);
      return false;
    }
  }

  if (!read8(imuAddress, 0x6B, power) ||
      !read8(imuAddress, 0x1B, gyroConfig) ||
      !read8(imuAddress, 0x1C, accelConfig)) {
    logLine("IMU: configuration read ERROR");
    return false;
  }

  if (power != 0x01 ||
      gyroConfig != 0x10 ||
      accelConfig != 0x10) {
    logf("IMU: configuration MISMATCH: PWR=0x%02X "
                  "GYRO=0x%02X ACCEL=0x%02X; expected 01/10/10\n",
                  power, gyroConfig, accelConfig);
    return false;
  }

  delay(100);

  imuReady = true;
  calibrateYaw();
  logf("%s [0x%02X]: READY\n", imuModel(), imuAddress);
  return true;
}

// Integrate rotation about the board Z axis at approximately 50 Hz.
// Relative rotation about body Z, not an earth-frame/compass heading.
void updateIMU() {
  if (!imuReady) return;
  uint32_t now = micros();
  if (!readBytes(imuAddress, 0x3B, imuSample, sizeof(imuSample))) {
    imuSampleValid = false;
    // A transient bus failure must not trigger a 300 ms device reset.
    // The next valid sample checks the actual elapsed time before integrating.
    if (++imuReadFailures >= 3) {
      imuReady = yawContinuous = false;
      imuFilterReady = false;
    }
    return;
  }
  now = micros();
  const float dt = imuSampleUs ? uint32_t(now - imuSampleUs) * 1e-6f : 0.0f;
  imuSampleUs = now;
  imuSampleValid = true;
  imuReadFailures = 0;
  // 50 ms low-pass for displayed acceleration/tilt, using actual sample time.
  const float alpha = dt > 0 ? dt / (0.05f + dt) : 1.0f;
  for (unsigned i = 0; i < 3; ++i) {
    const float raw = signed16(&imuSample[i * 2]) / 4096.0f;
    if (!imuFilterReady || dt > 0.1f) imuFilteredAccel[i] = raw;
    else imuFilteredAccel[i] += alpha * (raw - imuFilteredAccel[i]);
  }
  imuFilterReady = true;

  const float ax = signed16(&imuSample[0]) / 4096.0f;
  const float ay = signed16(&imuSample[2]) / 4096.0f;
  const float az = signed16(&imuSample[4]) / 4096.0f;
  const float gx = signed16(&imuSample[8]) / 32.8f;
  const float gy = signed16(&imuSample[10]) / 32.8f;
  const float gz = signed16(&imuSample[12]) / 32.8f;
  const float accelNorm = sqrtf(ax * ax + ay * ay + az * az);
  const bool restingGravity = accelNorm > 0.95f && accelNorm < 1.05f;

  if (!yawCalibrated) {
    // Reject obvious motion. Slow constant rotation cannot be distinguished
    // from bias: the operator must keep the board still during calibration.
    if (!restingGravity || fabsf(gx) > 5 || fabsf(gy) > 5 || fabsf(gz) > 5 || dt > 0.1f) {
      yawCalibrationCount = 0;
      yawSum = yawSumSq = 0;
      return;
    }
    yawSum += gz;
    yawSumSq += gz * gz;
    if (++yawCalibrationCount >= 200) {
      const float mean = yawSum / yawCalibrationCount;
      const float variance = yawSumSq / yawCalibrationCount - mean * mean;
      if (variance > 0.25f) {
        yawCalibrationCount = 0;
        yawSum = yawSumSq = 0;
        return;
      }
      yawBias = mean;
      ++yawReference;
      yawDeg = 0;
      previousYawRate = gz - yawBias;
      yawCalibrated = yawContinuous = true;
      logf("Yaw calibrated: Z bias=%+.4f dps; zero set; reference=%lu.\n",
           yawBias, (unsigned long)yawReference);
    }
    return;
  }

  const float rate = gz - yawBias;
  if (dt > 0.1f || abs(int(signed16(&imuSample[12]))) >= 32760) {
    // Rotation during this interval is unknown. Start a NEW relative reference
    // after stationary calibration rather than leaving yaw permanently invalid.
    logLine("Yaw: sample gap / gyro clipping; reference lost, recalibrating");
    calibrateYaw();
    return;
  }
  if (yawContinuous && dt > 0) {
    yawDeg += 0.5f * (previousYawRate + rate) * dt;
    if (!isfinite(yawDeg)) { calibrateYaw(); return; }
    yawDeg = fmodf(yawDeg + 180.0f, 360.0f);
    if (yawDeg < 0) yawDeg += 360.0f;
    yawDeg -= 180.0f;
  }
  previousYawRate = rate;
}

void publishIMU() {
  // Keep the last successful sample and its timestamp on a short read failure.
  // CSV freshness still expires it; never stamp an old sample as new.
  if (imuReady && !imuSampleValid) return;
  ImuReading value;
  value.valid = imuReady && imuSampleValid;
  value.identity = imuIdentity;
  value.calibrating = !yawCalibrated;
  value.calibrationSamples = yawCalibrationCount;
  value.yawReference = yawReference;
  value.updatedMs = millis();
  if (value.valid) {
    const float ax = imuFilteredAccel[0];
    const float ay = imuFilteredAccel[1];
    const float az = imuFilteredAccel[2];
    value.ax = ax * 9.80665f; value.ay = ay * 9.80665f; value.az = az * 9.80665f;
    value.temperatureC = imuIdentity == 0x70
      ? signed16(&imuSample[6]) / 333.87f + 21.0f
      : signed16(&imuSample[6]) / 340.0f + 36.53f;
    const float norm = sqrtf(ax * ax + ay * ay + az * az);
    const float yz = sqrtf(ay * ay + az * az);
    const bool clipped = abs(int(signed16(&imuSample[0]))) >= 32760 ||
                         abs(int(signed16(&imuSample[2]))) >= 32760 ||
                         abs(int(signed16(&imuSample[4]))) >= 32760;
    value.tiltValid = norm >= 0.85f && norm <= 1.15f && !clipped;
    value.rollValid = value.tiltValid && yz >= 0.02f;
    value.rollDeg = atan2f(ay, az) * 57.2957795f;
    value.pitchDeg = atan2f(-ax, yz) * 57.2957795f;
    value.yawValid = yawCalibrated && yawContinuous;
    value.yawDeg = yawDeg;
  }
  Lock lock(stateMutex);
  dashboard.imu = value;
}

// ============================================================
// Commands / initialization
// ============================================================
void printHelp() {
  logLine("\n--- COMMANDS ---");
  logLine("1 : GA0 ON -> OP3 / CN15; other outputs OFF");
  logLine("2 : GA1 ON -> OP2 / CN16; other outputs OFF");
  logLine("3 : GA2 ON -> OP1 / CN17; other outputs OFF");
  logLine("4 : GA3 ON -> OP0 / CN18; other outputs OFF");
  logLine("0 : Stop light chase; all GA0..3 outputs OFF");
  logLine("l : Start light chase GA3 -> GA2 -> GA1 -> GA0 (500 ms)");
  logLine("t : Set RTC to sketch compile time");
  logLine("s : Scan I2C");
  logLine("i : Diagnose and reinitialize only MPU6050 / MPU6500");
  logLine("r : Reinitialize sensors and MCP; outputs OFF");
  logLine("h : Show commands");
  logLine("c : Calibrate yaw gyro bias; keep board still");
  logLine("z : Set relative yaw to zero while board is still");
}

void initializeDevices() {
  logLine("\n--- INITIALIZE ---");

  // Initialize MCP first so output commands start OFF.
  mcpReady = initMCP();
  logf("MCP [0x27]: %s\n",
                mcpReady ? "CONFIG OK; outputs OFF" : "INIT FAILED");

  for (uint8_t i = 0; i < 3; i++) {
    inaReady[i] = initINA(i);
  }

  // Preserve the RTC's existing date and time.
  logf("RTC [0x6F]: %s\n",
                ping(RTC_ADDR) ? "ACK" : "NOT FOUND");

  initIMU();
}

bool enqueueCommand(QueueHandle_t queue, char command) {
  if (xQueueSend(queue, &command, 0) == pdTRUE) return true;
  logf("Command %c rejected: queue full; retry\n", command);
  return false;
}

// UI adapters should dispatch commands through these same queues.
void dispatchCommand(char command) {
  if (command >= 'A' && command <= 'Z') command += 'a' - 'A';
  if (!supplyBusEnabled.load() && command != 'h') {
    if (command != '\n' && command != '\r') logLine("I2C paused: supply OFF/UNKNOWN; command rejected");
    return;
  }
  if ((command >= '0' && command <= '4') || command == 'l') {
    if (deviceEnabled(DEV_MCP)) enqueueCommand(mcpCommands, command);
  } else if (command == 'c' || command == 'z' || command == 'i') {
    if (deviceEnabled(DEV_IMU)) enqueueCommand(imuCommands, command);
  } else if (command == 's' || command == 't') {
    enqueueCommand(sensorCommands, command);
  } else if (command == 'r') {
    enqueueCommand(mcpCommands, command);
    enqueueCommand(imuCommands, command);
    enqueueCommand(sensorCommands, command);
  } else if (command == 'h') printHelp();
}

// Fixed-period scheduling without a burst of catch-up samples after a delay.
void waitPeriod(TickType_t &wake, uint32_t periodMs) {
  TickType_t now = xTaskGetTickCount();
  TickType_t period = pdMS_TO_TICKS(periodMs);
  if (TickType_t(now - wake) >= period) wake = now;
  vTaskDelayUntil(&wake, period);
}

void imuTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastRetry = millis();
  uint32_t generation = UINT32_MAX;
  for (;;) {
    if (!supplyBusEnabled.load() || !deviceEnabled(DEV_IMU)) {
      generation = UINT32_MAX;
      imuReady = imuSampleValid = yawContinuous = false;
      xQueueReset(imuCommands);
      waitPeriod(wake, 20);
      continue;
    }
    if (generation != supplyGeneration.load()) {
      generation = supplyGeneration.load();
      initIMU();
      lastRetry = millis();
    }
    char command;
    if (xQueueReceive(imuCommands, &command, 0) == pdTRUE) {
      if (command == 'i' || command == 'r') {
        { Lock lock(stateMutex); dashboard.imu.valid = false; }
        initIMU();
        lastRetry = millis();
      } else if (command == 'c' && imuReady) calibrateYaw();
      else if (command == 'z' && imuReady && yawCalibrated) {
        yawDeg = 0; ++yawReference; yawContinuous = true; imuSampleUs = 0;
        logLine("Yaw zero set; relative body Z");
      } else logLine("Yaw: IMU not ready or calibration incomplete");
    }
    if (!imuReady && millis() - lastRetry >= 3000) {
      initIMU();
      lastRetry = millis();
    }
    updateIMU();
    publishIMU();
    waitPeriod(wake, 20);
  }
}

void mcpTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastRead = 0, lastRetry = 0;
  uint32_t generation = UINT32_MAX;
  bool wasEnabled = true;
  for (;;) {
    if (!deviceEnabled(DEV_MCP)) {
      if (wasEnabled) {
        lightChaseRunning = false;
        // A final OFF command precedes stopping MCP polling.
        uint8_t actual = 0xFF;
        bool ok = supplyBusEnabled.load() && write8(MCP_ADDR, 0x14, 0) &&
                  read8(MCP_ADDR, 0x12, actual) && (actual & 15) == 0;
        mcpStopFailed.store(!ok);
        logLine(ok ? "MCP disabled: GA0..3 OFF readback verified" : "MCP disabled: GA OFF could not be confirmed");
        mcpReady = false; outputMask = 0;
        { Lock lock(stateMutex); dashboard.mcp.valid = false; dashboard.mcp.chase = false; }
      }
      wasEnabled = false; generation = UINT32_MAX; xQueueReset(mcpCommands);
      waitPeriod(wake, 10); continue;
    }
    wasEnabled = true;
    if (!supplyBusEnabled.load()) {
      mcpReady = lightChaseRunning = false;
      outputMask = 0;
      xQueueReset(mcpCommands);
      waitPeriod(wake, 10);
      continue;
    }
    if (generation != supplyGeneration.load() || (!mcpReady && uint32_t(millis() - lastRetry) >= 3000)) {
      generation = supplyGeneration.load();
      lastRetry = millis();
      xQueueReset(mcpCommands); // Never replay commands issued before a bus failure.
      const bool initialized = initMCP();
      mcpStopFailed.store(!initialized); // Restore outputs OFF; do not replay old light commands.
    }
    char command;
    if (xQueueReceive(mcpCommands, &command, 0) == pdTRUE) {
      if (command == 'r') { initMCP(); lastRetry = millis(); }
      else if (command == 'l') startLightChase();
      else if (command >= 'U' && command <= 'X') {
        lightChaseRunning = false;
        setOutputs(outputMask ^ uint8_t(1U << (command - 'U')));
      } else if (command >= 'A' && command <= 'P') {
        lightChaseRunning = false;
        setOutputs(uint8_t(command - 'A'));
      } else {
        lightChaseRunning = false;
        setOutputs(command == '0' ? 0 : uint8_t(1U << (command - '1')));
      }
      lastRead = millis();
      sampleMCP();
    }
    updateLightChase();
    if (millis() - lastRead >= 100) {
      lastRead = millis();
      sampleMCP();
    }
    waitPeriod(wake, 10);
  }
}

// Owned by supplyTask; voltage ADC is independent of the I2C bus.
void sampleSupply() {
  if (!SUPPLY_MONITOR_ENABLED) return; // Never touch ADC2 in manual mode.
  static bool present = false;
  constexpr unsigned SAMPLES = 16;
  uint32_t sumMv = 0;
  bool overRange = false;
  SupplyReading value;
  for (unsigned i = 0; i < SAMPLES; i++) {
    // Separate raw sample detects clipping; calibrated mV supplies scaling.
    const uint16_t raw = analogRead(VC12V_PIN);
    const uint32_t mv = analogReadMilliVolts(VC12V_PIN);
    // The higher software ceiling does not extend the ADC hardware range.
    const bool sampleOverRange = raw >= SUPPLY_ADC_SATURATION_RAW ||
                                 mv > SUPPLY_ADC_MAX_MV;
    overRange |= sampleOverRange;
    if (sampleOverRange) value.overRangeSamples++;
    if (raw < value.rawMin) value.rawMin = raw;
    if (raw > value.rawMax) value.rawMax = raw;
    sumMv += mv;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  value.updatedMs = millis();
  value.adcMv = float(sumMv) / SAMPLES;
  value.voltageV = value.adcMv * 0.001f * SUPPLY_DIVIDER_RATIO * SUPPLY_CALIBRATION;
  value.overRange = overRange;
  value.valid = !overRange;
  if (value.valid) {
    if (value.voltageV >= SUPPLY_ON_V) present = true;
    else if (value.voltageV < SUPPLY_OFF_V) present = false;
  }
  // Consumers must check valid/freshness before interpreting present.
  value.present = present;
  {
    Lock lock(stateMutex);
    dashboard.supply12v = value;
  }
  // Serialize gate changes with complete bus transactions. A transaction
  // already in progress finishes before OFF; no new transaction starts.
  const bool enable = value.valid && value.present;
  Lock busLock(i2cMutex);
  if (enable != supplyBusEnabled.load()) {
    xQueueReset(imuCommands);
    xQueueReset(mcpCommands);
    xQueueReset(sensorCommands);
    {
      Lock stateLock(stateMutex);
      dashboard.imu.valid = dashboard.imu.yawValid = false;
      dashboard.mcp.valid = false;
      dashboard.mcp.chase = false;
      dashboard.rtc.valid = false;
      for (auto &ina : dashboard.ina) ina.valid = false;
    }
    if (enable) supplyGeneration.fetch_add(1);
    supplyBusEnabled.store(enable);
    logLine(enable ? "Supply ON: I2C resumed; initializing devices"
                   : "Supply OFF/UNKNOWN: I2C paused");
  }
}

void supplyTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    sampleSupply();
    waitPeriod(wake, 50);
  }
}

void sensorTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t generation = UINT32_MAX;
  for (;;) {
    if (!supplyBusEnabled.load()) {
      xQueueReset(sensorCommands);
      for (auto &ready : inaReady) ready = false;
      waitPeriod(wake, 50);
      continue;
    }
    if (generation != supplyGeneration.load()) {
      generation = supplyGeneration.load();
      for (auto &ready : inaReady) ready = false;
      { Lock lock(stateMutex);
        dashboard.rtc.valid = false;
        for (auto &ina : dashboard.ina) ina.valid = false;
      }
    }
    char command;
    if (xQueueReceive(sensorCommands, &command, 0) == pdTRUE) {
      if (command == 's') scanI2C();
      else if (command == 't' && deviceEnabled(DEV_RTC)) {
        { Lock lock(stateMutex); dashboard.rtc.valid = false; }
        logLine(setRTCFromCompileTime() ? "RTC set OK" : "RTC set FAILED");
      } else if (command == 'r') {
        for (uint8_t i = 0; i < 3; i++) if (deviceEnabled(DEV_INA1 + i)) inaReady[i] = initINA(i);
      }
    }
    if (deviceEnabled(DEV_RTC)) sampleRTC();
    for (uint8_t i = 0; i < 3; i++) {
      if (deviceEnabled(DEV_INA1 + i)) sampleINA(i);
      else inaReady[i] = false;
    }
    waitPeriod(wake, 500);
  }
}

void printDashboard() {
  const DashboardState state = getDashboardSnapshot();
  const uint32_t now = millis();
  Serial.println("\n--- RTOS dashboard snapshot ---");
  if (!supplyBusEnabled.load()) Serial.println("DHT22: PAUSED (supply OFF/UNKNOWN)");
  else if (!state.dht.updatedMs) Serial.println("DHT22: WARMING UP (GPIO4; wait about 3 seconds)");
  else if (!state.dht.valid || uint32_t(now - state.dht.updatedMs) >= 6500)
    Serial.println("DHT22: READ FAILED / STALE; check GPIO4, power, GND and DATA pull-up");
  else Serial.printf("DHT22 T=%.2f C RH=%.2f %% (GPIO4)\n", state.dht.temperatureC,
                     state.dht.humidityPct);
  for (unsigned i = 0; i < 4; i++) {
    const auto &ds = state.ds[i];
    Serial.printf("DS%u GPIO%d: %s", i+1, DS_PINS[i], dsStatusName(ds.status));
    if (ds.valid) Serial.printf(" T=%.2f C", ds.temperatureC);
    Serial.printf(" power=%s age=%lu ms\n", ds.parasite ? "PARASITE" : "EXTERNAL/UNKNOWN", (unsigned long)(now - ds.updatedMs));
  }
  const auto &enc = state.encoder;
  if (enc.valid) {
    Serial.printf("Encoder count=%lld rate=%+.1f counts/s", (long long)enc.count, enc.countsPerSecond);
    if (enc.rpmValid) Serial.printf(" shaft=%+.2f RPM", enc.shaftRpm);
    else Serial.print(" RPM=CONFIG REQUIRED (PPR)");
    if (enc.speedValid) Serial.printf(" wheel=%+.2f RPM speed=%+.3f km/h", enc.wheelRpm, enc.speedKmh);
    else Serial.print(" speed=CONFIG REQUIRED (wheel/ratio)");
    Serial.println();
  } else Serial.println("Encoder: NOT READY");
  const auto &supply = state.supply12v;
  if (!SUPPLY_MONITOR_ENABLED) {
    Serial.println("12V supply: MONITOR DISABLED (GPIO15 ADC not read)");
    Serial.println("I2C: ENABLED (manual mode; no automatic supply pause)");
  } else if (uint32_t(now - supply.updatedMs) >= 2500) {
    Serial.println("12V supply: STALE");
  } else if (!supply.valid) {
    if (supply.overRange) {
      Serial.printf("12V supply: ADC OVER RANGE | GPIO%d=%.0f mV "
                    "| raw=%u..%u /4095 | high=%u/16\n",
                    VC12V_PIN, supply.adcMv, supply.rawMin, supply.rawMax,
                    supply.overRangeSamples);
    } else Serial.println("12V supply: NOT READY");
  } else {
    Serial.printf("12V supply=%.2f V | status=%s | GPIO%d=%.0f mV | raw=%u..%u\n",
                  supply.voltageV, supply.present ? "ON" : "OFF", VC12V_PIN, supply.adcMv,
                  supply.rawMin, supply.rawMax);
  }
  if (!supplyBusEnabled.load()) {
    Serial.printf("I2C: PAUSED (supply OFF/UNKNOWN); monitoring GPIO%d\n", VC12V_PIN);
    return;
  }
  const auto &rtc = state.rtc;
  if (rtc.valid && uint32_t(now - rtc.updatedMs) < 2500) {
    Serial.printf("RTC %04d-%02d-%02d %02d:%02d:%02d RUN=%u BAT_EN=%u PWRFAIL=%u\n",
      rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second,
      rtc.running, rtc.batteryEnabled, rtc.powerFail);
  } else Serial.println("RTC: INVALID / STALE");
  for (unsigned i = 0; i < 3; i++) {
    const auto &ina = state.ina[i];
    if (ina.valid && uint32_t(now - ina.updatedMs) < 2500)
      Serial.printf("INA CH%u V=%.4f V Vsh=%+.4f mV I=%+.4f A P=%+.4f W\n",
        i + 1, ina.voltageV, ina.shuntMv, ina.currentA, ina.powerW);
    else Serial.printf("INA CH%u: INVALID / STALE\n", i + 1);
  }
  const auto &mcp = state.mcp;
  if (mcp.valid && uint32_t(now - mcp.updatedMs) < 1000)
    Serial.printf("MCP GA=0x%X GB0..5=0x%02X chase=%u readback=%s\n",
      mcp.portA & 15, mcp.portB & 63, mcp.chase,
      (mcp.portA & 15) == mcp.commandedOutputs ? "OK" : "MISMATCH");
  else Serial.println("MCP: INVALID / STALE");
  const auto &imu = state.imu;
  if (!imu.valid || uint32_t(now - imu.updatedMs) > 200) {
    Serial.println("IMU: INVALID / STALE");
    return;
  }
  Serial.printf("IMU ID=0x%02X Accel[m/s^2] X=%+.3f Y=%+.3f Z=%+.3f Temp=%.2f C\n",
    imu.identity, imu.ax, imu.ay, imu.az, imu.temperatureC);
  if (imu.tiltValid) {
    if (imu.rollValid) Serial.printf("Roll=%+.2f deg ", imu.rollDeg);
    else Serial.print("Roll=UNDEFINED ");
    Serial.printf("Pitch=%+.2f deg\n", imu.pitchDeg);
  } else Serial.println("Tilt: INVALID (acceleration outside range)");
  if (imu.calibrating) Serial.printf("Yaw: CALIBRATING %u/200; still\n", imu.calibrationSamples);
  else if (imu.yawValid) Serial.printf("Yaw=%+.2f deg (relative Z; reference=%lu; body Z)\n",
                                      imu.yawDeg, (unsigned long)imu.yawReference);
  else Serial.println("Yaw: INVALID; keep still to recalibrate");
}


// Runtime interval shared by telemetry and SD capture. Wi-Fi task owns NVS writes.
constexpr uint32_t DEFAULT_LOG_INTERVAL_MS = 250;
constexpr uint32_t MIN_LOG_INTERVAL_MS = 100, MAX_LOG_INTERVAL_MS = 60000;
std::atomic<uint32_t> logIntervalMs{DEFAULT_LOG_INTERVAL_MS};
// Bit 0 = recording; remaining bits identify each physical START session.
// Only recordingControlTask writes this token; boot always starts stopped.
constexpr const char *FIRMWARE_BUILD = "20260909-ds-separate-ga-hz-1";
std::atomic<uint32_t> recordingToken{0};
std::atomic<uint32_t> controlHeartbeatMs{0}, controlStackFree{0};
std::atomic<uint32_t> startPresses{0}, stopPresses{0};
std::atomic<bool> startStable{false}, stopStable{false};
std::atomic<uint32_t> debugBlinkMs{DEFAULT_DEBUG_BLINK_MS};
Preferences logPreferences;
bool logPreferencesReady = false;
bool validLogInterval(uint32_t ms) { return ms >= MIN_LOG_INTERVAL_MS && ms <= MAX_LOG_INTERVAL_MS; }
void loadLogSettings() {
  logPreferencesReady = logPreferences.begin("dashboard", false);
  uint32_t saved = logPreferencesReady ? logPreferences.getUInt("log_ms", DEFAULT_LOG_INTERVAL_MS) : DEFAULT_LOG_INTERVAL_MS;
  logIntervalMs.store(validLogInterval(saved) ? saved : DEFAULT_LOG_INTERVAL_MS);
  uint32_t blink = logPreferencesReady ? logPreferences.getUInt("blink_ms", DEFAULT_DEBUG_BLINK_MS) : DEFAULT_DEBUG_BLINK_MS;
  debugBlinkMs.store(blink >= 40 && blink <= 2000 ? blink : DEFAULT_DEBUG_BLINK_MS);
  const uint32_t gaStep = logPreferencesReady ? logPreferences.getUInt("ga_step_ms", DEFAULT_GA_STEP_MS) : DEFAULT_GA_STEP_MS;
  gaStepMs.store(gaStep >= 50 && gaStep <= 5000 ? gaStep : DEFAULT_GA_STEP_MS);
  EncoderConfig config;
  if (logPreferencesReady && logPreferences.getBytesLength("encoder") == sizeof(config)) {
    logPreferences.getBytes("encoder", &config, sizeof(config));
    if (!isfinite(config.ppr) || config.ppr <= 0 || config.ppr > 100000 ||
        !isfinite(config.wheelMm) || config.wheelMm < 0 || config.wheelMm > 10000 ||
        !isfinite(config.ratio) || config.ratio < 0 || config.ratio > 10000) config = EncoderConfig{};
  }
  encoderConfig = config;
  ShuntConfig shunts;
  if (logPreferencesReady && logPreferences.getBytesLength("shunt") == sizeof(shunts)) {
    logPreferences.getBytes("shunt", &shunts, sizeof(shunts));
    for (auto &value : shunts.mohm) if (!validShuntMohm(value)) value = 1.0f;
  }
  shuntConfig = shunts;
  const uint32_t savedMask = logPreferencesReady ? logPreferences.getUInt("devices", ALL_DEVICES_MASK) : ALL_DEVICES_MASK;
  enabledDevices.store(savedMask & ALL_DEVICES_MASK & (ENABLE_SD_LOGGING ? ALL_DEVICES_MASK : ~(1U << DEV_SD)));
  logf("Device switches restored: mask=0x%04lX\n", (unsigned long)enabledDevices.load());
}
struct LogRecord {
  uint32_t recordingSession = 0;
  uint32_t sequence = 0, capturedMs = 0;
  uint32_t intervalMs = logIntervalMs.load();
  uint32_t devicesMask = enabledDevices.load();
  bool supplyEnabled = false;
  DashboardState state;
};
QueueHandle_t sdRecords = nullptr;
std::atomic<uint32_t> sdDropped{0}, sdWritten{0};
std::atomic<bool> sdMounted{false};
std::atomic<const char *> sdStatus{"WAITING_FOR_START"};
std::atomic<uint32_t> sdCardMiB{0};

String csvNumber(float value, bool valid) {
  return valid && isfinite(value) ? String(value, 4) : String("");
}
bool fresh(bool valid, uint32_t now, uint32_t updated, uint32_t limit) {
  return valid && uint32_t(now - updated) < limit;
}
String csvHeader() {
  String text = "sequence,uptime_ms,rtc_datetime,rtc_valid,rtc_age_ms,supply_on,";
  text += "dht22_temp_c,dht22_humidity_pct,dht22_valid,dht22_age_ms";
  for (unsigned i = 1; i <= 3; i++) {
    String prefix = ",ina" + String(i);
    text += prefix + "_voltage_v" + prefix + "_current_a" + prefix + "_power_w";
    text += prefix + "_shunt_mv" + prefix + "_valid" + prefix + "_age_ms" + prefix + "_rshunt_mohm";
  }
  for (unsigned i = 1; i <= 4; i++) {
    String prefix = ",ds" + String(i);
    text += prefix + "_temp_c" + prefix + "_valid" + prefix + "_age_ms" + prefix + "_status";
  }
  text += ",imu_ax_ms2,imu_ay_ms2,imu_az_ms2,imu_temp_c,roll_deg,pitch_deg,yaw_deg";
  text += ",imu_valid,tilt_valid,roll_valid,yaw_valid,imu_age_ms,encoder_count,encoder_counts_s,shaft_rpm,wheel_rpm,speed_kmh,encoder_valid,rpm_valid,speed_valid,encoder_age_ms,supply_voltage_v,supply_adc_mv,supply_valid,dropped_records,yaw_calibrating,yaw_calibration_samples,yaw_reference,record_interval_ms,enabled_devices_mask\n";
  return text;
}
String csvRow(const LogRecord &record) {
  const auto &s = record.state;
  const uint32_t now = record.capturedMs;
  String line;
  line.reserve(1200);
  line += String(record.sequence) + "," + String(now) + ",";
  const bool rtcOK = record.supplyEnabled && s.rtc.running &&
    fresh(s.rtc.valid, now, s.rtc.updatedMs, 2500);
  if (rtcOK) {
    char stamp[24];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
      s.rtc.year, s.rtc.month, s.rtc.day, s.rtc.hour, s.rtc.minute, s.rtc.second);
    line += stamp;
  }
  line += "," + String(rtcOK) + "," + String(uint32_t(now - s.rtc.updatedMs));
  line += ",";
  if (SUPPLY_MONITOR_ENABLED && fresh(s.supply12v.valid, now, s.supply12v.updatedMs, 1000))
    line += String(s.supply12v.present); // Blank when supply presence is unknown.
  const bool dhtOK = record.supplyEnabled && fresh(s.dht.valid, now, s.dht.updatedMs, 6500);
  line += "," + csvNumber(s.dht.temperatureC, dhtOK);
  line += "," + csvNumber(s.dht.humidityPct, dhtOK);
  line += "," + String(dhtOK) + "," + String(uint32_t(now - s.dht.updatedMs));
  for (const auto &ina : s.ina) {
    const bool ok = record.supplyEnabled && fresh(ina.valid, now, ina.updatedMs, 2500);
    line += "," + csvNumber(ina.voltageV, ok) + "," + csvNumber(ina.currentA, ok);
    line += "," + csvNumber(ina.powerW, ok) + "," + csvNumber(ina.shuntMv, ok);
    line += "," + String(ok) + "," + String(uint32_t(now - ina.updatedMs));
    line += "," + csvNumber(ina.rshuntMohm, ok);
  }
  for (const auto &ds : s.ds) {
    const bool ok = record.supplyEnabled && fresh(ds.valid, now, ds.updatedMs, 2500);
    line += "," + csvNumber(ds.temperatureC, ok) + "," + String(ok);
    line += "," + String(uint32_t(now - ds.updatedMs)) + "," + dsStatusName(ds.status);
  }
  const auto &imu = s.imu;
  const bool imuOK = record.supplyEnabled && fresh(imu.valid, now, imu.updatedMs, 200);
  line += "," + csvNumber(imu.ax, imuOK) + "," + csvNumber(imu.ay, imuOK);
  line += "," + csvNumber(imu.az, imuOK) + "," + csvNumber(imu.temperatureC, imuOK);
  line += "," + csvNumber(imu.rollDeg, imuOK && imu.rollValid);
  line += "," + csvNumber(imu.pitchDeg, imuOK && imu.tiltValid);
  line += "," + csvNumber(imu.yawDeg, imuOK && imu.yawValid);
  line += "," + String(imuOK) + "," + String(imuOK && imu.tiltValid);
  line += "," + String(imuOK && imu.rollValid) + "," + String(imuOK && imu.yawValid);
  line += "," + String(uint32_t(now - imu.updatedMs));
  const auto &enc = s.encoder;
  const bool encOK = record.supplyEnabled && fresh(enc.valid, now, enc.updatedMs, 500);
  char count[32];
  snprintf(count, sizeof(count), "%lld", (long long)enc.count);
  line += ","; if (encOK) line += count;
  line += "," + csvNumber(enc.countsPerSecond, encOK);
  line += "," + csvNumber(enc.shaftRpm, encOK && enc.rpmValid);
  line += "," + csvNumber(enc.wheelRpm, encOK && enc.speedValid);
  line += "," + csvNumber(enc.speedKmh, encOK && enc.speedValid);
  line += "," + String(encOK) + "," + String(encOK && enc.rpmValid);
  line += "," + String(encOK && enc.speedValid) + "," + String(uint32_t(now - enc.updatedMs));
  const bool supplyOK = SUPPLY_MONITOR_ENABLED && fresh(s.supply12v.valid, now, s.supply12v.updatedMs, 1000);
  line += "," + csvNumber(s.supply12v.voltageV, supplyOK);
  line += "," + csvNumber(s.supply12v.adcMv, supplyOK) + "," + String(supplyOK);
  line += "," + String(sdDropped.load());
  line += "," + String(imuOK && imu.calibrating);
  line += "," + String(imu.calibrationSamples) + "," + String(imu.yawReference) + "," + String(record.intervalMs) + "," + String(record.devicesMask) + "\n";
  return line;
}

void logCaptureTask(void *) {
  uint32_t sequence = 0, interval = logIntervalMs.load(), lastCapture = millis();
  uint32_t previousToken = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint32_t token = recordingToken.load(), now = millis();
    const uint32_t requested = logIntervalMs.load();
    if (token != previousToken || requested != interval) {
      previousToken = token; interval = requested; lastCapture = now;
    }
    if (!(token & 1) || uint32_t(now - lastCapture) < interval) continue;
    lastCapture += (uint32_t(now - lastCapture) / interval) * interval;
    LogRecord record;
    record.recordingSession = token >> 1;
    record.intervalMs = interval;
    record.sequence = ++sequence;
    record.state = getDashboardSnapshot();
    record.capturedMs = millis();
    record.supplyEnabled = supplyBusEnabled.load();
    if (recordingToken.load() != token) continue;
    if (ENABLE_SD_LOGGING && deviceEnabled(DEV_SD) && xQueueSend(sdRecords, &record, 0) != pdTRUE) sdDropped.fetch_add(1);
  }
}

struct DebouncedSwitch {
  int pin;
  bool stable = false, candidate = false;
  uint32_t changedMs = 0;
  void begin(int gpio) {
    pin = gpio; pinMode(pin, INPUT); // GPIO34/39 use the schematic's external pull-ups.
    stable = candidate = digitalRead(pin) == LOW;
    changedMs = millis();
  }
  bool pressed(uint32_t now) {
    const bool value = digitalRead(pin) == LOW;
    if (value != candidate) { candidate = value; changedMs = now; }
    if (candidate != stable && uint32_t(now - changedMs) >= SWITCH_DEBOUNCE_MS) {
      stable = candidate; return stable;
    }
    return false;
  }
};

void recordingControlTask(void *) {
  DebouncedSwitch start, stop;
  start.begin(RECORD_START_PIN); stop.begin(RECORD_STOP_PIN);
  bool led = false;
  uint32_t ledChanged = millis(), priorPeriod = debugBlinkMs.load();
  for (;;) {
    const uint32_t now = millis();
    const bool startPressed = start.pressed(now), stopPressed = stop.pressed(now);
    controlHeartbeatMs.store(now);
    startStable.store(start.stable); stopStable.store(stop.stable);
    if (startPressed) startPresses.fetch_add(1);
    if (stopPressed) stopPresses.fetch_add(1);
    if (now % 1000 < 10) controlStackFree.store(uxTaskGetStackHighWaterMark(nullptr));
    uint32_t token = recordingToken.load();
    // STOP wins when both are pressed. Holding START never restarts after STOP.
    if (stopPressed || stop.stable) token &= ~1u;
    else if (startPressed && !(token & 1)) token = ((token + 2) & ~1u) | 1u;
    const bool starting = (token & 1) && !(recordingToken.load() & 1);
    recordingToken.store(token);
    const uint32_t period = debugBlinkMs.load();
    if (!(token & 1)) {
      if (led) digitalWrite(DEBUG_LED_PIN, DEBUG_LED_OFF);
      led = false; ledChanged = now;
    } else if (starting || period != priorPeriod || uint32_t(now - ledChanged) >= period / 2) {
      led = starting || period != priorPeriod ? true : !led;
      digitalWrite(DEBUG_LED_PIN, led ? DEBUG_LED_ON : DEBUG_LED_OFF);
      ledChanged = now;
    }
    priorPeriod = period;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Set from the encoder specification/mechanics. Zero means unknown, so
// no fabricated RPM or speed is displayed/logged before calibration.
// PPR, wheel diameter and ratio are configured from the dashboard (encoderConfig).
constexpr int ENCODER_SIGN = 1; // Change to -1 to reverse reported direction
ESP32Encoder encoder;

void encoderTask(void *) {
  // Hardware PCNT continues counting during DHT/OneWire timing sections.
  ESP32Encoder::useInternalWeakPullResistors = puType::none; // PCB has 4.7k pull-ups
  encoder.attachFullQuad(32, 33);
  if (!encoder.isAttached()) {
    logLine("Encoder: PCNT attach FAILED");
    vTaskDelete(nullptr);
    return;
  }
  encoder.setFilter(100); // 1.25 us at 80 MHz APB
  encoder.clearCount();
  TickType_t wake = xTaskGetTickCount();
  int64_t previous = 0;
  uint32_t previousUs = micros();
  uint32_t generation = UINT32_MAX;
  for (;;) {
    waitPeriod(wake, 100);
    const int64_t count = encoder.getCount();
    const uint32_t nowUs = micros();
    EncoderReading value;
    value.updatedMs = millis();
    value.count = count * ENCODER_SIGN;
    const float dt = uint32_t(nowUs - previousUs) * 1e-6f;
    const uint32_t currentGeneration = supplyGeneration.load();
    const EncoderConfig config = getEncoderConfig();
    if (deviceEnabled(DEV_ENCODER) && supplyBusEnabled.load() && generation == currentGeneration && dt > 0 && dt < 0.5f) {
      value.valid = true; // Counter active; not proof of a connected sensor
      value.countsPerSecond = (count - previous) * ENCODER_SIGN / dt;
      value.rpmValid = config.ppr > 0;
      if (value.rpmValid) value.shaftRpm = value.countsPerSecond * 60.0f / (config.ppr * 4.0f);
      value.speedValid = value.rpmValid && config.wheelMm > 0 && config.ratio > 0;
      if (value.speedValid) {
        value.wheelRpm = value.shaftRpm / config.ratio;
        value.speedKmh = value.wheelRpm * (3.14159265359f * config.wheelMm / 1000.0f) * 0.06f;
      }
    }
    generation = deviceEnabled(DEV_ENCODER) ? currentGeneration : UINT32_MAX;
    previous = count;
    previousUs = nowUs;
    Lock lock(stateMutex);
    dashboard.encoder = value;
  }
}

constexpr int DHT_PIN = 4;
constexpr int SD_CS = 5, SD_SCK = 18, SD_MISO = 19, SD_MOSI = 23;
// Independent OneWire buses; one sensor per PCB connector.
OneWire dsWire0(DS_PINS[0]), dsWire1(DS_PINS[1]), dsWire2(DS_PINS[2]), dsWire3(DS_PINS[3]);
OneWire *dsWires[4] = {&dsWire0, &dsWire1, &dsWire2, &dsWire3};
DallasTemperature ds0(&dsWire0), ds1(&dsWire1), ds2(&dsWire2), ds3(&dsWire3);
DallasTemperature *dsSensors[4] = {&ds0, &ds1, &ds2, &ds3};
DeviceAddress dsAddress[4] = {};
bool dsFound[4] = {}, dsParasite[4] = {};
String dsMapJson() {
  DeviceAddress copy[4];
  { Lock lock(stateMutex); memcpy(copy, dsAddress, sizeof(copy)); }
  String json = "[";
  for (unsigned i = 0; i < 4; ++i) {
    if (i) json += ",";
    if (!copy[i][0]) { json += "null"; continue; }
    char rom[17];
    for (unsigned j = 0; j < 8; ++j) snprintf(rom + j*2, 3, "%02X", copy[i][j]);
    json += "\"" + String(rom) + "\"";
  }
  return json + "]";
}
// RMT captures DHT timing in hardware; no interrupt-masked pulse polling.
constexpr rmt_channel_t DHT_RMT_CHANNEL = RMT_CHANNEL_0;
RingbufHandle_t dhtRxBuffer = nullptr;
bool initDhtReceiver() {
  if (dhtRxBuffer) return true;
  rmt_config_t config = {};
  config.rmt_mode = RMT_MODE_RX;
  config.channel = DHT_RMT_CHANNEL;
  config.gpio_num = gpio_num_t(DHT_PIN);
  config.clk_div = 80; // 1 us per captured tick at 80 MHz APB.
  config.mem_block_num = 1;
  config.rx_config.filter_en = true;
  config.rx_config.filter_ticks_thresh = 80; // Reject sub-microsecond glitches.
  config.rx_config.idle_threshold = 200; // End frame on a stuck/idle line.
  if (rmt_config(&config) != ESP_OK ||
      rmt_driver_install(DHT_RMT_CHANNEL, 1024, 0) != ESP_OK) return false;
  if (rmt_get_ringbuf_handle(DHT_RMT_CHANNEL, &dhtRxBuffer) != ESP_OK || !dhtRxBuffer) {
    rmt_driver_uninstall(DHT_RMT_CHANNEL);
    dhtRxBuffer = nullptr;
    return false;
  }
  return true;
}

DhtReading decodeDhtFrame(const rmt_item32_t *items, size_t count) {
  DhtReading value;
  uint8_t data[5] = {};
  // Flatten pairs because the initial host pulse may change item alignment.
  bool responseLow = false, receiving = false, haveLow = false;
  unsigned bits = 0;
  for (size_t n = 0; n < count * 2 && bits < 40; ++n) {
    const auto &item = items[n / 2];
    const unsigned duration = n % 2 ? item.duration1 : item.duration0;
    const unsigned level = n % 2 ? item.level1 : item.level0;
    if (!duration) break;
    if (!receiving) {
      if (responseLow && level == 1 && duration >= 60 && duration <= 110) {
        receiving = true;
      }
      responseLow = level == 0 && duration >= 60 && duration <= 110;
      continue;
    }
    if (!haveLow) {
      if (level != 0 || duration < 30 || duration > 75) return value;
      haveLow = true;
    } else {
      if (level != 1 || duration < 15 || duration > 100) return value;
      data[bits / 8] = (data[bits / 8] << 1) | (duration > 45);
      ++bits; haveLow = false;
    }
  }
  if (bits != 40 || data[4] != uint8_t(data[0] + data[1] + data[2] + data[3])) return value;
  value.humidityPct = ((uint16_t(data[0]) << 8) | data[1]) * 0.1f;
  value.temperatureC = ((uint16_t(data[2] & 0x7f) << 8) | data[3]) * 0.1f;
  if (data[2] & 0x80) value.temperatureC = -value.temperatureC;
  value.valid = value.humidityPct >= 0 && value.humidityPct <= 100 &&
                value.temperatureC >= -40 && value.temperatureC <= 80;
  return value;
}

DhtReading readDht22Bounded() {
  DhtReading value;
  if (!initDhtReceiver()) return value;
  size_t bytes = 0;
  // Discard at most one old frame; the driver is stopped between reads.
  void *old = xRingbufferReceive(dhtRxBuffer, &bytes, 0);
  if (old) vRingbufferReturnItem(dhtRxBuffer, old);
  gpio_set_level(gpio_num_t(DHT_PIN), 1);
  gpio_set_direction(gpio_num_t(DHT_PIN), GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode(gpio_num_t(DHT_PIN), GPIO_PULLUP_ONLY);
  gpio_set_level(gpio_num_t(DHT_PIN), 0);
  vTaskDelay(pdMS_TO_TICKS(2)); // Start LOW >=1 ms, yielding CPU.
  if (rmt_rx_start(DHT_RMT_CHANNEL, true) != ESP_OK) {
    gpio_set_level(gpio_num_t(DHT_PIN), 1);
    return value;
  }
  gpio_set_level(gpio_num_t(DHT_PIN), 1); // Release open drain, retain RX input.
  auto *items = static_cast<rmt_item32_t *>(xRingbufferReceive(dhtRxBuffer, &bytes, pdMS_TO_TICKS(20)));
  rmt_rx_stop(DHT_RMT_CHANNEL);
  if (items) {
    value = decodeDhtFrame(items, bytes / sizeof(rmt_item32_t));
    vRingbufferReturnItem(dhtRxBuffer, items);
  }
  return value;
}

// DHT has a dedicated owner so DS discovery/conversion cannot delay its cadence.
void dhtTask(void *) {
  bool active = false;
  uint32_t generation = 0, lastRead = 0;
  unsigned failures = 0;
  for (;;) {
    if (!supplyBusEnabled.load() || !deviceEnabled(DEV_DHT)) {
      active = false;
      { Lock lock(stateMutex); dashboard.dht.valid = false; }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    const uint32_t currentGeneration = supplyGeneration.load();
    if (!active || generation != currentGeneration) {
      gpio_set_pull_mode(gpio_num_t(DHT_PIN), GPIO_PULLUP_ONLY);
      active = true; generation = currentGeneration; failures = 0;
      lastRead = millis();
      { Lock lock(stateMutex); dashboard.dht = DhtReading{}; }
    }
    if (uint32_t(millis() - lastRead) >= 2500) {
      DhtReading value = readDht22Bounded();
      lastRead = millis();
      value.updatedMs = lastRead;
      if (value.valid) failures = 0;
      else if (++failures >= 3) { gpio_set_pull_mode(gpio_num_t(DHT_PIN), GPIO_PULLUP_ONLY); failures = 0; }
      Lock lock(stateMutex);
      if (!supplyBusEnabled.load() || !deviceEnabled(DEV_DHT) ||
          generation != supplyGeneration.load()) dashboard.dht.valid = false;
      else if (value.valid) dashboard.dht = value;
      // Failed reads do not refresh the last successful timestamp.
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void temperatureTask(void *) {
  uint32_t generation = UINT32_MAX;
  for (;;) {
    const uint32_t cycleStart = millis(), cycleGeneration = supplyGeneration.load();
    if (!supplyBusEnabled.load()) {
      { Lock lock(stateMutex);
        for (unsigned i = 0; i < 4; ++i) { dashboard.ds[i].valid = false; dashboard.ds[i].status = DS_SUPPLY_PAUSED; }
      }
      generation = UINT32_MAX;
      vTaskDelay(pdMS_TO_TICKS(100)); continue;
    }
    if (generation != cycleGeneration) {
      generation = cycleGeneration; memset(dsFound, 0, sizeof(dsFound));
    }
    DsReading readings[4];
    bool requested[4] = {}, anyRequested = false;
    uint32_t started[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
      if (!deviceEnabled(DEV_DS1+i)) { readings[i].status = DS_DISABLED; dsFound[i] = false; continue; }
      if (!dsFound[i]) {
        dsSensors[i]->setOneWire(dsWires[i]);
        dsSensors[i]->begin();
        dsSensors[i]->setAutoSaveScratchPad(false);
        dsSensors[i]->setWaitForConversion(false);
        DeviceAddress rom = {};
        for (unsigned j = 0; j < dsSensors[i]->getDeviceCount(); ++j) {
          if (dsSensors[i]->getAddress(rom, j) && rom[0] == 0x28) { dsFound[i] = true; break; }
        }
        { Lock lock(stateMutex);
          if (dsFound[i]) memcpy(dsAddress[i], rom, 8); else memset(dsAddress[i], 0, 8);
        }
        if (!dsFound[i]) { readings[i].status = DS_NOT_FOUND; continue; }
        dsParasite[i] = dsSensors[i]->isParasitePowerMode();
        if (!dsSensors[i]->setResolution(dsAddress[i], 12)) {
          readings[i].status = DS_CONFIG_FAILED; dsFound[i] = false; continue;
        }
      }
      readings[i].parasite = dsParasite[i];
      const auto request = dsSensors[i]->requestTemperaturesByAddress(dsAddress[i]);
      if (!request.result) { readings[i].status = DS_START_FAILED; dsFound[i] = false; continue; }
      started[i] = request.timestamp; requested[i] = true; anyRequested = true;
    }
    if (anyRequested) vTaskDelay(pdMS_TO_TICKS(760));
    if (!supplyBusEnabled.load() || generation != supplyGeneration.load()) continue;
    for (unsigned i = 0; i < 4; ++i) {
      readings[i].updatedMs = millis();
      if (!deviceEnabled(DEV_DS1+i)) { readings[i].status = DS_DISABLED; dsFound[i] = false; continue; }
      if (!requested[i]) continue;
      bool complete = dsParasite[i] || dsSensors[i]->isConversionComplete();
      while (!complete && uint32_t(millis() - started[i]) < 900) {
        vTaskDelay(pdMS_TO_TICKS(5)); complete = dsSensors[i]->isConversionComplete();
      }
      if (!complete) { readings[i].status = DS_TIMEOUT; dsFound[i] = false; continue; }
      const float t = dsSensors[i]->getTempC(dsAddress[i]);
      readings[i].valid = t != DEVICE_DISCONNECTED_C && isfinite(t) && t >= -55 && t <= 125;
      readings[i].temperatureC = t;
      readings[i].status = readings[i].valid ? DS_READY : DS_READ_FAILED;
      if (!readings[i].valid) dsFound[i] = false;
    }
    { Lock lock(stateMutex);
      for (unsigned i = 0; i < 4; ++i) {
        if (!deviceEnabled(DEV_DS1+i)) { readings[i].valid = false; readings[i].status = DS_DISABLED; }
        dashboard.ds[i] = readings[i];
      }
    }
    const uint32_t elapsed = millis() - cycleStart;
    if (elapsed < 1000) vTaskDelay(pdMS_TO_TICKS(1000 - elapsed));
  }
}

// SD is on the backed-up 3V3SUP rail in the schematic. Keep logging even
// when main supply is OFF; I2C values are blank and supply_on=0 in those rows.
// This task alone owns SPI/SD. No disk operation holds the dashboard mutex.
void sdWriterTask(void *) {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  bool mounted = false;
  char path[32] = {};
  uint32_t nextAttempt = 0;
  unsigned session = 1;
  uint32_t activeRecordingSession = 0;
  for (;;) {
    LogRecord record;
    if (xQueueReceive(sdRecords, &record, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    if (!deviceEnabled(DEV_SD)) continue;
    if (record.recordingSession != activeRecordingSession) {
      activeRecordingSession = record.recordingSession;
      mounted = false; sdMounted.store(false); nextAttempt = 0;
    }
    if (!mounted) {
      if (int32_t(millis() - nextAttempt) < 0) { sdDropped.fetch_add(1); continue; }
      sdStatus.store("MOUNTING");
      sdCardMiB.store(0);
      SD.end();
      mounted = SD.begin(SD_CS, SPI, 4000000);
      if (mounted && SD.cardType() != CARD_NONE) {
        sdCardMiB.store(uint32_t(SD.cardSize() / (1024ULL * 1024ULL)));
        if (!SD.exists("/logs")) mounted = SD.mkdir("/logs");
        bool chosen = false;
        while (mounted && session <= 99999) {
          snprintf(path, sizeof(path), "/logs/log_%05u.csv", session++);
          if (!SD.exists(path)) { chosen = true; break; }
        }
        mounted = mounted && chosen;
        if (mounted) {
          File file = SD.open(path, FILE_WRITE);
          String header = csvHeader();
          mounted = file && file.print(header) == header.length();
          if (file) { file.flush(); mounted = mounted && !file.getWriteError(); file.close(); }
        }
      } else mounted = false;
      sdMounted.store(mounted);
      if (!mounted) {
        sdStatus.store("MOUNT_OR_CREATE_FAILED");
        logLine("SD: mount/create failed; check FAT32 filesystem, card and wiring; retry in 5 seconds");
        nextAttempt = millis() + 5000;
        sdDropped.fetch_add(1);
        continue;
      }
      sdStatus.store("READY");
      logf("SD: recording %s; interval=%lu ms\n", path, (unsigned long)record.intervalMs);
    }
    String line = csvRow(record);
    File file = SD.open(path, FILE_APPEND);
    bool ok = file && file.print(line) == line.length();
    if (file) { file.flush(); ok = ok && !file.getWriteError(); file.close(); }
    if (ok) { sdWritten.fetch_add(1); sdStatus.store("READY"); }
    else {
      mounted = false;
      sdMounted.store(false);
      sdDropped.fetch_add(1);
      nextAttempt = millis() + 5000;
      sdStatus.store("WRITE_FAILED");
      logLine("SD: write failed; record lost, retry with NEW file in 5 seconds");
    }
  }
}


// The Wi-Fi server owns network IO. It never reads I2C directly.
void wifiTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WebServer server(80);
  const char *requestHeaders[] = {"X-Dashboard-Request"};
  server.collectHeaders(requestHeaders, 1);
  server.on("/api/health", HTTP_GET, [&]() {
    String json; json.reserve(900);
    json = "{\"reset_reason\":\"";
    json += resetReasonName(esp_reset_reason());
    json += "\",\"uptime_ms\":" + String(millis());
    json += ",\"free_heap_bytes\":" + String(ESP.getFreeHeap());
    json += ",\"min_free_heap_bytes\":" + String(ESP.getMinFreeHeap());
    json += ",\"largest_free_block_bytes\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const uint32_t token = recordingToken.load();
    json += ",\"firmware\":\"" + String(FIRMWARE_BUILD) + "\"";
    json += ",\"recording\":" + String(token & 1 ? "true" : "false");
    json += ",\"recording_session\":" + String(token >> 1);
    json += ",\"sw1_gpio\":" + String(RECORD_START_PIN);
    json += ",\"sw2_gpio\":" + String(RECORD_STOP_PIN);
    json += ",\"gpio34_raw\":" + String(digitalRead(34));
    json += ",\"gpio39_raw\":" + String(digitalRead(39));
    json += ",\"gpio36_raw\":" + String(digitalRead(36));
    json += ",\"sw1_pressed\":" + String(startStable.load() ? "true" : "false");
    json += ",\"sw2_pressed\":" + String(stopStable.load() ? "true" : "false");
    json += ",\"start_presses\":" + String(startPresses.load());
    json += ",\"stop_presses\":" + String(stopPresses.load());
    json += ",\"control_age_ms\":" + String(uint32_t(millis() - controlHeartbeatMs.load()));
    json += ",\"control_stack_free_bytes\":" + String(controlStackFree.load());
    json += ",\"ds_pins\":[27,16,13,14],\"ds_rom\":" + dsMapJson();
    json += ",\"ga_step_ms\":" + String(gaStepMs.load());
    json += ",\"sd_available\":" + String(ENABLE_SD_LOGGING ? "true" : "false");
    json += ",\"sd_status\":\"" + String(ENABLE_SD_LOGGING && deviceEnabled(DEV_SD) ? sdStatus.load() : "DISABLED") + "\"";
    json += ",\"sd_mounted\":" + String(sdMounted.load() ? "true" : "false");
    json += ",\"sd_card_mib\":" + String(sdCardMiB.load());
    json += ",\"sd_written\":" + String(sdWritten.load());
    json += ",\"sd_dropped\":" + String(sdDropped.load());
    json += ",\"blink_ms\":" + String(debugBlinkMs.load());
    json += ",\"web_core\":" + String(WEB_CORE) + ",\"sensor_core\":" + String(SENSOR_CORE);
    json += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.on("/api/led", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") { server.send(403, "text/plain", "Dashboard request required"); return; }
    const String raw = server.arg("blink_ms");
    bool digits = raw.length() > 0 && raw.length() <= 4;
    for (unsigned i = 0; i < raw.length(); ++i) digits &= raw[i] >= '0' && raw[i] <= '9';
    const uint32_t value = digits ? raw.toInt() : 0;
    if (value < 40 || value > 2000) { server.send(400, "text/plain", "Use 40-2000 ms per full blink cycle"); return; }
    if (value != debugBlinkMs.load()) {
      if (!logPreferencesReady || logPreferences.putUInt("blink_ms", value) != sizeof(uint32_t)) {
        server.send(500, "text/plain", "Could not save LED setting"); return;
      }
      debugBlinkMs.store(value);
    }
    server.send(200, "application/json", String("{\"blink_ms\":") + debugBlinkMs.load() + "}");
  });
  server.on("/api/settings", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") {
      server.send(403, "text/plain", "Dashboard request required"); return;
    }
    const String raw = server.arg("interval_ms");
    bool digits = raw.length() > 0 && raw.length() <= 5;
    for (unsigned i = 0; i < raw.length(); ++i) digits &= raw[i] >= '0' && raw[i] <= '9';
    const uint32_t value = digits ? uint32_t(raw.toInt()) : 0;
    if (!validLogInterval(value)) {
      server.send(400, "text/plain", "Use an integer from 100 to 60000 ms"); return;
    }
    if (value != logIntervalMs.load()) {
      if (!logPreferencesReady || logPreferences.putUInt("log_ms", value) != sizeof(uint32_t)) {
        server.send(500, "text/plain", "Could not save interval; setting unchanged"); return;
      }
      logIntervalMs.store(value);
      logf("Recording interval saved: %lu ms\n", (unsigned long)value);
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", String("{\"interval_ms\":") + logIntervalMs.load() + "}");
  });
  const String bootId = String(esp_random(), HEX);
  uint32_t webSequence = 0;
  server.on("/api/shunt", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") { server.send(403, "text/plain", "Dashboard request required"); return; }
    ShuntConfig config;
    for (unsigned i = 0; i < 3; ++i) {
      const String raw = server.arg(String("ch") + (i + 1)); char *end = nullptr;
      config.mohm[i] = strtof(raw.c_str(), &end);
      if (!raw.length() || end == raw.c_str() || *end || !validShuntMohm(config.mohm[i])) {
        server.send(400, "text/plain", "Each Rshunt must be 0.01..1000 milliohms"); return;
      }
    }
    const ShuntConfig old = getShuntConfig();
    bool changed = false;
    for (unsigned i = 0; i < 3; ++i) changed |= old.mohm[i] != config.mohm[i];
    if (changed) {
      if (!logPreferencesReady || logPreferences.putBytes("shunt", &config, sizeof(config)) != sizeof(config)) {
        server.send(500, "text/plain", "Could not save Rshunt; setting unchanged"); return;
      }
      { Lock lock(stateMutex);
        shuntConfig = config;
        for (unsigned i = 0; i < 3; ++i) if (old.mohm[i] != config.mohm[i]) dashboard.ina[i].valid = false;
      }
      logf("INA Rshunt saved [mOhm]: %.4f / %.4f / %.4f\n", config.mohm[0], config.mohm[1], config.mohm[2]);
    }
    server.send(200, "application/json", "{\"saved\":true}");
  });
  server.on("/api/device", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") { server.send(403, "text/plain", "Dashboard request required"); return; }
    int device = -1;
    for (unsigned i = 0; i < DEVICE_COUNT; ++i) if (server.arg("device") == deviceNames[i]) device = i;
    const String enabled = server.arg("enabled");
    if (device < 0 || (enabled != "0" && enabled != "1")) { server.send(400, "text/plain", "Invalid device/state"); return; }
    // Reject commands based on an old page/boot rather than changing an unintended state.
    if (server.arg("boot") != bootId || server.arg("revision") != String(deviceControlRevision)) {
      server.send(409, "text/plain", "Device state changed; wait for fresh status and try again"); return;
    }
    const uint32_t oldMask = enabledDevices.load();
    if (device == DEV_SD && enabled == "1" && !ENABLE_SD_LOGGING) {
      server.send(409, "text/plain", "SD disabled: set ENABLE_SD_LOGGING=true in include/runtime_config.h after installing a card"); return;
    }
    const uint32_t newMask = enabled == "1" ? oldMask | (1U << device) : oldMask & ~(1U << device);
    if (oldMask != newMask) {
      if (!logPreferencesReady || logPreferences.putUInt("devices", newMask) != sizeof(uint32_t)) {
        server.send(500, "text/plain", "Could not save device state; setting unchanged"); return;
      }
      enabledDevices.store(newMask);
      ++deviceControlRevision;
    }
    if (enabled == "0") {
      Lock lock(stateMutex);
      if (device <= DEV_INA3) dashboard.ina[device].valid = false;
      else if (device >= DEV_DS1 && device <= DEV_DS4) dashboard.ds[device - DEV_DS1].valid = false;
      else if (device == DEV_IMU) dashboard.imu.valid = dashboard.imu.yawValid = false;
      else if (device == DEV_RTC) dashboard.rtc.valid = false;
      else if (device == DEV_MCP) dashboard.mcp.valid = false;
      else if (device == DEV_DHT) dashboard.dht.valid = false;
      else if (device == DEV_ENCODER) dashboard.encoder.valid = dashboard.encoder.rpmValid = dashboard.encoder.speedValid = false;
    }
    logf("Device %s: sampling %s\n", deviceNames[device], enabled == "1" ? "enabled" : "disabled");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", String("{\"boot\":\"") + bootId + "\",\"devices_mask\":" + enabledDevices.load() + ",\"control_revision\":" + deviceControlRevision + "}");
  });
  server.on("/api/ga-frequency", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") { server.send(403, "text/plain", "Dashboard request required"); return; }
    const String raw = server.arg("hz"); char *end = nullptr;
    const float hz = strtof(raw.c_str(), &end);
    const uint32_t period = gaPeriodForHz(hz);
    if (!raw.length() || raw.length() > 16 || end == raw.c_str() || *end || !period) {
      server.send(400, "text/plain", "GA step frequency must be 0.2-20 Hz"); return;
    }
    if (period != gaStepMs.load()) {
      if (!logPreferencesReady || logPreferences.putUInt("ga_step_ms", period) != sizeof(uint32_t)) {
        server.send(500, "text/plain", "Could not save GA frequency"); return;
      }
      gaStepMs.store(period);
    }
    server.send(200, "application/json", String("{\"ga_step_ms\":") + gaStepMs.load() + "}");
  });
  server.on("/api/ga", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") { server.send(403, "text/plain", "Dashboard request required"); return; }
    if (!deviceEnabled(DEV_MCP) || !supplyBusEnabled.load()) { server.send(409, "text/plain", "MCP is disabled or supply paused"); return; }
    char command = 0;
    if (server.arg("mode") == "chase") command = 'l';
    else if (server.arg("mode") == "toggle") {
      const String channel = server.arg("channel");
      if (channel.length() == 1 && channel[0] >= '0' && channel[0] <= '3') command = 'U' + channel[0] - '0';
    } else {
      const String raw = server.arg("mask");
      bool digits = raw.length() > 0 && raw.length() <= 2;
      for (unsigned i = 0; i < raw.length(); ++i) digits &= raw[i] >= '0' && raw[i] <= '9';
      if (digits && raw.toInt() <= 15) command = 'A' + raw.toInt();
    }
    if (!command) { server.send(400, "text/plain", "Invalid GA command"); return; }
    if (!enqueueCommand(mcpCommands, command)) { server.send(503, "text/plain", "Command queue full"); return; }
    server.send(202, "application/json", "{\"queued\":true}");
  });
  server.on("/api/encoder", HTTP_POST, [&]() {
    if (server.header("X-Dashboard-Request") != "1") { server.send(403, "text/plain", "Dashboard request required"); return; }
    EncoderConfig config;
    float *fields[] = {&config.ppr, &config.wheelMm, &config.ratio};
    const char *names[] = {"ppr", "wheel_mm", "ratio"};
    for (unsigned i = 0; i < 3; ++i) {
      const String raw = server.arg(names[i]); char *end = nullptr;
      *fields[i] = strtof(raw.c_str(), &end);
      if (!raw.length() || end == raw.c_str() || *end || !isfinite(*fields[i]) || *fields[i] <= 0 || *fields[i] > (i == 0 ? 100000 : 10000)) {
        server.send(400, "text/plain", "PPR: 1..100000; wheel mm and ratio: >0..10000"); return;
      }
    }
    const EncoderConfig old = getEncoderConfig();
    if (floorf(config.ppr) != config.ppr) { server.send(400, "text/plain", "PPR must be an integer"); return; }
    if (old.ppr != config.ppr || old.wheelMm != config.wheelMm || old.ratio != config.ratio) {
      if (!logPreferencesReady || logPreferences.putBytes("encoder", &config, sizeof(config)) != sizeof(config)) {
        server.send(500, "text/plain", "Could not save encoder config"); return;
      }
      { Lock lock(stateMutex); encoderConfig = config; dashboard.encoder.rpmValid = dashboard.encoder.speedValid = false; }
    }
    server.send(200, "application/json", "{\"saved\":true}");
  });
  server.on("/", HTTP_GET, [&]() {
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char *>(DASHBOARD_GZIP), sizeof(DASHBOARD_GZIP));
  });
  server.on("/api/state", HTTP_GET, [&]() {
    LogRecord record;
    record.state = getDashboardSnapshot();
    record.capturedMs = millis();
    record.sequence = ++webSequence;
    record.supplyEnabled = supplyBusEnabled.load();
    const auto &power = record.state.supply12v;
    const bool known = fresh(power.valid, record.capturedMs, power.updatedMs, 1000);
    String header = csvHeader(); header.trim();
    String row = csvRow(record); row.trim();
    // Header and row contain only fixed field names, numeric fields and RTC
    // digits/separators, never user input or quotes.
    String json; json.reserve(header.length() + row.length() + 800);
    json = "{\"boot\":\"" + bootId + "\",\"supply\":\"";
    json += !SUPPLY_MONITOR_ENABLED ? "manual" : known ? (power.present ? "on" : "off") : "unknown";
    json += "\",\"interval_ms\":" + String(record.intervalMs) + ",\"header\":\"" + header + "\",\"csv\":\"" + row + "\",\"devices_mask\":" + String(enabledDevices.load()) + ",\"control_revision\":" + String(deviceControlRevision);
    const EncoderConfig config = getEncoderConfig();
    const ShuntConfig shunts = getShuntConfig();
    const uint32_t token = recordingToken.load();
    json += ",\"recording\":" + String((token & 1) ? "true" : "false");
    json += ",\"recording_session\":" + String(token >> 1);
    json += ",\"ds_pins\":[27,16,13,14],\"ds_rom\":" + dsMapJson();
    json += ",\"ga_step_ms\":" + String(gaStepMs.load());
    json += ",\"sd_available\":" + String(ENABLE_SD_LOGGING ? "true" : "false");
    json += ",\"sd_status\":\"" + String(ENABLE_SD_LOGGING && deviceEnabled(DEV_SD) ? sdStatus.load() : "DISABLED") + "\"";
    json += ",\"sd_mounted\":" + String(sdMounted.load() ? "true" : "false");
    json += ",\"sd_card_mib\":" + String(sdCardMiB.load());
    json += ",\"sd_written\":" + String(sdWritten.load());
    json += ",\"sd_dropped\":" + String(sdDropped.load());
    json += ",\"blink_ms\":" + String(debugBlinkMs.load());
    json += ",\"shunt_mohm\":[" + String(shunts.mohm[0], 4) + "," + String(shunts.mohm[1], 4) + "," + String(shunts.mohm[2], 4) + "]";
    const auto &mcp = record.state.mcp;
    json += ",\"encoder_config\":{\"ppr\":" + String(config.ppr, 4) + ",\"wheel_mm\":" + String(config.wheelMm, 4) + ",\"ratio\":" + String(config.ratio, 4) + "}";
    json += ",\"ga\":{\"valid\":" + String(mcp.valid && fresh(mcp.valid, record.capturedMs, mcp.updatedMs, 1000) ? "true" : "false");
    json += ",\"mask\":" + String(mcp.portA & 15) + ",\"commanded\":" + String(mcp.commandedOutputs) + ",\"chase\":" + String(mcp.chase ? "true" : "false");
    json += ",\"stop_failed\":" + String(mcpStopFailed.load() ? "true" : "false") + "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.onNotFound([&]() { server.send(404, "text/plain", "Not found"); });
  bool started = false;
  uint32_t lastRetry = millis();
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!started) {
        server.begin(); started = true;
        logf("Wi-Fi dashboard: http://%s (same Wi-Fi network)\n", WiFi.localIP().toString().c_str());
      }
      server.handleClient();
    } else {
      if (started) { server.stop(); started = false; }
      if (millis() - lastRetry >= 15000) {
        logLine("Wi-Fi: waiting for 2.4 GHz network; reconnecting");
        WiFi.reconnect(); lastRetry = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Desktop protocol: @H,<CSV header> and @D,<on|off|unknown|manual>,<CSV row>.
// One UART writer owns both telemetry and human-readable diagnostics.
void consoleTask(void *) {
  uint32_t lastPrint = 0, lastHeader = 0;
  uint32_t telemetrySequence = 0;
  bool streaming = false;
  String lastWebIp;
  for (;;) {
    if (SERIAL_IP_ONLY) {
      // Print once per connection/IP change; sensor data stays on the web.
      if (WiFi.status() == WL_CONNECTED) {
        const String ip = WiFi.localIP().toString();
        if (ip != "0.0.0.0" && ip != lastWebIp) {
          Serial.printf("http://%s/\n", ip.c_str());
          lastWebIp = ip;
        }
      } else {
        lastWebIp = "";
      }
      // Ignore legacy telemetry commands in IP-only mode.
      for (unsigned i = 0; i < 64 && Serial.available(); ++i) Serial.read();
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    for (unsigned i = 0; i < 16 && Serial.available(); i++) {
      const char command = char(Serial.read());
      if (command == 'j' || command == 'J') {
        streaming = true;
        lastHeader = millis() - 2000;
      } else if (command == 'v' || command == 'V') streaming = false;
      else dispatchCommand(command);
    }
    LogMessage message;
    for (unsigned i = 0; i < 16 && xQueueReceive(logQueue, &message, 0) == pdTRUE; i++)
      if (!streaming) Serial.print(message.text);
    if (streaming) {
      if (millis() - lastHeader >= 2000) {
        lastHeader = millis();
        Serial.print("@H,");
        Serial.print(csvHeader());
      }
      if (millis() - lastPrint >= 200) {
        lastPrint = millis();
        LogRecord record;
        record.sequence = ++telemetrySequence;
        record.state = getDashboardSnapshot();
        record.capturedMs = millis();
        record.supplyEnabled = supplyBusEnabled.load();
        const auto &power = record.state.supply12v;
        const bool known = fresh(power.valid, record.capturedMs, power.updatedMs, 1000);
        Serial.print("@D,");
        Serial.print(!SUPPLY_MONITOR_ENABLED ? "manual," : known ? (power.present ? "on," : "off,") : "unknown,");
        Serial.print(csvRow(record));
      }
    } else if (millis() - lastPrint >= 1000) {
      lastPrint = millis();
      printDashboard();
      Serial.printf("SD mounted=%u written=%lu dropped=%lu\n", sdMounted.load(),
                    (unsigned long)sdWritten.load(), (unsigned long)sdDropped.load());
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  pinMode(DEBUG_LED_PIN, OUTPUT);
  digitalWrite(DEBUG_LED_PIN, DEBUG_LED_OFF);
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(1000);
  const esp_reset_reason_t reason = esp_reset_reason();
  if (reason != ESP_RST_POWERON && reason != ESP_RST_EXT && reason != ESP_RST_DEEPSLEEP)
    Serial.printf("RESET: %s (%d)\n", resetReasonName(reason), int(reason));

  logLine("\n========================================");
  logLine("ESP32 DASHBOARD - FREERTOS 6 DEVICE TEST");
  logLine("INA226 x3 + MCP7940N + MCP23017 + MPU6050/MPU6500");
  logLine("========================================");

  logf("SDA=%d SCL=%d I2C=%lu Hz\n",
                SDA_PIN, SCL_PIN,
                static_cast<unsigned long>(I2C_SPEED));

  if (!Wire.begin(SDA_PIN, SCL_PIN, I2C_SPEED)) {
    logLine("I2C initialization FAILED");

    while (true) {
      delay(1000);
    }
  }

  Wire.setTimeOut(50);
  if (SUPPLY_MONITOR_ENABLED) {
    pinMode(VC12V_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(VC12V_PIN, ADC_11db);
  }

  i2cMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();
  imuCommands = xQueueCreate(8, sizeof(char));
  mcpCommands = xQueueCreate(8, sizeof(char));
  sensorCommands = xQueueCreate(8, sizeof(char));
  logQueue = xQueueCreate(32, sizeof(LogMessage));
  if (!i2cMutex || !stateMutex || !imuCommands || !mcpCommands ||
      !sensorCommands || !logQueue) {
    Serial.println("FATAL: RTOS allocation failed; reset board");
    while (true) delay(1000);
  }
  if (SUPPLY_MONITOR_ENABLED) sampleSupply();
  else {
    // Initialize the sensor generation before any device owner task starts.
    supplyGeneration.fetch_add(1);
    supplyBusEnabled.store(true);
    logLine("Supply monitor DISABLED: GPIO15 ADC off; sensors enabled; Wi-Fi stays on");
  }
  loadLogSettings();
  logf("Recording interval: %lu ms\n", (unsigned long)logIntervalMs.load());
  printHelp();

  // Device owners initialize only after the supply gate opens.
  // ESP-IDF task stack sizes are in bytes. Start console last.
  sdRecords = xQueueCreate(12, sizeof(LogRecord));
  if (!sdRecords) {
    Serial.println("FATAL: SD record queue allocation failed");
    while (true) delay(1000);
  }
  TaskHandle_t handles[12] = {};
  bool ok = true;
  if (SUPPLY_MONITOR_ENABLED)
    ok = xTaskCreatePinnedToCore(supplyTask, "supply", 3072, nullptr, 4, &handles[4], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, 3, &handles[0], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(mcpTask, "mcp", 3072, nullptr, 2, &handles[1], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(sensorTask, "sensors", 4096, nullptr, 1, &handles[2], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(dhtTask, "dht", 3072, nullptr, 1, &handles[10], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(temperatureTask, "temperature", 4096, nullptr, 1, &handles[5], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(recordingControlTask, "recordControl", 3072, nullptr, 2, &handles[11], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(logCaptureTask, "logCapture", 4096, nullptr, 2, &handles[6], SENSOR_CORE) == pdPASS;
  if (ok && ENABLE_SD_LOGGING) ok = xTaskCreatePinnedToCore(sdWriterTask, "sdWriter", 8192, nullptr, 1, &handles[7], WEB_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(encoderTask, "encoder", 4096, nullptr, 2, &handles[8], SENSOR_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(wifiTask, "wifiWeb", 12288, nullptr, 1, &handles[9], WEB_CORE) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(consoleTask, "console", 6144, nullptr, 1, &handles[3], WEB_CORE) == pdPASS;
  if (!ok) {
    Serial.println("FATAL: task creation failed; restarting");
    delay(100);
    ESP.restart();
  }
}

void loop() {
  // Arduino loop task has no application work; workers own all devices.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
