const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../data/LiveReadings.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const logic = script.slice(script.indexOf('function relayBadges('), script.indexOf('function applyFlags('));
const input = { disabled: false, checked: true };
const name = { innerHTML: '' };
const row = { className: 'relay-row off', innerHTML: 'original', querySelector: () => name };
const timers = new Map();
let id = 0, requests = 0, resolve, reject;
const context = vm.createContext({
  _relayPending: {}, AbortController,
  document: { getElementById: key => key === 'relay-row-0' ? row : {} },
  setTimeout: (fn, ms) => { timers.set(++id, {fn, ms}); return id; },
  clearTimeout: timer => timers.delete(timer),
  fetch: (url, options) => {
    requests++;
    assert.equal(url, '/RelayControl.json?relay=0&action=ON');
    assert.equal(options.cache, 'no-store');
    options.signal.addEventListener('abort', () => reject(new Error('Aborted')));
    return new Promise((res, rej) => {resolve=res; reject=rej;});
  }
});
const flush = () => new Promise(res => setImmediate(res));
function release() {
  for (const [key, timer] of timers) {
    if (timer.ms !== 8000) {timers.delete(key); timer.fn();}
  }
}
async function main() {
  vm.runInContext(logic, context);
  context.ctrlRelay(0, 'ON', input);
  assert(input.disabled);
  context.relayBadges([{nb: 0, state: 'OFF'}]);
  assert.equal(row.innerHTML, 'original', 'polling must not replace a disabled control');
  context.ctrlRelay(0, 'ON', input);
  assert.equal(requests, 1);
  resolve({ok: true, json: async () => ({ok: true, state: 'ON'})});
  await flush();
  assert(input.checked && input.disabled);
  context.ctrlRelay(0, 'ON', input);
  assert.equal(requests, 1, 'minimum inter-command delay must remain active');
  release();
  assert(!input.disabled);
  context.relayBadges([{nb: 0, state: 'OFF'}]);
  assert.notEqual(row.innerHTML, 'original');
  // Rendering status must never send a command.
  assert.equal(requests, 1);
  for (const failure of ['http', 'rejected', 'timeout']) {
    input.checked=true;
    context.ctrlRelay(0, 'ON', input);
    if (failure === 'timeout') {
      [...timers.values()].find(timer => timer.ms === 8000).fn();
    } else {
      resolve({ok: failure !== 'http', json: async () => ({ok: false})});
    }
    await flush();
    release();
    assert(!input.disabled && !input.checked);
  }
  assert.equal(requests, 4, 'failed commands must not be retried automatically');
  console.log('PASS: one relay command per click, polling preserves pending controls, failure/timeout recovery');
}
main().catch(error => {console.error(error); process.exitCode=1;});
