const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

let now = Date.now();
class DemoClock extends Date {
  constructor(...args) { super(...(args.length ? args : [now])); }
  static now() { return now; }
}
const window = {
  location: { href: 'http://localhost/' },
  fetch: async () => { throw Error('Unexpected external request'); }
};
const sandbox = {
  window, document: { addEventListener() {} }, Date: DemoClock,
  URL, URLSearchParams, Response, setTimeout, Math
};
const source = fs.readFileSync(path.join(__dirname, 'dashboard_demo_mock.js'), 'utf8');
vm.runInNewContext(source, sandbox, { filename: 'dashboard_demo_mock.js' });

async function state() { return (await window.fetch('/api/state')).json(); }
async function record(boot, action) {
  const response = await window.fetch('/api/record', {
    body: new URLSearchParams({ boot, action })
  });
  assert.equal(response.status, 202);
}

(async () => {
  let packet = await state();
  assert.equal(packet.ga.mask, 0b1111, 'startup warning is all ON');
  assert.equal(packet.ga.chase, false);
  assert.equal(packet.ga.alert_reason, 'startup_pending');

  now += 500;
  packet = await state();
  assert.equal(packet.ga.mask, 0, 'ready and stopped is all OFF');
  assert.equal(packet.ga.alert_reason, '');

  await record(packet.boot, 'start');
  packet = await state();
  assert.equal(packet.ga.mask, 0b1111, 'not-yet-written SD session warns with all ON');
  assert.equal(packet.ga.alert_reason, 'sd_not_ready');

  now += 1000;
  packet = await state();
  assert.equal(packet.ga.chase, true, 'first SD row starts the chase');
  assert.ok([1, 2, 4, 8].includes(packet.ga.mask));
  assert.equal(packet.ga.alert_reason, '');

  await record(packet.boot, 'stop');
  packet = await state();
  assert.equal(packet.ga.mask, 0b1111, 'STOP warns until SD closes');
  assert.equal(packet.ga.alert_reason, 'sd_closing');
  assert.equal(packet.ga.chase, false);

  now += 200;
  packet = await state();
  assert.equal(packet.ga.mask, 0, 'GA turns OFF after SD closes');
  assert.equal(packet.ga.alert_reason, '');

  await record(packet.boot, 'start');
  packet = await state();
  assert.equal(packet.ga.mask, 0b1111, 'prior session cannot make new recording look ready');
  assert.equal(packet.ga.alert_reason, 'sd_not_ready');
  console.log('PASS: GA startup/SD pending/STOP closing ON, ready stopped OFF, recording chase after first write');
})().catch(error => { console.error(error); process.exitCode = 1; });
