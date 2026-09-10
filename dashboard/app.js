(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const C = DashboardCore;
  const deviceState = new C.ControlState();
  const imuModel = ImuModel.create($('imu-model'), $('imu-model-status'), $('imu-model-reset'), $('imu-model-restore'), $('imu-model-pause'));
  const reasons = { switch_stop: 'กด SW2 หยุดบันทึก', switch_restart: 'เริ่มรอบใหม่ด้วย SW1', supply_off: 'ปิดซัพพลาย', connection_lost: 'การเชื่อมต่อขาด', manual: 'บันทึกด้วยตนเอง', device_reset: 'บอร์ดเริ่มใหม่', recovered: 'กู้คืนจากครั้งก่อน', schema_change: 'รูปแบบข้อมูลเปลี่ยน', '24h_segment': 'ครบ 24 ชั่วโมง', record_limit: 'ครบ 86,400 แถว', interval_change: 'เปลี่ยนช่วงเวลาบันทึก' };
  let session = new C.Session(), db = null, files = [], armed = false, lastSuccess = 0, received = false, lostSaved = false;
  let latest = null, chartPoints = [], lastChartUptime = null, requestBusy = false;
  let deviceMask = null, ga = null, controlsBusy = false, encoderDirty = false, shuntDirty = false;
  let gaHzDirty = false, gaHzBusy = false;
  let ledDirty = false, ledBusy = false, sdAvailable = true;
  let currentInterval = 250, intervalDirty = false, settingsBusy = false;
  let persistedSession = null, persistedRows = 0;
  const fmt = (value, digits = 2) => value === null ? '—' : value.toFixed(digits);
  function alert(text) { $('alert').hidden = !text; $('alert').textContent = text; }
  function database() {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open('esp32-test-sessions', 2);
      request.onupgradeneeded = () => {
        if (!request.result.objectStoreNames.contains('records')) request.result.createObjectStore('records', { keyPath: 'id' });
        if (!request.result.objectStoreNames.contains('rows')) request.result.createObjectStore('rows');
      };
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  }
  function writeRecords(records, deleteActive = false) {
    if (!db) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(['records', 'rows'], 'readwrite'), store = tx.objectStore('records'), rows = tx.objectStore('rows');
      let nextSession = persistedSession, nextRows = persistedRows;
      if (deleteActive) { store.delete('active'); rows.clear(); nextSession = null; nextRows = 0; }
      records.forEach(record => {
        if (record.kind !== 'active') { store.put(record); return; }
        const current = record.session;
        if (nextSession !== current.id) { rows.clear(); nextSession = current.id; nextRows = 0; }
        for (let i = nextRows; i < current.rows.length; i++) rows.put(current.rows[i], i);
        nextRows = current.rows.length;
        store.put({ ...record, session: { ...current, rows: [] } });
      });
      tx.oncomplete = () => { persistedSession = nextSession; persistedRows = nextRows; resolve(); };
      tx.onerror = () => reject(tx.error); tx.onabort = () => reject(tx.error);
    });
  }
  function allRecords() {
    return new Promise((resolve, reject) => {
      const req = db.transaction('records').objectStore('records').getAll();
      req.onsuccess = () => resolve(req.result); req.onerror = () => reject(req.error);
    });
  }
  function filename(record) { return 'ESP32_' + record.started.replace(/[:.]/g, '-').replace('T', '_') + '_' + record.reason + '.csv'; }
  function download(record) {
    const url = URL.createObjectURL(new Blob([C.csv(record)], { type: 'text/csv;charset=utf-8' }));
    const anchor = document.createElement('a'); anchor.href = url; anchor.download = filename(record);
    document.body.append(anchor); anchor.click(); anchor.remove(); setTimeout(() => URL.revokeObjectURL(url), 30000);
  }
  function renderFiles() {
    $('files').replaceChildren();
    if (!files.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = 'เมื่อบันทึกเสร็จ ไฟล์การทดสอบจะปรากฏที่นี่'; $('files').append(p); return; }
    files.slice(0, 30).forEach(record => {
      const row = document.createElement('div'); row.className = 'file';
      const info = document.createElement('div'), name = document.createElement('strong'), meta = document.createElement('small');
      name.textContent = filename(record); meta.textContent = `${record.rows.length.toLocaleString()} แถว · ${reasons[record.reason] || record.reason}`;
      info.append(name, meta); const button = document.createElement('button'); button.textContent = 'ดาวน์โหลด CSV'; button.onclick = () => download(record);
      row.append(info, button); $('files').append(row);
    });
  }
  async function persist(completed = []) {
    const records = completed.map(record => ({ ...record, kind: 'finished' }));
    if (session.active) records.push({ id: 'active', kind: 'active', session: session.active });
    // Keep finished files available in memory even if browser storage is full.
    if (completed.length) { files.unshift(...completed); renderFiles(); }
    try { await writeRecords(records, !session.active); }
    catch { alert('พื้นที่สำรองในเบราว์เซอร์ไม่พร้อม กรุณากดบันทึก CSV และเก็บหน้านี้เปิดไว้'); }
    for (const record of completed) {
      $('save-note').textContent = `เซฟแล้ว ${record.rows.length} แถว · ${reasons[record.reason] || record.reason}`;
      if (armed) download(record);
    }
  }
  function recordingUI() {
    $('row-count').textContent = (session.active?.rows.length || 0).toLocaleString();
    $('record-pill').textContent = session.active ? 'กำลังเก็บ' : 'พร้อม'; $('record-pill').className = 'pill' + (session.active ? ' on' : '');
    $('save').disabled = !session.active?.rows.length;
  }
  function updateGauge(key, value, max) {
    const scale = C.gauge(value, max);
    $('gauge-' + key).setAttribute('stroke-dasharray', `${scale.fill} 100`);
    $('scale-' + key).textContent = scale.max;
  }
  function render(data, supply, stale = false) {
    imuModel.update(data, stale, deviceState.boot);
    const n = key => stale ? null : C.number(data, key);
    const valid = key => !stale && data?.[key] === '1';
    $('supply').textContent = fmt(n('supply_voltage_v'));
    const powerText = { on: 'ON', off: 'OFF', unknown: 'ไม่ทราบ', manual: 'ปิดการตรวจซัพพลาย' };
    $('power-pill').textContent = stale ? 'ขาดการเชื่อมต่อ' : powerText[supply] || 'รอข้อมูล';
    $('power-pill').className = 'pill ' + (!stale && supply === 'on' ? 'on' : 'off');
    $('power-note').textContent = stale ? 'แสดง — แทนข้อมูลที่ค้าง' : supply === 'manual' ? 'ไม่อ่าน GPIO15 · ใช้ปุ่มบันทึกหรือเมื่อหลุด Wi-Fi' : supply === 'off' ? 'หยุดอ่าน I2C · รอไฟกลับ' : supply === 'unknown' ? 'ยังไม่ยืนยันไฟดับ · ไม่ปิดรอบบันทึก' : 'เชื่อมต่อกับบอร์ดผ่าน Wi-Fi';
    $('record-mode-note').textContent = supply === 'manual' ? `เก็บทุก ${currentInterval} ms · เซฟด้วยปุ่มหรือเมื่อหลุด Wi-Fi` : `เก็บทุก ${currentInterval} ms · เซฟเมื่อยืนยันซัพพลายปิด`;
    const speed = valid('speed_valid') ? n('speed_kmh') : null;
    const temperature = valid('ds1_valid') ? n('ds1_temp_c') : null;
    $('speed').textContent = fmt(speed, 1);
    $('gauge-temperature-value').textContent = fmt(temperature, 1);
    updateGauge('speed', speed, 60); updateGauge('temperature', temperature, 120);
    for (let i = 1; i <= 3; ++i) {
      const ok = valid('ina' + i + '_valid');
      const current = ok ? n('ina' + i + '_current_a') : null;
      $('gauge-ina' + i + '-current').textContent = fmt(current, 2);
      $('gauge-ina' + i + '-voltage').textContent = fmt(ok ? n('ina' + i + '_voltage_v') : null, 2);
      $('gauge-ina' + i + '-power').textContent = fmt(ok ? n('ina' + i + '_power_w') : null, 2);
      updateGauge('ina' + i, current, 20);
    }
    $('rpm').textContent = fmt(valid('rpm_valid') ? n('shaft_rpm') : null, 1);
    $('encoder-note').textContent = !C.enabled(deviceMask, 'encoder') ? '· ปิดการอ่าน Encoder' : valid('speed_valid') ? '· คำนวณจากขนาดล้อและอัตราทดที่ตั้งไว้' : valid('rpm_valid') ? '· RPM พร้อม · ต้องตั้งขนาดล้อ/อัตราทด' : '· รอ Encoder / ตรวจค่า P/R';
    $('clock').textContent = valid('rtc_valid') ? data.rtc_datetime : '—';
    $('clock-note').textContent = valid('rtc_valid') ? 'เวลาจาก RTC บนบอร์ด' : 'RTC ไม่พร้อม · ไฟล์ยังมีเวลาจากคอม';
    ['ax', 'ay', 'az'].forEach(key => $(key).textContent = fmt(valid('imu_valid') ? n('imu_' + key + '_ms2') : null));
    $('roll').textContent = fmt(valid('roll_valid') ? n('roll_deg') : null, 1);
    $('pitch').textContent = fmt(valid('tilt_valid') ? n('pitch_deg') : null, 1);
    $('yaw').textContent = fmt(valid('yaw_valid') ? n('yaw_deg') : null, 1);
    $('yaw-note').textContent = stale ? 'ขาดการเชื่อมต่อ' : !valid('imu_valid') ? 'IMU ไม่พร้อม / หยุดอ่าน / ข้อมูลเก่า' : valid('yaw_calibrating') ? 'ให้บอร์ดอยู่นิ่ง · คาลิเบรต ' + (n('yaw_calibration_samples') ?? 0) + '/200' : valid('yaw_valid') ? 'มุมหมุนรอบแกน Z · รอบอ้างอิง ' + (n('yaw_reference') ?? '—') : 'Yaw ไม่พร้อม · อยู่นิ่งเพื่อเริ่มอ้างอิงใหม่';
    $('dht-note').textContent = stale ? 'ขาดการเชื่อมต่อ' : supply === 'off' || supply === 'unknown' ? 'หยุดอ่าน / รอยืนยันซัพพลาย' : valid('dht22_valid') ? 'DHT22 · GPIO4 · อ่านทุก 2 วินาที' : 'DHT22 กำลังเริ่มทำงานหรืออ่านไม่สำเร็จ · ดู Serial Monitor';
    $('imu-pill').textContent = valid('imu_valid') ? 'ออนไลน์' : 'ไม่พร้อม'; $('imu-pill').className = 'pill' + (valid('imu_valid') ? ' on' : '');
    for (let i = 1; i <= 3; i++) {
      const ok = valid('ina' + i + '_valid');
      ['voltage_v', 'current_a', 'power_w'].forEach(field => $('ina' + i + '_' + field).textContent = fmt(ok ? n('ina' + i + '_' + field) : null, 3));
      $('ina' + i + '_status').textContent = stale ? 'ขาดการเชื่อมต่อ' : !C.enabled(deviceMask, 'ina' + i) ? 'ปิดการอ่าน' : ok ? 'ออนไลน์' : 'อ่านไม่สำเร็จ / รอข้อมูล';
    }
    const dsMessages = { WAITING: 'รอเริ่มอ่าน', READY: 'อ่านได้', DISABLED: 'ปิดการอ่าน', SUPPLY_PAUSED: 'รอซัพพลาย', NOT_FOUND: 'ไม่พบเซนเซอร์ · ตรวจสาย/ไฟ', CONFIG_FAILED: 'ตั้งค่าไม่ได้ · ตรวจสาย', START_FAILED: 'เริ่มวัดไม่ได้ · กำลังค้นหาใหม่', CONVERSION_TIMEOUT: 'วัดไม่เสร็จ · กำลังค้นหาใหม่', READ_FAILED: 'อ่านผิดพลาด · ตรวจสาย/ไฟ' };
    for (let i = 1; i <= 4; i++) {
      $('ds' + i).textContent = fmt(valid('ds' + i + '_valid') ? n('ds' + i + '_temp_c') : null, 1);
      $('ds' + i + '-state').textContent = stale ? 'ขาดการเชื่อมต่อ' : !C.enabled(deviceMask, 'ds' + i) ? 'ปิดการอ่าน' : dsMessages[data?.['ds' + i + '_status']] || 'รอข้อมูล';
    }
    $('dht').textContent = fmt(valid('dht22_valid') ? n('dht22_temp_c') : null, 1);
    $('imu-temp').textContent = fmt(valid('imu_valid') ? n('imu_temp_c') : null, 1);
    $('humidity').textContent = fmt(valid('dht22_valid') ? n('dht22_humidity_pct') : null, 1) + ' %RH';
  }
  function graph() {
    const canvas = $('chart'), rect = canvas.getBoundingClientRect(), dpr = devicePixelRatio || 1;
    canvas.width = rect.width * dpr; canvas.height = rect.height * dpr;
    const ctx = canvas.getContext('2d'); ctx.scale(dpr, dpr);
    const w = rect.width, h = rect.height, left = 40, top = 8, bottom = h - 18;
    ctx.font = '12px Segoe UI'; ctx.fillStyle = '#8a9fb5'; ctx.strokeStyle = '#243044'; ctx.lineWidth = 1;
    [-20, -10, 0, 10, 20].forEach(value => {
      const y = top + (20 - value) / 40 * (bottom - top);
      ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(w, y); ctx.stroke(); ctx.fillText(String(value), 3, y + 4);
    });
    const now = Date.now(), colors = ['#4f85ff', '#10bc90', '#a377ff'];
    for (let axis = 0; axis < 3; axis++) {
      ctx.beginPath(); ctx.strokeStyle = colors[axis]; ctx.lineWidth = 2; let drawing = false;
      chartPoints.forEach(point => {
        const value = point.values[axis];
        if (value === null) { drawing = false; return; }
        const x = left + (point.time - (now - 60000)) / 60000 * (w - left);
        const y = top + (20 - Math.max(-20, Math.min(20, value))) / 40 * (bottom - top);
        if (!drawing) ctx.moveTo(x, y); else ctx.lineTo(x, y); drawing = true;
      }); ctx.stroke();
    }
    if (!chartPoints.length) { ctx.fillStyle = '#92a4b8'; ctx.textAlign = 'center'; ctx.fillText('รอข้อมูลความเร่งจากบอร์ด', w / 2, h / 2); ctx.textAlign = 'left'; }
  }
  async function controlPost(path, values) {
    const response = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-Dashboard-Request': '1' }, body: new URLSearchParams(values), signal: AbortSignal.timeout(5000) });
    if (!response.ok) throw Error(await response.text());
    return response.json();
  }
  async function command(path, values, message) {
    if (controlsBusy) return;
    controlsBusy = true; renderControls();
    $('control-status').textContent = 'กำลังส่งคำสั่ง…';
    try {
      if (path === '/api/device') {
        if (!deviceState.boot) throw Error('รอสถานะอุปกรณ์จากเฟิร์มแวร์ล่าสุด');
        const response = await controlPost(path, {...values, boot: deviceState.boot, revision: String(deviceState.revision)});
        if (!deviceState.accept(response)) throw Error('สถานะตอบกลับเก่า · รอข้อมูลล่าสุดจากบอร์ด');
        deviceMask = deviceState.mask;
        $('control-status').textContent = message + ' · บันทึกสถานะไว้แล้ว';
      } else {
        await controlPost(path, values); $('control-status').textContent = message + ' · รอสถานะจากบอร์ด';
      }
    }
    catch (error) { $('control-status').textContent = 'ยังยืนยันคำสั่งไม่ได้: ' + error.message; }
    finally { controlsBusy = false; renderControls(); }
  }
  function renderControls() {
    const online = received && Date.now() - lastSuccess < 2500;
    C.DEVICES.forEach(([id, label]) => {
      const button = $('device-' + id); if (!button) return;
      const on = C.enabled(deviceMask, id);
      button.setAttribute('aria-pressed', String(on)); button.disabled = !online || deviceMask === null || !deviceState.boot || controlsBusy;
      if (id === 'sd' && !sdAvailable) { button.disabled = true; button.textContent = 'SD card · ยังไม่ได้ติดตั้ง'; return; }
      button.textContent = label + ' · ' + (deviceMask === null ? 'รอข้อมูล' : on ? 'เปิดอยู่ · กดปิด' : 'ปิดอยู่ · กดเปิด');
    });
    const available = online && C.enabled(deviceMask, 'mcp') && ga?.valid && !controlsBusy;
    for (let i = 0; i < 4; ++i) {
      const button = $('ga-' + i); if (!button) continue;
      const on = !!(ga?.mask & (1 << i)); button.setAttribute('aria-pressed', String(available && on));
      button.textContent = 'GA' + i + ' · ' + (available ? on ? 'ON' : 'OFF' : '—'); button.disabled = !available;
    }
    $('ga-all-off').disabled = !online || !C.enabled(deviceMask, 'mcp') || controlsBusy;
    $('ga-chase').disabled = !available;
    $('ga-status').textContent = !online ? 'ขาดการเชื่อมต่อ' : ga?.stop_failed ? 'ยืนยัน GA OFF ไม่สำเร็จ' : !C.enabled(deviceMask, 'mcp') ? 'MCP ปิด' : !ga?.valid ? 'MCP ไม่พร้อม' : ga.mask !== ga.commanded ? 'รอยืนยันเอาต์พุต' : ga.chase ? 'ไฟไล่ทำงาน' : 'อ่านเอาต์พุตแล้ว';
    $('encoder-apply').disabled = !online || controlsBusy;
    $('shunt-apply').disabled = !online || controlsBusy;
  }
  async function poll() {
    if (requestBusy) return;
    requestBusy = true;
    let responseReceived = false;
    try {
      const response = await fetch('/api/state', { cache: 'no-store', signal: AbortSignal.timeout(2000) });
      if (!response.ok) throw Error('HTTP ' + response.status);
      const body = await response.text();
      responseReceived = true; // Complete body received; timeout is not a schema error.
      const packet = JSON.parse(body);
      sdAvailable = packet.sd_available !== false;
      if (Array.isArray(packet.ds_rom)) packet.ds_rom.forEach((rom,i) => {
        const label = $('ds' + (i+1) + '-rom');
        if (label) label.textContent = 'GPIO' + (packet.ds_pins?.[i] ?? [27,16,13,14][i]) + ' · ' + (rom || 'ยังไม่พบเซนเซอร์');
      });
      // A response started before a successful OFF command cannot roll the UI back to ON.
      if (packet.control_revision != null) {
        if (!deviceState.accept(packet)) return;
      } else if (deviceState.boot) return;
      const result = session.ingest(packet);
      currentInterval = result.parsed.intervalMs;
      if (Number.isInteger(packet.ga_step_ms)) {
        const hz = 1000 / packet.ga_step_ms;
        if (!gaHzDirty && !gaHzBusy) $('ga-hz').value = hz.toFixed(3);
        $('ga-hz-apply').disabled = gaHzBusy;
        $('ga-hz-note').textContent = `${hz.toFixed(2)} ครั้ง/วินาที · ${packet.ga_step_ms} ms ต่อขา · ครบ GA3→GA0 ใน ${packet.ga_step_ms * 4} ms`;
      }
      if (Number.isInteger(packet.blink_ms)) {
        if (!ledDirty && !ledBusy) $('blink-ms').value = packet.blink_ms;
        $('blink-apply').disabled = ledBusy;
        $('blink-status').textContent = `ไฟหนึ่งรอบ ${packet.blink_ms} ms · ${(1000 / packet.blink_ms).toFixed(1)} ครั้ง/วินาที · ไม่เปลี่ยนรอบบันทึก`;
      }
      $('switch-status').textContent = packet.recording === true ? 'กำลังบันทึก · กด SW2 เพื่อหยุดและดับไฟ' : 'หยุดบันทึก · กด SW1 เพื่อเริ่ม · หน้าเว็บยังอ่านค่าปัจจุบัน';
      if (!intervalDirty && !settingsBusy) $('interval-ms').value = currentInterval;
      $('interval-apply').disabled = settingsBusy || packet.interval_ms == null;
      if (!settingsBusy) $('interval-status').textContent = `ค่าที่บอร์ดใช้: ${currentInterval} ms · ${(1000 / currentInterval).toFixed(2)} แถว/วินาทีโดยประมาณ`;
      latest = result.parsed; lastSuccess = Date.now(); received = true; lostSaved = false;
      if (/^(กำลังรอ ESP32|การเชื่อมต่อขาด|ข้อมูลจากบอร์ดไม่ถูกต้อง)/.test($('alert').textContent)) alert('');
      $('link-dot').className = 'live'; $('link-text').textContent = 'เชื่อมต่อแล้ว';
      $('last-update').textContent = 'อัปเดต ' + new Date().toLocaleTimeString('th-TH');
      deviceMask = deviceState.mask ?? (Number.isInteger(packet.devices_mask) ? packet.devices_mask : null); ga = packet.ga;
      if (packet.encoder_config) {
        const config = packet.encoder_config;
        if (!encoderDirty && !controlsBusy) { $('encoder-ppr').value = config.ppr; $('encoder-wheel').value = config.wheel_mm; $('encoder-ratio').value = config.ratio || ''; }
        $('encoder-config-note').textContent = `บอร์ดใช้ ${config.ppr} P/R · ล้อ ${config.wheel_mm} mm · อัตราทด ${config.ratio || 'ยังไม่ตั้ง'} · ×4 อัตโนมัติ`;
      }
      if (Array.isArray(packet.shunt_mohm) && packet.shunt_mohm.length === 3) {
        if (!shuntDirty && !controlsBusy) packet.shunt_mohm.forEach((v, i) => $('shunt-' + (i + 1)).value = v);
        $('shunt-config-note').textContent = 'บอร์ดใช้ CH1 / CH2 / CH3: ' + packet.shunt_mohm.join(' / ') + ' mΩ';
      }
      renderControls();
      render(latest.data, latest.supply);
      const uptime = latest.data.uptime_ms;
      if (uptime !== lastChartUptime) {
        chartPoints.push({ time: Date.now(), values: ['imu_ax_ms2', 'imu_ay_ms2', 'imu_az_ms2'].map(key => C.number(latest.data, key)) });
        lastChartUptime = uptime;
      }
      chartPoints = chartPoints.filter(p => p.time > Date.now() - 60000);
      if (result.changed) await persist(result.completed);
      if (packet.power_note) { $('power-note').textContent = packet.power_note; }
    } catch (error) {
      renderControls();
      const age = Date.now() - lastSuccess;
      $('link-dot').className = ''; $('link-text').textContent = received ? 'รอเชื่อมต่อกลับ' : 'ยังไม่พบข้อมูล';
      if (received && age > 2500) render(latest?.data, 'unknown', true);
      if (received && age > 6000 && !lostSaved) {
        lostSaved = true; const done = session.finish('connection_lost');
        if (done) await persist([done]);
        alert('การเชื่อมต่อขาด บันทึกข้อมูลที่รับมาแล้วไว้ในรายการไฟล์ ระบบจะรอเชื่อมต่อใหม่');
      }
      if (error.name === 'AbortError' || error.name === 'TimeoutError') {
        if (!lostSaved) alert('การเชื่อมต่อขาดช่วง: รอข้อมูลจาก ESP32 เกินเวลา กำลังลองเชื่อมต่อใหม่');
      }
      else if (responseReceived) alert('ข้อมูลจากบอร์ดไม่ถูกต้อง: ' + error.message + ' · ตรวจว่าเฟิร์มแวร์กับหน้าเว็บเป็นรุ่นเดียวกัน');
      else if (!received) alert('กำลังรอ ESP32: ตรวจว่าอัปโหลดเฟิร์มแวร์ล่าสุดและใช้งาน Wi-Fi เครือข่ายเดียวกัน');
    } finally { recordingUI(); graph(); requestBusy = false; }
  }
  async function init() {
    for (let i = 1; i <= 3; i++) {
      const row = document.createElement('tr'); row.innerHTML = `<td>CH ${i}</td><td id="ina${i}_voltage_v">—</td><td id="ina${i}_current_a">—</td><td id="ina${i}_power_w">—</td><td id="ina${i}_status">รอข้อมูล</td>`; $('ina-rows').append(row);
    }
    [['dht', 'DHT22'], ['ds1', 'DS18B20 · 1'], ['ds2', 'DS18B20 · 2'], ['ds3', 'DS18B20 · 3'], ['ds4', 'DS18B20 · 4'], ['imu-temp', 'IMU']].forEach(([id, label]) => {
      const cell = document.createElement('div'); cell.innerHTML = `<label>${label}</label><strong id="${id}">—</strong><small>°C</small>`; if (/^ds[1-4]$/.test(id)) { const status = document.createElement('div'); status.id = id + '-state'; status.className = 'ds-state'; status.textContent = 'รอข้อมูล'; cell.append(status); const rom = document.createElement('div'); rom.id = id + '-rom'; rom.className = 'ds-state'; cell.append(rom); } $('temperatures').append(cell);
    });
    try {
      db = await database(); const records = await allRecords();
      files = records.filter(r => r.kind === 'finished').sort((a, b) => b.started.localeCompare(a.started));
      const active = records.find(r => r.id === 'active');
      if (active?.session && !active.session.rows.length) {
        active.session.rows = await new Promise((resolve, reject) => {
          const req = db.transaction('rows').objectStore('rows').getAll();
          req.onsuccess = () => resolve(req.result); req.onerror = () => reject(req.error);
        });
      }
      if (active?.session?.rows.length) {
        session = new C.Session(active.session); const done = session.finish('recovered'); await persist([done]);
        alert('กู้คืนข้อมูลจากหน้าเว็บครั้งก่อนแล้ว ดาวน์โหลดได้จากรายการไฟล์');
      }
    } catch { alert('เบราว์เซอร์นี้ไม่อนุญาตการสำรองข้อมูล ให้กดบันทึก CSV ก่อนปิดหน้าเว็บ'); }
    C.DEVICES.forEach(([id, label]) => {
      const button = document.createElement('button'); button.id = 'device-' + id; button.type = 'button'; button.disabled = true; button.textContent = label + ' · รอข้อมูล';
      button.onclick = () => command('/api/device', { device: id, enabled: C.enabled(deviceMask, id) ? '0' : '1' }, 'บอร์ดรับคำสั่ง ' + label + ' แล้ว');
      $('device-controls').append(button);
    });
    for (let i = 0; i < 4; ++i) {
      const button = document.createElement('button'); button.id = 'ga-' + i; button.disabled = true; button.textContent = 'GA' + i;
      button.onclick = () => command('/api/ga', { mode: 'toggle', channel: String(i) }, 'ส่งคำสั่ง GA แล้ว'); $('ga-controls').append(button);
    }
    $('ga-all-off').onclick = () => command('/api/ga', { mask: '0' }, 'ส่งคำสั่งปิด GA ทั้งหมดแล้ว');
    $('ga-hz').oninput = () => { gaHzDirty = true; };
    $('ga-hz-form').onsubmit = async event => {
      event.preventDefault(); if (gaHzBusy) return;
      const hz = Number($('ga-hz').value);
      if (!Number.isFinite(hz) || hz < .2 || hz > 20) { alert('ความถี่ GA ต้องอยู่ระหว่าง 0.2–20 Hz'); return; }
      gaHzBusy = true; $('ga-hz-apply').disabled = true;
      try { await controlPost('/api/ga-frequency', {hz}); gaHzDirty = false; }
      catch (error) { alert('บันทึกความถี่ GA ไม่สำเร็จ: ' + error.message); }
      finally { gaHzBusy = false; }
    };
    $('ga-chase').onclick = () => command('/api/ga', { mode: 'chase' }, 'ส่งคำสั่งไฟไล่แล้ว');
    ['encoder-ppr', 'encoder-wheel', 'encoder-ratio'].forEach(id => $(id).oninput = () => { encoderDirty = true; });
    $('encoder-form').onsubmit = async event => {
      event.preventDefault(); if (controlsBusy) return;
      let config;
      try { config = C.encoderSettings($('encoder-ppr').value, $('encoder-wheel').value, $('encoder-ratio').value); }
      catch (error) { $('encoder-config-note').textContent = error.message; return; }
      controlsBusy = true; renderControls();
      try { await controlPost('/api/encoder', config); encoderDirty = false; $('encoder-config-note').textContent = 'บันทึกแล้ว · รอค่าจากบอร์ด'; }
      catch (error) { $('encoder-config-note').textContent = 'ยังยืนยันการบันทึกไม่ได้: ' + error.message; }
      finally { controlsBusy = false; renderControls(); }
    };
    [1, 2, 3].forEach(i => $('shunt-' + i).oninput = () => { shuntDirty = true; });
    $('shunt-form').onsubmit = async event => {
      event.preventDefault(); if (controlsBusy) return;
      let config;
      try { config = C.shuntSettings([1, 2, 3].map(i => $('shunt-' + i).value)); }
      catch (error) { $('shunt-config-note').textContent = error.message; return; }
      controlsBusy = true; renderControls();
      try { await controlPost('/api/shunt', config); shuntDirty = false; $('shunt-config-note').textContent = 'บันทึกแล้ว · รอค่าจากบอร์ด'; }
      catch (error) { $('shunt-config-note').textContent = 'ยังยืนยันการบันทึกไม่ได้: ' + error.message; }
      finally { controlsBusy = false; renderControls(); }
    };
    renderFiles(); graph();
    $('blink-ms').oninput = () => { ledDirty = true; };
    $('blink-form').onsubmit = async event => {
      event.preventDefault(); if (ledBusy) return;
      const value = Number($('blink-ms').value);
      if (!Number.isInteger(value) || value < 40 || value > 2000) { alert('กำหนดรอบไฟ 40–2000 ms'); return; }
      ledBusy = true; $('blink-apply').disabled = true;
      try { await controlPost('/api/led', { blink_ms: value }); ledDirty = false; }
      catch (error) { alert('บันทึกความเร็วไฟไม่สำเร็จ: ' + error.message); }
      finally { ledBusy = false; }
    };
    $('interval-ms').oninput = () => { intervalDirty = true; };
    $('interval-form').onsubmit = async event => {
      event.preventDefault();
      if (settingsBusy) return;
      let value;
      try { value = C.interval($('interval-ms').value); }
      catch (error) { alert(error.message); return; }
      settingsBusy = true; $('interval-apply').disabled = true;
      $('interval-status').textContent = 'กำลังบันทึกค่าลง ESP32…';
      try {
        const response = await fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-Dashboard-Request': '1' }, body: new URLSearchParams({ interval_ms: String(value) }), signal: AbortSignal.timeout(5000) });
        if (!response.ok) throw Error(await response.text());
        const config = await response.json(); C.interval(config.interval_ms);
        intervalDirty = false;
        alert('บันทึกช่วงเวลาแล้ว · รอข้อมูลยืนยันจากบอร์ด');
      } catch (error) { alert('ยังยืนยันการบันทึกค่าไม่ได้: ' + error.message + ' · ตรวจค่าที่บอร์ดใช้ก่อนลองอีกครั้ง'); }
      finally { settingsBusy = false; $('interval-apply').disabled = false; }
    };
    $('save').onclick = async () => { const done = session.finish('manual'); if (done) { await persist([done]); if (!armed) download(done); } recordingUI(); };
    $('arm').onclick = () => { armed = !armed; $('arm').textContent = armed ? 'ดาวน์โหลดอัตโนมัติ: เปิด' : 'เปิดการดาวน์โหลดอัตโนมัติ'; $('save-note').textContent = armed ? 'หากเบราว์เซอร์บล็อก ให้ดาวน์โหลดจากรายการไฟล์' : 'ไฟล์ยังสำรองในรายการเมื่อจบรอบ'; };
    const navLinks = [...document.querySelectorAll('nav a[href^="#"]')];
    navLinks.forEach(link => link.addEventListener('click', () => {
      navLinks.forEach(item => { item.classList.toggle('active', item === link); if (item === link) item.setAttribute('aria-current', 'location'); else item.removeAttribute('aria-current'); });
    }));
    window.addEventListener('resize', graph);
    window.addEventListener('beforeunload', event => { if (session.active?.rows.length) { event.preventDefault(); event.returnValue = ''; } });
    // One outstanding request; slow down for long recording intervals.
    async function pollNext() {
      await poll();
      setTimeout(pollNext, Math.min(250, Math.max(100, currentInterval / 2)));
    }
    pollNext();
  }
  init();
})();
