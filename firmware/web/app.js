/* RS232 Web Console – browser side
 * WebSocket protocol (port 81):
 *   binary  = [port][serial bytes]  (both directions)   [0x7F][bytes] = file data for XMODEM/YMODEM
 *   text    = JSON: client -> {cmd:'hello'|'serial'|'break'|'autobaud'|'status'|'xfer'|'xferAbort'|'xferResume', port}
 *                   server -> {type:'status'|'sync'|'synced'|'msg'|'xfer'}
 */
(() => {
  'use strict';

  const $ = (s) => document.querySelector(s);
  const $$ = (s) => Array.from(document.querySelectorAll(s));
  const enc = new TextEncoder();
  const LOG_MAX = 8 * 1024 * 1024;       // per port, in the browser
  const XFER_TAG = 0x7f;

  const store = {
    get(k, d) { try { const v = localStorage.getItem('rs232.' + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem('rs232.' + k, JSON.stringify(v)); } catch (e) { /* private mode */ } },
  };

  const state = {
    ws: null, connected: false, retryMs: 500, retryTimer: null,
    boot: 0, token: '', wsPort: 81,
    status: null, ctrl: false, kbd: false, focus: null,
    active: store.get('port', 0), breakArmed: null,
    xfer: null,          // own upload: {file, size, pushed, id, srv, resumed}
    xsrv: null,          // last transfer status from the device (any client)
    play: null,          // running config playback
  };

  // ------------------------------------------------------------------ ports + terminals
  const TERM_OPTS = {
    cursorBlink: false,      // set true once the WebSocket session is actually up (see setCursorBlink)
    cursorStyle: 'block',            // classic terminal look
    cursorInactiveStyle: 'outline',  // hollow while another element has the focus
    fontSize: store.get('fontSize', window.innerWidth < 500 ? 12 : 14),
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", "DejaVu Sans Mono", monospace',
    scrollback: 10000,
    theme: { background: '#0b0f14', foreground: '#d6dde6', cursor: '#7ee787', selectionBackground: '#264f78' },
  };
  const ports = [];               // by port id: {id, name, info, term, fit, el, seq, log, logBytes, welcomed, unread, tail, outCount}
  const dec = new TextDecoder('latin1');

  function portObj(id) {
    if (ports[id]) return ports[id];
    const el = document.createElement('div');
    el.className = 'pterm';
    el.hidden = true;
    $('#term').appendChild(el);
    const term = new Terminal(TERM_OPTS);
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open(el);
    term.options.cursorBlink = state.connected;   // a port opened while already connected starts live
    const p = { id, name: 'Port ' + (id + 1), info: null, term, fit, el, seq: 0, log: [], logBytes: 0, welcomed: false, unread: false, tail: '', outCount: 0 };
    term.onData((data) => {
      if (state.ctrl) { data = applyCtrl(data); setCtrl(false); }
      sendRaw(data, id);
    });
    if (term.textarea) {
      term.textarea.addEventListener('focus', () => { state.kbd = true; state.focus = 'term'; });
      term.textarea.addEventListener('blur', () => { state.kbd = false; });
    }
    ports[id] = p;
    return p;
  }
  const act = () => portObj(state.active);

  function refit() { const p = ports[state.active]; if (p) { try { p.fit.fit(); } catch (e) { /* hidden */ } } }

  function showPort(id) {
    if (!ports[id]) return;
    state.active = id;
    store.set('port', id);
    ports.forEach((p) => { if (p) p.el.hidden = p.id !== id; });
    ports[id].unread = false;
    renderPortbar();
    refit();
    updateHeader();
    $('#cfgPlay').textContent = 'Auf ' + ports[id].name + ' abspielen';
  }

  function renderPortbar() {
    const bar = $('#portbar');
    const list = (state.status && state.status.ports) || [];
    bar.hidden = list.length < 2;
    if (bar.hidden) { refit(); return; }
    bar.textContent = '';
    for (const info of list) {
      const p = portObj(info.id);
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'ptab' + (info.id === state.active ? ' active' : '') + (p.unread ? ' unread' : '');
      b.textContent = (info.id + 1) + ' · ' + info.name;
      b.title = info.serial.label + (info.hw ? '' : ' (Software-UART)');
      b.addEventListener('mousedown', (e) => e.preventDefault());
      b.addEventListener('click', () => {
        showPort(info.id);
        if (document.body.classList.contains('termonly')) ports[info.id].term.focus();
        else if (state.focus === 'cmd') cmdEl.focus();
      });
      bar.appendChild(b);
    }
  }

  function onViewport() {
    const vv = window.visualViewport;
    const h = vv ? vv.height : window.innerHeight;
    document.documentElement.style.setProperty('--app-h', h + 'px');
    if (window.scrollY) window.scrollTo(0, 0);
    refit();
  }
  if (window.ResizeObserver) new ResizeObserver(() => refit()).observe($('#term'));
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(refit);
  if (window.visualViewport) window.visualViewport.addEventListener('resize', onViewport);
  window.addEventListener('resize', onViewport);
  window.addEventListener('orientationchange', () => setTimeout(onViewport, 300));

  function termNote(text, id = state.active) {
    portObj(id).term.write('\r\n\x1b[90m[' + text + ']\x1b[0m\r\n');
  }

  // ------------------------------------------------------------------ helpers
  let toastTimer = null;
  function toast(text, ms = 2600) {
    const t = $('#toast');
    t.textContent = text;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), ms);
  }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  function fmtBytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' kB';
    return (n / 1048576).toFixed(2) + ' MB';
  }
  function fmtUptime(s) {
    const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
    return (d ? d + ' d ' : '') + (h || d ? h + ' h ' : '') + m + ' min';
  }
  function esc(s) { return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

  // ------------------------------------------------------------------ sending
  function wsOpen() { return state.ws && state.ws.readyState === WebSocket.OPEN; }

  function sendBytes(bytes, id = state.active) {
    if (!state.connected || !wsOpen()) { toast('Nicht verbunden'); return false; }
    const f = new Uint8Array(bytes.length + 1);
    f[0] = id;
    f.set(bytes, 1);
    state.ws.send(f);
    return true;
  }
  function sendRaw(str, id = state.active) { return sendBytes(enc.encode(str), id); }

  function sendCmd(obj) {
    if (!wsOpen()) { toast('Nicht verbunden'); return false; }
    state.ws.send(JSON.stringify(obj));
    return true;
  }

  // Sticky Ctrl: next typed character becomes a control character
  const CTRL_MAP = { '@': 0, ' ': 0, '[': 27, '\\': 28, ']': 29, '^': 30, '6': 30, '_': 31, '-': 31, '?': 127 };
  function applyCtrl(data) {
    if (data.length !== 1) return data;
    const c = data.toLowerCase();
    if (c >= 'a' && c <= 'z') return String.fromCharCode(c.charCodeAt(0) - 96);
    if (c in CTRL_MAP) return String.fromCharCode(CTRL_MAP[c]);
    return data;
  }
  function setCtrl(on) {
    state.ctrl = on;
    const b = $('#keys [data-mod="ctrl"]');
    if (b) b.classList.toggle('on', on);
  }

  // ------------------------------------------------------------------ special keys
  const KEYS = [
    { label: 'Ctrl', mod: 'ctrl' },
    { label: 'Tab', seq: '\t' },
    { label: '?', seq: '?' },
    { label: '↑', seq: '\x1b[A' },
    { label: '↓', seq: '\x1b[B' },
    { label: '←', seq: '\x1b[D' },
    { label: '→', seq: '\x1b[C' },
    { label: 'Space', seq: ' ' },
    { label: 'q', seq: 'q' },
    { label: '^C', seq: '\x03' },
    { label: '^Z', seq: '\x1a' },
    { label: '^⇧6', seq: '\x1e', title: 'Ctrl-Shift-6 (Cisco: Abbruch)' },
    { label: 'Esc', seq: '\x1b' },
    { label: '⏎', seq: '\r', title: 'Enter' },
    { label: '⌨', kbd: true, title: 'Tastatur ein/aus' },
  ];
  const keysEl = $('#keys');
  const cmdEl = $('#cmd');
  for (const k of KEYS) {
    const b = document.createElement('button');
    b.type = 'button';
    b.tabIndex = -1;
    b.textContent = k.label;
    if (k.title) b.title = k.title;
    if (k.mod) b.dataset.mod = k.mod;
    b.addEventListener('mousedown', (e) => e.preventDefault());   // keep focus (and keyboard)
    b.addEventListener('click', () => {
      const inCmd = state.focus === 'cmd';
      const refocus = () => { if (inCmd) cmdEl.focus(); else if (state.kbd) act().term.focus(); };
      if (k.kbd) {
        if (state.kbd || inCmd) { act().term.blur(); cmdEl.blur(); } else cmdEl.focus();
        return;
      }
      if (k.mod === 'ctrl') { setCtrl(!state.ctrl); refocus(); return; }
      // Tab / "?" complete the text of the input line on the device (Cisco completion / help)
      if (inCmd && cmdEl.value && (k.seq === '\t' || k.seq === '?')) {
        if (sendRaw(cmdEl.value + k.seq)) cmdEl.value = '';
      } else {
        sendRaw(k.seq);
      }
      refocus();
    });
    keysEl.appendChild(b);
  }

  // ------------------------------------------------------------------ input line
  const EOL = { cr: '\r', crlf: '\r\n', lf: '\n' };
  const eol = () => EOL[store.get('eol', 'cr')] || '\r';
  const hist = store.get('hist', []);
  let histPos = hist.length;

  cmdEl.addEventListener('focus', () => { state.focus = 'cmd'; });
  cmdEl.addEventListener('blur', () => { setTimeout(() => { if (document.activeElement !== cmdEl) state.focus = null; }, 0); });

  function sendLine() {
    const v = cmdEl.value;
    if (!sendRaw(v + eol())) return;
    if (v.trim() && hist[hist.length - 1] !== v) {
      hist.push(v);
      while (hist.length > 50) hist.shift();
      store.set('hist', hist);
    }
    histPos = hist.length;
    cmdEl.value = '';
  }
  $('#inbar').addEventListener('submit', (e) => { e.preventDefault(); sendLine(); cmdEl.focus(); });
  $('#cmdSend').addEventListener('mousedown', (e) => e.preventDefault());

  cmdEl.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowUp' && !e.shiftKey) {
      if (histPos > 0) { histPos--; cmdEl.value = hist[histPos]; }
      e.preventDefault();
    } else if (e.key === 'ArrowDown' && !e.shiftKey) {
      if (histPos < hist.length) { histPos++; cmdEl.value = hist[histPos] || ''; }
      e.preventDefault();
    } else if (e.key === 'Tab') {
      e.preventDefault();
      if (sendRaw(cmdEl.value + '\t')) cmdEl.value = '';
    } else if (e.ctrlKey && e.key.length === 1) {
      e.preventDefault();
      sendRaw(applyCtrl(e.key));
    }
  });
  cmdEl.addEventListener('beforeinput', (e) => {
    if (state.ctrl && e.inputType === 'insertText' && e.data) {
      e.preventDefault();
      sendRaw(applyCtrl(e.data.slice(-1)));
      setCtrl(false);
    }
  });

  // ------------------------------------------------------------------ log
  function logAppend(p, u8) {
    p.log.push(u8);
    p.logBytes += u8.length;
    while (p.logBytes > LOG_MAX && p.log.length > 1) p.logBytes -= p.log.shift().length;
  }

  // Emulate CR/BS/TAB like a terminal so "--More--" erasing etc. disappears
  function cleanText(s) {
    s = s.replace(/\x1b\[[0-9;?]*[ -\/]*[@-~]/g, '')
         .replace(/\x1b[()][0-9A-Za-z]/g, '')
         .replace(/\x1b[=>78DEHMc]/g, '');
    const out = [];
    let line = [], col = 0;
    for (const ch of s) {
      if (ch === '\n') { out.push(line.join('').replace(/\s+$/, '')); line = []; col = 0; }
      else if (ch === '\r') col = 0;
      else if (ch === '\b') { if (col > 0) col--; }
      else if (ch === '\t') { const n = 8 - (col % 8); for (let i = 0; i < n; i++) line[col++] = ' '; }
      else if (ch >= ' ' && ch !== '\x7f') line[col++] = ch;
    }
    if (line.length) out.push(line.join('').replace(/\s+$/, ''));
    return out.join('\n') + '\n';
  }

  function download(name, blob) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(url); a.remove(); }, 2000);
  }

  function saveLog() {
    const p = act();
    if (!p.logBytes) { toast('Noch nichts aufgezeichnet'); return; }
    const all = new Uint8Array(p.logBytes);
    let off = 0;
    for (const c of p.log) { all.set(c, off); off += c.length; }
    let text = new TextDecoder('utf-8').decode(all);
    if ($('#logClean').checked) text = cleanText(text);
    const d = new Date();
    const z = (n) => String(n).padStart(2, '0');
    const host = (state.status && state.status.host) || 'rs232';
    const pname = p.name.replace(/[^A-Za-z0-9_-]+/g, '-');
    const name = `${host}_${pname}_${d.getFullYear()}${z(d.getMonth() + 1)}${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}${z(d.getSeconds())}.log`;
    download(name, new Blob([text], { type: 'text/plain' }));
    toast('Gespeichert: ' + name);
  }

  // ------------------------------------------------------------------ connection
  function setDot(mode) {
    const d = $('#dot');
    d.className = 'dot ' + mode;
    d.title = mode === 'on' ? 'Verbunden' : mode === 'wait' ? 'Verbinde …' : 'Getrennt';
  }

  // Blinking cursor = "you can type now". There's no DCD/DSR wiring on the MAX3232
  // modules to sense whether a real cable is plugged in, so this reflects the next
  // best thing: a live session to the device (WebSocket up and replay finished).
  function setCursorBlink(on) {
    for (const p of ports) if (p) p.term.options.cursorBlink = on;
  }

  function scheduleReconnect() {
    clearTimeout(state.retryTimer);
    state.retryTimer = setTimeout(connect, state.retryMs);
    state.retryMs = Math.min(state.retryMs * 2, 5000);
  }

  async function connect() {
    clearTimeout(state.retryTimer);
    if (state.ws && (state.ws.readyState === WebSocket.OPEN || state.ws.readyState === WebSocket.CONNECTING)) return;
    setDot('wait');
    try {
      // WLAN packets can get lost: never hang forever on a request
      const ctl = typeof AbortController === 'function' ? new AbortController() : null;
      const t = ctl ? setTimeout(() => ctl.abort(), 6000) : 0;
      const r = await fetch('/api/session', { cache: 'no-store', signal: ctl ? ctl.signal : undefined });
      clearTimeout(t);
      if (!r.ok) throw new Error(r.status);
      const s = await r.json();
      state.token = s.token;
      state.wsPort = s.wsPort || 81;
    } catch (e) {
      setDot('off');
      scheduleReconnect();
      return;
    }
    let ws;
    try {
      // over HTTPS the TLS front end forwards /ws to the WebSocket server
      const url = location.protocol === 'https:'
        ? `wss://${location.host}/ws?t=${encodeURIComponent(state.token)}`
        : `ws://${location.hostname}:${state.wsPort}/?t=${encodeURIComponent(state.token)}`;
      ws = new WebSocket(url);
    } catch (e) {
      scheduleReconnect();
      return;
    }
    ws.binaryType = 'arraybuffer';
    state.ws = ws;
    // a WebSocket stuck in CONNECTING would only fail after ~1 min: retry sooner
    const openTimer = setTimeout(() => {
      if (ws.readyState !== WebSocket.OPEN) { try { ws.close(); } catch (e) { /* ignore */ } }
    }, 6000);
    ws.onopen = () => {
      clearTimeout(openTimer);
      const seq = [0, 1, 2, 3].map((i) => (ports[i] ? ports[i].seq : 0));
      ws.send(JSON.stringify({ cmd: 'hello', seq, boot: state.boot, time: Math.floor(Date.now() / 1000) }));
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') onText(ev.data);
      else onBinary(new Uint8Array(ev.data));
    };
    ws.onclose = () => {
      clearTimeout(openTimer);
      if (state.ws !== ws) return;
      const was = state.connected;
      state.connected = false;
      setDot('off');
      setCursorBlink(false);
      if (was) toast('Verbindung getrennt – verbinde neu …');
      scheduleReconnect();
    };
    ws.onerror = () => { try { ws.close(); } catch (e) { /* ignore */ } };
  }

  function onBinary(u8) {
    const id = u8[0];
    if (id > 3) return;
    const data = u8.subarray(1);
    const p = portObj(id);
    p.seq += data.length;
    p.term.write(data);
    logAppend(p, data.slice());
    // output tail for config playback (prompt / @expect detection)
    p.tail = (p.tail + dec.decode(data)).slice(-4096);
    p.outCount += data.length;
    p.lastOut = Date.now();
    if (id !== state.active && state.connected && !p.unread) { p.unread = true; renderPortbar(); }   // not for the replay
  }

  function onText(data) {
    let m;
    try { m = JSON.parse(data); } catch (e) { return; }
    if (m.type === 'status') {
      updateStatus(m);
    } else if (m.type === 'sync') {
      const p = portObj(m.port);
      if (m.lost > 0) termNote(fmtBytes(m.lost) + ' verpasst (Puffer übergelaufen)', m.port);
      p.seq = m.seq;
    } else if (m.type === 'synced') {
      if (state.boot && m.reboot) ports.forEach((p) => { if (p) termNote('Gerät wurde neu gestartet', p.id); });
      state.boot = m.boot;
      state.connected = true;
      state.retryMs = 500;
      setDot('on');
      setCursorBlink(true);
      if (state.xfer && state.xfer.id) {                  // own upload survived the reconnect?
        state.xfer.resumed = true;
        sendCmd({ cmd: 'xferResume', id: state.xfer.id });
      }
    } else if (m.type === 'msg') {
      toast(m.text);
    } else if (m.type === 'xfer') {
      onXfer(m);
    }
  }

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && (!state.ws || state.ws.readyState > WebSocket.OPEN)) connect();
  });

  // ------------------------------------------------------------------ status
  function updateHeader() {
    const s = state.status;
    if (!s) return;
    const info = (s.ports || []).find((x) => x.id === state.active);
    if (info) $('#serChip').textContent = info.autobaud ? 'Auto-Baud …' : info.serial.label;
  }

  function updateStatus(s) {
    state.status = s;
    $('#host').textContent = s.host;
    document.title = s.host + ' · RS232';
    const list = s.ports || [];
    for (const info of list) {
      const p = portObj(info.id);
      p.info = info;
      p.name = info.name;
      if (!p.welcomed) {                                  // first status comes before the replay:
        p.welcomed = true;                                // the device prompt stays the last line
        termNote(`verbunden · ${list.length > 1 ? info.name + ' · ' : ''}${info.serial.label} · Befehl unten eingeben`, info.id);
      }
    }
    if (!list.some((x) => x.id === state.active)) state.active = list.length ? list[0].id : 0;
    if (ports[state.active] && ports[state.active].el.hidden) showPort(state.active);
    else { renderPortbar(); updateHeader(); }
    $('#cfgPlay').textContent = 'Auf ' + act().name + ' abspielen';

    const bc = $('#batChip');
    bc.hidden = !s.bat.measured;
    if (s.bat.present) {
      bc.textContent = '🔋 ' + s.bat.pct + '%';
      bc.title = `Akku ${s.bat.mv} mV (${s.bat.type})`;
    } else {
      bc.textContent = 'USB';
      bc.title = 'Keine Akkuspannung gemessen';
    }
    bc.classList.toggle('low', !!s.bat.low);

    $('#dBat').textContent = !s.bat.measured ? 'keine Messung (Ports → Akku-Messung)'
      : s.bat.present ? `${s.bat.pct} % (${(s.bat.mv / 1000).toFixed(2)} V, ${s.bat.type})${s.bat.low ? ' – schwach!' : ''}` : 'kein Akku (USB-Betrieb)';
    $('#dAp').textContent = `${s.ap.ssid} · ${s.ap.ip} · Kanal ${s.ap.channel || '?'} · ${s.ap.stations} Client(s)`;
    const h = s.https || {};
    $('#dHttps').textContent = !h.on ? 'aus'
      : h.running ? `an · Port ${h.port} · ${h.sessions} Sitzung(en) · ${h.cert || '–'}${h.lanOnly ? ' · im LAN erzwungen' : ''}`
        : (h.busy ? 'Zertifikat wird erzeugt …' : 'startet …' + (h.err ? ' – ' + h.err : ''));
    $('#dSta').textContent = s.sta.ssid
      ? (s.sta.connected ? `${s.sta.ssid} · ${s.sta.ip} · ${s.sta.rssi} dBm · ${s.sta.auth}`
        : `${s.sta.ssid} · ${s.sta.auth} · verbinde …${s.sta.reason ? ' (' + s.sta.reason + ')' : ''}`)
      : 'aus';
    $('#dClients').textContent = `${s.clients} Web${s.tcp ? ' + TCP' : ''}`;
    $('#dPorts').innerHTML = list.map((x) => `${x.id + 1} · ${esc(x.name)}: ${x.serial.label}, GPIO ${x.rxPin}/${x.txPin} (${x.hw ? 'HW' : 'SW'}), `
      + `RX ${fmtBytes(x.rx)} / TX ${fmtBytes(x.tx)}${s.tcpEnabled ? `, TCP ${x.tcpPort}${x.tcp ? ' verbunden' : ''}` : ''}`).join('<br>');
    $('#dUp').textContent = fmtUptime(s.uptime);
    $('#dFw').textContent = 'v' + s.fw;
    $('#dBoard').textContent = s.board || '–';
    $('#dHeap').textContent = fmtBytes(s.heap);

    if ($('#panel').hidden || currentTab !== 'serial') fillSerialForm();
    $('#logInfo').textContent = fmtBytes(act().logBytes) + ' aufgezeichnet (' + act().name + ')';
  }

  // ------------------------------------------------------------------ panel
  let currentTab = 'serial';
  function openPanel(tab) {
    $('#panel').hidden = false;
    showTab(tab || currentTab);
    act().term.blur();
  }
  function closePanel() {
    $('#panel').hidden = true;
    refit();
    if (document.body.classList.contains('termonly')) act().term.focus();   // cursor blinks again
  }
  function showTab(tab) {
    currentTab = tab;
    $$('.tabs button').forEach((b) => {
      b.classList.toggle('active', b.dataset.tab === tab);
      if (b.dataset.tab === tab && b.scrollIntoView) b.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    });
    $$('.tab').forEach((t) => { t.hidden = t.dataset.tab !== tab; });
    if (tab === 'setup') loadSettings();
    if (tab === 'ports') loadPorts();
    if (tab === 'configs') loadConfigs();
    if (tab === 'net') { loadNet(); loadTls(); }
    if (tab === 'session') {
      $('#logInfo').textContent = fmtBytes(act().logBytes) + ' aufgezeichnet (' + act().name + ')';
      $('#fontSize').textContent = TERM_OPTS.fontSize + ' px';
    }
    if (tab === 'serial') fillSerialForm();
  }
  $$('.tabs button').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));
  $('#btnMenu').addEventListener('click', () => openPanel());
  $('#btnClose').addEventListener('click', closePanel);
  $('#serChip').addEventListener('click', () => openPanel('serial'));
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('#panel').hidden) closePanel(); });

  // ------------------------------------------------------------------ serial tab (active port)
  function fillSerialForm() {
    const p = act();
    if (!p.info) return;
    const s = p.info.serial;
    $('#sPortInfo').textContent = `${p.id + 1} · ${p.name} · ${p.info.hw ? 'Hardware-UART' : `Software-UART (max. ${p.info.maxBaud} Baud)`} · GPIO ${p.info.rxPin} RX / ${p.info.txPin} TX`;
    const sel = $('#sBaud');
    const std = Array.from(sel.options).some((o) => o.value === String(s.baud));
    sel.value = std ? String(s.baud) : 'custom';
    $('#sBaudCustomWrap').hidden = std;
    if (!std) $('#sBaudCustom').value = s.baud;
    $('#sBits').value = String(s.bits);
    $('#sParity').value = s.parity;
    $('#sStop').value = String(s.stop);
    $('#sSwap').checked = !!s.swap;
    $('#sSwap').disabled = p.info.swapOk === false && !s.swap;
    $('#sSwap').parentElement.title = p.info.swapOk === false ? 'Der RX-Pin dieses Ports kann nicht senden (nur Eingang)' : '';
    $('#sEol').value = store.get('eol', 'cr');
  }
  $('#sBaud').addEventListener('change', () => { $('#sBaudCustomWrap').hidden = $('#sBaud').value !== 'custom'; });
  $('#sApply').addEventListener('click', () => {
    const baud = $('#sBaud').value === 'custom' ? parseInt($('#sBaudCustom').value, 10) : parseInt($('#sBaud').value, 10);
    if (!(baud >= 300 && baud <= 1000000)) { toast('Ungültige Baudrate'); return; }
    const p = act();
    if (p.info && p.info.maxBaud && baud > p.info.maxBaud) { toast(`Software-UART: höchstens ${p.info.maxBaud} Baud`); return; }
    if (sendCmd({ cmd: 'serial', port: p.id, baud, bits: +$('#sBits').value, parity: $('#sParity').value, stop: +$('#sStop').value, swap: $('#sSwap').checked })) closePanel();
  });
  $('#sEol').addEventListener('change', () => store.set('eol', $('#sEol').value));
  $('#sAuto').addEventListener('click', () => { if (sendCmd({ cmd: 'autobaud', port: state.active })) closePanel(); });
  $('#sBreak').addEventListener('click', () => { sendCmd({ cmd: 'break', port: state.active, ms: +$('#sBreakMs').value }); });

  // Break in the top bar: tap twice within 3 s
  $('#btnBreak').addEventListener('click', () => {
    const b = $('#btnBreak');
    if (state.breakArmed) {
      clearTimeout(state.breakArmed);
      state.breakArmed = null;
      b.classList.remove('armed');
      b.textContent = 'Break';
      sendCmd({ cmd: 'break', port: state.active, ms: +$('#sBreakMs').value });
    } else {
      b.classList.add('armed');
      b.textContent = 'Break?';
      toast('Nochmal tippen, um BREAK zu senden');
      state.breakArmed = setTimeout(() => {
        state.breakArmed = null;
        b.classList.remove('armed');
        b.textContent = 'Break';
      }, 3000);
    }
  });

  // ------------------------------------------------------------------ session tab
  $('#logSave').addEventListener('click', saveLog);
  $('#logClear').addEventListener('click', () => {
    const p = act();
    p.log = [];
    p.logBytes = 0;
    p.term.clear();
    $('#logInfo').textContent = '0 B aufgezeichnet';
    toast('Geleert');
  });
  function setFont(sz) {
    sz = Math.max(9, Math.min(28, sz));
    TERM_OPTS.fontSize = sz;
    ports.forEach((p) => { if (p) p.term.options.fontSize = sz; });
    store.set('fontSize', sz);
    $('#fontSize').textContent = sz + ' px';
    refit();
  }
  $('#fontMinus').addEventListener('click', () => setFont(TERM_OPTS.fontSize - 1));
  $('#fontPlus').addEventListener('click', () => setFont(TERM_OPTS.fontSize + 1));

  // ------------------------------------------------------------------ terminal-only mode
  // Real keyboard + mouse: default to the plain terminal window. Touch devices keep
  // the input line, where a native text field beats xterm's hidden textarea.
  const desktopLike = !!(window.matchMedia && window.matchMedia('(pointer: fine) and (min-width: 700px)').matches);
  function setTermOnly(on, focus = true) {
    document.body.classList.toggle('termonly', on);
    $('#termOnly').checked = on;
    store.set('termOnly', on);
    refit();
    if (focus) (on ? act().term : cmdEl).focus();
  }
  $('#termOnly').addEventListener('change', (e) => setTermOnly(e.target.checked));

  // ------------------------------------------------------------------ run bar (upload / playback)
  function showRun(text, pct) {
    $('#runbar').hidden = false;
    $('#runText').textContent = text;
    const pr = $('#runProg');
    if (pct === null || pct === undefined) pr.removeAttribute('value');
    else pr.value = pct;
    refit();
  }
  let runHideTimer = null;
  function hideRun(ms = 0) {
    clearTimeout(runHideTimer);
    runHideTimer = setTimeout(() => { $('#runbar').hidden = true; refit(); }, ms);
  }
  $('#runStop').addEventListener('click', () => {
    if (state.play) { state.play.cancel = true; return; }
    if (state.xsrv && (state.xsrv.state === 'wait' || state.xsrv.state === 'run')) sendCmd({ cmd: 'xferAbort' });
  });

  // ------------------------------------------------------------------ file upload (XMODEM / YMODEM)
  const XF_CHUNK = 1024;
  let wakeLock = null;
  async function keepAwake(on) {
    try {
      if (on && navigator.wakeLock && !wakeLock) wakeLock = await navigator.wakeLock.request('screen');
      if (!on && wakeLock) { await wakeLock.release(); wakeLock = null; }
    } catch (e) { wakeLock = null; }
  }

  $('#xfStart').addEventListener('click', () => {
    const file = $('#xfFile').files[0];
    if (!file) { toast('Bitte eine Datei auswählen'); return; }
    if (!file.size) { toast('Die Datei ist leer'); return; }
    if (state.play) { toast('Es wird gerade eine Konfiguration abgespielt'); return; }
    if (state.xsrv && (state.xsrv.state === 'wait' || state.xsrv.state === 'run')) { toast('Es läuft schon eine Übertragung'); return; }
    const proto = $('#xfProto').value;
    state.xfer = { file, size: file.size, pushed: 0, id: 0, srv: null, cache: null, cacheOff: 0, pumping: false, t0: Date.now() };
    if (!sendCmd({ cmd: 'xfer', port: state.active, proto, name: file.name, size: file.size })) { state.xfer = null; return; }
    closePanel();
    keepAwake(true);
    showRun(`${file.name}: warte auf Gerät …`, null);
  });

  async function readSlice(x, off, n) {
    if (!x.cache || off < x.cacheOff || off + n > x.cacheOff + x.cache.length) {
      const end = Math.min(x.size, off + 65536);
      x.cache = new Uint8Array(await x.file.slice(off, end).arrayBuffer());
      x.cacheOff = off;
    }
    return x.cache.subarray(off - x.cacheOff, off - x.cacheOff + n);
  }

  async function xfPump() {
    const x = state.xfer;
    if (!x || x.pumping || !x.srv || !wsOpen()) return;
    x.pumping = true;
    try {
      const win = (x.srv.buf || 16384) - 4096;
      while (state.xfer === x && x.pushed < x.size && x.pushed - x.srv.taken + XF_CHUNK <= win && wsOpen()) {
        const n = Math.min(XF_CHUNK, x.size - x.pushed);
        const data = await readSlice(x, x.pushed, n);
        const f = new Uint8Array(n + 1);
        f[0] = XFER_TAG;
        f.set(data, 1);
        state.ws.send(f);
        x.pushed += n;
      }
    } catch (e) {
      toast('Datei kann nicht gelesen werden');
      sendCmd({ cmd: 'xferAbort' });
    } finally {
      x.pumping = false;
    }
  }

  function onXfer(m) {
    state.xsrv = m;
    const x = state.xfer;
    if (m.state === 'idle') {                      // after a reconnect: device has no transfer (any more)
      if (x) { state.xfer = null; keepAwake(false); hideRun(); }
      return;
    }
    const own = x && (x.id === m.id || (!x.id && m.name));
    if (own) {
      x.id = m.id;
      x.srv = m;
      if (x.resumed && m.recv !== undefined) { x.pushed = m.recv; x.resumed = false; }
    }
    const pname = (ports[m.port] && ports[m.port].name) || 'Port ' + (m.port + 1);
    if (m.state === 'wait') {
      showRun(`${m.proto} → ${pname}: warte auf Empfänger (C/NAK) …`, null);
    } else if (m.state === 'run') {
      const pct = m.size ? Math.floor((m.sent / m.size) * 100) : 0;
      const kbs = m.ms > 500 ? (m.sent / m.ms).toFixed(1) + ' kB/s' : '';
      showRun(`${m.proto} → ${pname}: ${pct} % · ${fmtBytes(m.sent)} von ${fmtBytes(m.size)}${kbs ? ' · ' + kbs : ''}${m.retries ? ' · ' + m.retries + ' Wdh.' : ''}`, pct);
    } else if (m.state === 'done' || m.state === 'error') {
      if (m.state === 'done') {
        showRun(`${m.proto} → ${pname}: fertig, ${m.msg}`, 100);
        toast(`${m.name}: ${m.msg}`, 5000);
      } else {
        showRun(`${m.proto} → ${pname}: ${m.msg || 'Fehler'}`, 0);
        toast(`Übertragung: ${m.msg || 'Fehler'}`, 6000);
      }
      if (own || !m.id) { state.xfer = null; keepAwake(false); }
      hideRun(5000);
      return;
    }
    if (own) xfPump();
  }

  // ------------------------------------------------------------------ configs (stored on the device)
  let cfgEditing = null;          // name of the config in the editor (null = new)

  async function loadConfigs() {
    try {
      const r = await fetch('/api/configs', { cache: 'no-store' });
      const j = await r.json();
      const list = $('#cfgList');
      list.textContent = '';
      j.configs.sort((a, b) => a.name.localeCompare(b.name, 'de'));
      if (!j.configs.length) list.innerHTML = '<p class="hint">Noch keine Konfiguration gespeichert.</p>';
      for (const c of j.configs) {
        const row = document.createElement('div');
        row.className = 'item';
        row.innerHTML = `<span class="iname">${esc(c.name)}</span><span class="isize">${fmtBytes(c.size)}</span>`;
        const play = document.createElement('button');
        play.type = 'button';
        play.className = 'btn primary small';
        play.textContent = '▶';
        play.title = 'Abspielen auf ' + act().name;
        play.addEventListener('click', async () => { const t = await fetchConfig(c.name); if (t !== null) startPlay(c.name, t); });
        const edit = document.createElement('button');
        edit.type = 'button';
        edit.className = 'btn small';
        edit.textContent = 'Bearbeiten';
        edit.addEventListener('click', async () => {
          const t = await fetchConfig(c.name);
          if (t === null) return;
          cfgEditing = c.name;
          $('#cfgName').value = c.name;
          $('#cfgText').value = t;
          $('#cfgEditTitle').textContent = 'Bearbeiten: ' + c.name;
          $('#cfgDelete').hidden = false;
          $('#cfgName').scrollIntoView({ block: 'center' });
        });
        row.append(play, edit);
        list.appendChild(row);
      }
      $('#cfgInfo').textContent = j.ok ? `Speicher: ${fmtBytes(j.used)} von ${fmtBytes(j.total)} belegt` : 'Speicher nicht verfügbar';
    } catch (e) {
      toast('Konfigurationen konnten nicht geladen werden');
    }
  }

  async function fetchConfig(name) {
    try {
      const r = await fetch('/api/config?name=' + encodeURIComponent(name), { cache: 'no-store' });
      if (!r.ok) throw new Error(r.status);
      return await r.text();
    } catch (e) {
      toast('Laden fehlgeschlagen');
      return null;
    }
  }

  function cfgReset() {
    cfgEditing = null;
    $('#cfgName').value = '';
    $('#cfgText').value = '';
    $('#cfgEditTitle').textContent = 'Neue Konfiguration';
    $('#cfgDelete').hidden = true;
  }
  $('#cfgNew').addEventListener('click', cfgReset);

  $('#cfgSave').addEventListener('click', async () => {
    const name = $('#cfgName').value.trim();
    const text = $('#cfgText').value;
    if (!name) { toast('Bitte einen Namen eingeben'); return; }
    try {
      const q = '?name=' + encodeURIComponent(name) + '&old=' + encodeURIComponent(cfgEditing || '');
      const r = await fetch('/api/config' + q, { method: 'POST', headers: { 'Content-Type': 'text/plain;charset=utf-8' }, body: text });
      const j = await r.json();
      if (!j.ok) { toast('Fehler: ' + j.error, 4000); return; }
      cfgEditing = name;
      $('#cfgEditTitle').textContent = 'Bearbeiten: ' + name;
      $('#cfgDelete').hidden = false;
      toast('Gespeichert: ' + name);
      loadConfigs();
    } catch (e) {
      toast('Speichern fehlgeschlagen');
    }
  });

  $('#cfgDelete').addEventListener('click', async () => {
    if (!cfgEditing || !confirm(`„${cfgEditing}“ löschen?`)) return;
    try {
      await fetch('/api/config/delete', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name: cfgEditing }) });
      toast('Gelöscht');
      cfgReset();
      loadConfigs();
    } catch (e) {
      toast('Löschen fehlgeschlagen');
    }
  });

  $('#cfgFile').addEventListener('change', async () => {
    const f = $('#cfgFile').files[0];
    if (!f) return;
    if (f.size > 32768) { toast('Datei zu groß (max. 32 kB)'); return; }
    $('#cfgText').value = (await f.text()).replace(/\r\n?/g, '\n');
    if (!$('#cfgName').value.trim()) $('#cfgName').value = f.name.replace(/\.[^.]+$/, '').slice(0, 48);
    $('#cfgFile').value = '';
    toast('Geladen – zum Behalten „Speichern“');
  });

  $('#cfgPlay').addEventListener('click', () => {
    const text = $('#cfgText').value;
    if (!text.trim()) { toast('Kein Text im Editor'); return; }
    startPlay($('#cfgName').value.trim() || 'Editor', text);
  });

  // ---- playback --------------------------------------------------------------
  const VAR_RE = /\{\{\s*([A-Za-z0-9_]+)\s*\}\}/g;
  const PROMPT_RE = /[>#$%\]:)]\s?$/;
  const ERROR_RE = /(^|\n)\s*% ?(Invalid|Incomplete|Ambiguous|Unknown|Unrecognized|Bad|Error)|Command fail|(^|\n)\s*error:|unknown command|Unknown action/i;

  function askVars(names, title) {
    return new Promise((resolve) => {
      const dlg = $('#varsDlg');
      const box = $('#varsFields');
      box.textContent = '';
      const saved = store.get('vars', {});
      for (const n of names) {
        const l = document.createElement('label');
        l.textContent = n;
        const i = document.createElement('input');
        i.name = n;
        i.value = saved[n] || '';
        i.autocapitalize = 'off';
        i.setAttribute('autocorrect', 'off');
        i.spellcheck = false;
        l.appendChild(i);
        box.appendChild(l);
      }
      $('#varsTitle').textContent = title;
      dlg.hidden = false;
      const first = box.querySelector('input');
      if (first) setTimeout(() => first.focus(), 50);
      const done = (vals) => {
        dlg.hidden = true;
        $('#varsForm').onsubmit = null;
        $('#varsCancel').onclick = null;
        resolve(vals);
      };
      $('#varsForm').onsubmit = (e) => {
        e.preventDefault();
        const vals = {};
        for (const n of names) vals[n] = box.querySelector(`input[name="${n}"]`).value;
        store.set('vars', Object.assign(saved, vals));
        done(vals);
      };
      $('#varsCancel').onclick = () => done(null);
    });
  }

  // wait until pred() is true (or timeout / cancel)
  async function waitOutput(run, pred, timeoutMs) {
    const t0 = Date.now();
    while (!run.cancel) {
      if (pred()) return true;
      if (Date.now() - t0 > timeoutMs) return false;
      await sleep(40);
    }
    return false;
  }

  // output of port p since output counter "mark" (as far as the 4 kB tail reaches)
  function tailSince(p, mark) {
    const n = Math.min(p.tail.length, p.outCount - mark);
    return n > 0 ? p.tail.slice(-n) : '';
  }

  async function startPlay(name, text) {
    if (state.play) { toast('Es läuft schon eine Wiedergabe'); return; }
    if (state.xsrv && (state.xsrv.state === 'wait' || state.xsrv.state === 'run')) { toast('Es läuft gerade eine Datei-Übertragung'); return; }
    if (!state.connected) { toast('Nicht verbunden'); return; }
    const names = [...new Set([...text.matchAll(VAR_RE)].map((m) => m[1]))];
    if (names.length) {
      const vals = await askVars(names, `„${name}“: Werte eintragen`);
      if (!vals) return;
      text = text.replace(VAR_RE, (_, n) => vals[n]);
    }
    const lines = text.replace(/\r\n?/g, '\n').split('\n');
    while (lines.length && lines[lines.length - 1] === '') lines.pop();
    if (!lines.length) { toast('Kein Text'); return; }

    const p = act();
    const mode = $('#playMode').value;
    const delay = +$('#playDelay').value;
    const stopOnError = $('#playStop').checked;
    const run = { cancel: false };
    state.play = run;
    closePanel();
    let timeout = 30000;
    let errors = 0, i = 0, result = '';
    let mark = p.outCount - p.tail.length;      // @expect may match text that is already on screen
    const label = (k) => `„${name}“ → ${p.name}: Zeile ${k} von ${lines.length}`;
    try {
      for (; i < lines.length; i++) {
        if (run.cancel) { result = 'abgebrochen'; break; }
        if (!state.connected) {                                    // WLAN hiccup: wait for the reconnect
          showRun(`${label(i + 1)} · warte auf Verbindung …`, null);
          await waitOutput(run, () => state.connected, 60000);
          if (!state.connected) { result = 'Verbindung verloren'; break; }
        }
        const pct = Math.floor((i / lines.length) * 100);
        showRun(label(i + 1), pct);
        const line = lines[i];
        const dir = /^@(\w+)\s*(.*)$/.exec(line);
        if (dir) {
          const cmd = dir[1].toLowerCase(), arg = dir[2].trim();
          if (cmd === 'pause') {
            const t0 = Date.now(), ms = (parseFloat(arg) || 1) * 1000;
            while (!run.cancel && Date.now() - t0 < ms) await sleep(100);
          } else if (cmd === 'expect') {
            const want = arg.toLowerCase();
            showRun(`${label(i + 1)} · warte auf „${arg}“`, pct);
            const ok = await waitOutput(run, () => tailSince(p, mark).toLowerCase().includes(want), timeout);
            if (!ok) { if (!run.cancel) result = `„${arg}“ kam nicht (Zeile ${i + 1})`; break; }
            mark = p.outCount;
          } else if (cmd === 'break') {
            const ms = parseInt(arg, 10) || 500;
            sendCmd({ cmd: 'break', port: p.id, ms });
            await sleep(ms + 300);
          } else if (cmd === 'timeout') {
            timeout = Math.max(1, parseFloat(arg) || 30) * 1000;
          } else {
            result = `unbekannte Steuerzeile @${cmd} (Zeile ${i + 1})`;
            break;
          }
          continue;
        }
        const base = p.outCount;
        mark = base;
        if (!sendRaw(line + eol(), p.id)) { result = 'Verbindung verloren'; break; }
        if (mode === 'delay') {
          await sleep(delay);
        } else {
          // wait for echo + answer until the device shows a prompt again (or stays quiet)
          let seen = p.outCount, quietSince = Date.now(), moreAt = -1;
          const t0 = Date.now();
          await waitOutput(run, () => {
            if (p.outCount !== seen) { seen = p.outCount; quietSince = Date.now(); }
            const got = p.outCount - base, quiet = Date.now() - quietSince;
            if (got > 0 && /--More--\s*$/.test(p.tail)) {
              if (moreAt !== p.outCount) { moreAt = p.outCount; sendRaw(' ', p.id); }
              return false;
            }
            if (got > 0 && quiet >= 120 && PROMPT_RE.test(p.tail)) return true;
            return quiet >= 2000 && Date.now() - t0 >= 2000;         // nothing more comes
          }, timeout);
          if (delay > 150) await sleep(delay - 150);
        }
        if (ERROR_RE.test(tailSince(p, base))) {
          errors++;
          if (stopOnError) { result = `Fehler bei Zeile ${i + 1}: ${line.trim()}`; i++; break; }
        }
      }
    } finally {
      state.play = null;
    }
    const done = i >= lines.length && !result;
    const msg = done ? `„${name}“: ${lines.length} Zeilen gesendet${errors ? `, ${errors} Fehlermeldung(en)` : ''}` : `„${name}“: ${result}`;
    showRun(msg, done ? 100 : Math.floor((i / lines.length) * 100));
    toast(msg, 5000);
    hideRun(done && !errors ? 4000 : 8000);
  }

  // ------------------------------------------------------------------ ports tab (pin assignment)
  let portsCfg = null;

  function pinLabel(pin) {
    if (!portsCfg) return String(pin);
    const n = portsCfg.pinNameMap[pin];
    if (portsCfg.pinPrefix === 'D') return `GPIO${pin} (${n || 'D' + pin})`;
    return `${portsCfg.pinPrefix}${pin}${n ? ' (' + n + ')' : ''}`;
  }

  function pinSelect(list, value) {
    const s = document.createElement('select');
    for (const pin of list) {
      const o = document.createElement('option');
      o.value = pin;
      o.textContent = pinLabel(pin);
      s.appendChild(o);
    }
    if (!list.includes(value)) {
      const o = document.createElement('option');
      o.value = value;
      o.textContent = pinLabel(value) + ' (?)';
      s.appendChild(o);
    }
    s.value = value;
    return s;
  }

  async function loadPorts() {
    try {
      const r = await fetch('/api/settings', { cache: 'no-store' });
      const s = await r.json();
      const map = {};
      (s.pinNames || '').split(',').forEach((kv) => { const [k, v] = kv.split(':'); if (k && v) map[+k] = v; });
      portsCfg = { ...s, pinNameMap: map };
      const box = $('#portCards');
      box.textContent = '';
      s.ports.forEach((pt, i) => {
        const card = document.createElement('div');
        card.className = 'pcard';
        card.dataset.port = i;
        card.innerHTML = `<div class="pcard-head"><label class="check"><input type="checkbox" data-k="enabled"> Port ${i + 1}</label>`
          + `<span class="tag ${pt.hw ? '' : 'sw'}">${pt.hw ? 'Hardware-UART' : 'Software-UART'}</span></div>`
          + `<div class="grid3"><label class="wide">Name <input data-k="name" maxlength="24" placeholder="Port ${i + 1}"></label>`
          + `<label>RX-Pin <span class="slot" data-slot="rx"></span></label><label>TX-Pin <span class="slot" data-slot="tx"></span></label></div>`
          + `<p class="hint wiring"></p>`;
        const en = card.querySelector('[data-k="enabled"]');
        en.checked = pt.enabled;
        if (i === 0) { en.checked = true; en.disabled = true; }
        card.querySelector('[data-k="name"]').value = pt.name;
        const rx = pinSelect(s.pinsRx, pt.rx);
        rx.dataset.k = 'rx';
        const tx = pinSelect(s.pinsTx, pt.tx);
        tx.dataset.k = 'tx';
        card.querySelector('[data-slot="rx"]').replaceWith(rx);
        card.querySelector('[data-slot="tx"]').replaceWith(tx);
        box.appendChild(card);
      });
      const bp = $('#batPin');
      bp.textContent = '';
      bp.appendChild(new Option('aus', '-1'));
      for (const pin of s.pinsAdc) bp.appendChild(new Option(pinLabel(pin), String(pin)));
      bp.value = String(s.batPin);
      const lb = $('#batLbo');
      lb.textContent = '';
      lb.appendChild(new Option('aus', '-1'));
      for (const pin of s.pinsRx) lb.appendChild(new Option(pinLabel(pin), String(pin)));
      lb.value = String(s.batLbo);
      $('#batType').value = String(s.batType);
      $('#batDiv').value = String(s.batDiv);
      $('#portsIntro').textContent = `Bis zu 4 MAX3232-Module. Ports 1–${s.hwPorts} laufen über Hardware-UARTs, `
        + `Ports ${s.hwPorts + 1}–4 über Software-UARTs (bis ${s.swMaxBaud} Baud, für Konsolen reicht das). Jedes Modul: VCC an 3V3, GND an GND.`;
      checkPorts();
    } catch (e) {
      toast('Port-Einstellungen konnten nicht geladen werden');
    }
  }

  function readPorts() {
    return $$('#portCards .pcard').map((c) => ({
      enabled: c.querySelector('[data-k="enabled"]').checked,
      name: c.querySelector('[data-k="name"]').value.trim(),
      rx: +c.querySelector('[data-k="rx"]').value,
      tx: +c.querySelector('[data-k="tx"]').value,
    }));
  }

  // highlight double pins, show wiring per port; returns an error text or ''
  function checkPorts() {
    const list = readPorts();
    const bat = +$('#batPin').value;
    const use = {};
    const mark = (pin, what) => { (use[pin] = use[pin] || []).push(what); };
    list.forEach((p, i) => { if (p.enabled) { mark(p.rx, `Port ${i + 1} RX`); mark(p.tx, `Port ${i + 1} TX`); } });
    if (bat >= 0) mark(bat, 'Akku');
    const lbo = +$('#batLbo').value;
    if (lbo >= 0) mark(lbo, 'LBO');
    let err = '';
    $$('#portCards .pcard').forEach((c, i) => {
      const p = list[i];
      c.classList.toggle('off', !p.enabled);
      c.querySelectorAll('input[data-k="name"], select').forEach((el) => { el.disabled = !p.enabled; });
      const rxDup = p.enabled && use[p.rx].length > 1, txDup = p.enabled && use[p.tx].length > 1;
      c.querySelector('[data-k="rx"]').classList.toggle('bad', rxDup);
      c.querySelector('[data-k="tx"]').classList.toggle('bad', txDup);
      if (p.enabled && p.rx === p.tx) err = err || `Port ${i + 1}: RX und TX gleich`;
      if (rxDup || txDup) err = err || `GPIO${rxDup ? p.rx : p.tx} ist doppelt belegt (${use[rxDup ? p.rx : p.tx].join(', ')})`;
      c.querySelector('.wiring').textContent = p.enabled
        ? `MAX3232-Modul: TXD an ${pinLabel(p.tx)}, RXD an ${pinLabel(p.rx)}, VCC an 3V3, GND an GND` : 'aus';
    });
    $('#batPin').classList.toggle('bad', bat >= 0 && use[bat].length > 1);
    $('#batLbo').classList.toggle('bad', lbo >= 0 && use[lbo].length > 1);
    $('#portsSave').disabled = !!err;
    if (err) $('#portsSave').title = err; else $('#portsSave').removeAttribute('title');
    return err;
  }
  $('#portCards').addEventListener('change', checkPorts);
  $('#batPin').addEventListener('change', checkPorts);
  $('#batLbo').addEventListener('change', checkPorts);

  $('#portsSave').addEventListener('click', async () => {
    const err = checkPorts();
    if (err) { toast(err, 4000); return; }
    if (!confirm('Ports speichern und neu starten?')) return;
    const body = { ports: readPorts(), batPin: +$('#batPin').value, batLbo: +$('#batLbo').value,
                   batType: +$('#batType').value, batDiv: +$('#batDiv').value };
    try {
      const r = await fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      const j = await r.json();
      if (j.ok) { toast('Gespeichert – Neustart …', 4000); closePanel(); }
      else toast('Fehler: ' + (j.error || r.status), 5000);
    } catch (e) {
      toast('Fehler beim Speichern');
    }
  });

  // ------------------------------------------------------------------ device tab
  $('#devReboot').addEventListener('click', async () => {
    if (!confirm('Gerät neu starten?')) return;
    try { await fetch('/api/reboot', { method: 'POST' }); toast('Neustart …'); } catch (e) { toast('Fehler'); }
  });

  // ------------------------------------------------------------------ setup tab
  async function loadSettings() {
    try {
      const r = await fetch('/api/settings', { cache: 'no-store' });
      const s = await r.json();
      const f = $('#setupForm');
      f.apSsid.value = s.apSsid;
      f.apPass.value = '';
      f.hostname.value = s.hostname;
      f.webPass.value = '';
      f.webPass.placeholder = s.webPassSet ? 'unverändert (gesetzt)' : 'keins';
      f.webPassClear.checked = false;
      f.webPassClear.parentElement.hidden = !s.webPassSet;
      f.tcpEnabled.checked = s.tcpEnabled;
      f.tcpLan.checked = s.tcpLan;
      f.oledType.value = String(s.oledType);
      f.oledFlip.checked = s.oledFlip;
      f.displayTimeout.value = s.displayTimeout;
      f.oledBrightness.value = s.oledBrightness;
      f.ledBrightness.value = s.ledBrightness;
      $('#tcpPort').textContent = s.tcpPort;
      f.apChannel.value = String(s.apChannel || 0);
      f.txPower.value = String(s.txPower || 44);
    } catch (e) {
      toast('Einstellungen konnten nicht geladen werden');
    }
  }

  // live preview while dragging (the device only stores it on "Speichern & Neustart")
  let oledPrevAt = 0;
  $('#setupForm').oledBrightness.addEventListener('input', (e) => {
    const now = Date.now();
    if (!state.connected || now - oledPrevAt < 60) return;   // no toast storm while dragging
    oledPrevAt = now;
    sendCmd({ cmd: 'oledBrightness', value: +e.target.value });
  });

  $('#setupForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    const body = {
      apSsid: f.apSsid.value.trim(),
      hostname: f.hostname.value.trim(),
      webPassClear: f.webPassClear.checked,
      tcpEnabled: f.tcpEnabled.checked,
      tcpLan: f.tcpLan.checked,
      oledType: +f.oledType.value,
      oledFlip: f.oledFlip.checked,
      displayTimeout: +f.displayTimeout.value,
      oledBrightness: +f.oledBrightness.value,
      ledBrightness: +f.ledBrightness.value,
      apChannel: +f.apChannel.value,
      txPower: +f.txPower.value,
    };
    if (f.apPass.value) body.apPass = f.apPass.value;
    if (f.webPass.value) body.webPass = f.webPass.value;
    if (body.apPass && body.apPass.length < 8) { toast('Hotspot-Passwort: mindestens 8 Zeichen'); return; }
    const apChanged = body.apSsid !== (state.status && state.status.ap.ssid) || !!body.apPass;
    if (!confirm('Speichern und neu starten?' + (apChanged ? '\n\nHotspot-Name/Passwort ändern sich – danach neu verbinden.' : ''))) return;
    try {
      const r = await fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      const j = await r.json();
      if (j.ok) { toast('Gespeichert – Neustart …', 4000); closePanel(); }
      else toast('Fehler: ' + (j.error || r.status), 4000);
    } catch (err) {
      toast('Fehler beim Speichern');
    }
  });

  $('#fwUpload').addEventListener('click', () => {
    const file = $('#fwFile').files[0];
    if (!file) { toast('Bitte firmware.bin auswählen'); return; }
    if (!/\.bin$/i.test(file.name)) { toast('Nur .bin-Dateien'); return; }
    if (!confirm(`Firmware „${file.name}“ (${fmtBytes(file.size)}) flashen?`)) return;
    const prog = $('#fwProg');
    const btn = $('#fwUpload');
    prog.hidden = false;
    prog.value = 0;
    btn.disabled = true;
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update');
    xhr.upload.onprogress = (ev) => { if (ev.lengthComputable) prog.value = (ev.loaded / ev.total) * 100; };
    xhr.onload = () => {
      btn.disabled = false;
      let j = {};
      try { j = JSON.parse(xhr.responseText); } catch (e) { /* ignore */ }
      if (xhr.status === 200 && j.ok) { toast('Firmware übernommen – Neustart …', 5000); }
      else { toast('Update fehlgeschlagen: ' + (j.error || xhr.status), 6000); prog.hidden = true; }
    };
    xhr.onerror = () => { btn.disabled = false; prog.hidden = true; toast('Upload-Fehler'); };
    const fd = new FormData();
    fd.append('firmware', file, file.name);
    xhr.send(fd);
  });

  $('#factory').addEventListener('click', async () => {
    if (!confirm('Alle Einstellungen löschen?\nDas Hotspot-Passwort wird neu erzeugt (steht danach auf dem OLED bzw. im USB-Log).')) return;
    try { await fetch('/api/factory', { method: 'POST' }); toast('Werksreset – Neustart …', 4000); } catch (e) { toast('Fehler'); }
  });

  // ------------------------------------------------------------------ net tab (WLAN client, HTTPS, certificates)
  function authVisibility() {
    const a = $('#netForm').staAuth.value;
    $$('#netForm [data-auth]').forEach((el) => { el.hidden = !el.dataset.auth.includes(a); });
  }
  $('#netForm').staAuth.addEventListener('change', authVisibility);

  async function loadNet() {
    try {
      const r = await fetch('/api/settings', { cache: 'no-store' });
      const s = await r.json();
      const f = $('#netForm');
      f.staSsid.value = s.staSsid || '';
      f.staPass.value = '';
      f.staEapPass.value = '';
      f.staPass.placeholder = s.staPassSet ? 'unverändert' : '';
      f.staEapPass.placeholder = s.staPassSet ? 'unverändert' : '';
      f.staAuth.value = String(s.staAuth || 0);
      f.staIdentity.value = s.staIdentity || '';
      f.staUser.value = s.staUser || '';
      f.staPhase2.value = String(s.staPhase2 || 0);
      f.staCaCheck.checked = !!s.staCaCheck;
      f.staClear.checked = false;
      f.httpsEnabled.checked = !!s.httpsEnabled;
      f.httpsLanOnly.checked = !!s.httpsLanOnly;
      f.httpsCert.value = String(s.httpsCert || 0);
      authVisibility();
      const warn = $('#netWarn');
      warn.hidden = !(s.staSsid && !s.webPassSet);
      warn.textContent = 'Kein Web-Passwort gesetzt: im Firmennetz kommt jeder an die Konsolen. Tab Setup → Zugriff.';
    } catch (e) {
      toast('Einstellungen konnten nicht geladen werden');
    }
  }

  $('#netForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const f = e.target;
    const auth = +f.staAuth.value;
    const body = {
      staSsid: f.staSsid.value.trim(),
      staAuth: auth,
      staIdentity: f.staIdentity.value.trim(),
      staUser: f.staUser.value.trim(),
      staPhase2: +f.staPhase2.value,
      staCaCheck: f.staCaCheck.checked,
      staClear: f.staClear.checked,
      httpsEnabled: f.httpsEnabled.checked,
      httpsLanOnly: f.httpsLanOnly.checked,
      httpsCert: +f.httpsCert.value,
    };
    const pass = auth === 0 ? f.staPass.value : f.staEapPass.value;
    if (pass) body.staPass = pass;
    if (!confirm('Speichern und neu starten?')) return;
    try {
      const r = await fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      const j = await r.json();
      if (j.ok) { toast('Gespeichert – Neustart …', 4000); closePanel(); }
      else toast('Fehler: ' + (j.error || r.status), 5000);
    } catch (err) {
      toast('Fehler beim Speichern');
    }
  });

  // ---- certificates
  const SLOTS = [
    { id: 'ca', title: 'CA-Zertifikat', hint: 'Zertifikat der Stelle, die das RADIUS-Server-Zertifikat ausgestellt hat. Ohne dieses prüft das Gerät den Server nicht.' },
    { id: 'client', title: 'Client-Zertifikat (802.1X)', hint: 'Gerätezertifikat mit privatem Schlüssel, meist als .p12/.pfx aus der Firmen-PKI. Braucht EKU „clientAuth“.' },
    { id: 'https', title: 'HTTPS-Zertifikat', hint: 'Für die Web-Oberfläche. Braucht EKU „serverAuth“ und den verwendeten Namen im SAN. Ohne Upload stellt sich das Gerät selbst eines aus.' },
  ];

  function certTable(c, full) {
    const rows = [['Inhaber', c.subject], ['Aussteller', c.issuer], ['Gültig', `${c.from} bis ${c.to}`]];
    if (full) {
      if (c.key) rows.push(['Schlüssel im Zertifikat', c.key]);
      if (c.eku) rows.push(['Verwendung (EKU)', c.eku.length ? c.eku.join(', ') : 'keine Einschränkung']);
      if (c.san && c.san.length) rows.push(['Namen (SAN)', c.san.join(', ')]);
      if (c.sha256) rows.push(['Fingerabdruck', c.sha256.replace(/(.{4})(?=.)/g, '$1 ')]);
    }
    const flags = [];
    if (c.expired) flags.push('<b class="warn">abgelaufen</b>');
    if (c.notYet) flags.push('<b class="warn">noch nicht gültig</b>');
    if (c.ca) flags.push('CA');
    if (c.self) flags.push('selbst signiert');
    if (flags.length) rows.push(['Hinweis', flags.join(' · ')]);
    return '<table class="kv">' + rows.map((r) => `<tr><th>${r[0]}</th><td>${r[1] === undefined ? '–' : (r[0] === 'Hinweis' ? r[1] : esc(r[1]))}</td></tr>`).join('') + '</table>';
  }

  function renderTls(info) {
    const el = $('#tlsCards');
    const by = {};
    (info.slots || []).forEach((s) => { by[s.id] = s; });
    el.innerHTML = SLOTS.map((def) => {
      const s = by[def.id] || {};
      const certs = s.certs || [];
      let state = 'leer';
      if (s.ready) state = 'bereit';
      else if (s.cert && def.id !== 'ca') state = s.key ? 'Schlüssel passt nicht' : 'Schlüssel fehlt';
      else if (s.key && !s.cert) state = 'nur Schlüssel';
      const extra = certs.length > 1 ? `<p class="hint">+ ${certs.length - 1} weitere(s) Zertifikat(e) in der Kette: ${certs.slice(1).map((c) => esc(c.subject)).join(' · ')}</p>` : '';
      return `<div class="pcard">
        <div class="pcard-head"><b>${def.title}</b><span class="chip static">${state}${s.auto ? ' · vom Gerät' : ''}</span></div>
        <p class="hint">${def.hint}</p>
        ${certs.length ? certTable(certs[0], true) + extra : '<p class="hint">Kein Zertifikat gespeichert.</p>'}
        ${s.keyType ? `<p class="hint">Privater Schlüssel: ${esc(s.keyType)}</p>` : ''}
        <div class="row">
          <input type="file" data-file="${def.id}" accept=".p12,.pfx,.pem,.crt,.cer,.der,.key,.p7b,application/x-pkcs12,application/x-pem-file">
          <input type="password" data-pass="${def.id}" placeholder="Passwort der Datei" maxlength="64" style="max-width:190px">
          <button class="btn" type="button" data-up="${def.id}">Hochladen</button>
          ${s.cert ? `<button class="btn" type="button" data-dl="${def.id}">Zertifikat laden</button><button class="btn danger" type="button" data-rm="${def.id}">Löschen</button>` : ''}
        </div>
      </div>`;
    }).join('');
    const ca = info.devca || {};
    $('#devCaDownload').hidden = !ca.subject;
    if (ca.subject) $('#devCaInfo').dataset.ca = ca.subject;
    const csr = info.csr || {};
    $('#csrDownload').hidden = !csr.have;
    $('#csrInfo').textContent = csr.busy ? 'Schlüssel wird erzeugt … (kann eine Minute dauern)'
      : csr.have ? `Antrag bereit: ${csr.subject || ''} (${csr.key || ''})` : '';
    $('#csrStart').disabled = !!csr.busy;
    if (csr.busy) setTimeout(loadTls, 3000);
  }

  async function loadTls() {
    try {
      const r = await fetch('/api/tls', { cache: 'no-store' });
      renderTls(await r.json());
    } catch (e) {
      $('#tlsCards').innerHTML = '<p class="hint">Zertifikate konnten nicht geladen werden.</p>';
    }
  }

  $('#tlsCards').addEventListener('click', async (e) => {
    const btn = e.target.closest('button');
    if (!btn) return;
    const up = btn.dataset.up, rm = btn.dataset.rm, dl = btn.dataset.dl;
    if (dl) { window.location = '/api/tls/download?what=' + dl; return; }
    if (rm) {
      if (!confirm('Zertifikat und Schlüssel löschen?')) return;
      await fetch('/api/tls/delete', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ slot: rm }) });
      toast('Gelöscht – Neustart übernimmt die Änderung');
      loadTls();
      return;
    }
    if (!up) return;
    const file = $(`[data-file="${up}"]`).files[0];
    if (!file) { toast('Bitte eine Datei auswählen'); return; }
    const fd = new FormData();
    fd.append('slot', up);
    fd.append('pass', $(`[data-pass="${up}"]`).value);
    fd.append('file', file, file.name);
    btn.disabled = true;
    try {
      const r = await fetch('/api/tls/upload', { method: 'POST', body: fd });
      const j = await r.json();
      toast(j.ok ? j.msg : 'Fehler: ' + (j.error || r.status), j.ok ? 6000 : 8000);
      if (j.ok) $(`[data-pass="${up}"]`).value = '';
      loadTls();
    } catch (err) {
      toast('Upload fehlgeschlagen');
    }
    btn.disabled = false;
  });

  $('#csrStart').addEventListener('click', async () => {
    const subject = $('#csrSubject').value.trim();
    if (!subject) { toast('Bitte einen Namen (CN) angeben'); return; }
    try {
      const r = await fetch('/api/tls/csr', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ subject, san: $('#csrSan').value.trim(), rsa: $('#csrKey').value === '1' }),
      });
      const j = await r.json();
      if (!j.ok) { toast('Fehler: ' + (j.error || r.status), 5000); return; }
      toast('Schlüssel wird erzeugt …');
      $('#csrStart').disabled = true;
      $('#csrInfo').textContent = 'Schlüssel wird erzeugt … (kann eine Minute dauern)';
      setTimeout(loadTls, 2000);
    } catch (e) {
      toast('Fehler');
    }
  });
  $('#csrDownload').addEventListener('click', () => { window.location = '/api/tls/download?what=csr'; });
  $('#devCaDownload').addEventListener('click', () => { window.location = '/api/tls/download?what=devca'; });

  // ------------------------------------------------------------------ go
  if (!(state.active >= 0 && state.active < 4)) state.active = 0;
  portObj(state.active);
  showPort(state.active);
  onViewport();
  requestAnimationFrame(refit);
  connect();
  // Hint if the terminal channel (WebSocket, port 81) does not come up, e.g. inside
  // a phone's captive-portal mini browser that blocks it.
  setTimeout(() => {
    if (!state.connected) {
      termNote('Terminal verbindet nicht. Tipp: Hotspot-Fenster mit "Fertig"/"Ohne Internet verwenden" schließen '
        + 'und im Browser http://' + location.hostname + '/ öffnen.');
    }
  }, 7000);
  setTermOnly(store.get('termOnly', desktopLike), false);
  setTimeout(() => (document.body.classList.contains('termonly') ? act().term : cmdEl).focus(), 300);
})();
