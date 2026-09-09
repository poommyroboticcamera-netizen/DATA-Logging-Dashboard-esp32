// The native harness produced these rows with the actual firmware functions.
// Parse them with the same module used by the dashboard, then verify meaning.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const core = require(path.resolve(__dirname, '../../dashboard/core.js'));
const { header, rows } = JSON.parse(fs.readFileSync(0, 'utf8'));
const parsed = Object.fromEntries(Object.entries(rows).map(([name, csv]) => [name, core.parse({
  header, csv, supply: 'manual', boot: 'native-regression', interval_ms: 250
}).data]));
const num = (data, key, expected) => assert.equal(core.number(data, key), expected, key);
const empty = (data, key) => assert.equal(data[key], '', key + ' must be blank');
const h = parsed.healthy;
assert.equal(h.rtc_datetime, '2026-09-08 16:42:31');
num(h, 'sequence', 42); num(h, 'uptime_ms', 10000);
num(h, 'dht22_temp_c', 24.5); num(h, 'dht22_humidity_pct', 61.75);
for (let i = 1; i <= 3; i++) {
  for (const [suffix, expected] of Object.entries({voltage_v: 11.25 + i, current_a: -0.25 - i,
    power_w: 17.5 + i, shunt_mv: 0.75 - i, rshunt_mohm: 0.5 + i * 0.5, valid: 1, age_ms: 50}))
    num(h, `ina${i}_${suffix}`, expected);
}
for (let i = 1; i <= 4; i++) {
  num(h, `ds${i}_temp_c`, 19.25 + i); num(h, `ds${i}_valid`, 1);
  num(h, `ds${i}_age_ms`, 50); assert.equal(h[`ds${i}_status`], 'READY');
}
for (const [key, value] of Object.entries({imu_ax_ms2: 1.25, imu_ay_ms2: -2.5, imu_az_ms2: 9.75,
  imu_temp_c: 32.5, roll_deg: -10.25, pitch_deg: 5.5, yaw_deg: 123.75, imu_valid: 1,
  tilt_valid: 1, roll_valid: 1, yaw_valid: 1, imu_age_ms: 50, encoder_counts_s: 1440,
  shaft_rpm: 60, wheel_rpm: 60, speed_kmh: 1.885, encoder_valid: 1, rpm_valid: 1,
  speed_valid: 1, encoder_age_ms: 50, dropped_records: 3, yaw_calibrating: 0,
  yaw_calibration_samples: 200, yaw_reference: 7, record_interval_ms: 250,
  enabled_devices_mask: 8191})) num(h, key, value);
assert.equal(h.encoder_count, '1234567890123');
// The user disabled supply monitoring. Do not invent a voltage or ON state.
empty(h, 'supply_on'); empty(h, 'supply_voltage_v'); empty(h, 'supply_adc_mv');
num(h, 'supply_valid', 0);

for (const name of ['disabled', 'stale', 'paused']) {
  const d = parsed[name];
  for (const key of ['rtc_valid', 'dht22_valid', 'ina1_valid', 'ina2_valid', 'ina3_valid',
    'ds1_valid', 'ds2_valid', 'ds3_valid', 'ds4_valid', 'imu_valid', 'tilt_valid',
    'roll_valid', 'yaw_valid', 'encoder_valid', 'rpm_valid', 'speed_valid']) num(d, key, 0);
  for (const key of ['rtc_datetime', 'dht22_temp_c', 'dht22_humidity_pct', 'imu_ax_ms2',
    'imu_ay_ms2', 'imu_az_ms2', 'imu_temp_c', 'roll_deg', 'pitch_deg', 'yaw_deg',
    'encoder_count', 'encoder_counts_s', 'shaft_rpm', 'wheel_rpm', 'speed_kmh']) empty(d, key);
  for (let i = 1; i <= 3; i++) for (const suffix of ['voltage_v', 'current_a', 'power_w', 'shunt_mv', 'rshunt_mohm']) empty(d, `ina${i}_${suffix}`);
  for (let i = 1; i <= 4; i++) {
    empty(d, `ds${i}_temp_c`);
    assert.equal(d[`ds${i}_status`], {disabled: 'DISABLED', stale: 'READY', paused: 'SUPPLY_PAUSED'}[name]);
  }
}
num(parsed.disabled, 'enabled_devices_mask', 0);
for (const [i, status] of ['NOT_FOUND', 'START_FAILED', 'CONVERSION_TIMEOUT', 'READ_FAILED'].entries()) {
  assert.equal(parsed.ds_errors[`ds${i + 1}_status`], status);
  empty(parsed.ds_errors, `ds${i + 1}_temp_c`);
}
for (const [key, value] of Object.entries(h)) {
  if (key !== 'uptime_ms') assert.equal(parsed.millis_wrap[key], value, `wrap: ${key}`);
}
for (const key of ['imu_valid', 'encoder_valid', 'rtc_valid', 'ina1_valid', 'ds1_valid', 'dht22_valid']) num(parsed.age_boundary, key, 0);
num(parsed.age_boundary, 'ina2_valid', 1); num(parsed.age_boundary, 'ds2_valid', 1);
// Prove the check rejects the historical bug: a missing DS status shifts all
// fields after that DS channel and must never reach the UI as valid telemetry.
const keys = header.trim().split(',');
const broken = rows.healthy.trim().split(',');
broken.splice(keys.indexOf('ds1_status'), 1);
assert.throws(() => core.parse({header, csv: broken.join(','), supply: 'manual'}));
console.log(`Firmware CSV -> DashboardCore.parse: ${Object.keys(rows).length} scenarios passed (${keys.length} columns).`);
