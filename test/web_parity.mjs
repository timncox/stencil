// The web page and the firmware must agree on what a word spells and on the
// bytes a command carries. `make -C test web` runs this against the C engine.
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import readline from 'node:readline';

const here = path.dirname(fileURLToPath(import.meta.url));
const SC = createRequire(import.meta.url)(path.join(here, '../docs/stencil-core.js'));

const drv = spawn(path.join(here, 'build/web_parity_driver'));
const rl = readline.createInterface({ input: drv.stdout });
const lines = [];
let waiting = null;
rl.on('line', l => { if (waiting) { const w = waiting; waiting = null; w(l); } else lines.push(l); });
const ask = q => new Promise(res => {
  drv.stdin.write(q + '\n');
  if (lines.length) res(lines.shift()); else waiting = res;
});

let checks = 0, fails = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL', what); } };

const texts = ['E', 'T', 'SOS', 'PARIS', 'paris', 'A B', '  A   B  ', 'A#B', '', '   ',
  'HELLO WORLD', 'Stencil 2026', "what's up? (ok) @tim = +1 / -2 _x_ \"q\" $5 & ;:,.!",
  'the quick brown fox jumps over the lazy dog 0123456789', '0'.repeat(300)];
for (const t of texts) {
  const c = await ask('M ' + t);
  const js = SC.morseEncode(t).join('');
  check(c === js, `morse "${t.slice(0, 20)}": C ${c.length} JS ${js.length}`);
}

const hex = a => a.map(b => b.toString(16)).join(' ');
// user pattern packing: random masks of every length
let seed = 7;
const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
for (let len = 1; len <= 32; len++) {
  const steps = Array.from({ length: len }, () => (rnd() < 0.5 ? 1 : 0));
  const r = (await ask('C ' + hex(SC.cmdSetUser(steps)))).split(' ');
  let mask = 0;
  steps.forEach((v, i) => { if (v) mask = (mask | (1 << i)) >>> 0; });
  check(r[0] === '0' && +r[1] === len && parseInt(r[2], 16) === mask, `user len ${len}`);
}
// words: what the page shows as cleaned is what the firmware stores
for (const w of ['hello there', 'tab\there', 'x'.repeat(60), 'ünïcode ok']) {
  const r = await ask('C ' + hex(SC.cmdSetWord(3, w)));
  check(r.includes('[' + SC.cleanWord(w) + ']') && r.startsWith('0 '), `word "${w.slice(0, 12)}"`);
}
// select
for (const [s, i] of [[0, 16], [1, 32], [2, 7]]) {
  const r = (await ask('C ' + hex(SC.cmdSelect(s, i)))).split(' ');
  check(r[0] === '0' && +r[r.length - 2] === s && +r[r.length - 1] === i, `select ${s} ${i}`);
}
check((await ask('C ' + hex(SC.cmdSelect(0, 17)))).startsWith('1 '), 'select out of range refused');

// HostLink framing
check(SC.crc32([...'123456789'].map(c => c.charCodeAt(0))) === 0xcbf43926, 'crc32 check value');
for (const body of [[], [0], [0, 0, 0], Array.from({ length: 600 }, (_, i) => i & 0xff), [1, 2, 0, 3]]) {
  const w = SC.hostlinkFrame(0x60, 0x1234, body);
  check(w[w.length - 1] === 0 && !w.slice(0, -1).includes(0), 'cobs has no inner zero');
  const p = SC.hostlinkParse(w.slice(0, -1));
  check(p && p.type === 0x60 && p.seq === 0x1234 && p.body.join() === body.join(), `frame round trip ${body.length}`);
}
const bad = SC.hostlinkFrame(0x60, 1, [5, 6]);
bad[3] ^= 1;
check(SC.hostlinkParse(bad.slice(0, -1)) === null, 'corrupt frame rejected');
// sysex stays 7-bit
const sx = SC.sysex(SC.cmdSetWord(0, 'any ~ text'));
check(sx[0] === 0xf0 && sx[sx.length - 1] === 0xf7 && sx.slice(1, -1).every(b => b < 0x80), 'sysex 7-bit');
check(sx.length <= 128 + 2, 'sysex fits libDaisy SYSEX_BUFFER_LEN');
check(SC.sysex(SC.cmdSetWord(0, 'x'.repeat(200))).length - 2 <= 128, 'longest word fits');

drv.stdin.end();
console.log(`${checks} checks, ${fails} failures`);
process.exit(fails ? 1 : 0);
