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
  const CMD_NAMES = { ping: 'Ping', status: 'Zustand', settings: 'Einstellungen', configs: 'Konfigurationen', play: 'Abspielen', reboot: 'Neustart' };

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
    if (selected && !devices.some((d) => d.id === selected)) { selected = ''; $('#detail').hidden = true; }
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
