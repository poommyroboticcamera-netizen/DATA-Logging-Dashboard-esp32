// Compiled after verbatim firmware declarations/functions in generated.cpp.
DashboardState healthyState(uint32_t updatedMs) {
  DashboardState s;
  s.rtc.valid = s.rtc.running = true;
  s.rtc.updatedMs = updatedMs;
  s.rtc.year = 2026; s.rtc.month = 9; s.rtc.day = 8;
  s.rtc.hour = 16; s.rtc.minute = 42; s.rtc.second = 31;
  s.dht.valid = true; s.dht.updatedMs = updatedMs;
  s.dht.temperatureC = 24.5f; s.dht.humidityPct = 61.75f;
  for (unsigned i = 0; i < 3; ++i) {
    auto &v = s.ina[i];
    v.valid = true; v.updatedMs = updatedMs;
    v.voltageV = 12.25f + i; v.currentA = -1.25f - i;
    v.powerW = 18.5f + i; v.shuntMv = -0.25f - i;
    v.rshuntMohm = 1.0f + i * 0.5f;
  }
  for (unsigned i = 0; i < 4; ++i) {
    auto &v = s.ds[i];
    v.valid = true; v.updatedMs = updatedMs;
    v.temperatureC = 20.25f + i; v.status = DS_READY;
  }
  auto &v = s.imu;
  v.valid = v.tiltValid = v.rollValid = v.yawValid = true;
  v.calibrating = false; v.updatedMs = updatedMs;
  v.ax = 1.25f; v.ay = -2.5f; v.az = 9.75f; v.temperatureC = 32.5f;
  v.rollDeg = -10.25f; v.pitchDeg = 5.5f; v.yawDeg = 123.75f;
  v.calibrationSamples = 200; v.yawReference = 7;
  auto &e = s.encoder;
  e.valid = e.rpmValid = e.speedValid = true; e.updatedMs = updatedMs;
  e.count = 1234567890123LL; e.countsPerSecond = 1440;
  e.shaftRpm = e.wheelRpm = 60; e.speedKmh = 1.885f;
  s.supply12v.valid = s.supply12v.present = true;
  s.supply12v.updatedMs = updatedMs;
  s.supply12v.voltageV = 12; s.supply12v.adcMv = 1090.9f;
  return s;
}

void emit(const char *name, uint32_t now = 10000) {
  LogRecord record;
  record.sequence = 42; record.capturedMs = now;
  record.intervalMs = 250;
  record.devicesMask = enabledDevices.load();
  record.supplyEnabled = supplyBusEnabled.load();
  record.state = getDashboardSnapshot();
  std::cout << name << '|' << csvRow(record);
}

int main() {
  std::cout << csvHeader();
  sdDropped.store(3);
  dashboard = healthyState(9950);
  emit("healthy");

  enabledDevices.store(0);
  emit("disabled");
  enabledDevices.store(ALL_DEVICES_MASK);

  dashboard = healthyState(0);
  emit("stale");

  dashboard = healthyState(9950);
  supplyBusEnabled.store(false);
  emit("paused");
  supplyBusEnabled.store(true);

  dashboard = healthyState(9950);
  dashboard.ds[0].status = DS_NOT_FOUND; dashboard.ds[0].valid = false;
  dashboard.ds[1].status = DS_START_FAILED; dashboard.ds[1].valid = false;
  dashboard.ds[2].status = DS_TIMEOUT; dashboard.ds[2].valid = false;
  dashboard.ds[3].status = DS_READ_FAILED; dashboard.ds[3].valid = false;
  emit("ds_errors");

  dashboard = healthyState(UINT32_MAX - 24);
  emit("millis_wrap", 25); // Exactly 50 ms old, across the uint32_t wrap.

  dashboard = healthyState(9950);
  dashboard.imu.updatedMs = 9800;
  dashboard.encoder.updatedMs = 9500;
  dashboard.rtc.updatedMs = dashboard.ina[0].updatedMs = dashboard.ds[0].updatedMs = 7500;
  dashboard.dht.updatedMs = 3500; // 6500 ms freshness boundary
  emit("age_boundary");
  return 0;
}
