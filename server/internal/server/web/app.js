// Administrator's interface of the command-and-control server. Everything shown
// here comes from the admin API (/admin/v1/...), called with the session cookie
// of a signed-in account or with the admin token typed in at login. Names and
// answers of devices and the names of accounts are data: they only ever go into
// the page as text, never as HTML.
(() => {
  'use strict';
  const $ = (s) => document.querySelector(s);
  const HOTSPOT = 'http://192.168.4.1/';     // a device's own web interface, where the QR code leads
  const STATES = { pending: 'wartet auf Bestätigung', active: 'verbunden', revoked: 'gesperrt' };
  const CMD_STATES = { queued: 'wartet', sent: 'abgeholt', done: 'erledigt', failed: 'abgelehnt', expired: 'verfallen' };
  const CMD_NAMES = { ping: 'Ping', status: 'Zustand', settings: 'Einstellungen', configs: 'Konfigurationen', play: 'Abspielen', reboot: 'Neustart', console: 'Fernkonsole' };

  let token = sessionStorage.getItem('c2admin') || '';   // admin token, if that is how this tab got in
  let signedIn = false, me = '', methods = { token: false, entra: false };
  let devices = [], selected = '', timer = 0, expTimer = 0;
  let view = 'geraete', server = null, status = null, loaded = false;
  const VIEWS = { geraete: 'Geräte', benutzer: 'Benutzer', protokoll: 'Protokoll', server: 'Server' };
  // The cookie of a signed-in account travels by itself. The X-C2 header shows
  // the server that the call comes from this page and not from another site.
  const authHeaders = () => (token ? { Authorization: 'Bearer ' + token } : { 'X-C2': '1' });

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
      headers: Object.assign(authHeaders(), body ? { 'Content-Type': 'application/json' } : {}),
      body: body ? JSON.stringify(body) : undefined,
    });
    if (r.status === 401) { showLogin(); throw new Error(token ? 'Token nicht angenommen' : 'Nicht mehr angemeldet'); }
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
  // On a narrow screen a table row becomes a block of its own; each cell then
  // carries the name of its column (style.css, table.stack).
  function label(table) {
    const heads = [...table.tHead.rows[0].cells].map((th) => th.textContent);
    for (const tr of table.tBodies[0].rows) [...tr.cells].forEach((td, i) => { td.dataset.label = heads[i] || ''; });
  }
  // A question before something that cannot be taken back. Resolves to true
  // only for the button that says what will happen.
  function ask(title, text, yes, kind) {
    const dlg = $('#askDlg');
    $('#askTitle').textContent = title;
    $('#askText').textContent = text;
    $('#askYes').textContent = yes;
    $('#askYes').className = 'btn primary ' + (kind === undefined ? 'danger' : kind);
    dlg.returnValue = '';
    dlg.showModal();
    $('#askNo').focus();                            // Enter must not be the dangerous answer
    return new Promise((done) => dlg.addEventListener('close', () => done(dlg.returnValue === 'yes'), { once: true }));
  }
  $('#askNo').addEventListener('click', () => $('#askDlg').close(''));
  $('#askYes').addEventListener('click', () => $('#askDlg').close('yes'));

  // ---- where one is: #geraete, #geraete/<id>, #benutzer, #protokoll, #server
  function route() {
    const [v, id] = location.hash.slice(1).split('/');
    const was = selected;
    view = VIEWS[v] ? v : 'geraete';
    selected = view === 'geraete' && id ? id : '';
    document.body.classList.toggle('detail', !!selected);
    document.body.classList.toggle('popped', !!was && !selected && view === 'geraete');   // back from a device: the list returns from the left
    for (const name of Object.keys(VIEWS)) $('#v-' + name).hidden = name !== view || !signedIn;
    for (const a of $('#nav').children) {
      if (a.dataset.view === view) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
    }
    $('#title').textContent = signedIn ? VIEWS[view] : 'RS232-Leitstelle';
    document.title = signedIn ? `${VIEWS[view]} – RS232-Leitstelle` : 'RS232-Leitstelle';
    if (!signedIn) return;
    if (was !== selected) {
      if (was) conDetach();
      renderDevices();
      if (selected) { renderDetail(); window.scrollTo(0, 0); }
    }
    showDetail();
    if (view === 'benutzer' || view === 'protokoll') loadAdmin();
    if (view === 'server') renderServer();
  }
  window.addEventListener('hashchange', route);
  function showDetail() {
    const d = devices.find((x) => x.id === selected);
    $('#detail').hidden = !d;
    $('#pick').hidden = !!d || !devices.length;
  }

  // ---- login
  async function auth(path, body) {             // the calls around signing in: no token, no session yet
    const r = await fetch('/auth/' + path, {
      method: body ? 'POST' : 'GET', cache: 'no-store',
      headers: body ? { 'Content-Type': 'application/json', 'X-C2': '1' } : {},
      body: body ? JSON.stringify(body) : undefined,
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw Object.assign(new Error(r.status === 429 ? 'Zu viele Versuche – einen Moment warten' : j.error || 'HTTP ' + r.status), { status: r.status });
    return j;
  }
  function showLogin() {
    token = '';
    signedIn = loaded = false;
    me = '';
    sessionStorage.removeItem('c2admin');
    clearTimeout(timer);
    conDetach();
    $('#app').hidden = true;
    $('#logout').hidden = $('#nav').hidden = true;
    $('#whoami').textContent = '';
    document.body.classList.remove('detail', 'popped');
    $('#title').textContent = document.title = 'RS232-Leitstelle';
    $('#entraLogin').hidden = !methods.entra;
    $('#tokenBox').hidden = !methods.token;
    $('#login').hidden = false;
    $('#loginName').focus();
  }
  function loginError(text) {
    $('#loginErr').textContent = text;
    $('#loginErr').hidden = false;
  }
  async function start() {
    try {
      server = await api('GET', '/server');
      if (token) sessionStorage.setItem('c2admin', token);
      signedIn = true;
      $('#whoami').textContent = token ? 'Admin-Token' : me;
      $('#login').hidden = true;
      $('#app').hidden = false;
      $('#logout').hidden = $('#nav').hidden = false;
      selected = '';
      route();
      refresh();
    } catch (e) {
      showLogin();
      loginError(e.message);
    }
  }
  $('#loginForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    $('#loginErr').hidden = true;
    try {
      const r = await auth('login', { name: $('#loginName').value, password: $('#loginPass').value, code: $('#loginCode').value });
      me = r.user;
      $('#loginForm').reset();
      start();
    } catch (err) {
      $('#loginPass').value = $('#loginCode').value = '';
      loginError(err.message);
    }
  });
  $('#tokenForm').addEventListener('submit', (e) => {
    e.preventDefault();
    token = $('#adminToken').value.trim();
    $('#adminToken').value = '';
    $('#loginErr').hidden = true;
    start();
  });
  $('#logout').addEventListener('click', async () => {
    if (!token) try { await auth('logout', {}); } catch (e) { /* signed out here in any case */ }
    showLogin();
  });

  // ---- setting up an account: setup code and password, then the authenticator app
  let pendingSetup = null;
  function setupError(text) {
    $('#setupErr').textContent = text;
    $('#setupErr').hidden = !text;
  }
  $('#setupForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    setupError('');
    if ($('#setupPass').value !== $('#setupPass2').value) return setupError('Die beiden Passwörter sind nicht gleich');
    const name = $('#setupName').value, setup = $('#setupCode').value;
    try {
      const r = await auth('setup', { name, setup, password: $('#setupPass').value });
      pendingSetup = { name, setup };
      $('#totpSecret').textContent = r.secret.replace(/(.{4})/g, '$1 ').trim();
      drawQr($('#totpQr'), r.uri);
      $('#setupForm').reset();
      $('#setupForm').hidden = true;
      $('#totpForm').hidden = false;
      $('#totpCode').focus();
    } catch (err) { setupError(err.message); }
  });
  $('#totpForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    setupError('');
    try {
      const r = await auth('setup/confirm', Object.assign({ code: $('#totpCode').value }, pendingSetup));
      me = r.user;
      pendingSetup = null;
      $('#totpForm').reset();
      $('#totpSecret').textContent = '';
      $('#totpForm').hidden = true;
      $('#setupForm').hidden = false;
      $('#setupBox').open = false;
      start();
    } catch (err) { $('#totpCode').value = ''; setupError(err.message); }
  });

  // ---- device list
  async function refresh() {
    clearTimeout(timer);
    try {
      [devices, server, status] = await Promise.all([api('GET', '/devices'), api('GET', '/server'), api('GET', '/status')]);
      loaded = true;
      renderDevices();
      renderServer();
      if (selected) await renderDetail();
    } catch (e) { if (signedIn) toast(e.message); }
    if (signedIn) timer = setTimeout(refresh, 3000);
  }
  function renderDevices() {
    const list = $('#devices');
    const at = document.activeElement && document.activeElement.closest && document.activeElement.closest('.dev');
    const focus = at && at.dataset.id;
    list.textContent = '';
    for (const d of devices) {
      const st = d.status || {}, meta = [];
      if (d.last_seen) meta.push('zuletzt ' + ago(d.last_seen));
      if (st.bat_pct != null) meta.push(`Akku ${st.bat_pct} %${st.charge ? ' (' + st.charge + ')' : ''}`);
      if (st.rssi != null) meta.push(`${st.rssi} dBm`);
      const fw = st.fw || (d.info && d.info.fw);
      if (fw) meta.push('Firmware ' + fw);
      if (d.open_commands) meta.push(d.open_commands === 1 ? '1 Auftrag offen' : `${d.open_commands} Aufträge offen`);
      if (d.old_key) meta.push('alter Serverschlüssel');
      const a = el('a', { className: 'dev', href: '#geraete/' + d.id, role: 'listitem' },
        el('b', { textContent: d.name || d.id.slice(0, 8) }), badge(d.state, STATES),
        el('span', { className: 'meta', textContent: meta.join(' · ') || 'hat sich noch nicht gemeldet' }));
      a.dataset.id = d.id;
      if (d.id === selected) a.setAttribute('aria-current', 'true');
      list.append(a);
      if (d.id === focus) a.focus({ preventScroll: true });     // the list is rebuilt under the keyboard's feet
    }
    $('#noDevices').hidden = !!devices.length;
    if (loaded && selected && !devices.some((d) => d.id === selected)) { conDetach(); location.hash = '#geraete'; }   // it is gone
    showDetail();
  }

  // ---- one device
  function pretty(v) {
    return typeof v === 'string' ? v : JSON.stringify(v, null, 1);
  }
  async function renderDetail() {
    const d = devices.find((x) => x.id === selected);
    if (!d) return;
    showDetail();
    $('#dOldKey').hidden = !d.old_key;
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
        el('td', { className: c.result === undefined ? 'none' : 'wide' }, c.result === undefined ? '' : el('pre', { textContent: pretty(c.result) }))));
    }
    label($('#history'));
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
  $('#commands').addEventListener('click', async (e) => {
    const type = e.target.dataset && e.target.dataset.cmd;
    if (!type) return;
    if (type === 'reboot' && !await ask('Gerät neu starten?', 'Laufende Konsolensitzungen brechen ab.', 'Neu starten')) return;
    send(type);
  });
  $('#playForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const name = $('#playName').value.trim(), port = +$('#playPort').value;
    if (!await ask(`„${name}“ auf Port ${port} abspielen?`, 'Die Zeilen gehen an die Konsole des dort angeschlossenen Geräts.', 'Abspielen')) return;
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
          cache: 'no-store', headers: authHeaders(), signal: c.abort.signal,
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
      label($('#conLog'));
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
    if (!d || !await ask(`„${d.name || d.id}“ sperren?`, 'Das gilt sofort und endgültig: Das Gerät kommt auch mit einer neuen Einladung nicht zurück.', 'Sperren')) return;
    try { await api('POST', `/devices/${selected}/revoke`); toast('Gesperrt'); } catch (err) { toast(err.message); }
    refresh();
  });

  // ---- invitation
  function drawQr(c, text) {
    const qr = qrcode(0, 'L');                       // smallest version that fits
    qr.addData(text, 'Byte');
    qr.make();
    const n = qr.getModuleCount(), scale = 4, margin = 4;
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
    drawQr($('#qr'), HOTSPOT + '#c2=' + encodeURIComponent(t.token));
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

  // ---- accounts and the audit record
  const KINDS = { local: 'Passwort + App', entra: 'Microsoft Entra ID' };
  const ACTIONS = {
    login: 'Anmeldung', 'login entra': 'Anmeldung über Microsoft', logout: 'Abmeldung', setup: 'Konto einrichten',
    'POST /users': 'Benutzer angelegt', 'POST /users/{name}/reset': 'Konto zurückgesetzt', 'POST /users/{name}/disable': 'Konto gesperrt oder freigegeben',
    'DELETE /users/{name}': 'Benutzer gelöscht', 'POST /enroll-tokens': 'Einladung erzeugt', 'POST /devices/{id}/approve': 'Gerät bestätigt',
    'POST /devices/{id}/revoke': 'Gerät gesperrt', 'POST /devices/{id}/commands': 'Auftrag', 'POST /devices/{id}/console/{sess}/close': 'Fernkonsole beendet',
    'POST /server/rotate-key': 'Serverschlüssel gewechselt', 'POST /server/rotate-key/end': 'Schlüsselwechsel beendet',
  };
  function showCode(name, code) {
    $('#codeFor').textContent = name;
    $('#codeText').textContent = code;
    $('#codeDlg').showModal();
  }
  async function userAction(title, text, yes, method, path, body) {
    if (!await ask(title, text, yes, yes === 'Freigeben' ? '' : 'danger')) return;
    try {
      const r = await api(method, path, body);
      if (r.setup) showCode(r.name, r.setup);
    } catch (err) { toast(err.message, 5000); }
    loadAdmin();
  }
  async function loadAdmin() {
    let users, log;
    try { [users, log] = await Promise.all([api('GET', '/users'), api('GET', '/audit?n=200')]); } catch (e) { return; }
    const ub = $('#users tbody');
    ub.textContent = '';
    for (const u of users) {
      const state = u.disabled ? 'gesperrt' : u.ready ? 'aktiv' : u.setup_expires ? `wartet auf Einrichtung (Code gilt bis ${when(u.setup_expires)})` : 'nicht eingerichtet, Code abgelaufen';
      const how = KINDS[u.kind] + (u.kind === 'entra' ? ` (${u.entra_name || u.entra_oid}${u.entra_name && !u.entra_oid ? ', noch nie angemeldet' : ''})` : '');
      const acts = el('td', { className: 'acts' });
      const name = encodeURIComponent(u.name);
      if (u.kind === 'local') acts.append(el('button', { className: 'btn small', textContent: 'Zurücksetzen',
        onclick: () => userAction(`Konto „${u.name}“ zurücksetzen?`, 'Passwort und Authenticator-App gelten dann nicht mehr; es gibt einen neuen Einrichtungscode.', 'Zurücksetzen', 'POST', `/users/${name}/reset`) }));
      acts.append(el('button', { className: 'btn small', textContent: u.disabled ? 'Freigeben' : 'Sperren',
        onclick: () => userAction(`Konto „${u.name}“ ${u.disabled ? 'freigeben' : 'sperren'}?`, u.disabled ? 'Die Person kann sich danach wieder anmelden.' : 'Die Person wird abgemeldet und kommt nicht mehr herein, bis das Konto freigegeben wird.',
          u.disabled ? 'Freigeben' : 'Sperren', 'POST', `/users/${name}/disable`, { disabled: !u.disabled }) }));
      acts.append(el('button', { className: 'btn small danger', textContent: 'Löschen',
        onclick: () => userAction(`Konto „${u.name}“ löschen?`, 'Das lässt sich nicht zurücknehmen.', 'Löschen', 'DELETE', `/users/${name}`) }));
      ub.append(el('tr', null, el('td', { textContent: u.name }), el('td', { textContent: how }), el('td', { textContent: state }),
        el('td', { textContent: when(u.last_login) }), acts));
    }
    label($('#users'));
    $('#users').hidden = !users.length;
    $('#noUsers').hidden = !!users.length;
    const ab = $('#audit tbody');
    ab.textContent = '';
    for (const a of log) {
      const dev = devices.find((d) => d.id === a.target);
      ab.append(el('tr', { className: a.ok ? '' : 'failed' },
        el('td', { textContent: when(a.time) }), el('td', { textContent: a.who }), el('td', { textContent: a.ip }),
        el('td', { textContent: (ACTIONS[a.action] || a.action) + (a.detail ? ': ' + a.detail : '') }),
        el('td', { textContent: dev ? dev.name || a.target.slice(0, 8) : a.target || '' }),
        el('td', { textContent: a.ok ? '' : 'abgelehnt' })));
    }
    label($('#audit'));
  }

  // ---- the server: how it is, and its key
  function span(s) {                                // seconds, as a person says it
    if (s < 120) return `${s} s`;
    if (s < 7200) return `${Math.round(s / 60)} min`;
    if (s < 172800) return `${Math.round(s / 3600)} h`;
    return `${Math.round(s / 86400)} Tagen`;
  }
  function renderServer() {
    if (!server || !status) return;
    const st = status.state, kc = server.key_change, dv = status.devices;
    // what is wrong is said on every page, not only where the numbers are
    $('#alert').hidden = st.ok;
    if (!st.ok) $('#alert').textContent = `Der Server kann seinen Zustand nicht speichern (seit ${when(st.since)}). Änderungen gehen verloren, wenn er jetzt neu startet.`;
    const mark = $('#navMark');
    mark.hidden = st.ok && !kc;
    mark.className = 'mark' + (st.ok ? '' : ' bad');
    const b = $('#stState');
    b.className = 'badge ' + (st.ok ? 'ok' : 'failed');
    b.textContent = st.ok ? 'in Ordnung' : 'Fehler';
    $('#stError').hidden = st.ok;
    $('#stError').textContent = st.ok ? '' : `Speichern scheitert seit ${when(st.since)}: ${st.error}`;
    const turned = status.turned_away;
    const set = (id, n, attn) => { $(id).textContent = n; $(id).parentNode.classList.toggle('attn', !!attn); };
    set('#stActive', dv.active);
    set('#stPending', dv.pending, dv.pending);
    set('#stSilent', dv.silent, dv.silent);
    set('#stRevoked', dv.revoked);
    set('#stConsoles', status.consoles_open);
    set('#stSignedIn', status.signed_in);
    set('#stDenied', turned.handshake + turned.sign_in + turned.admin);
    $('#stUptime').textContent = span(status.uptime_s);
    $('#stUptime').className = 'text';
    $('#stNote').textContent = `„Still“ heißt: verbunden, aber seit über ${span(status.silent_after_s)} nicht gemeldet. Abgewiesen wird, wer zu schnell zu oft anfragt (Geräte ${turned.handshake}, Anmeldungen ${turned.sign_in}, Aufrufe ohne Anmeldung ${turned.admin}).`;

    $('#srvUrl').textContent = server.public_url;
    $('#srvFp').textContent = server.fingerprint;
    $('#keyChange').hidden = !kc;
    $('#kcEnd').hidden = !kc;
    $('#kcStart').hidden = !!kc;
    if (kc) {
      const waiting = devices.filter((d) => d.old_key).map((d) => d.name || d.id.slice(0, 8));
      $('#kcText').textContent = kc.devices_waiting ? 'Geräte, die den neuen noch nicht haben, erkennen den Server weiter am alten.' : 'Alle Geräte haben den neuen Schlüssel – der Wechsel kann beendet werden.';
      $('#kcOld').textContent = kc.old_fingerprint;
      $('#kcUntil').textContent = when(kc.old_until);
      $('#kcWaiting').textContent = waiting.length ? `${waiting.length}: ${waiting.join(', ')}` : 'kein Gerät';
    }
  }
  $('#kcStart').addEventListener('click', () => {
    $('#rotateForm').reset();
    $('#rotateErr').hidden = true;
    $('#rotateDlg').showModal();
    $('#rotateCancel').focus();
  });
  $('#rotateCancel').addEventListener('click', () => $('#rotateDlg').close());
  $('#rotateForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    try {
      server = await api('POST', '/server/rotate-key', { days: +$('#rotateDays').value });
      $('#rotateDlg').close();
      toast('Neuer Serverschlüssel – die Geräte erfahren ihn, sobald sie sich melden', 5000);
      refresh();
    } catch (err) {
      $('#rotateErr').textContent = err.message;
      $('#rotateErr').hidden = false;
    }
  });
  $('#kcEnd').addEventListener('click', async () => {
    const waiting = devices.filter((d) => d.old_key).map((d) => d.name || d.id.slice(0, 8));
    const text = waiting.length
      ? `Der alte Schlüssel gilt dann nicht mehr. ${waiting.length === 1 ? 'Dieses Gerät hat' : 'Diese Geräte haben'} den neuen noch nicht und ${waiting.length === 1 ? 'muss' : 'müssen'} danach neu angemeldet werden:\n${waiting.join(', ')}`
      : 'Der alte Schlüssel gilt dann nicht mehr. Alle Geräte haben den neuen schon.';
    if (!await ask('Schlüsselwechsel beenden?', text, 'Beenden', waiting.length ? 'danger' : '')) return;
    try { server = await api('POST', '/server/rotate-key/end'); toast('Der alte Schlüssel ist vergessen'); } catch (err) { toast(err.message, 5000); }
    refresh();
  });
  $('#auditReload').addEventListener('click', loadAdmin);
  $('#addUser').addEventListener('click', () => {
    $('#userForm').reset();
    $('#userErr').hidden = true;
    $('#userEntraRow').hidden = true;
    $('#userDlg').showModal();
  });
  $('#userKind').addEventListener('change', () => { $('#userEntraRow').hidden = $('#userKind').value !== 'entra'; });
  $('#userCancel').addEventListener('click', () => $('#userDlg').close());
  $('#codeClose').addEventListener('click', () => { $('#codeText').textContent = ''; $('#codeDlg').close(); });
  $('#userForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    try {
      const r = await api('POST', '/users', { name: $('#userName').value, kind: $('#userKind').value, entra: $('#userEntra').value });
      $('#userDlg').close();
      if (r.setup) showCode(r.name, r.setup); else toast('Angelegt');
      loadAdmin();
    } catch (err) {
      $('#userErr').textContent = err.message;
      $('#userErr').hidden = false;
    }
  });

  // ---- start: who is signed in, and what the login page may offer
  (async () => {
    try {
      methods = await auth('methods');
      if (methods.user && methods.kind !== 'token') { token = ''; me = methods.user; return start(); }
    } catch (e) { /* the login page says what is wrong once somebody tries */ }
    if (token) return start();
    showLogin();
    if (location.hash === '#entra=refused') {
      history.replaceState(null, '', '/');
      loginError('Die Anmeldung über Microsoft wurde nicht angenommen: Für dieses Microsoft-Konto gibt es hier kein freigegebenes Konto, oder die Antwort war ungültig.');
    }
  })();
})();
