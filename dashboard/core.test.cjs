const assert = require('node:assert/strict');
const C = require('./core.js');
function packet(time, supply, boot = 'one') {
  return { boot, supply, header: 'uptime_ms,rtc_datetime,imu_ax_ms2', csv: `${time},2026-09-08 12:00:00,-1.5` };
}
let s = new C.Session();
assert.equal(s.ingest(packet(0, 'off')).completed.length, 0);
for (let t = 100; t <= 1900; t += 200) s.ingest(packet(t, 'on'));
assert.equal(s.active.rows.length, 8, 'one row per 250 ms uptime bucket');
assert.equal(s.ingest(packet(2100, 'unknown')).completed.length, 0, 'unknown is not power off');
const ended = s.ingest(packet(2200, 'off')).completed;
assert.equal(ended.length, 1); assert.equal(ended[0].reason, 'supply_off');
assert.equal(ended[0].rows.length, 10, 'OFF boundary is kept');
assert.equal(s.ingest(packet(2400, 'off')).completed.length, 0, 'no duplicate OFF saves');
const csv = C.csv(ended[0]);
assert.ok(csv.startsWith('\ufeffpc_received_at,supply_state,uptime_ms'));
assert.ok(csv.includes(',-1.5'), 'negative acceleration stays numeric');
assert.equal(csv.trim().split('\r\n').length, 11);
s.ingest(packet(2600, 'on'));
assert.equal(s.ingest(packet(10, 'on', 'two')).completed[0].reason, 'device_reset');
assert.equal(s.active.rows.length, 1, 'new boot starts clean session');
const recovered = new C.Session(JSON.parse(JSON.stringify(s.active)));
assert.equal(recovered.finish('recovered').rows.length, 1);
assert.equal(recovered.finish('manual'), null);
assert.equal(C.number({ x: '' }, 'x'), null);
assert.equal(C.number({ x: 'nan' }, 'x'), null);
assert.equal(C.number({ x: '0' }, 'x'), 0);
assert.equal(C.canSyncPreview(300, 300, 0), true);
assert.equal(C.canSyncPreview(299, 300, 0), false, 'older live preview must not look like a device reset after a buffered sample');
assert.equal(C.canSyncPreview(400, 300, 1), false, 'preview waits while older buffered samples remain');
assert.throws(() => C.parse({ header: 'uptime_ms,a', csv: '1', supply: 'on' }));
let manual = new C.Session();
manual.ingest(packet(100, 'manual'));
assert.equal(manual.finish('connection_lost').rows.length, 1);
console.log('PASS: cadence, OFF/unknown, single save, CSV, reboot, recovery, manual mode, invalid data');

const jitter = new C.Session();
for (const time of [0, 101, 202, 249, 301, 403, 502, 603, 704, 802, 903]) jitter.ingest(packet(time, 'manual'));
assert.equal(jitter.active.rows.length, 4, 'poll jitter does not halve the requested 4 Hz recording rate');
jitter.ingest(packet(903, 'manual'));
assert.equal(jitter.active.rows.length, 4, 'same bucket is not duplicated');
jitter.ingest(packet(850, 'manual'));
assert.equal(jitter.active.rows.length, 4, 'late same-boot packet is ignored instead of resetting the file');
jitter.ingest(packet(3000, 'manual'));
assert.equal(jitter.active.rows.length, 5, 'missing intervals are not fabricated');
const limit = new C.Session();
limit.ingest(packet(0, 'manual'));
limit.active.rows = Array(C.MAX_SESSION_ROWS - 1).fill(limit.active.rows[0]);
assert.equal(limit.ingest(packet(250, 'manual')).completed[0].reason, 'record_limit');
console.log('PASS: 250 ms cadence with jitter, no duplicates/backfill, bounded session size');

