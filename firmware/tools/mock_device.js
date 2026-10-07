#!/usr/bin/env node
/*
 * Device simulator for developing the web UI without hardware.
 * Emulates the firmware's HTTP API + WebSocket protocol (2 serial ports by default,
 * port 1 with a small Cisco-like CLI incl. --More--), stored configs and a simulated
 * XMODEM/YMODEM receiver (type "xmodem" on a port, then send a file from the UI).
 *
 *   npm install ws
 *   node tools/mock_device.js            -> http://localhost:8080  (WebSocket on 8081)
 */
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');

const HTTP_PORT = +(process.env.HTTP_PORT || 8080);
const WS_PORT = +(process.env.WS_PORT || 8081);
const WEB = path.join(__dirname, '..', 'web');
const RING = 16384;
const XFER_BUF = 16384;

const FILES = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/style.css': ['style.css', 'text/css'],
  '/app.js': ['app.js', 'application/javascript'],
  '/xterm.js': ['vendor/xterm.js', 'application/javascript'],
  '/xterm.css': ['vendor/xterm.css', 'text/css'],
  '/addon-fit.js': ['vendor/addon-fit.js', 'application/javascript'],
  '/favicon.svg': ['favicon.svg', 'image/svg+xml'],
};

const bootId = (crypto.randomBytes(4).readUInt32LE(0) | 1) >>> 0;
const token = crypto.randomBytes(8).toString('hex');
const t0 = Date.now();

const settings = {
  apSsid: 'RS232-3F2A', staSsid: '', hostname: 'rs232', webPassSet: false,
  oledType: 0, oledFlip: false, displayTimeout: 60, ledBrightness: 12, tcpEnabled: true, tcpPort: 2000,
  apChannel: 0, txPower: 34,
  staAuth: 0, staIdentity: '', staUser: '', staPassSet: false, staCaCheck: true, staPhase2: 0,
  httpsEnabled: false, httpsLanOnly: false, httpsCert: 0,
  ports: [
    { enabled: true, name: 'Core-SW', rx: 16, tx: 17, hw: true },
    { enabled: true, name: 'Router', rx: 19, tx: 18, hw: true },
    { enabled: false, name: '', rx: 27, tx: 26, hw: false },
    { enabled: false, name: '', rx: 33, tx: 25, hw: false },
  ],
  pinsRx: [4, 5, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39],
  pinsTx: [4, 13, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33],
  pinsAdc: [32, 33, 34, 35, 36, 39],
  pinNames: '36:VP,39:VN,16:RX2,17:TX2', pinPrefix: 'D', hwPorts: 2, swMaxBaud: 38400,
  batPin: 35, batType: 1, batDiv: 30,
};
const batCal = { empty: 0, full: 0 };
const configs = new Map([['Beispiel Grundkonfig', 'enable\nconfigure terminal\nhostname {{HOSTNAME}}\nend\n']]);

