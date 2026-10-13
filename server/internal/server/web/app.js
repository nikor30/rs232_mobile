// Administrator's interface of the command-and-control server. Everything shown
// here comes from the admin API (/admin/v1/...), called with the session cookie
// of a signed-in account or with the admin token typed in at login. Names and
// answers of devices and the names of accounts are data: they only ever go into
// the page as text, never as HTML.
(() => {
  'use strict';
  const $ = (s) => document.querySelector(s);
  const HOTSPOT = 'http://192.168.4.1/';     // a device's own web interface, where the QR code leads
  // The page is written in English; I18N (i18n.js) has the other languages.
  const { t } = I18N;
  const STATES = { pending: t('waiting for confirmation'), active: t('connected'), revoked: t('revoked') };
  const CMD_STATES = { queued: t('queued'), sent: t('fetched'), done: t('done'), failed: t('refused'), expired: t('expired') };
  const CMD_NAMES = { ping: t('Ping'), status: t('Status'), settings: t('Settings'), configs: t('Configurations'), play: t('Play'), reboot: t('Restart'), console: t('Remote console'), update: t('Firmware update'),
    set: t('Change settings'), config_get: t('Read configuration'), config_put: t('Write configuration'), config_delete: t('Delete configuration') };
  const APP = t('RS232 Control Center');

  let token = sessionStorage.getItem('c2admin') || '';   // admin token, if that is how this tab got in
  let signedIn = false, me = '', methods = { token: false, entra: false };
  let devices = [], selected = '', timer = 0, expTimer = 0;
  let view = 'devices', shownView = '', server = null, status = null, loaded = false;
  const VIEWS = { devices: t('Devices'), manage: t('Manage'), library: t('Library'), users: t('Users'), audit: t('Audit log'), server: t('Server') };
  // The cookie of a signed-in account travels by itself. The X-C2 header shows
  // the server that the call comes from this page and not from another site.
  const authHeaders = () => (token ? { Authorization: 'Bearer ' + token } : { 'X-C2': '1' });

  function el(tag, props, ...kids) {
    const e = Object.assign(document.createElement(tag), props || {});
    for (const k of kids) e.append(k);
    return e;
  }
  function toast(text, ms = 3000) {
    const box = $('#toast');
    box.textContent = text;
    box.hidden = false;
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => { box.hidden = true; }, ms);
  }

  async function api(method, path, body) {
    const r = await fetch('/admin/v1' + path, {
      method, cache: 'no-store',
      headers: Object.assign(authHeaders(), body ? { 'Content-Type': 'application/json' } : {}),
      body: body ? JSON.stringify(body) : undefined,
    });
    if (r.status === 401) { showLogin(); throw new Error(token ? t('The token was not accepted') : t('No longer signed in')); }
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw Object.assign(new Error(j.error ? t(j.error) : 'HTTP ' + r.status), { status: r.status });   // the server answers in English
    return j;
  }

  function ago(iso) {
    if (!iso) return '–';
    const s = Math.max(0, Math.round((Date.now() - new Date(iso)) / 1000));
    if (s < 90) return t('{n} s ago', { n: s });
    if (s < 5400) return t('{n} min ago', { n: Math.round(s / 60) });
    if (s < 129600) return t('{n} h ago', { n: Math.round(s / 3600) });
    return new Date(iso).toLocaleString(I18N.locale);
  }
  const when = (iso) => (iso ? new Date(iso).toLocaleString(I18N.locale) : '–');
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

  // ---- where one is: #devices, #devices/<id>, #users, #audit, #server
  function route() {
    const [v, id] = location.hash.slice(1).split('/');
    const was = selected;
    view = VIEWS[v] ? v : 'devices';
    // "Devices" is for using a device, "Manage" for looking after it: the same
    // list and the same device, with different things shown (style.css).
    const wasView = shownView, listed = view === 'devices' || view === 'manage';
    shownView = view;
    selected = listed && id ? id : '';
    document.body.classList.toggle('manage', view === 'manage');
    document.body.classList.toggle('detail', !!selected);
    document.body.classList.toggle('popped', !!was && !selected && listed);   // back from a device: the list returns from the left
    for (const name of Object.keys(VIEWS)) if (name !== 'manage') $('#v-' + name).hidden = (name === 'devices' ? !listed : name !== view) || !signedIn;
    $('#back').href = '#' + view;
    $('#back').textContent = $('#listTitle').textContent = VIEWS[listed ? view : 'devices'];
    for (const a of $('#nav').children) {
      if (a.dataset.view === view) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
    }
    $('#title').textContent = signedIn ? VIEWS[view] : APP;
    document.title = signedIn ? `${VIEWS[view]} – ${APP}` : APP;
    if (!signedIn) return;
    if (was !== selected) {
      if (was) conDetach();
      renderDevices();
      if (selected) { renderDetail(); window.scrollTo(0, 0); }
    } else if (wasView !== view && listed) {
      renderDevices();                              // the list leads to the same device in the other place
      if (selected) renderDetail();
    }
    showDetail();
    if (view === 'users' || view === 'audit') loadAdmin();
    if (view === 'server') renderServer();
    if (view === 'library') loadLibrary();
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
    if (!r.ok) throw Object.assign(new Error(r.status === 429 ? t('Too many attempts – wait a moment') : j.error ? t(j.error) : 'HTTP ' + r.status), { status: r.status });
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
    document.body.classList.remove('in');
    $('#logout').hidden = $('#nav').hidden = true;
    $('#whoami').textContent = '';
    document.body.classList.remove('detail', 'popped');
    $('#title').textContent = document.title = APP;
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
      $('#whoami').textContent = token ? t('Admin token') : me;
      $('#login').hidden = true;
      $('#app').hidden = false;
      document.body.classList.add('in');            // the layout of someone who is signed in
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
    if ($('#setupPass').value !== $('#setupPass2').value) return setupError(t('The two passwords are not the same'));
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
      if (view === 'manage' && selected) [packages, templates] = await Promise.all([api('GET', '/firmware'), api('GET', '/templates')]);
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
    // Sorted by where they are: region, site, name. A device without a place comes last.
    const find = $('#find').value.trim().toLowerCase();
    $('#find').hidden = devices.length < 6 && !find;
    const place = (d) => [d.region, d.site].filter(Boolean).join(' · ');
    const shown = devices.filter((d) => !find || [d.name, d.id, d.region, d.site].some((x) => (x || '').toLowerCase().includes(find)))
      .sort((x, y) => (!place(x) - !place(y)) || place(x).localeCompare(place(y)) || (x.name || x.id).localeCompare(y.name || y.id));
    const grouped = devices.some((d) => place(d));
    let group = null;
    for (const d of shown) {
      if (grouped && place(d) !== group) {
        group = place(d);
        list.append(el('div', { className: 'grp', textContent: group || t('No site') }));
      }
      const st = d.status || {}, meta = [];
      const use = uses(d);
      if (use.length) meta.push(use.join(', '));
      else if (d.state === 'active' && !d.online && d.last_seen) meta.push(t('last seen {ago}', { ago: ago(d.last_seen) }));
      if (st.bat_pct != null) meta.push(t('battery {n} %', { n: st.bat_pct }));
      if (view === 'manage') {
        if (st.rssi != null) meta.push(`${st.rssi} dBm`);
        const fw = st.fw || (d.info && d.info.fw);
        if (fw) meta.push(t('firmware {v}', { v: fw }));
        if (d.open_commands) meta.push(d.open_commands === 1 ? t('1 command open') : t('{n} commands open', { n: d.open_commands }));
        if (d.old_key) meta.push(t('old server key'));
      }
      // connected right now, in a word and a colour; a state other than "active" says more than that
      const now = d.state !== 'active' ? badge(d.state, STATES) : el('span', { className: 'now ' + (use.length ? 'busy' : d.online ? 'on' : 'off'), textContent: use.length ? t('in use') : d.online ? t('online') : t('offline') });
      const a = el('a', { className: 'dev', href: `#${view === 'manage' ? 'manage' : 'devices'}/${d.id}`, role: 'listitem' },
        el('b', { textContent: d.name || d.id.slice(0, 8) }), now,
        el('span', { className: 'meta', textContent: meta.join(' · ') || (d.last_seen ? '' : t('has not reported in yet')) }));
      a.dataset.id = d.id;
      if (d.id === selected) a.setAttribute('aria-current', 'true');
      list.append(a);
      if (d.id === focus) a.focus({ preventScroll: true });     // the list is rebuilt under the keyboard's feet
    }
    $('#noDevices').hidden = !!devices.length;
    if (loaded && selected && !devices.some((d) => d.id === selected)) { conDetach(); location.hash = '#' + view; }   // it is gone
    showDetail();
  }

  $('#find').addEventListener('input', renderDevices);
  // Who is on the serial ports of a device right now: an administrator through
  // this server, or somebody at the device itself (it says so when it reports in).
  function uses(d) {
    const out = [], st = d.status || {};
    if (d.console) out.push(d.console.by && d.console.by !== 'token' ? t('port {n}: {who}', { n: d.console.port, who: d.console.by }) : t('port {n}: remote console', { n: d.console.port }));
    if (!d.online) return out;
    if (st.web) out.push(st.web === 1 ? t('1 browser at the device') : t('{n} browsers at the device', { n: st.web }));
    if (st.tcp) out.push(t('raw TCP'));
    if (st.bt) out.push(t('Bluetooth'));
    return out;
  }

  // ---- one device
  function pretty(v) {
    return typeof v === 'string' ? t(v) : JSON.stringify(v, null, 1);   // a refusal comes in English: 'refused at the device'
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
    const st = d.status || {}, line = [];
    if (d.last_seen) line.push(t('last seen {ago}', { ago: ago(d.last_seen) }));
    if (st.bat_pct != null) line.push(t('battery {n} %', { n: st.bat_pct }));
    $('#dLine').textContent = [d.online ? t('online') : d.state === 'active' ? t('offline') : '', [d.region, d.site].filter(Boolean).join(' · ')].concat(d.online ? line.slice(1) : line).filter(Boolean).join(' · ');
    const use = uses(d);
    $('#dUse').hidden = !use.length;
    $('#dUse').textContent = use.length ? t('In use – {list}', { list: use.join(', ') }) : '';
    for (const [id, key] of [['#dRegion', 'region'], ['#dSite', 'site']]) {
      if (document.activeElement !== $(id) && placeFor !== d.id + (d[key] || '')) $(id).value = d[key] || '';
      const dl = $(id).list;
      dl.textContent = '';
      for (const v of [...new Set(devices.map((x) => x[key]).filter(Boolean))].sort()) dl.append(el('option', { value: v }));
    }
    placeFor = d.id + (d.region || '') + (d.site || '');
    $('#dNotActive').hidden = d.state === 'active';
    $('#dNotActiveText').textContent = d.state === 'pending' ? t('This device still waits for its confirmation.') : t('This device is revoked.');
    $('#toManage').href = '#manage/' + d.id;
    $('#approveForm').hidden = d.state !== 'pending';
    $('#commands').hidden = $('#backup').hidden = $('#settings').hidden = $('#firmware').hidden = d.state !== 'active';
    renderFirmware(d);
    if (document.activeElement !== $('#tplPick')) {
      const was = $('#tplPick').value;
      $('#tplPick').textContent = '';
      for (const tpl of templates.slice().sort(byFolder)) $('#tplPick').append(el('option', { value: tpl.id, textContent: (tpl.folder ? tpl.folder + ' / ' : '') + tpl.name }));
      $('#tplPick').value = was;
      if ($('#tplPick').selectedIndex < 0) $('#tplPick').selectedIndex = 0;
    }
    $('#tplSendForm').hidden = !templates.length;
    if (setFor !== d.id) { setFor = d.id; setLoaded = null; $('#setForm').hidden = true; $('#setState').textContent = $('#bkState').textContent = $('#fwState').textContent = ''; }
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
        el('td', { textContent: c.by && c.by !== 'token' ? c.by : '' }),
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

  let placeFor = '';
  $('#placeSave').addEventListener('click', async () => {
    try {
      await api('POST', `/devices/${selected}/place`, { region: $('#dRegion').value, site: $('#dSite').value });
      toast(t('Saved'));
    } catch (e) { toast(e.message, 5000); }
    placeFor = '';
    refresh();
  });
  $('#tplSendForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const tpl = templates.find((x) => String(x.id) === $('#tplPick').value);
    if (!tpl) return;
    try {
      await api('POST', `/templates/${tpl.id}/send`, { device: selected });
      toast(t('{cmd}: queued – the device fetches the command the next time it reports in', { cmd: tpl.name }));
    } catch (err) { toast(err.message, 5000); }
    refresh();
  });

  // ---- the library: templates and firmware, kept here for every device
  let templates = [], images = { images: [] }, imgBusy = false;
  const bytes = (n) => (n >= 1073741824 ? (n / 1073741824).toFixed(2) + ' GB' : n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' kB');
  const byFolder = (x, y) => (x.folder || '').localeCompare(y.folder || '');
  // One row per folder in front of what is in it.
  function folderRows(body, items, cols, row) {
    let folder = null;
    for (const it of items.slice().sort(byFolder)) {
      if ((it.folder || '') !== folder && items.some((x) => x.folder)) {
        folder = it.folder || '';
        body.append(el('tr', { className: 'grp' }, el('td', { colSpan: cols, textContent: folder || t('No folder') })));
      }
      body.append(row(it));
    }
  }
  // A small question with one field: a device to pick, or a name to type.
  function pick(title, labelText, ok, options, value) {
    const dlg = $('#pickDlg'), sel = $('#pickDev'), inp = $('#pickText');
    $('#pickTitle').textContent = title;
    $('#pickLabel').textContent = labelText;
    $('#pickOk').textContent = ok;
    sel.hidden = !Array.isArray(options) || typeof value === 'string';
    inp.hidden = !sel.hidden;
    sel.required = !sel.hidden;
    if (!sel.hidden) {
      sel.textContent = '';
      for (const [v, text] of options) sel.append(el('option', { value: v, textContent: text }));
    } else {
      inp.value = value || '';
      $('#pickList').textContent = '';
      for (const v of options || []) $('#pickList').append(el('option', { value: v }));
    }
    dlg.returnValue = '';
    dlg.showModal();
    return new Promise((done) => dlg.addEventListener('close', () => done(dlg.returnValue === 'ok' ? (sel.hidden ? inp.value.trim() : sel.value) : null), { once: true }));
  }
  $('#pickCancel').addEventListener('click', () => $('#pickDlg').close(''));
  $('#pickForm').addEventListener('submit', (e) => { e.preventDefault(); $('#pickDlg').close('ok'); });

  async function loadLibrary() {
    try { [templates, packages, images] = await Promise.all([api('GET', '/templates'), api('GET', '/firmware'), api('GET', '/images')]); } catch (e) { return; }
    const active = devices.filter((d) => d.state === 'active').map((d) => [d.id, (d.name || d.id.slice(0, 8)) + ([d.region, d.site].filter(Boolean).length ? ' – ' + [d.region, d.site].filter(Boolean).join(' · ') : '')]);
    const tb = $('#tpls').tBodies[0];
    tb.textContent = '';
    folderRows(tb, templates, 4, (tpl) => {
      const send = el('button', { className: 'btn small', type: 'button', textContent: t('Send'), disabled: !active.length });
      const edit = el('button', { className: 'btn small', type: 'button', textContent: t('Edit') });
      const del = el('button', { className: 'btn small danger', type: 'button', textContent: t('Delete') });
      send.addEventListener('click', async () => {
        const dev = await pick(t('Send “{name}”', { name: tpl.name }), t('Device'), t('Send'), active);
        if (!dev) return;
        try { await api('POST', `/templates/${tpl.id}/send`, { device: dev }); toast(t('{cmd}: queued – the device fetches the command the next time it reports in', { cmd: tpl.name })); } catch (e) { toast(e.message, 5000); }
      });
      edit.addEventListener('click', () => editTemplate(tpl));
      del.addEventListener('click', async () => {
        if (!await ask(t('Delete this template?'), tpl.name, t('Delete'))) return;
        try { await api('DELETE', `/templates/${tpl.id}`); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      });
      return el('tr', {}, el('td', { textContent: tpl.name }), el('td', { textContent: ago(tpl.updated) }),
        el('td', { textContent: tpl.by && tpl.by !== 'token' ? tpl.by : '' }), el('td', { className: 'acts' }, send, edit, del));
    });
    label($('#tpls'));
    $('#tpls').hidden = !templates.length;
    $('#noTpls').hidden = !!templates.length;

    const fb = $('#libFw').tBodies[0];
    fb.textContent = '';
    folderRows(fb, packages, 6, (p) => {
      const move = el('button', { className: 'btn small', type: 'button', textContent: t('Folder') });
      const del = el('button', { className: 'btn small danger', type: 'button', textContent: t('Delete') });
      move.addEventListener('click', async () => {
        const folder = await pick(t('Folder'), t('Folder'), t('Save'), [...new Set(packages.map((x) => x.folder).filter(Boolean))], p.folder || '');
        if (folder === null) return;
        try { await api('POST', `/firmware/${p.id}/folder`, { folder }); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      });
      del.addEventListener('click', async () => {
        if (!await ask(t('Delete this package?'), t('Version {v} ({lang}), signed {when}. Devices that have it installed keep it.', { v: p.fw, lang: LANGS[p.lang] || p.lang, when: when(new Date(p.seq * 1000).toISOString()) }), t('Delete'))) return;
        try { await api('DELETE', `/firmware/${p.id}`); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      });
      return el('tr', {}, el('td', { textContent: p.fw }), el('td', { textContent: LANGS[p.lang] || p.lang }), el('td', { textContent: p.board }),
        el('td', { textContent: when(new Date(p.seq * 1000).toISOString()) }), el('td', { textContent: (p.size / 1048576).toFixed(1) + ' MB' }), el('td', { className: 'acts' }, move, del));
    });
    label($('#libFw'));
    $('#libFw').hidden = !packages.length;
    $('#noLibFw').hidden = !!packages.length;

    const ib = $('#imgs').tBodies[0], list = images.images;
    ib.textContent = '';
    folderRows(ib, list, 6, (im) => {
      const get = el('button', { className: 'btn small', type: 'button', textContent: t('Download') });
      const name = el('button', { className: 'btn small', type: 'button', textContent: t('Rename') });
      const move = el('button', { className: 'btn small', type: 'button', textContent: t('Folder') });
      const del = el('button', { className: 'btn small danger', type: 'button', textContent: t('Delete') });
      const sum = el('button', { className: 'btn small mono', type: 'button', textContent: im.sha256.slice(0, 12) + '…', title: im.sha256 });
      // The whole checksum is what one compares with the maker's; the cell shows its beginning.
      sum.addEventListener('click', async () => {
        try { await navigator.clipboard.writeText(im.sha256); toast(t('SHA-256 copied')); } catch (e) { toast(im.sha256, 15000); }
      });
      get.addEventListener('click', async () => {
        try { location.assign((await api('POST', `/images/${im.id}/link`)).url); } catch (e) { toast(e.message, 5000); }
      });
      const change = async (body) => {
        try { await api('PUT', `/images/${im.id}`, Object.assign({ name: im.name, folder: im.folder || '' }, body)); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      };
      name.addEventListener('click', async () => {
        const v = await pick(t('Rename'), t('Name'), t('Save'), [], im.name);
        if (v) change({ name: v });
      });
      move.addEventListener('click', async () => {
        const folder = await pick(t('Folder'), t('Folder'), t('Save'), [...new Set(list.map((x) => x.folder).filter(Boolean))], im.folder || '');
        if (folder !== null) change({ folder });
      });
      del.addEventListener('click', async () => {
        if (!await ask(t('Delete this image?'), im.name + ' (' + bytes(im.size) + ')', t('Delete'))) return;
        try { await api('DELETE', `/images/${im.id}`); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      });
      return el('tr', {}, el('td', { textContent: im.name }), el('td', { textContent: bytes(im.size) }), el('td', {}, sum),
        el('td', { textContent: ago(im.added) }), el('td', { textContent: im.by && im.by !== 'token' ? im.by : '' }), el('td', { className: 'acts' }, get, name, move, del));
    });
    label($('#imgs'));
    $('#imgs').hidden = !list.length;
    $('#noImgs').hidden = !!list.length;
    if (!imgBusy) $('#imgState').textContent = t('{used} of {total} used, up to {max} per file', { used: bytes(images.used), total: bytes(images.total), max: bytes(images.max) });
  }
  // An image is large: it goes up as it is, and the bar of the browser is not
  // enough to know how far it has got.
  $('#imgAdd').addEventListener('click', () => { $('#imgFile').value = ''; $('#imgFile').click(); });
  $('#imgFile').addEventListener('change', () => {
    const file = $('#imgFile').files[0];
    if (!file || imgBusy) return;
    if (file.size > images.max) { toast(t('the file is larger than this server takes'), 6000); return; }
    const done = (text, bad) => { imgBusy = false; $('#imgAdd').disabled = false; if (bad) toast(text, 6000); loadLibrary().then(() => { if (bad) $('#imgState').textContent = text; }); };
    const x = new XMLHttpRequest();
    x.open('POST', '/admin/v1/images?name=' + encodeURIComponent(file.name));
    for (const [k, v] of Object.entries(authHeaders())) x.setRequestHeader(k, v);
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.upload.addEventListener('progress', (e) => {
      if (e.lengthComputable) $('#imgState').textContent = t('Uploading “{name}”: {n} %', { name: file.name, n: Math.floor(e.loaded * 100 / e.total) });
    });
    x.addEventListener('load', () => {
      let j = {};
      try { j = JSON.parse(x.responseText); } catch (e) { /* not JSON */ }
      if (x.status === 401) { showLogin(); done(t('No longer signed in'), true); } else if (x.status >= 300) done(j.error ? t(j.error) : 'HTTP ' + x.status, true);
      else done('', false);
    });
    x.addEventListener('error', () => done(t('the upload broke off'), true));
    imgBusy = true;
    $('#imgAdd').disabled = true;
    $('#imgState').textContent = t('Uploading “{name}” …', { name: file.name });
    x.send(file);
  });
  let tplEditing = 0;
  function editTemplate(tpl) {
    tplEditing = tpl ? tpl.id : 0;
    $('#tplTitle').textContent = tpl ? t('Edit template') : t('New template');
    $('#tplName').value = tpl ? tpl.name : '';
    $('#tplFolder').value = tpl ? tpl.folder || '' : '';
    $('#tplText').value = tpl ? tpl.text : '';
    $('#tplErr').hidden = true;
    $('#tplFolders').textContent = '';
    for (const v of [...new Set(templates.map((x) => x.folder).filter(Boolean))]) $('#tplFolders').append(el('option', { value: v }));
    $('#tplDlg').showModal();
  }
  $('#tplNew').addEventListener('click', () => editTemplate(null));
  $('#tplCancel').addEventListener('click', () => $('#tplDlg').close());
  $('#tplForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const body = { name: $('#tplName').value, folder: $('#tplFolder').value, text: $('#tplText').value.replace(/\r\n/g, '\n') };
    try {
      await api(tplEditing ? 'PUT' : 'POST', tplEditing ? `/templates/${tplEditing}` : '/templates', body);
      $('#tplDlg').close();
      loadLibrary();
    } catch (err) { $('#tplErr').textContent = err.message; $('#tplErr').hidden = false; }
  });
  $('#libFwAdd').addEventListener('click', () => { $('#fwFile').value = ''; $('#fwFile').click(); });

  $('#approveForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    try {
      await api('POST', `/devices/${selected}/approve`, { sas: $('#sas').value.trim() });
      toast(t('Device confirmed'));
    } catch (err) {
      toast(err.status === 403 ? t('The code does not match') : err.status === 404 ? t('Too many wrong entries – the enrollment was discarded') : err.message, 5000);
    }
    $('#sas').value = '';
    refresh();
  });

  async function send(type, args) {
    try {
      await api('POST', `/devices/${selected}/commands`, args ? { type, args } : { type });
      toast(t('{cmd}: queued – the device fetches the command the next time it reports in', { cmd: CMD_NAMES[type] || type }));
    } catch (err) { toast(err.message, 5000); }
    refresh();
  }
  $('#commands').addEventListener('click', async (e) => {
    const type = e.target.dataset && e.target.dataset.cmd;
    if (!type) return;
    if (type === 'reboot' && !await ask(t('Restart the device?'), t('Console sessions that are running break off.'), t('Restart'))) return;
    send(type);
  });
  $('#playForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const name = $('#playName').value.trim(), port = +$('#playPort').value;
    if (!await ask(t('Play “{name}” on port {port}?', { name, port }), t('The lines go to the console of whatever is connected there.'), t('Play'))) return;
    send('play', { name, port });
  });
  // ---- settings and backup: commands whose answer the page waits for
  let setFor = '', setLoaded = null;
  const DENIED = 'not allowed at the device (Menu -> Network -> Control center)';
  // Sends a command and waits until the device has answered it.
  async function run(dev, type, args) {
    const c = await api('POST', `/devices/${dev}/commands`, args ? { type, args } : { type });
    for (let i = 0; i < 90; i++) {                  // a device reports in every half minute or so
      await new Promise((r) => setTimeout(r, 1000));
      const now = (await api('GET', `/devices/${dev}/commands`)).find((x) => x.id === c.id);
      if (!now) break;
      if (now.state === 'done') return now.result;
      if (now.state === 'failed' || now.state === 'expired') throw new Error(typeof now.result === 'string' ? t(now.result) : CMD_STATES[now.state]);
    }
    throw new Error(t('The device did not answer'));
  }

  // What the device's own web interface can set, group by group:
  // [key, kind, label, ...]. A "secret" is only sent when something was typed.
  const NUMS = (from, to) => Array.from({ length: to - from + 1 }, (_, i) => [from + i, String(from + i)]);
  const SET = [
    [t('Device'), [
      ['hostname', 'text', t('Host name')],
      ['webPass', 'secret', t('Password of the web interface'), 'webPassSet'],
      ['webPassClear', 'flag', t('Remove the password of the web interface')],
      ['displayBrightness', 'number', t('Display brightness (0–255)'), 0, 255],
      ['displayTimeout', 'number', t('Display off after seconds (0 = never)'), 0, 3600],
      ['displayFlip', 'check', t('Turn the display by 180°')]]],
    [t('Hotspot'), [
      ['apSsid', 'text', t('Network name (SSID)')],
      ['apPass', 'secret', t('Password of the hotspot'), ''],
      ['apChannel', 'select', t('Channel'), [[0, t('automatic')]].concat(NUMS(1, 13))],
      ['txPower', 'select', t('Transmit power'), [[34, '8.5 dBm'], [44, '11 dBm'], [60, '15 dBm'], [78, '19.5 dBm']]]]],
    [t('Wi-Fi client'), [
      ['staSsid', 'text', t('Network name (SSID), empty = off')],
      ['staAuth', 'select', t('Sign-in'), [[0, t('Password (WPA2/WPA3)')], [1, '802.1X EAP-TLS'], [2, '802.1X PEAP'], [3, '802.1X EAP-TTLS']]],
      ['staPass', 'secret', t('Wi-Fi password'), 'staPassSet'],
      ['staIdentity', 'text', t('802.1X identity')],
      ['staUser', 'text', t('802.1X user name')],
      ['staPhase2', 'select', t('TTLS inner method'), [[0, 'MSCHAPv2'], [1, 'PAP']]],
      ['staCaCheck', 'check', t('Check the RADIUS server against the stored CA certificate')]]],
    [t('Access'), [
      ['httpsEnabled', 'check', t('Web interface also over HTTPS')],
      ['httpsLanOnly', 'check', t('In the LAN redirect HTTP to HTTPS')],
      ['httpsCert', 'select', t('HTTPS certificate'), [[0, t('its own')], [1, t('the 802.1X client certificate')]]],
      ['tcpEnabled', 'check', t('Raw TCP and Telnet to the ports')],
      ['tcpLan', 'check', t('Raw TCP also from the LAN (no sign-in!)')]]],
    [t('Battery'), [
      ['batPin', 'pin', t('Measuring pin'), 'pinsAdc'],
      ['batType', 'select', t('Type'), [[0, 'LiPo 1S'], [1, 'NiCd/NiMH 4 cells']]],
      ['batDiv', 'number', t('Divider ratio × 10'), 10, 100]]],
  ];
  const PORT = [
    ['enabled', 'check', t('Switched on')],
    ['name', 'text', t('Name')],
    ['rx', 'pin', 'RX', 'pinsRx'],
    ['tx', 'pin', 'TX', 'pinsTx'],
  ];
  const LINE = [
    ['baud', 'select', t('Baud rate'), [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600].map((b) => [b, String(b)])],
    ['bits', 'select', t('Data bits'), NUMS(5, 8)],
    ['parity', 'select', t('Parity'), [['N', t('none')], ['E', t('even')], ['O', t('odd')]]],
    ['stop', 'select', t('Stop bits'), NUMS(1, 2)],
    ['swap', 'check', t('Swap RX/TX at the level converter')],
  ];
  // One control for one setting; data-path says where its value belongs.
  function field(def, value, cfg, path) {
    const [key, kind, text, extra, max] = def;
    let input;
    if (kind === 'check' || kind === 'flag') {
      input = el('input', { type: 'checkbox', checked: !!value });
    } else if (kind === 'select' || kind === 'pin') {
      const options = kind === 'pin' ? [[-1, '–']].concat((cfg[extra] || []).map((n) => [n, (cfg.pinPrefix || 'GPIO') + n])) : extra;
      input = el('select');
      for (const [v, name] of options) input.append(el('option', { value: String(v), textContent: name }));
      if (![...input.options].some((o) => o.value === String(value))) input.append(el('option', { value: String(value), textContent: String(value) }));
      input.value = String(value);
    } else if (kind === 'secret') {
      input = el('input', { type: 'password', autocomplete: 'new-password', placeholder: extra && cfg[extra] ? t('set – empty leaves it as it is') : t('empty leaves it as it is') });
    } else {
      input = el('input', kind === 'number' ? { type: 'number', min: extra, max, value } : { type: 'text', value: value == null ? '' : value, autocapitalize: 'none', spellcheck: false });
    }
    input.dataset.path = path + key;
    input.dataset.kind = kind;
    return kind === 'check' || kind === 'flag' ? el('label', { className: 'check' }, input, ' ' + text) : el('label', null, text, input);
  }
  function valueOf(input) {
    const kind = input.dataset.kind;
    if (kind === 'check' || kind === 'flag') return input.checked;
    if (kind === 'number' || kind === 'pin') return +input.value;
    if (kind === 'select') return /^-?\d+$/.test(input.value) ? +input.value : input.value;
    return input.value;
  }
  function fillSettings(cfg) {
    setLoaded = cfg;
    const box = $('#setFields');
    box.textContent = '';
    const group = (title, fields) => box.append(el('fieldset', { className: 'group' }, el('legend', { textContent: title }), ...fields));
    for (const [title, defs] of SET) group(title, defs.map((d) => field(d, cfg[d[0]], cfg, '')));
    (cfg.ports || []).forEach((p, i) => {
      const line = p.serial || {};
      group(t('Port {n}', { n: i + 1 }), PORT.map((d) => field(d, p[d[0]], cfg, `ports.${i}.`)).concat(LINE.map((d) => field(d, line[d[0]], cfg, `ports.${i}.serial.`))));
    });
    const first = box.querySelector('[data-path="ports.0.enabled"]');
    if (first) first.disabled = true;                // the first port is always there
    $('#setHint').hidden = false;
    $('#setForm').hidden = false;
  }
  // Only what was changed goes to the device; the ports go as a whole list.
  function changedSettings() {
    const out = {}, ports = (setLoaded.ports || []).map((p) => ({ enabled: p.enabled, name: p.name, rx: p.rx, tx: p.tx, serial: Object.assign({}, p.serial) }));
    let portsChanged = false;
    for (const input of $('#setFields').querySelectorAll('[data-path]')) {
      const path = input.dataset.path.split('.'), kind = input.dataset.kind, v = valueOf(input);
      if (path[0] === 'ports') {
        const at = path[2] === 'serial' ? ports[+path[1]].serial : ports[+path[1]], key = path[path.length - 1];
        if (at[key] !== v) { at[key] = v; portsChanged = true; }
      } else if (kind === 'secret' || kind === 'flag') {
        if (v) out[path[0]] = v;
      } else if (setLoaded[path[0]] !== v) out[path[0]] = v;
    }
    if (portsChanged) out.ports = ports;
    return out;
  }
  $('#setLoad').addEventListener('click', async () => {
    const dev = selected;
    $('#setLoad').disabled = true;
    $('#setState').textContent = t('Waiting for the device …');
    try {
      const cfg = await run(dev, 'settings');
      if (selected !== dev) return;
      fillSettings(cfg);
      $('#setState').textContent = cfg.c2Control === false ? t('The device does not let this server change anything yet: tick the box at the device (Menu → Network → Control center).') : '';
    } catch (e) { $('#setState').textContent = e.message; } finally { $('#setLoad').disabled = false; }
  });
  $('#setForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const dev = selected, change = changedSettings();
    if (!Object.keys(change).length) { toast(t('Nothing was changed')); return; }
    if (!await ask(t('Save these settings?'), t('The device stores them and restarts. With wrong Wi-Fi settings it does not find back to this server – then it has to be put right at the device itself.'), t('Save and restart'))) return;
    $('#setState').textContent = t('Waiting for the device …');
    try {
      await run(dev, 'set', change);
      toast(t('Settings saved – the device restarts'));
      if (selected === dev) { $('#setForm').hidden = true; $('#setState').textContent = t('Saved. The device restarts; read the settings again when it is back.'); }
    } catch (err) { $('#setState').textContent = err.message; toast(err.message, 6000); }
    refresh();
  });

  // ---- firmware: packages signed on the owner's computer. The server keeps
  // them and hands them out; whether one is genuine is the device's decision.
  let packages = [], fwBusy = false;
  const fwNote = (text) => { $('#fwState').textContent = $('#libFwState').textContent = text; };
  const LANGS = { en: 'English', de: 'Deutsch', pt: 'Português' };
  function renderFirmware(d) {
    const st = d.status || {};
    const fw = st.fw || (d.info && d.info.fw);
    $('#fwNow').textContent = fw ? t('This device runs {v}.', { v: fw + (st.lang ? ' (' + (LANGS[st.lang] || st.lang) + ')' : '') }) : '';
    const body = $('#fwList').tBodies[0];
    body.textContent = '';
    for (const p of packages) {
      const install = el('button', { className: 'btn warn', type: 'button', textContent: t('Install'), disabled: fwBusy });
      const remove = el('button', { className: 'btn', type: 'button', textContent: t('Delete'), disabled: fwBusy });
      install.addEventListener('click', () => installFirmware(d, p));
      remove.addEventListener('click', async () => {
        if (!await ask(t('Delete this package?'), t('Version {v} ({lang}), signed {when}. Devices that have it installed keep it.', { v: p.fw, lang: LANGS[p.lang] || p.lang, when: when(new Date(p.seq * 1000).toISOString()) }), t('Delete'))) return;
        try { await api('DELETE', `/firmware/${p.id}`); } catch (e) { toast(e.message, 6000); }
        refresh();
      });
      body.append(el('tr', {}, el('td', { textContent: p.fw }), el('td', { textContent: LANGS[p.lang] || p.lang }),
        el('td', { textContent: when(new Date(p.seq * 1000).toISOString()) }), el('td', { textContent: (p.size / 1048576).toFixed(1) + ' MB' }),
        el('td', {}, el('span', { className: 'row left wrap' }, install, remove))));
    }
    label($('#fwList'));
    $('#fwList').hidden = !packages.length;
    $('#fwNone').hidden = !!packages.length;
    if (d.update) fwNote(t('Sending the package: {n} %', { n: Math.floor(d.update.sent * 100 / d.update.size) }));
  }
  async function installFirmware(d, p) {
    if (!await ask(t('Install this firmware?'), t('Version {v} ({lang}) on “{name}”. The device fetches the package, checks the signature and restarts with the new firmware; that takes a few minutes. Do not switch it off meanwhile.',
      { v: p.fw, lang: LANGS[p.lang] || p.lang, name: d.name || d.id.slice(0, 8) }), t('Install'), 'warn')) return;
    fwBusy = true;
    fwNote(t('Waiting for the device …'));
    try {
      const c = await api('POST', `/devices/${d.id}/commands`, { type: 'update', args: { fw: p.id } });
      let done = null;
      for (let i = 0; i < 900 && !done; i++) {      // a large image over a weak link takes its time
        await new Promise((r) => setTimeout(r, 2000));
        const now = (await api('GET', `/devices/${d.id}/commands`)).find((x) => x.id === c.id);
        if (!now) throw new Error(t('The device did not answer'));
        if (now.state === 'failed' || now.state === 'expired') throw new Error(typeof now.result === 'string' ? t(now.result) : CMD_STATES[now.state]);
        if (now.state === 'done') done = now;
      }
      if (!done) throw new Error(t('The device did not answer'));
      fwNote(t('Installed: version {v}. The device restarts.', { v: p.fw }));
      toast(t('Firmware installed – the device restarts'));
    } catch (e) { fwNote(e.message); toast(e.message, 6000); }
    fwBusy = false;
    refresh();
  }
  $('#fwAdd').addEventListener('click', () => { $('#fwFile').value = ''; $('#fwFile').click(); });
  $('#fwFile').addEventListener('change', async () => {
    const file = $('#fwFile').files[0];
    if (!file) return;
    fwNote(t('Uploading “{name}” …', { name: file.name }));
    try {
      const r = await fetch('/admin/v1/firmware', { method: 'POST', cache: 'no-store', headers: Object.assign(authHeaders(), { 'Content-Type': 'application/octet-stream' }), body: file });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(j.error ? t(j.error) : 'HTTP ' + r.status);
      fwNote(t('Uploaded: version {v} ({lang}).', { v: j.fw, lang: LANGS[j.lang] || j.lang }));
    } catch (e) { fwNote(e.message); toast(e.message, 6000); }
    refresh();
    if (view === 'library') loadLibrary();
  });

  // A backup is a file on the administrator's computer: the settings as the
  // device reports them and every stored configuration.
  const bkNote = (text) => { $('#bkState').textContent = text; };
  async function busy(work) {
    $('#bkSave').disabled = $('#bkLoad').disabled = true;
    try { await work(); } catch (e) { bkNote(e.message); toast(e.message, 6000); } finally { $('#bkSave').disabled = $('#bkLoad').disabled = false; refresh(); }
  }
  $('#bkSave').addEventListener('click', () => busy(async () => {
    const dev = selected, d = devices.find((x) => x.id === dev);
    bkNote(t('Reading the settings …'));
    const settings = await run(dev, 'settings');
    const list = (await run(dev, 'configs')).configs || [];
    if (list.length && settings.c2Control === false) throw new Error(t(DENIED));
    const configs = [], skipped = [];
    for (const c of list) {
      bkNote(t('Reading “{name}” …', { name: c.name }));
      try { const r = await run(dev, 'config_get', { name: c.name }); configs.push({ name: r.name, text: r.text }); } catch (e) { skipped.push(`${c.name} (${e.message})`); }
    }
    const doc = { kind: 'rs232-backup', version: 1, saved: new Date().toISOString(), device: { id: d.id, name: d.name }, settings, configs };
    const url = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 1)], { type: 'application/json' }));
    const a = el('a', { href: url, download: `rs232-${(d.name || d.id.slice(0, 8)).replace(/[^\w-]+/g, '_')}-${doc.saved.slice(0, 10)}.json` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    bkNote(t('Saved: the settings and {n} configurations.', { n: configs.length }) + (skipped.length ? ' ' + t('Not in it: {list}', { list: skipped.join(', ') }) : ''));
  }));
  $('#bkLoad').addEventListener('click', () => { $('#bkFile').value = ''; $('#bkFile').click(); });
  $('#bkFile').addEventListener('change', () => busy(async () => {
    const file = $('#bkFile').files[0], dev = selected;
    if (!file) return;
    let doc;
    try { doc = JSON.parse(await file.text()); } catch (e) { doc = null; }
    if (!doc || doc.kind !== 'rs232-backup' || !doc.settings) throw new Error(t('This file is not a backup of a device'));
    const configs = Array.isArray(doc.configs) ? doc.configs : [];
    if (!await ask(t('Restore this backup?'), t('From “{name}”, saved {when}: the settings and {n} configurations. Configurations of the same name are overwritten; the device restarts at the end.',
      { name: (doc.device && doc.device.name) || '?', when: when(doc.saved), n: configs.length }), t('Restore'))) return;
    const skipped = [];
    for (const c of configs) {
      const args = { name: String(c.name), text: String(c.text) };
      if (new Blob([JSON.stringify(args)]).size > 6000) { skipped.push(args.name); continue; }   // more than one command can carry
      bkNote(t('Writing “{name}” …', { name: args.name }));
      await run(dev, 'config_put', args);
    }
    // only what can be set: the report also names pins and what the board has
    const s = {}, known = new Set(SET.flatMap(([, defs]) => defs.filter((d) => d[1] !== 'secret' && d[1] !== 'flag').map((d) => d[0])));
    for (const k of Object.keys(doc.settings)) if (known.has(k)) s[k] = doc.settings[k];
    if (Array.isArray(doc.settings.ports)) s.ports = doc.settings.ports.map((p) => ({ enabled: !!p.enabled, name: String(p.name || ''), rx: p.rx, tx: p.tx, serial: p.serial }));
    bkNote(t('Writing the settings …'));
    await run(dev, 'set', s);
    bkNote(t('Restored. The device restarts.') + (skipped.length ? ' ' + t('Too large to write from here: {list}', { list: skipped.join(', ') }) : ''));
  }));

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
        if (r.status === 404) { o = { state: 'ended', reason: 'no longer known to the server', d: '', next: c.next }; } else if (!r.ok) throw new Error('HTTP ' + r.status);
        else o = await r.json();
      } catch (e) {
        if (con !== c) return;
        await new Promise((r) => setTimeout(r, 1000));
        continue;
      }
      if (con !== c) return;
      if (o.lost) c.term.write(`\r\n\x1b[90m[${t('{n} characters are missing', { n: o.lost })}]\x1b[0m\r\n`);
      if (o.d) c.term.write(unb64(o.d));
      c.next = o.next;
      c.left = o.left_s;
      c.leftAt = Date.now();
      if (o.state === 'ended') {
        c.ended = true;
        c.term.write(`\r\n\x1b[90m[${t('Session ended: {why}', { why: t(o.reason) })}]\x1b[0m\r\n`);
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
      // why a session ended comes from the server in English, or from the device in the device's language
      if (cur && cur.state === 'requested') { text = t('Requested (port {port}) – somebody at the device now has to tap “Allow” on its display.', { port: cur.port }); cls = 'wait'; }
      else if (cur && cur.state === 'granted') { text = t('Allowed at the device – the connection is coming up.'); cls = 'wait'; }
      else if (cur && cur.state === 'open') { text = t('Open on port {port}.', { port: cur.port }); cls = 'open'; }
      else if (cur && cur.state === 'refused') text = t('Did not come about: {why}', { why: t(cur.reason) });
      else if (cur && cur.state === 'ended') text = t('Last session ended: {why}', { why: t(cur.reason) });
      st.textContent = text;
      st.className = 'dim ' + cls;
      const busy = cur && ['requested', 'granted', 'open'].includes(cur.state);
      $('#conForm').hidden = !!busy || d.state !== 'active';
      if (cur && cur.state === 'open' && !(con && con.id === cur.id)) conAttach(d.id, cur.id, cur.left_s);
      if (con) {
        const left = Math.max(0, con.left - Math.round((Date.now() - con.leftAt) / 1000));
        $('#conInfo').textContent = con.ended ? t('ended') : t('Port {port} · {left} min left', { port: cur ? cur.port : '', left: mmss(left) });
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
          el('td', { textContent: dur == null ? t('running') : mmss(dur) + ' min' }),
          el('td', { textContent: l.reason ? t(l.reason) : '' }),
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
    if (!d || !await ask(t('Revoke “{name}”?', { name: d.name || d.id }), t('This takes effect at once and for good: the device does not come back, not even with a new invitation.'), t('Revoke'))) return;
    try { await api('POST', `/devices/${selected}/revoke`); toast(t('Device revoked')); } catch (err) { toast(err.message); }
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
    let inv;
    try { inv = await api('POST', '/enroll-tokens', { ttl_s: 600 }); } catch (err) { toast(err.message); return; }
    $('#tokenText').value = inv.token;
    drawQr($('#qr'), HOTSPOT + '#c2=' + encodeURIComponent(inv.token));
    const until = new Date(inv.expires);
    clearInterval(expTimer);
    const tick = () => {
      const s = Math.round((until - Date.now()) / 1000);
      $('#tokenExp').textContent = s > 0 ? t('valid for another {left} min, for one device', { left: `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}` }) : t('expired');
    };
    tick();
    expTimer = setInterval(tick, 1000);
    $('#inviteDlg').showModal();
  });
  $('#copyToken').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('#tokenText').value); toast(t('Copied')); } catch (e) { $('#tokenText').select(); toast(t('Selected – copy with Ctrl+C')); }
  });
  $('#closeInvite').addEventListener('click', () => $('#inviteDlg').close());
  $('#inviteDlg').addEventListener('close', () => { clearInterval(expTimer); $('#tokenText').value = ''; refresh(); });

  // ---- accounts and the audit record
  const KINDS = { local: t('Password + app'), entra: 'Microsoft Entra ID' };
  const ACTIONS = {
    login: t('Sign-in'), 'login entra': t('Sign-in with Microsoft'), logout: t('Sign-out'), setup: t('Account set up'),
    'POST /users': t('User added'), 'POST /users/{name}/reset': t('Account reset'), 'POST /users/{name}/disable': t('Account disabled or enabled'),
    'DELETE /users/{name}': t('User deleted'), 'POST /enroll-tokens': t('Invitation created'), 'POST /devices/{id}/approve': t('Device confirmed'),
    'POST /devices/{id}/revoke': t('Device revoked'), 'POST /devices/{id}/commands': t('Command'), 'POST /devices/{id}/console/{sess}/close': t('Remote console ended'),
    'POST /server/rotate-key': t('Server key changed'), 'POST /server/rotate-key/end': t('Key change ended'),
  };
  function showCode(name, code) {
    $('#codeFor').textContent = name;
    $('#codeText').textContent = code;
    $('#codeDlg').showModal();
  }
  async function userAction(title, text, yes, method, path, body, harmless) {
    if (!await ask(title, text, yes, harmless ? '' : 'danger')) return;
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
      const state = u.disabled ? t('disabled') : u.ready ? t('active') : u.setup_expires ? t('waiting to be set up (code valid until {until})', { until: when(u.setup_expires) }) : t('not set up, code expired');
      const how = KINDS[u.kind] + (u.kind === 'entra' ? ` (${u.entra_name || u.entra_oid}${u.entra_name && !u.entra_oid ? ', ' + t('never signed in') : ''})` : '');
      const acts = el('td', { className: 'acts' });
      const name = encodeURIComponent(u.name);
      if (u.kind === 'local') acts.append(el('button', { className: 'btn small', textContent: t('Reset'),
        onclick: () => userAction(t('Reset the account “{name}”?', { name: u.name }), t('Password and authenticator app stop working; there will be a new setup code.'), t('Reset'), 'POST', `/users/${name}/reset`) }));
      acts.append(el('button', { className: 'btn small', textContent: u.disabled ? t('Enable') : t('Disable'),
        onclick: () => userAction(u.disabled ? t('Enable the account “{name}”?', { name: u.name }) : t('Disable the account “{name}”?', { name: u.name }),
          u.disabled ? t('The person can sign in again afterwards.') : t('The person is signed out and cannot get in until the account is enabled again.'),
          u.disabled ? t('Enable') : t('Disable'), 'POST', `/users/${name}/disable`, { disabled: !u.disabled }, u.disabled) }));
      acts.append(el('button', { className: 'btn small danger', textContent: t('Delete'),
        onclick: () => userAction(t('Delete the account “{name}”?', { name: u.name }), t('This cannot be undone.'), t('Delete'), 'DELETE', `/users/${name}`) }));
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
        el('td', { textContent: a.ok ? '' : t('refused') })));
    }
    label($('#audit'));
  }

  // ---- the server: how it is, and its key
  function span(s) {                                // seconds, as a person says it
    if (s < 120) return `${s} s`;
    if (s < 7200) return `${Math.round(s / 60)} min`;
    if (s < 172800) return `${Math.round(s / 3600)} h`;
    return t('{n} days', { n: Math.round(s / 86400) });
  }
  function renderServer() {
    if (!server || !status) return;
    const st = status.state, kc = server.key_change, dv = status.devices;
    // what is wrong is said on every page, not only where the numbers are
    $('#alert').hidden = st.ok;
    if (!st.ok) $('#alert').textContent = t('The server cannot save its state (since {since}). Changes are lost if it restarts now.', { since: when(st.since) });
    const mark = $('#navMark');
    mark.hidden = st.ok && !kc;
    mark.className = 'mark' + (st.ok ? '' : ' bad');
    const b = $('#stState');
    b.className = 'badge ' + (st.ok ? 'ok' : 'failed');
    b.textContent = st.ok ? t('all right') : t('error');
    $('#stError').hidden = st.ok;
    $('#stError').textContent = st.ok ? '' : t('Saving has been failing since {since}: {error}', { since: when(st.since), error: st.error });
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
    $('#stNote').textContent = t('“Silent” means: connected, but not heard from for more than {span}. Turned away is who asks too often too fast (devices {h}, sign-ins {s}, calls without a sign-in {a}).', { span: span(status.silent_after_s), h: turned.handshake, s: turned.sign_in, a: turned.admin });

    $('#srvUrl').textContent = server.public_url;
    $('#srvFp').textContent = server.fingerprint;
    $('#keyChange').hidden = !kc;
    $('#kcEnd').hidden = !kc;
    $('#kcStart').hidden = !!kc;
    if (kc) {
      const waiting = devices.filter((d) => d.old_key).map((d) => d.name || d.id.slice(0, 8));
      $('#kcText').textContent = kc.devices_waiting ? t('Devices that do not have the new one yet still recognise the server by the old one.') : t('All devices have the new key – the change can be ended.');
      $('#kcOld').textContent = kc.old_fingerprint;
      $('#kcUntil').textContent = when(kc.old_until);
      $('#kcWaiting').textContent = waiting.length ? `${waiting.length}: ${waiting.join(', ')}` : t('no device');
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
      toast(t('New server key – the devices learn it as soon as they report in'), 5000);
      refresh();
    } catch (err) {
      $('#rotateErr').textContent = err.message;
      $('#rotateErr').hidden = false;
    }
  });
  $('#kcEnd').addEventListener('click', async () => {
    const waiting = devices.filter((d) => d.old_key).map((d) => d.name || d.id.slice(0, 8));
    const text = waiting.length
      ? (waiting.length === 1 ? t('The old key stops counting. This device does not have the new one yet and has to be enrolled again afterwards:')
        : t('The old key stops counting. These devices do not have the new one yet and have to be enrolled again afterwards:')) + '\n' + waiting.join(', ')
      : t('The old key stops counting. All devices have the new one already.');
    if (!await ask(t('End the key change?'), text, t('End'), waiting.length ? 'danger' : '')) return;
    try { server = await api('POST', '/server/rotate-key/end'); toast(t('The old key is forgotten')); } catch (err) { toast(err.message, 5000); }
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
      if (r.setup) showCode(r.name, r.setup); else toast(t('Added'));
      loadAdmin();
    } catch (err) {
      $('#userErr').textContent = err.message;
      $('#userErr').hidden = false;
    }
  });

  // ---- the language: chosen here, kept in this browser, applied by loading the page again
  $('#lang').value = I18N.lang;
  $('#lang').addEventListener('change', () => I18N.set($('#lang').value));

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
      loginError(t('The sign-in with Microsoft was not accepted: there is no enabled account here for this Microsoft account, or the answer was invalid.'));
    }
  })();
})();
