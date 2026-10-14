const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../data/AdvancedTemperatureConf.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = Object.fromEntries([...html.matchAll(/id="([^"]+)"/g)].map(match => [
  match[1], { value: '', textContent: '-', checked: false }
]));
const config = {
  enabled: true, relay: 0, low: 30, high: 90, buffer: 5,
  tankTopic: 'tank', panelTopic: 'panel', tankGpio: 14, panelGpio: 12,
  tank: 0, panel: 0, relayState: 'OFF'
};
let next = config;
let requests = 0;
let pendingResolve;
let poll;
let expire;
let cleared = 0;
const context = vm.createContext({
  document: { getElementById: id => elements[id] },
  AbortController,
  fetch: (url, options) => {
    requests++;
    assert.equal(url, '/TemperatureConfig.json');
    assert.equal(options.cache, 'no-store');
    assert.equal(options.credentials, 'same-origin');
    if (next === 'pending') return new Promise(resolve => { pendingResolve = resolve; });
    if (next === 'timeout') return new Promise((resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(new Error('Aborted')));
    });
    if (next instanceof Error) return Promise.reject(next);
    if (next === 'http-error') return Promise.resolve({ ok: false });
    return Promise.resolve({ ok: true, json: async () => next });
  },
  setInterval: (callback, ms) => { assert.equal(ms, 2500); poll = callback; },
  setTimeout: (callback, ms) => { assert.equal(ms, 8000); expire = callback; return 1; },
  clearTimeout: () => { cleared++; }
});
const flush = () => new Promise(resolve => setImmediate(resolve));

async function main() {
  vm.runInContext(script, context);
  await flush();
  assert.equal(elements.tank.textContent, '0.00');
  assert.equal(elements.panel.textContent, '0.00');
  assert.equal(elements.low.value, 30);
  assert.equal(elements.enabled.checked, true);
  assert.equal(elements.tankPin.textContent, 'GPIO14');

  elements.low.value = '42';
  elements.tankTopic.value = 'edited/topic';
  elements.enabled.checked = false;
  next = { ...config, tank: 25.0625, panel: -10.125, relayState: 'ON' };
  await poll();
  assert.equal(elements.tank.textContent, '25.06');
  assert.equal(elements.panel.textContent, '-10.13');
  assert.equal(elements.relayState.textContent, 'ON');
  assert.equal(elements.low.value, '42');
  assert.equal(elements.tankTopic.value, 'edited/topic');
  assert.equal(elements.enabled.checked, false);

  for (const missing of [null, undefined, '-', '', ' ', false, NaN, Infinity]) {
    next = { ...config, tank: missing, panel: missing };
    await poll();
    assert.equal(elements.tank.textContent, '-');
    assert.equal(elements.panel.textContent, '-');
  }
  next = { ...config, tank: -127, panel: '19.75' };
  await poll();
  assert.equal(elements.tank.textContent, '-127.00');
  assert.equal(elements.panel.textContent, '19.75');

  for (const failure of [new Error('Offline'), 'http-error']) {
    next = failure;
    await poll();
    assert.equal(elements.tank.textContent, '-');
    assert.equal(elements.panel.textContent, '-');
    assert.equal(elements.relayState.textContent, '-');
  }
  next = 'pending';
  const pending = poll();
  const before = requests;
  await poll();
  assert.equal(requests, before, 'polls must not overlap');
  pendingResolve({ ok: true, json: async () => ({ ...config, tank: 18, panel: 21 }) });
  await pending;
  assert.equal(elements.tank.textContent, '18.00');

  next = 'timeout';
  const timedOut = poll();
  expire();
  await timedOut;
  assert.equal(elements.tank.textContent, '-');
  next = { ...config, tank: 18.5, panel: 21.25 };
  await poll();
  assert.equal(elements.tank.textContent, '18.50');
  assert.equal(elements.panel.textContent, '21.25');
  assert.equal(cleared, requests);
  console.log('Temperature page: polling, actual values, missing readings, form preservation, timeout and recovery passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