// ---- ports: serial settings, replay ring, tiny device ----
class Port {
  constructor(id) {
    this.id = id;
    this.serial = { baud: 9600, bits: 8, parity: 'N', stop: 1, swap: false };
    this.ring = Buffer.alloc(RING);
    this.total = 0;
    this.rx = 0; this.tx = 0; this.autobaud = false;
    this.line = ''; this.mode = '>'; this.more = null;
    this.host = ['Switch', 'Router', 'FW', 'AP'][id];
  }
  get label() { return `${this.serial.baud} ${this.serial.bits}${this.serial.parity}${this.serial.stop}`; }
  out(str) {
    const buf = Buffer.from(str, 'latin1');
    for (let i = 0; i < buf.length; i++) this.ring[(this.total + i) % RING] = buf[i];
    this.total += buf.length;
    this.rx += buf.length;
    const f = Buffer.concat([Buffer.from([this.id]), buf]);
    for (const c of clients) if (c.ready) c.ws.send(f);
  }
  copyFrom(from) {
    const start = this.total > RING ? this.total - RING : 0;
    if (from < start) from = start;
    const n = Math.max(0, this.total - from);
    const out = Buffer.alloc(n);
    for (let i = 0; i < n; i++) out[i] = this.ring[(from + i) % RING];
    return out;
  }
  prompt() { return `\r\n${this.host}${this.mode}`; }
  page(lines) {
    const first = lines.slice(0, 20), rest = lines.slice(20);
    this.out('\r\n' + first.join('\r\n'));
    if (rest.length) { this.more = rest; this.out('\r\n --More-- '); } else this.out(this.prompt());
  }
  exec(cmd) {
    const c = cmd.trim().toLowerCase();
    if (!c) return this.out(this.prompt());
    if (c === 'en' || c === 'enable') { this.mode = '#'; return this.out(this.prompt()); }
    if (c === 'disable') { this.mode = '>'; return this.out(this.prompt()); }
    if (c.startsWith('sh') && c.includes('ver')) return this.page(VERSION);
    if (c.startsWith('sh') && c.includes('clock')) return this.out('\r\n*' + new Date().toUTCString() + this.prompt());
    if (c.startsWith('xmodem') || c === 'loady' || c.startsWith('copy xmodem')) {
      this.out('\r\nReady to receive file ...\r\n');
      this.waitRx = true;
      return this.out('C');
    }
    if (c === 'conf t' || c === 'configure terminal') { this.mode = '(config)#'; return this.out('\r\nEnter configuration commands, one per line.  End with CNTL/Z.' + this.prompt()); }
    if (c.startsWith('hostname ') && this.mode.startsWith('(')) { this.host = cmd.trim().split(/\s+/)[1]; return this.out(this.prompt()); }
    if (c === 'end' || c === 'exit') { this.mode = this.mode.startsWith('(') ? '#' : this.mode; return this.out(this.prompt()); }
    if (this.mode.startsWith('(')) return this.out(this.prompt());
    this.out('\r\n% Invalid input detected at \'^\' marker.\r\n' + this.prompt());
  }
  input(buf) {
    this.tx += buf.length;
    for (const b of buf) {
      const ch = String.fromCharCode(b);
      if (this.more) {
        this.out('\b\b\b\b\b\b\b\b\b\b          \b\b\b\b\b\b\b\b\b\b');
        if (ch === ' ') { const m = this.more; this.more = null; this.page(m); }
        else if (ch === '\r') { const m = this.more; this.more = m.length > 1 ? m.slice(1) : null; this.out(m[0] + (this.more ? '\r\n --More-- ' : this.prompt())); }
        else { this.more = null; this.out(this.prompt()); }
        continue;
      }
      if (b === 0x0d) { this.exec(this.line); this.line = ''; }
      else if (b === 0x1a) { if (this.mode.startsWith('(')) this.mode = '#'; this.line = ''; this.out('^Z' + this.prompt()); }
      else if (b === 0x03) { this.line = ''; this.out('^C' + this.prompt()); }
      else if (b === 0x7f || b === 0x08) { if (this.line) { this.line = this.line.slice(0, -1); this.out('\b \b'); } }
      else if (ch === '?') this.out('?\r\nExec commands:\r\n  enable    Turn on privileged commands\r\n  show      Show running system information' + this.prompt() + this.line);
      else if (ch === '\t' || b === 0x1b) { /* no completion, ignore escapes */ }
      else if (b >= 0x20 && b < 0x7f) { this.line += ch; this.out(ch); }
    }
  }
}
const VERSION = [
  'Cisco IOS XE Software, Version 17.09.04a',
  'Cisco IOS Software [Cupertino], Catalyst L3 Switch Software (CAT9K_IOSXE), Version 17.9.4a, RELEASE SOFTWARE (fc3)',
  'Technical Support: http://www.cisco.com/techsupport',
  'Copyright (c) 1986-2023 by Cisco Systems, Inc.',
  '',
  'ROM: IOS-XE ROMMON',
  'BOOTLDR: System Bootstrap, Version 17.9.1r, RELEASE SOFTWARE (P)',
  '',
  'Switch uptime is 12 weeks, 3 days, 4 hours, 17 minutes',
  'Uptime for this control processor is 12 weeks, 3 days, 4 hours, 19 minutes',
  'System returned to ROM by Reload Command',
  'System image file is "flash:packages.conf"',
  'Last reload reason: Reload Command',
  '',
  'cisco C9300-48P (X86) processor with 1331521K/6147K bytes of memory.',
  'Processor board ID FOC2238X0AB',
  '2048K bytes of non-volatile configuration memory.',
  '8388608K bytes of physical memory.',
  '1638400K bytes of Crash Files at crashinfo:.',
  '11264000K bytes of Flash at flash:.',
  '',
  'Base Ethernet MAC Address          : 70:0f:6a:11:22:33',
  'Motherboard Assembly Number        : 73-17952-06',
  'Model Number                       : C9300-48P',
  'System Serial Number               : FOC2238X0AB',
  '',
  'Configuration register is 0x102',
];
const ports = [0, 1, 2, 3].map((i) => new Port(i));
const enabledPorts = () => ports.filter((p) => settings.ports[p.id].enabled);
const portName = (p) => settings.ports[p.id].name || `Port ${p.id + 1}`;

