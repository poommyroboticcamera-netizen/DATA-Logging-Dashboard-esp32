/* Pure session logic; also executed by the offline regression tests. */
const DashboardCore = (() => {
  const DEVICES = [ ['ina1','INA226 · 1'], ['ina2','INA226 · 2'], ['ina3','INA226 · 3'], ['rtc','RTC'], ['imu','IMU'], ['mcp','MCP23017'], ['dht','DHT22'], ['ds1','DS18B20 · 1'], ['ds2','DS18B20 · 2'], ['ds3','DS18B20 · 3'], ['ds4','DS18B20 · 4'], ['encoder','Encoder'], ['sd','SD card'] ];
  function enabled(mask, id) { const i = DEVICES.findIndex(d => d[0] === id); return i >= 0 && Number.isInteger(mask) && (mask & (1 << i)) !== 0; }
  function encoderSettings(ppr, wheel, ratio) {
    const values = [ppr, wheel, ratio].map(Number);
    if (!Number.isInteger(values[0]) || values.some((v, i) => !Number.isFinite(v) || v <= 0 || v > (i ? 10000 : 100000))) throw Error('ระบุ P/R เป็นจำนวนเต็ม 1–100000 และขนาดล้อ/อัตราทดมากกว่า 0 ถึง 10000');
    return { ppr: values[0], wheel_mm: values[1], ratio: values[2] };
  }
  const SAMPLE_INTERVAL_MS = 250;
  const MAX_SESSION_ROWS = 86400; // About 6 hours at 4 Hz.
  function shuntSettings(values) {
    if (!Array.isArray(values) || values.length !== 3) throw Error('ระบุ Rshunt ให้ครบ 3 ช่อง');
    const numbers = values.map(Number);
    if (numbers.some(v => !Number.isFinite(v) || v < 0.01 || v > 1000)) throw Error('Rshunt แต่ละช่องต้องอยู่ระหว่าง 0.01–1000 mΩ');
    return { ch1: numbers[0], ch2: numbers[1], ch3: numbers[2] };
  }
  function gauge(value, baseMax) {
    if (value === null || !Number.isFinite(value)) return { fill: 0, max: baseMax };
    const magnitude = Math.abs(value);
    const max = Math.max(baseMax, Math.ceil(magnitude / baseMax) * baseMax);
    return { fill: Math.min(100, magnitude / max * 100), max };
  }
  function interval(value) {
    if (typeof value === 'string' && !/^\d+$/.test(value)) throw Error('กรอกจำนวนเต็ม 100–60000 ms');
    const ms = Number(value);
    if (!Number.isInteger(ms) || ms < 100 || ms > 60000) throw Error('กรอกจำนวนเต็ม 100–60000 ms');
    return ms;
  }
  function canSyncPreview(liveUptime, newestBufferedUptime, pending) {
    const live = Number(liveUptime), newest = Number(newestBufferedUptime);
    return Number(pending) === 0 && Number.isFinite(live) &&
      (newestBufferedUptime == null || (Number.isFinite(newest) && live >= newest));
  }
  function parse(packet) {
    if (!packet || typeof packet.header !== 'string' || typeof packet.csv !== 'string') throw Error('รูปแบบข้อมูลไม่ถูกต้อง');
    const keys = packet.header.trim().split(',');
    const values = packet.csv.trim().split(',');
    if (keys.length !== values.length || !keys.includes('uptime_ms') || new Set(keys).size !== keys.length) throw Error('คอลัมน์ข้อมูลไม่ตรงกัน');
    if (!['on', 'off', 'unknown', 'manual'].includes(packet.supply)) throw Error('สถานะซัพพลายไม่ถูกต้อง');
    const data = Object.fromEntries(keys.map((key, i) => [key, values[i]]));
    if (!Number.isFinite(Number(data.uptime_ms))) throw Error('เวลาอุปกรณ์ไม่ถูกต้อง');
    return { keys, values, data, supply: packet.supply, boot: packet.boot || '', recording: packet.recording !== false, recordingSession: packet.recording_session ?? null, intervalMs: interval(packet.interval_ms ?? SAMPLE_INTERVAL_MS) };
  }
  function number(data, key) {
    if (!data || data[key] == null || data[key] === '') return null;
    const value = Number(data[key]);
    return Number.isFinite(value) ? value : null;
  }
  function csv(session) {
    const quote = value => {
      let text = String(value ?? '');
      if (/^[=+@]/.test(text)) text = "'" + text;
      return /[",\r\n]/.test(text) ? '"' + text.replaceAll('"', '""') + '"' : text;
    };
    return '\ufeff' + [['pc_received_at', 'supply_state', ...session.keys], ...session.rows]
      .map(row => row.map(quote).join(',')).join('\r\n') + '\r\n';
  }
  class ControlState {
    constructor() { this.boot = null; this.revision = -1; this.mask = null; this.retired = new Set(); }
    accept(packet) {
      const {boot, control_revision: revision, devices_mask: mask} = packet;
      if (typeof boot !== 'string' || !boot || !Number.isInteger(revision) || revision < 0 || !Number.isInteger(mask) || mask < 0 || mask >= (1 << DEVICES.length)) return false;
      if (this.retired.has(boot)) return false;
      if (boot === this.boot && (revision < this.revision || (revision === this.revision && mask !== this.mask))) return false;
      if (this.boot && boot !== this.boot) this.retired.add(this.boot);
      this.boot = boot; this.revision = revision; this.mask = mask;
      return true;
    }
  }
  class Session {
    constructor(saved = null) { this.active = saved; this.lastUptime = null; this.boot = null; this.lastSample = null; }
    finish(reason) {
      if (!this.active?.rows.length) { this.active = null; return null; }
      const result = { ...this.active, reason, ended: new Date().toISOString() };
      this.active = null; this.lastSample = null;
      return result;
    }
    ingest(packet, stamp = new Date().toISOString(), storeSample = true) {
      const p = parse(packet), completed = [];
      const uptime = Number(p.data.uptime_ms);
      const bootChanged = this.boot !== null && p.boot !== this.boot;
      // Network/buffer races can deliver an older preview after a newer exact
      // sample. Ignore it; only a changed boot ID proves an ESP32 restart.
      if (this.lastUptime !== null && uptime < this.lastUptime && !bootChanged)
        return { parsed: p, completed, changed: false, ignored: true };
      const reset = bootChanged;
      if (reset || (this.active && this.active.keys.join(',') !== p.keys.join(','))) {
        const done = this.finish(reset ? 'device_reset' : 'schema_change');
        if (done) completed.push(done);
      }
      if (this.active && (this.active.intervalMs ?? SAMPLE_INTERVAL_MS) !== p.intervalMs) {
        const done = this.finish('interval_change'); if (done) completed.push(done);
      }
      this.boot = p.boot; this.lastUptime = uptime;
      // Physical STOP ends storage while live monitoring continues.
      if (this.active && (!p.recording || (p.recordingSession !== null && this.active.recordingSession !== p.recordingSession))) {
        const done = this.finish(!p.recording ? 'switch_stop' : 'switch_restart');
        if (done) completed.push(done);
      }
      if (!p.recording) return { parsed: p, completed, changed: completed.length > 0 };
      // Manual supply monitoring is independent of the recording switch.
      if (storeSample && ['on', 'manual'].includes(p.supply) && !this.active) {
        this.active = { id: Date.now() + '-' + Math.random().toString(36).slice(2, 8), started: stamp, keys: p.keys, recordingSession: p.recordingSession, intervalMs: p.intervalMs, rows: [] };
        this.lastSample = null;
      }
      let added = false;
      if (storeSample && this.active && (this.lastSample === null || Math.floor(uptime / p.intervalMs) > Math.floor(this.lastSample / p.intervalMs))) {
        this.active.rows.push([stamp, p.supply, ...p.values]); this.lastSample = uptime; added = true;
      }
      if (p.supply === 'off' && this.active) {
        if (storeSample && !added) this.active.rows.push([stamp, p.supply, ...p.values]);
        const done = this.finish('supply_off'); if (done) completed.push(done);
      } else if (this.active?.rows.length >= MAX_SESSION_ROWS) {
        const done = this.finish('record_limit'); if (done) completed.push(done);
      }
      return { parsed: p, completed, changed: added || completed.length > 0 };
    }
  }
  return { shuntSettings, gauge, ControlState, DEVICES, enabled, encoderSettings, interval, canSyncPreview, parse, number, csv, Session, SAMPLE_INTERVAL_MS, MAX_SESSION_ROWS };
})();
if (typeof module !== 'undefined') module.exports = DashboardCore;
