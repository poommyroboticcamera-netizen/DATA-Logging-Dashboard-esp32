/* Browser-only API simulator for the public GitHub Pages demo. */
(() => {
  'use strict';

  const nativeFetch = window.fetch.bind(window);
  const startedAt = Date.now();
  const boot = 'github-demo';
  const deviceIds = ['ina1', 'ina2', 'ina3', 'rtc', 'imu', 'mcp', 'dht', 'ds1', 'ds2', 'ds3', 'ds4', 'encoder', 'sd'];
  const header = [
    'sequence', 'uptime_ms', 'rtc_datetime', 'rtc_valid', 'rtc_age_ms', 'supply_on',
    'dht22_temp_c', 'dht22_humidity_pct', 'dht22_valid', 'dht22_age_ms',
    'ina1_voltage_v', 'ina1_current_a', 'ina1_power_w', 'ina1_shunt_mv', 'ina1_valid', 'ina1_age_ms', 'ina1_rshunt_mohm',
    'ina2_voltage_v', 'ina2_current_a', 'ina2_power_w', 'ina2_shunt_mv', 'ina2_valid', 'ina2_age_ms', 'ina2_rshunt_mohm',
    'ina3_voltage_v', 'ina3_current_a', 'ina3_power_w', 'ina3_shunt_mv', 'ina3_valid', 'ina3_age_ms', 'ina3_rshunt_mohm',
    'ds1_temp_c', 'ds1_valid', 'ds1_age_ms', 'ds1_status',
    'ds2_temp_c', 'ds2_valid', 'ds2_age_ms', 'ds2_status',
    'ds3_temp_c', 'ds3_valid', 'ds3_age_ms', 'ds3_status',
    'ds4_temp_c', 'ds4_valid', 'ds4_age_ms', 'ds4_status',
    'imu_ax_ms2', 'imu_ay_ms2', 'imu_az_ms2', 'imu_temp_c', 'roll_deg', 'pitch_deg', 'yaw_deg',
    'imu_valid', 'tilt_valid', 'roll_valid', 'yaw_valid', 'imu_age_ms',
    'encoder_count', 'encoder_counts_s', 'shaft_rpm', 'wheel_rpm', 'speed_kmh',
    'encoder_valid', 'rpm_valid', 'speed_valid', 'encoder_age_ms',
    'supply_voltage_v', 'supply_adc_mv', 'supply_valid', 'dropped_records',
    'yaw_calibrating', 'yaw_calibration_samples', 'yaw_reference', 'record_interval_ms', 'enabled_devices_mask'
  ];

  let devicesMask = (1 << deviceIds.length) - 1;
  let revision = 1;
  let intervalMs = 250;
  let blinkMs = 100;
  let gaMask = 0b0101;
  let gaChase = false;
  let gaStepMs = 500;
  let shuntMohm = [1, 1, 1];
  let encoder = { ppr: 360, wheel_mm: 100, ratio: 1 };
  let yawCalibrationStarted = null;
  let yawReference = 3;

  const enabled = id => {
    const bit = deviceIds.indexOf(id);
    return bit >= 0 && (devicesMask & (1 << bit)) !== 0;
  };
  const f = (value, digits = 4) => Number(value).toFixed(digits);
  const pad = value => String(value).padStart(2, '0');
  const rtcText = date => `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
  const json = (value, status = 200) => new Response(JSON.stringify(value), {
    status,
    headers: { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' }
  });
  const formValues = init => new URLSearchParams(init?.body?.toString?.() || '');

  function sampleValues() {
    const uptime = Date.now() - startedAt;
    const seconds = uptime / 1000;
    const wave = Math.sin(seconds * 0.72);
    const slowWave = Math.sin(seconds * 0.24);
    const rpm = 105.87 + wave * 7.5;
    const wheelCircumferenceM = Math.PI * encoder.wheel_mm / 1000;
    const wheelRpm = rpm / encoder.ratio;
    let yawSamples = 200;
    if (yawCalibrationStarted !== null) {
      yawSamples = Math.min(200, Math.floor((Date.now() - yawCalibrationStarted) / 20));
      if (yawSamples >= 200) { yawCalibrationStarted = null; yawReference += 1; }
    }
    const yawCalibrating = yawCalibrationStarted !== null;
    const speed = wheelRpm * wheelCircumferenceM * 60 / 1000;
    const ina = [
      { voltage: 12.48 + slowWave * 0.03, current: 1.24 + wave * 0.08 },
      { voltage: 12.46 + slowWave * 0.02, current: 0.83 + wave * 0.05 },
      { voltage: 12.43 + slowWave * 0.04, current: 2.10 + wave * 0.12 }
    ];
    const values = {
      sequence: String(Math.floor(uptime / intervalMs)),
      uptime_ms: String(uptime),
      rtc_datetime: rtcText(new Date()),
      rtc_valid: enabled('rtc') ? '1' : '0',
      rtc_age_ms: '112',
      supply_on: '',
      dht22_temp_c: f(27.1 + slowWave * 0.3),
      dht22_humidity_pct: f(54.8 + slowWave * 1.2),
      dht22_valid: enabled('dht') ? '1' : '0',
      dht22_age_ms: '530',
      ds1_temp_c: f(29.4375 + slowWave * 0.18),
      ds2_temp_c: f(31.1875 + slowWave * 0.15),
      ds3_temp_c: f(30.0625 + slowWave * 0.16),
      ds4_temp_c: f(28.6875 + slowWave * 0.14),
      imu_ax_ms2: f(Math.sin(seconds * 1.3) * 0.55),
      imu_ay_ms2: f(Math.cos(seconds * 1.1) * 0.42),
      imu_az_ms2: f(9.79 + Math.sin(seconds * 0.9) * 0.10),
      imu_temp_c: f(34.2 + slowWave * 0.15),
      roll_deg: f(Math.sin(seconds * 0.45) * 5.5),
      pitch_deg: f(Math.cos(seconds * 0.38) * 4.2),
      yaw_deg: f((seconds * 3.2) % 360),
      imu_valid: enabled('imu') ? '1' : '0',
      tilt_valid: enabled('imu') ? '1' : '0',
      roll_valid: enabled('imu') ? '1' : '0',
      yaw_valid: enabled('imu') && !yawCalibrating ? '1' : '0',
      imu_age_ms: '7',
      encoder_count: String(Math.floor(seconds * 2540.9)),
      encoder_counts_s: f(2540.9 + wave * 180),
      shaft_rpm: f(rpm),
      wheel_rpm: f(wheelRpm),
      speed_kmh: f(speed),
      encoder_valid: enabled('encoder') ? '1' : '0',
      rpm_valid: enabled('encoder') ? '1' : '0',
      speed_valid: enabled('encoder') ? '1' : '0',
      encoder_age_ms: '54',
      supply_voltage_v: '',
      supply_adc_mv: '',
      supply_valid: '0',
      dropped_records: '0',
      yaw_calibrating: yawCalibrating ? '1' : '0',
      yaw_calibration_samples: String(yawSamples),
      yaw_reference: String(yawReference),
      record_interval_ms: String(intervalMs),
      enabled_devices_mask: String(devicesMask)
    };

    ina.forEach((channel, index) => {
      const number = index + 1;
      const isEnabled = enabled(`ina${number}`);
      values[`ina${number}_voltage_v`] = f(channel.voltage);
      values[`ina${number}_current_a`] = f(channel.current);
      values[`ina${number}_power_w`] = f(channel.voltage * channel.current);
      values[`ina${number}_shunt_mv`] = f(channel.current * shuntMohm[index]);
      values[`ina${number}_valid`] = isEnabled ? '1' : '0';
      values[`ina${number}_age_ms`] = '93';
      values[`ina${number}_rshunt_mohm`] = String(shuntMohm[index]);
    });
    for (let number = 1; number <= 4; number += 1) {
      const isEnabled = enabled(`ds${number}`);
      values[`ds${number}_valid`] = isEnabled ? '1' : '0';
      values[`ds${number}_age_ms`] = '178';
      values[`ds${number}_status`] = isEnabled ? 'READY' : 'DISABLED';
    }
    return values;
  }

  function statePacket() {
    if (gaChase && enabled('mcp')) {
      const step = Math.floor((Date.now() - startedAt) / gaStepMs) % 4;
      gaMask = 1 << (3 - step);
    }
    const values = sampleValues();
    return {
      boot,
      supply: 'manual',
      interval_ms: intervalMs,
      header: header.join(','),
      csv: header.map(key => values[key] ?? '').join(','),
      devices_mask: devicesMask,
      control_revision: revision,
      recording: true,
      recording_session: 1,
      blink_ms: blinkMs,
      shunt_mohm: shuntMohm,
      encoder_config: encoder,
      ga: {
        valid: enabled('mcp'),
        mask: enabled('mcp') ? gaMask : 0,
        commanded: enabled('mcp') ? gaMask : 0,
        chase: gaChase,
        stop_failed: false
      },
      ga_step_ms: gaStepMs,
      sd_available: true,
      sd_mounted: enabled('sd'),
      sd_status: enabled('sd') ? 'READY' : 'DISABLED',
      sd_written: Math.floor((Date.now() - startedAt) / intervalMs),
      demo: true
    };
  }

  async function mockApi(path, init) {
    await new Promise(resolve => setTimeout(resolve, 25));
    if (path === '/api/state') return json(statePacket());

    const form = formValues(init);
    if (path === '/api/device') {
      const bit = deviceIds.indexOf(form.get('device'));
      if (bit < 0) return json({ error: 'Unknown demo device' }, 400);
      if (form.get('enabled') === '1') devicesMask |= 1 << bit;
      else devicesMask &= ~(1 << bit);
      if (!enabled('mcp')) { gaMask = 0; gaChase = false; }
      revision += 1;
      return json({ boot, control_revision: revision, devices_mask: devicesMask });
    }
    if (path === '/api/ga') {
      if (form.has('mask')) { gaMask = Number(form.get('mask')) & 0x0f; gaChase = false; }
      else if (form.get('mode') === 'toggle') { gaMask ^= 1 << Number(form.get('channel')); gaChase = false; }
      else if (form.get('mode') === 'chase') gaChase = true;
      return json({ ok: true });
    }
    if (path === '/api/imu') {
      if (form.get('action') !== 'calibrate' || !enabled('imu')) return json({ error: 'IMU is not ready' }, 409);
      yawCalibrationStarted = Date.now();
      return json({ queued: true }, 202);
    }
    if (path === '/api/ga-frequency') {
      const hz = Math.min(20, Math.max(0.2, Number(form.get('hz'))));
      gaStepMs = Math.round(1000 / hz);
      return json({ hz });
    }
    if (path === '/api/encoder') {
      encoder = {
        ppr: Number(form.get('ppr')),
        wheel_mm: Number(form.get('wheel_mm')),
        ratio: Number(form.get('ratio'))
      };
      return json(encoder);
    }
    if (path === '/api/shunt') {
      shuntMohm = [Number(form.get('ch1')), Number(form.get('ch2')), Number(form.get('ch3'))];
      return json({ shunt_mohm: shuntMohm });
    }
    if (path === '/api/led') {
      blinkMs = Number(form.get('blink_ms'));
      return json({ blink_ms: blinkMs });
    }
    if (path === '/api/settings') {
      intervalMs = Number(form.get('interval_ms'));
      return json({ interval_ms: intervalMs });
    }
    return json({ error: 'Unknown demo endpoint' }, 404);
  }

  window.fetch = (input, init) => {
    const rawUrl = typeof input === 'string' ? input : input?.url;
    const path = new URL(rawUrl, window.location.href).pathname;
    return path.startsWith('/api/') ? mockApi(path, init) : nativeFetch(input, init);
  };

  document.addEventListener('DOMContentLoaded', () => {
    setTimeout(() => {
      const notice = document.getElementById('alert');
      if (!notice) return;
      notice.hidden = false;
      notice.textContent = 'Interactive GitHub demo · Representative sensor data · Controls affect this browser preview only';
    }, 80);
  });
})();