// ---- clients ----
const clients = new Set();   // {ws, ready}
function status() {
  return {
    type: 'status', fw: '1.4.0-mock', board: 'Simulator', host: settings.hostname, boot: bootId,
    ports: enabledPorts().map((p) => ({
      id: p.id, name: portName(p), hw: settings.ports[p.id].hw, rxPin: settings.ports[p.id].rx, txPin: settings.ports[p.id].tx,
      maxBaud: settings.ports[p.id].hw ? 1000000 : 38400, swapOk: ![34, 35, 36, 39].includes(settings.ports[p.id].rx),
      serial: { ...p.serial, label: p.label }, autobaud: p.autobaud, rx: p.rx, tx: p.tx, tcp: false, tcpPort: 2000 + p.id,
    })),
    xfer: !!xfer,
    bat: { measured: settings.batPin >= 0, present: true, mv: 5010, pct: 60, low: false, type: 'NiCd/NiMH 4 Zellen',
      calEmpty: batCal.empty, calFull: batCal.full },
    clients: clients.size, tcp: false, tcpEnabled: settings.tcpEnabled,
    ap: { ssid: settings.apSsid, ip: '192.168.4.1', stations: 1, channel: 11, txPower: 34 },
    sta: { ssid: settings.staSsid, connected: false, ip: '', rssi: 0,
      auth: ['WPA2/WPA3-PSK', '802.1X EAP-TLS', '802.1X PEAP-MSCHAPv2', '802.1X EAP-TTLS'][settings.staAuth] },
    https: { on: settings.httpsEnabled, running: settings.httpsEnabled, port: 443, lanOnly: settings.httpsLanOnly,
      sessions: 1, cert: 'rs232 · bis 2028-12-31 (Geräte-CA)', busy: false },
    uptime: Math.floor((Date.now() - t0) / 1000), heap: 151000,
  };
}
// ---- certificates (mock: no real crypto, just what the UI needs) ----
const tls = { ca: null, client: null, https: null };
let csr = { busy: false, have: false };
function mockCert(slot, file) {
  const cn = slot === 'ca' ? 'CN=Test Issuing CA' : slot === 'https' ? 'CN=rs232.local' : 'CN=rs232-01';
  return {
    id: slot, cert: true, key: slot !== 'ca', match: slot !== 'ca', ready: true,
    keyType: slot === 'https' ? 'EC P-256' : 'RSA 2048',
    certs: [{
      subject: cn + ', O=Test AG, C=DE', issuer: 'CN=Test Issuing CA, O=Test AG, C=DE',
      from: '2026-01-01', to: '2028-01-01', ca: slot === 'ca', self: false,
      key: slot === 'https' ? 'EC P-256' : 'RSA 2048',
      eku: slot === 'ca' ? [] : slot === 'https' ? ['serverAuth'] : ['clientAuth', 'serverAuth'],
      san: slot === 'ca' ? [] : ['rs232-01.test.local'],
      sha256: 'AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899',
    }],
    file,
  };
}
function tlsInfo() {
  return {
    slots: ['ca', 'client', 'https'].map((id) => tls[id] || { id, cert: false, key: false, match: false, ready: false }),
    csr, devca: {}, time: Math.floor(Date.now() / 1000),
  };
}

