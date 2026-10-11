// Administrator's interface of the command-and-control server. Everything shown
// here comes from the admin API (/admin/v1/...), called with the token typed in
// at login. Device names and answers are data from devices: they only ever go
// into the page as text, never as HTML.
(() => {
  'use strict';
  const $ = (s) => document.querySelector(s);
  const HOTSPOT = 'http://192.168.4.1/';     // a device's own web interface, where the QR code leads
  const STATES = { pending: 'wartet auf Bestätigung', active: 'verbunden', revoked: 'gesperrt' };
  const CMD_STATES = { queued: 'wartet', sent: 'abgeholt', done: 'erledigt', failed: 'abgelehnt', expired: 'verfallen' };
  const CMD_NAMES = { ping: 'Ping', status: 'Zustand', settings: 'Einstellungen', configs: 'Konfigurationen', play: 'Abspielen', reboot: 'Neustart', console: 'Fernkonsole' };

  let token = sessionStorage.getItem('c2admin') || '';
  let devices = [], selected = '', timer = 0, expTimer = 0;

  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    for (const k of kids) e.append(k);
    return e;
  }
  function toast(text, ms = 3000) {
    const t = $('#toast');
    t.textContent = text;
    t.hidden = false;
    clearTimeout(toast.t);
    toast.t = setTimeout(() => { t.hidden = true; }, ms);
  }

  async function api(method, path, body) {
    const r = await fetch('/admin/v1' + path, {
      method, cache: 'no-store',
      headers: Object.assign({ Authorization: 'Bearer ' + token }, body ? { 'Content-Type': 'application/json' } : {}),
      body: body ? JSON.stringify(body) : undefined,
    });
    if (r.status === 401) { logout(); throw new Error('Token nicht angenommen'); }
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw Object.assign(new Error(j.error || 'HTTP ' + r.status), { status: r.status });
    return j;
  }

  function ago(iso) {
    if (!iso) return '–';
    const s = Math.max(0, Math.round((Date.now() - new Date(iso)) / 1000));
    if (s < 90) return `vor ${s} s`;
    if (s < 5400) return `vor ${Math.round(s / 60)} min`;
    if (s < 129600) return `vor ${Math.round(s / 3600)} h`;
    return new Date(iso).toLocaleString('de-DE');
  }
  const when = (iso) => (iso ? new Date(iso).toLocaleString('de-DE') : '–');
  function badge(state, names) {
    return el('span', { className: 'badge ' + state, textContent: names[state] || state });
  }

  // ---- login
  function logout() {
    token = '';
    sessionStorage.removeItem('c2admin');
    clearTimeout(timer);
    conDetach();
    $('#app').hidden = true;
    $('#logout').hidden = true;
    $('#srvInfo').textContent = '';
    $('#login').hidden = false;
    $('#adminToken').focus();
  }
  async function start() {
    try {
      const s = await api('GET', '/server');
      sessionStorage.setItem('c2admin', token);
      $('#srvInfo').textContent = `${s.public_url} · Serverschlüssel ${s.fingerprint.slice(0, 16)}…`;
      $('#srvInfo').title = 'Fingerabdruck des Serverschlüssels: ' + s.fingerprint;
      $('#login').hidden = true;
      $('#app').hidden = false;
      $('#logout').hidden = false;
      refresh();
    } catch (e) {
      $('#loginErr').textContent = e.message;
      $('#loginErr').hidden = false;
      logout();
    }
  }
  $('#loginForm').addEventListener('submit', (e) => {
    e.preventDefault();
    token = $('#adminToken').value.trim();
    $('#adminToken').value = '';
    $('#loginErr').hidden = true;
    start();
  });
  $('#logout').addEventListener('click', logout);

  // ---- device list
  async function refresh() {
    clearTimeout(timer);
    try {
      devices = await api('GET', '/devices');
      renderDevices();
      if (selected) await renderDetail();
    } catch (e) { if (token) toast(e.message); }
    if (token) timer = setTimeout(refresh, 3000);
  }
  function renderDevices() {
    const body = $('#devices tbody');
    body.textContent = '';
    for (const d of devices) {
      const st = d.status || {};
      const tr = el('tr', { className: d.id === selected ? 'sel' : '' },
        el('td', { textContent: d.name || d.id.slice(0, 8) }),
        el('td', null, badge(d.state, STATES)),
        el('td', { textContent: ago(d.last_seen) }),
        el('td', { textContent: st.bat_pct == null ? '–' : `${st.bat_pct} %${st.charge ? ' (' + st.charge + ')' : ''}` }),
        el('td', { textContent: st.rssi == null ? '–' : `${st.rssi} dBm` }),
        el('td', { textContent: st.fw || (d.info && d.info.fw) || '–' }),
        el('td', { textContent: d.open_commands || '' }));
      tr.addEventListener('click', () => { selected = d.id; renderDevices(); renderDetail(); });
      body.append(tr);
    }
    $('#devices').hidden = !devices.length;
    $('#noDevices').hidden = !!devices.length;
    if (selected && !devices.some((d) => d.id === selected)) { selected = ''; $('#detail').hidden = true; conDetach(); }
  }

  // ---- one device
  function pretty(v) {
    return typeof v === 'string' ? v : JSON.stringify(v, null, 1);
  }
  async function renderDetail() {
    const d = devices.find((x) => x.id === selected);
    if (!d) return;
    $('#detail').hidden = false;
    $('#dName').textContent = d.name || d.id;
    const b = $('#dState');
    b.className = 'badge ' + d.state;
    b.textContent = STATES[d.state] || d.state;
    $('#dId').textContent = d.id;
    $('#dFp').textContent = d.fingerprint;
    $('#dEnrolled').textContent = when(d.enrolled);
    $('#dSeen').textContent = d.last_seen ? `${when(d.last_seen)} (${ago(d.last_seen)})` : '–';
    $('#dInfo').textContent = d.info ? Object.entries(d.info).map(([k, v]) => `${k}: ${v}`).join(' · ') : '–';
    $('#dStatus').textContent = d.status ? Object.entries(d.status).map(([k, v]) => `${k}: ${v}`).join(' · ') : '–';
    $('#approveForm').hidden = d.state !== 'pending';
    $('#commands').hidden = d.state !== 'active';
    $('#revoke').hidden = d.state === 'revoked';

    $('#console').hidden = d.state !== 'active' && !con;
    renderConsole(d);

    let cmds = [];
    try { cmds = await api('GET', `/devices/${d.id}/commands`); } catch (e) { return; }
    if (selected !== d.id) return;                  // another device was picked meanwhile
    const body = $('#history tbody');
    body.textContent = '';
    for (const c of cmds.slice().reverse()) {
      const what = (CMD_NAMES[c.type] || c.type) + (c.args ? ' ' + JSON.stringify(c.args) : '');
      body.append(el('tr', null,
        el('td', { textContent: c.id }),
        el('td', { textContent: what }),
        el('td', null, badge(c.state, CMD_STATES)),
        el('td', { textContent: ago(c.done || c.sent || c.created) }),
        el('td', null, c.result === undefined ? '' : el('pre', { textContent: pretty(c.result) }))));
    }
    $('#history').hidden = $('#histTitle').hidden = !cmds.length;
    // names for "Abspielen": from the most recent answer to "Konfigurationen"
    const list = cmds.filter((c) => c.type === 'configs' && c.state === 'done' && c.result && c.result.configs).pop();
    const names = $('#playNames');
    names.textContent = '';
    if (list) for (const cfg of list.result.configs) names.append(el('option', { value: cfg.name }));
  }

  $('#approveForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    try {
      await api('POST', `/devices/${selected}/approve`, { sas: $('#sas').value.trim() });
      toast('Gerät bestätigt');
    } catch (err) {
      toast(err.status === 403 ? 'Der Code stimmt nicht' : err.status === 404 ? 'Zu viele falsche Eingaben – die Anmeldung wurde verworfen' : err.message, 5000);
    }
    $('#sas').value = '';
    refresh();
  });

  async function send(type, args) {
    try {
      await api('POST', `/devices/${selected}/commands`, args ? { type, args } : { type });
      toast(`${CMD_NAMES[type] || type}: eingereiht – das Gerät holt den Auftrag bei seiner nächsten Meldung ab`);
    } catch (err) { toast(err.message, 5000); }
    refresh();
  }
  $('#commands').addEventListener('click', (e) => {
    const type = e.target.dataset && e.target.dataset.cmd;
    if (!type) return;
    if (type === 'reboot' && !confirm('Das Gerät neu starten? Laufende Konsolensitzungen brechen ab.')) return;
    send(type);
  });
  $('#playForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const name = $('#playName').value.trim(), port = +$('#playPort').value;
    if (!confirm(`„${name}“ auf Port ${port} abspielen?\nDie Zeilen gehen an die Konsole des dort angeschlossenen Geräts.`)) return;
    send('play', { name, port });
  });
  // ---- remote console
  // One terminal at a time, bound to a device and a session. Output comes by
  // long poll from an offset, so a lost answer costs nothing; input is sent in
  // order, one request after the other.
  let con = null;                                   // { dev, id, term, fit, next, abort, ended, queue, sending }
  const b64 = (bytes) => btoa(String.fromCharCode(...bytes));
  const unb64 = (text) => Uint8Array.from(atob(text || ''), (c) => c.charCodeAt(0));
  const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

  function conDetach() {
    if (!con) return;
    con.abort.abort();
    con.term.dispose();
    con = null;
    $('#conBox').hidden = true;
  }
  async function conSend() {
    const c = con;
    if (!c || c.sending || c.ended || !c.queue.length) return;
    c.sending = true;
    while (con === c && !c.ended && c.queue.length) {
      const item = c.queue[0];
      try {
        await api('POST', `/devices/${c.dev}/console/${c.id}/input`, item);
        c.queue.shift();
      } catch (e) {
        if (e.status === 429) { await new Promise((r) => setTimeout(r, 300)); continue; }   // the device is slower than the typing
        if (e.status !== 409 && e.status !== 404) toast(e.message);
        c.queue.length = 0;
      }
    }
    c.sending = false;
  }
  function conInput(item) {
    if (!con || con.ended) return;
    const last = con.queue[con.queue.length - 1];
    // bytes typed while a request is on its way travel together (but not past 1 kB)
    if (item.d && last && last.d && last !== con.queue[0] && last.d.length + item.d.length < 1400) last.d = b64([...unb64(last.d), ...unb64(item.d)]);
    else con.queue.push(item);
    conSend();
  }
  async function conRead(c) {
    while (con === c && !c.ended) {
      let o;
      try {
        const r = await fetch(`/admin/v1/devices/${c.dev}/console/${c.id}/output?from=${c.next}`, {
          cache: 'no-store', headers: { Authorization: 'Bearer ' + token }, signal: c.abort.signal,
        });
        if (r.status === 404) { o = { state: 'ended', reason: 'dem Server nicht mehr bekannt', d: '', next: c.next }; } else if (!r.ok) throw new Error('HTTP ' + r.status);
        else o = await r.json();
      } catch (e) {
        if (con !== c) return;
        await new Promise((r) => setTimeout(r, 1000));
        continue;
      }
      if (con !== c) return;
      if (o.lost) c.term.write(`\r\n\x1b[90m[${o.lost} Zeichen fehlen]\x1b[0m\r\n`);
      if (o.d) c.term.write(unb64(o.d));
      c.next = o.next;
      c.left = o.left_s;
      c.leftAt = Date.now();
      if (o.state === 'ended') {
        c.ended = true;
        c.term.write(`\r\n\x1b[90m[Sitzung beendet: ${o.reason}]\x1b[0m\r\n`);
        c.term.options.disableStdin = true;
        refresh();
      }
    }
  }
  function conAttach(dev, id, left) {
    conDetach();
    $('#conBox').hidden = false;
    const term = new Terminal({ cursorBlink: true, scrollback: 5000, fontSize: 14, convertEol: false });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open($('#term'));
    fit.fit();
    const c = con = { dev, id, term, fit, next: 0, abort: new AbortController(), ended: false, queue: [], sending: false, left: left || 0, leftAt: Date.now() };
    term.onData((data) => {                         // a pasted text arrives in one piece: hand it on in small ones
      const bytes = new TextEncoder().encode(data);
      for (let i = 0; i < bytes.length; i += 512) conInput({ d: b64(bytes.subarray(i, i + 512)) });
    });
    term.focus();
    conRead(c);
  }
  function renderConsole(d) {
    if (con && con.dev !== d.id) conDetach();
    api('GET', `/devices/${d.id}/console`).then((v) => {
      if (selected !== d.id) return;
      const cur = v.console, st = $('#conState');
      let text = '', cls = '';
      if (cur && cur.state === 'requested') { text = `Angefragt (Port ${cur.port}) – am Gerät muss jetzt jemand auf dem Display „Erlauben“ tippen.`; cls = 'wait'; }
      else if (cur && cur.state === 'granted') { text = 'Am Gerät erlaubt – die Verbindung kommt.'; cls = 'wait'; }
      else if (cur && cur.state === 'open') { text = `Offen auf Port ${cur.port}.`; cls = 'open'; }
      else if (cur && cur.state === 'refused') text = `Nicht zustande gekommen: ${cur.reason}`;
      else if (cur && cur.state === 'ended') text = `Letzte Sitzung beendet: ${cur.reason}`;
      st.textContent = text;
      st.className = 'dim ' + cls;
      const busy = cur && ['requested', 'granted', 'open'].includes(cur.state);
      $('#conForm').hidden = !!busy || d.state !== 'active';
      if (cur && cur.state === 'open' && !(con && con.id === cur.id)) conAttach(d.id, cur.id, cur.left_s);
      if (con) {
        const left = Math.max(0, con.left - Math.round((Date.now() - con.leftAt) / 1000));
        $('#conInfo').textContent = con.ended ? 'beendet' : `Port ${cur ? cur.port : ''} · noch ${mmss(left)} min`;
        $('#conBreak').hidden = $('#conClose').hidden = con.ended;
      }
      const body = $('#conLog tbody');
      body.textContent = '';
      for (const l of v.sessions.slice().reverse()) {
        const dur = l.end ? Math.round((new Date(l.end) - new Date(l.start)) / 1000) : null;
        body.append(el('tr', null,
          el('td', { textContent: l.id }),
          el('td', { textContent: l.port }),
          el('td', { textContent: when(l.start) }),
          el('td', { textContent: dur == null ? 'läuft' : mmss(dur) + ' min' }),
          el('td', { textContent: l.reason || '' }),
          el('td', { textContent: l.end ? `${l.bytes_in} / ${l.bytes_out}` : '' })));
      }
      $('#conLog').hidden = !v.sessions.length;
    }).catch(() => {});
  }
  $('#conForm').addEventListener('submit', (e) => {
    e.preventDefault();
    send('console', { port: +$('#conPort').value, minutes: +$('#conMinutes').value });
  });
  $('#conBreak').addEventListener('click', () => { conInput({ brk: true }); if (con) con.term.focus(); });
  $('#conClose').addEventListener('click', async () => {
    if (!con) return;
    try { await api('POST', `/devices/${con.dev}/console/${con.id}/close`); } catch (err) { toast(err.message); }
  });
  window.addEventListener('resize', () => { if (con) con.fit.fit(); });

  $('#revoke').addEventListener('click', async () => {
    const d = devices.find((x) => x.id === selected);
    if (!d || !confirm(`„${d.name || d.id}“ sperren?\nDas gilt sofort und endgültig: Das Gerät kommt auch mit einer neuen Einladung nicht zurück.`)) return;
    try { await api('POST', `/devices/${selected}/revoke`); toast('Gesperrt'); } catch (err) { toast(err.message); }
    refresh();
  });

  // ---- invitation
  function drawQr(text) {
    const qr = qrcode(0, 'L');                       // smallest version that fits
    qr.addData(text, 'Byte');
    qr.make();
    const n = qr.getModuleCount(), scale = 4, margin = 4, c = $('#qr');
    c.width = c.height = (n + 2 * margin) * scale;
    const g = c.getContext('2d');
    g.fillStyle = '#fff';
    g.fillRect(0, 0, c.width, c.height);
    g.fillStyle = '#000';
    for (let y = 0; y < n; y++)
      for (let x = 0; x < n; x++)
        if (qr.isDark(y, x)) g.fillRect((x + margin) * scale, (y + margin) * scale, scale, scale);
  }
  $('#invite').addEventListener('click', async () => {
    let t;
    try { t = await api('POST', '/enroll-tokens', { ttl_s: 600 }); } catch (err) { toast(err.message); return; }
    $('#tokenText').value = t.token;
    drawQr(HOTSPOT + '#c2=' + encodeURIComponent(t.token));
    const until = new Date(t.expires);
    clearInterval(expTimer);
    const tick = () => {
      const s = Math.round((until - Date.now()) / 1000);
      $('#tokenExp').textContent = s > 0 ? `gilt noch ${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')} min, für ein Gerät` : 'abgelaufen';
    };
    tick();
    expTimer = setInterval(tick, 1000);
    $('#inviteDlg').showModal();
  });
  $('#copyToken').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('#tokenText').value); toast('Kopiert'); } catch (e) { $('#tokenText').select(); toast('Markiert – mit Strg+C kopieren'); }
  });
  $('#closeInvite').addEventListener('click', () => $('#inviteDlg').close());
  $('#inviteDlg').addEventListener('close', () => { clearInterval(expTimer); $('#tokenText').value = ''; refresh(); });

  if (token) start(); else logout();
})();