for (const bad of [0, 99, 60001, -1, 250.5, '', '250.5', 'abc', '1e3', null]) assert.throws(() => C.interval(bad));
for (const ms of [100, 250, 333, 1000, 60000]) {
 assert.equal(C.interval(String(ms)), ms);
 const configurable = new C.Session();
 for (const t of [0, Math.floor(ms / 2), ms, ms + 1, ms * 2]) configurable.ingest({...packet(t, 'manual'), interval_ms: ms});
 assert.equal(configurable.active.rows.length, 3, `cadence at ${ms} ms`);
}
const changing = new C.Session();
changing.ingest({...packet(0, 'manual'), interval_ms: 250});
changing.ingest({...packet(250, 'manual'), interval_ms: 250});
const transition = changing.ingest({...packet(300, 'manual'), interval_ms: 1000});
assert.equal(transition.completed[0].reason, 'interval_change');
assert.equal(transition.completed[0].rows.length, 2);
assert.equal(changing.active.intervalMs, 1000);
assert.equal(changing.active.rows.length, 1);
assert.equal(changing.ingest({...packet(310, 'manual'), interval_ms: 1000}).completed.length, 0);
assert.equal(changing.active.rows.length, 1);
console.log('PASS: configurable intervals, input rejection, interval change preserves previous session');

const buffered = new C.Session();
const exact = time => ({...packet(time, 'manual'), interval_ms:100, recording:true, recording_session:7});
buffered.ingest(exact(100), '2026-09-08T12:00:00.100Z', true);
buffered.ingest(exact(160), '2026-09-08T12:00:00.160Z', false);
assert.equal(buffered.active.rows.length, 1, 'live preview sync is not stored as an off-cadence row');
const bufferedStop = buffered.ingest({...exact(170), recording:false}, '2026-09-08T12:00:00.170Z', false);
assert.equal(bufferedStop.completed[0].rows.length, 1, 'STOP saves firmware-buffered rows without an extra preview row');
const previewOnly = new C.Session();
previewOnly.ingest(exact(200), '2026-09-08T12:00:00.200Z', false);
assert.equal(previewOnly.active, null, 'preview sync waits for the first firmware-timed sample');
const longBuffered = new C.Session();
for (let t = 100; t <= 113000; t += 100) {
  longBuffered.ingest(exact(t), new Date(1757332800000 + t).toISOString(), true);
  longBuffered.ingest(exact(t + 50), new Date(1757332800050 + t).toISOString(), false);
}
assert.equal(longBuffered.active.rows.length, 1130, '1:53 at 100 ms retains all 1,130 firmware-timed rows');
console.log('PASS: firmware-buffered samples stay separate from live preview synchronization');

assert.equal(C.DEVICES.length, 13);
C.DEVICES.forEach(([id], i) => {
 assert.equal(C.enabled(1 << i, id), true);
 for (const [other] of C.DEVICES) if (other !== id) assert.equal(C.enabled(1 << i, other), false);
});
assert.equal(C.enabled(null, 'imu'), false);
assert.deepEqual(C.encoderSettings('360', '100', '1'), {ppr:360, wheel_mm:100, ratio:1});
for (const args of [[0,100,1],[360,0,1],[360,100,0],[360.5,100,1],[360,100,Infinity],['bad',100,1]]) assert.throws(() => C.encoderSettings(...args));
const fs = require('node:fs');
const firmware = fs.readFileSync(require('node:path').join(__dirname, '../src/main.cpp'), 'utf8');
const names = firmware.match(/deviceNames\[\] = \{([\s\S]*?)\};/)[1].match(/"[^"]+"/g).map(x=>x.slice(1,-1));
assert.deepEqual(names, C.DEVICES.map(d=>d[0]), 'firmware bit positions match all UI switches');
// All 16 GA masks must remain distinct from serial chase/reinitialize commands.
assert.ok(firmware.includes("command = 'A' + raw.toInt()"));
assert.ok(firmware.includes("uint8_t(command - 'A')"));
for(let mask=0;mask<16;mask++) assert.ok(!['l','r','0','1','2','3','4'].includes(String.fromCharCode(65+mask)));
console.log('PASS: device/UI bit mapping, encoder validation, all GA masks separated from serial commands');