function broadcastText(obj) {
  const s = JSON.stringify(obj);
  for (const c of clients) if (c.ready) c.ws.send(s);
}
function msg(text) { broadcastText({ type: 'msg', text }); }

// ---- simulated file transfer (the receiver takes data at the port's baud rate) ----
let xfer = null;
function xferJson(state, m) {
  return {
    type: 'xfer', id: xfer.id, state, phase: state === 'wait' ? 'warte auf Empfänger' : 'Daten', port: xfer.port, proto: xfer.proto,
    name: xfer.name, size: xfer.size, recv: xfer.recv, taken: xfer.taken, sent: xfer.sent, blocks: Math.floor(xfer.sent / 1024),
    bs: xfer.proto === 'XMODEM' ? 128 : 1024, crc: true, retries: 0, ms: xfer.t0 ? Date.now() - xfer.t0 : 0, buf: XFER_BUF, msg: m,
  };
}
function xferTick() {
  if (!xfer) return;
  const p = ports[xfer.port];
  if (!xfer.t0) {
    if (!p.waitRx && Date.now() - xfer.start < 1500) return;       // receiver not started yet
    p.waitRx = false;
    xfer.t0 = Date.now();
    xfer.last = Date.now();
  }
  const rate = p.serial.baud / 10 / 1000;                      // bytes per ms
  const n = Math.min(Math.floor((Date.now() - xfer.last) * rate), xfer.recv - xfer.taken);
  xfer.last += n / rate;
  if (n <= 0) xfer.last = Date.now();
  xfer.taken += Math.max(0, n);
  xfer.sent = xfer.taken;
  if (xfer.sent >= xfer.size) {
    const m = `${xfer.size} Bytes in ${Math.round((Date.now() - xfer.t0) / 1000)} s übertragen`;
    broadcastText(xferJson('done', m));
    p.out('\r\nFile transfer complete.' + p.prompt());
    xfer = null;
    return;
  }
  broadcastText(xferJson('run'));
}
setInterval(xferTick, 200);

