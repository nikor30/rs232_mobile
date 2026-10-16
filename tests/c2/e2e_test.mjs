// The browser's half (as server/internal/server/web/app.js does it) against
// e2e_host: same keys, same code, frames readable both ways, a changed frame refused.
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
const { subtle } = globalThis.crypto;
const hex = (b) => Buffer.from(b).toString('hex');
const unhex = (h) => new Uint8Array(Buffer.from(h, 'hex'));
const fail = (m) => { console.log('FAILED: ' + m); process.exit(1); };

const kp = await subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, false, ['deriveBits']);
const bpub = new Uint8Array(await subtle.exportKey('raw', kp.publicKey));
const dev = spawn(process.argv[2], [hex(bpub)]);
const lines = createInterface({ input: dev.stdout })[Symbol.asyncIterator]();
const next = async () => (await lines.next()).value || '';

const dpub = unhex((await next()).slice(4));
const devCode = (await next()).slice(5);
const devKey = await subtle.importKey('raw', dpub, { name: 'ECDH', namedCurve: 'P-256' }, false, []);
const z = await subtle.deriveBits({ name: 'ECDH', public: devKey }, kp.privateKey, 256);
const salt = new Uint8Array(130); salt.set(bpub, 0); salt.set(dpub, 65);
const ikm = await subtle.importKey('raw', z, 'HKDF', false, ['deriveBits']);
const okm = new Uint8Array(await subtle.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt, info: new TextEncoder().encode('rs232-console-e2e-v1') }, ikm, 68 * 8));
const kOut = await subtle.importKey('raw', okm.slice(0, 32), 'AES-GCM', false, ['encrypt']);
const kIn = await subtle.importKey('raw', okm.slice(32, 64), 'AES-GCM', false, ['decrypt']);
const code = String(new DataView(okm.buffer, 64, 4).getUint32(0) % 1000000).padStart(6, '0');
if (code !== devCode) fail(`code ${code} here, ${devCode} at the device`);
console.log('ok   same six digits on both sides: ' + code);
const iv = (n) => { const v = new Uint8Array(12); new DataView(v.buffer).setBigUint64(4, BigInt(n)); return v; };

for (let i = 0; i < 2; i++) {
  const f = unhex((await next()).slice(6));
  const len = (f[0] << 8) | f[1];
  if (len !== f.length - 2) fail('frame length');
  const plain = new TextDecoder().decode(await subtle.decrypt({ name: 'AES-GCM', iv: iv(i) }, kIn, f.slice(2)));
  if (!plain.startsWith('Switch>show version')) fail('device frame read as ' + plain);
}
console.log('ok   device -> browser: two frames read');

const seal = async (text, n) => {
  const ct = new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv: iv(n) }, kOut, new TextEncoder().encode(text)));
  const f = new Uint8Array(2 + ct.length); f[0] = ct.length >> 8; f[1] = ct.length & 255; f.set(ct, 2);
  return f;
};
dev.stdin.write(hex(await seal('show running-config', 0)) + '\n');
let r = await next();
if (r !== 'PLAIN show running-config') fail('browser frame read as ' + JSON.stringify(r));
const bad = await seal('reload\r', 1);
bad[5] ^= 1;                              // what a server in between would have to do to change a command
dev.stdin.write(hex(bad) + '\n');
r = await next();
if (r !== 'BAD') fail('a changed frame was taken: ' + r);
console.log('ok   browser -> device: frame read, changed frame refused');
dev.stdin.end();
console.log('E2E CODE INTEROPERATES WITH WEBCRYPTO');