assert.deepEqual(C.shuntSettings(['1','1','1']), {ch1:1,ch2:1,ch3:1});
assert.deepEqual(C.shuntSettings([0.01,2.5,1000]), {ch1:0.01,ch2:2.5,ch3:1000});
for (const values of [[0,1,1],[-1,1,1],[1001,1,1],['',1,1],['bad',1,1],[Infinity,1,1],[1,1]]) assert.throws(()=>C.shuntSettings(values));
assert.deepEqual(C.gauge(null,60), {fill:0,max:60});
assert.deepEqual(C.gauge(NaN,60), {fill:0,max:60});
assert.deepEqual(C.gauge(30,60), {fill:50,max:60});
assert.deepEqual(C.gauge(-30,60), {fill:50,max:60});
assert.deepEqual(C.gauge(90,60), {fill:75,max:120});
console.log('PASS: Rshunt unit/input boundaries; gauges handle missing, negative and above-scale values');

const switches = new C.ControlState();
const all = (1 << C.DEVICES.length) - 1;
assert.ok(switches.accept({boot:'boot1',control_revision:0,devices_mask:all}));
assert.ok(switches.accept({boot:'boot1',control_revision:1,devices_mask:all & ~1}));
assert.equal(switches.accept({boot:'boot1',control_revision:0,devices_mask:all}),false,'old ON response must not overwrite confirmed OFF');
assert.equal(C.enabled(switches.mask,'ina1'),false);
assert.equal(switches.accept({boot:'boot1',control_revision:1,devices_mask:all}),false);
assert.ok(switches.accept({boot:'boot2',control_revision:0,devices_mask:all & ~1}));
assert.equal(switches.accept({boot:'boot1',control_revision:2,devices_mask:all}),false,'retired boot must not overwrite restarted board');
assert.equal(C.enabled(switches.mask,'ina1'),false);
assert.ok(switches.accept({boot:'boot2',control_revision:1,devices_mask:all}));
assert.equal(C.enabled(switches.mask,'ina1'),true,'new explicit ON is accepted');
assert.equal(switches.accept({boot:'boot2',control_revision:2,devices_mask:65535}),false);
console.log('PASS: OFF confirmation survives old packets, reboot and stale boot responses; explicit ON works');

// Physical switches gate storage independently from live supply monitoring.
{
  const session = new C.Session();
  const sw = (time, recording, id) => ({ ...packet(time, 'manual'), recording, recording_session: id });
  assert.equal(session.ingest(sw(0, false, 0)).changed, false);
  assert.equal(session.active, null, 'boot is stopped');
  session.ingest(sw(250, true, 1)); session.ingest(sw(500, true, 1));
  const stop = session.ingest(sw(750, false, 1));
  assert.equal(stop.completed[0].reason, 'switch_stop');
  assert.equal(stop.completed[0].rows.length, 2, 'STOP never appends a new row');
  assert.equal(stop.parsed.data.uptime_ms, '750', 'live values still available');
  assert.equal(session.ingest(sw(1000, false, 1)).completed.length, 0);
  session.ingest(sw(1250, true, 2));
  assert.equal(session.active.rows.length, 1, 'new START starts a separate session');
  const restarted = session.ingest(sw(1500, true, 3));
  assert.equal(restarted.completed[0].reason, 'switch_restart', 'detect STOP/START between polls');
  assert.equal(session.active.rows.length, 1);
  const persisted = new C.Session(JSON.parse(JSON.stringify(session.active)));
  assert.equal(persisted.ingest(sw(1750, false, 3)).completed[0].reason, 'switch_stop');
}
console.log('PASS: physical START/STOP, live monitoring while stopped, separate sessions');