// ---- HTTP ----
function sendJson(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(obj));
}
http.createServer((req, res) => {
  const [url, qs] = req.url.split('?');
  const q = new URLSearchParams(qs || '');
  if (req.method === 'GET' && FILES[url]) {
    const [f, mime] = FILES[url];
    res.writeHead(200, { 'Content-Type': mime, 'Cache-Control': 'no-cache' });
    return fs.createReadStream(path.join(WEB, f)).pipe(res);
  }
  if (url === '/api/session') return sendJson(res, 200, { token, wsPort: WS_PORT, boot: bootId });
  if (url === '/api/status') return sendJson(res, 200, status());
  if (url === '/api/settings' && req.method === 'GET') return sendJson(res, 200, settings);
  if (url === '/api/tls' && req.method === 'GET') return sendJson(res, 200, tlsInfo());
  if (url === '/api/tls/download') {
    res.writeHead(200, { 'Content-Type': 'application/x-pem-file', 'Content-Disposition': 'attachment; filename="' + q.get('what') + '.crt"' });
    return res.end('-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n');
  }
  if (url === '/api/configs') {
    return sendJson(res, 200, { ok: true, used: 12288, total: 131072, maxText: 32768,
      configs: [...configs].map(([name, text]) => ({ name, size: Buffer.byteLength(text) })) });
  }
  if (url === '/api/config' && req.method === 'GET') {
    if (!configs.has(q.get('name'))) return sendJson(res, 404, { ok: false, error: 'nicht gefunden' });
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    return res.end(configs.get(q.get('name')));
  }
  if (req.method === 'POST') {
    let body = [];
    req.on('data', (d) => body.push(d));
    req.on('end', () => {
      body = Buffer.concat(body);
      if (url === '/api/config') {                                   // body = text, name in the query
        const name = q.get('name'), old = q.get('old');
        if (!name) return sendJson(res, 400, { ok: false, error: 'Name: 1-48 Zeichen' });
        if (old && old !== name) configs.delete(old);
        configs.set(name, body.toString('utf8'));
        return sendJson(res, 200, { ok: true });
      }
      let j = {};
      try { j = body.length && url.startsWith('/api/') ? JSON.parse(body.toString()) : {}; } catch (e) { return sendJson(res, 400, { ok: false, error: 'JSON ungültig' }); }
      if (url === '/api/settings') {
        for (const k of ['apSsid', 'staSsid', 'hostname', 'oledType', 'oledFlip', 'displayTimeout', 'ledBrightness', 'tcpEnabled', 'apChannel', 'txPower', 'batPin', 'batType', 'batDiv'])
          if (k in j) settings[k] = j[k];
        if (j.staClear) settings.staSsid = '';
        if (Array.isArray(j.ports)) j.ports.forEach((p, i) => Object.assign(settings.ports[i], p, { enabled: i === 0 ? true : !!p.enabled }));
        console.log('settings saved:', JSON.stringify(j));
        broadcastText(status());
        return sendJson(res, 200, { ok: true, reboot: true });
      }
      if (url === '/api/tls/delete') { tls[j.slot] = null; return sendJson(res, 200, { ok: true }); }
      if (url === '/api/tls/csr') {
        if (!j.subject) return sendJson(res, 400, { ok: false, error: 'Name (CN) fehlt' });
        csr = { busy: true, have: false };
        setTimeout(() => { csr = { busy: false, have: true, subject: j.subject, key: j.rsa ? 'RSA 2048' : 'EC P-256' }; }, 2000);
        return sendJson(res, 200, { ok: true });
      }
      if (url === '/api/config/delete') return sendJson(res, configs.delete(j.name) ? 200 : 404, { ok: true });
      if (url === '/api/tls/upload') {
        const text = body.toString('latin1');
        const slot = (text.match(/name="slot"\r?\n\r?\n([a-z]+)/) || [])[1] || 'client';
        const pass = (text.match(/name="pass"\r?\n\r?\n([^\r\n]*)/) || [])[1] || '';
        const file = (text.match(/filename="([^"]*)"/) || [])[1] || '';
        if (/\.p12$|\.pfx$/i.test(file) && !pass) return sendJson(res, 400, { ok: false, error: 'Datei ist verschlüsselt – bitte Passwort angeben' });
        tls[slot] = mockCert(slot, file);
        return sendJson(res, 200, { ok: true, msg: 'Gespeichert: ' + tls[slot].certs[0].subject });
      }
      if (url === '/update') { console.log('firmware upload', body.length, 'bytes'); return sendJson(res, 200, { ok: true }); }
      if (url === '/api/batcal') {
        if (j.point === 'full') batCal.full = 5010;
        else if (j.point === 'empty') batCal.empty = 5010;
        else { batCal.empty = j.empty || 0; batCal.full = j.full || 0; }
        if ((batCal.full || 5400) - (batCal.empty || 4000) < 300) {
          batCal.empty = batCal.full = 0;
          return sendJson(res, 400, { ok: false, error: '0 % und 100 % liegen zu dicht beieinander' });
        }
        broadcastText(status());
        return sendJson(res, 200, { ok: true, mv: 5010, pct: 60, calEmpty: batCal.empty, calFull: batCal.full });
      }
      if (url === '/api/reboot' || url === '/api/factory') return sendJson(res, 200, { ok: true });
      res.writeHead(404); res.end('404');
    });
    return;
  }
  res.writeHead(404); res.end('404');
}).listen(HTTP_PORT, () => console.log(`mock device: http://localhost:${HTTP_PORT}/  (ws ${WS_PORT})`));

// ---- WebSocket ----
const wss = new WebSocketServer({ port: WS_PORT });
wss.on('connection', (ws, req) => {
  if (!req.url.includes(token)) { ws.close(); return; }
  const c = { ws, ready: false };
  clients.add(c);
  ws.on('close', () => clients.delete(c));
  ws.on('message', (data, isBinary) => {
    if (isBinary) {
      const b = Buffer.from(data);
      if (b[0] === 0x7f) {                                          // file data
        if (xfer) xfer.recv += b.length - 1;
        return;
      }
      const p = ports[b[0]];
      if (p && settings.ports[p.id].enabled && !(xfer && xfer.t0 && xfer.port === p.id)) p.input(b.subarray(1));
      return;
    }
    let m;
    try { m = JSON.parse(data.toString()); } catch (e) { return; }
    const p = ports[m.port || 0];
    if (m.cmd === 'hello') {
      const reboot = !!m.boot && m.boot !== bootId;
      const fresh = !m.boot || reboot;
      ws.send(JSON.stringify(status()));
      for (const pt of enabledPorts()) {
        const start = pt.total > RING ? pt.total - RING : 0;
        const seq = (m.seq || [])[pt.id] || 0;
        let from = start, lost = 0;
        if (!fresh) {
          if (seq > pt.total) from = start;
          else if (seq < start) { lost = start - seq; from = start; }
          else from = seq;
        }
        ws.send(JSON.stringify({ type: 'sync', port: pt.id, seq: from, lost, boot: bootId }));
        const buf = pt.copyFrom(from);
        if (buf.length) ws.send(Buffer.concat([Buffer.from([pt.id]), buf]));
      }
      ws.send(JSON.stringify({ type: 'synced', reboot, boot: bootId }));
      c.ready = true;
      if (xfer) ws.send(JSON.stringify(xferJson(xfer.t0 ? 'run' : 'wait')));
    } else if (m.cmd === 'serial') {
      Object.assign(p.serial, { baud: m.baud, bits: m.bits, parity: m.parity, stop: m.stop, swap: !!m.swap });
      msg((enabledPorts().length > 1 ? portName(p) + ' · ' : '') + 'Seriell: ' + p.label);
      broadcastText(status());
    } else if (m.cmd === 'break') {
      msg(`BREAK: ${m.ms} ms gesendet`);
    } else if (m.cmd === 'autobaud') {
      p.autobaud = true;
      msg('Auto-Baud: läuft ...');
      broadcastText(status());
      setTimeout(() => {
        p.autobaud = false;
        Object.assign(p.serial, { baud: 9600, bits: 8, parity: 'N', stop: 1 });
        msg('Auto-Baud: ' + p.label + ' erkannt');
        broadcastText(status());
        p.out(p.prompt());
      }, 1500);
    } else if (m.cmd === 'status') {
      ws.send(JSON.stringify(status()));
    } else if (m.cmd === 'xfer') {
      if (xfer) return ws.send(JSON.stringify({ type: 'xfer', state: 'error', msg: 'Es läuft schon eine Übertragung' }));
      const proto = m.proto === 'y' ? 'YMODEM' : m.proto === 'x' ? 'XMODEM' : 'XMODEM-1K';
      xfer = { id: (crypto.randomBytes(4).readUInt32LE(0) | 1) >>> 0, port: m.port || 0, proto, name: m.name, size: m.size,
        recv: 0, taken: 0, sent: 0, start: Date.now(), t0: 0 };
      broadcastText(xferJson('wait'));
    } else if (m.cmd === 'xferAbort') {
      if (xfer) { broadcastText(xferJson('error', 'abgebrochen')); xfer = null; }
    } else if (m.cmd === 'xferResume') {
      if (xfer && xfer.id === m.id) ws.send(JSON.stringify(xferJson(xfer.t0 ? 'run' : 'wait')));
      else ws.send(JSON.stringify({ type: 'xfer', state: 'idle' }));
    }
  });
});

setInterval(() => broadcastText(status()), 3000);
ports[0].out('\r\nPress RETURN to get started!\r\n\r\n\r\nSwitch>');
ports[1].out('\r\nRouter>');
