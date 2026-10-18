// Administrator's interface of the command-and-control server. Everything shown
// here comes from the admin API (/admin/v1/...), called with the session cookie
// of a signed-in account or with the admin token typed in at login. Names and
// answers of devices and the names of accounts are data: they only ever go into
// the page as text, never as HTML.
//
// The page has four places (the #-part of the address says where one is):
//   #fleet                 the devices, as cards; the side filters by location and group
//   #fleet/<id>/<tab>      one device: overview, console, what is on it, settings, activity
//   #library/<tab>         templates, firmware, images
//   #access/<tab>          groups and users (administrators)
//   #server/<tab>          status, key, audit log (administrators)
(() => {
  'use strict';
  const $ = (s) => document.querySelector(s);
  const HOTSPOT = 'http://192.168.4.1/';     // a device's own web interface, where the QR code leads
  // The page is written in English; I18N (i18n.js) has the other languages.
  const { t } = I18N;
  const STATES = { pending: t('waiting for confirmation'), active: t('connected'), revoked: t('revoked') };
  const CMD_STATES = { queued: t('queued'), sent: t('fetched'), done: t('done'), failed: t('refused'), expired: t('expired') };
  const CMD_NAMES = { ping: t('Ping'), status: t('Status'), settings: t('Settings'), configs: t('Configurations'), play: t('Play'), reboot: t('Restart'), console: t('Remote console'), update: t('Firmware update'), sd_put: t('File to the SD card'), sd_delete: t('Delete from the SD card'),
    set: t('Change settings'), config_get: t('Read configuration'), config_put: t('Write configuration'), config_delete: t('Delete configuration'),
    cert_put: t('Certificate to the device'), sd_backup: t('Backup to the SD card'), sd_restore: t('Restore from the SD card') };
  const APP = t('RS232 Control Center');

  let token = sessionStorage.getItem('c2admin') || '';   // admin token, if that is how this tab got in
  let signedIn = false, me = '', admin = false, methods = { token: false, entra: false };
  let devices = [], groups = [], users = [], selected = '', timer = 0, expTimer = 0, libAt = 0;
  let view = 'fleet', tab = '', server = null, status = null, loaded = false;
  // what the side and the bar above the cards have narrowed the fleet down to
  let place = null, groupFilter = 0, filter = 'all';
  const VIEWS = { fleet: t('Devices'), library: t('Library'), access: t('Access'), server: t('Server') };
  const TABS = { device: ['overview', 'console', 'content', 'settings', 'activity'], library: ['templates', 'firmware', 'images', 'profiles'], access: ['groups', 'users'], server: ['status', 'key', 'audit'] };
  const PANEL = { device: 'p', library: 'l', access: 'a', server: 's' };
  const RIGHTS = { use: t('use'), manage: t('manage') };
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
    // A request that does not get through at all (the connection was dropped
    // under the browser: WebKit then says no more than "Load failed") is tried
    // again - but not what a console session types, which must not arrive twice.
    let r;
    for (let i = 0; ; i++) {
      try {
        r = await fetch('/admin/v1' + path, {
          method, cache: 'no-store',
          headers: Object.assign(authHeaders(), body ? { 'Content-Type': 'application/json' } : {}),
          body: body ? JSON.stringify(body) : undefined,
        });
        break;
      } catch (e) {
        if (i >= 2 || path.includes('/console/')) throw new Error(t('The server cannot be reached'));
        await new Promise((ok) => setTimeout(ok, 500 * (i + 1)));
      }
    }
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

  // ---- where one is
  function tabs(kind, box, href) {
    for (const a of box.children) {
      if (a.dataset.tab === tab) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
      if (href) a.href = href(a.dataset.tab);
    }
    for (const name of TABS[kind]) $(`#${PANEL[kind]}-${name}`).hidden = name !== tab;
  }
  function route() {
    const [v, a, b] = location.hash.slice(1).split('/');
    const was = selected;
    view = VIEWS[v] && (admin || v === 'fleet' || v === 'library') ? v : 'fleet';
    selected = view === 'fleet' && a ? a : '';
    const kind = selected ? 'device' : view;
    const want = selected ? b : a;
    tab = TABS[kind] ? (TABS[kind].includes(want) ? want : TABS[kind][0]) : '';
    document.body.classList.toggle('one', !!selected);
    for (const name of Object.keys(VIEWS)) $('#v-' + name).hidden = name !== view || (name === 'fleet' && !!selected) || !signedIn;
    $('#v-device').hidden = !selected || !signedIn;
    for (const n of $('#nav').children) {
      if (n.dataset.view === view) n.setAttribute('aria-current', 'page'); else n.removeAttribute('aria-current');
    }
    document.title = signedIn ? `${VIEWS[view]} – ${APP}` : APP;
    if (!signedIn) return;
    if (was !== selected) {
      if (was) conDetach();
      setFor = placeFor = sdNote = '';
      sdChecked = {};
      window.scrollTo(0, 0);
    }
    if (!selected && TABS[view]) tabs(view, $(`[data-tabs=${view}]`));
    render();
    if (view === 'library') loadLibrary();
    if (view === 'access') loadAccess();
    if (view === 'server' && tab === 'audit') loadAudit();
    if (con && tab === 'console') con.fit.fit();
  }
  window.addEventListener('hashchange', route);

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
    signedIn = loaded = admin = false;
    me = '';
    sessionStorage.removeItem('c2admin');
    clearTimeout(timer);
    conDetach();
    $('#app').hidden = true;
    document.body.classList.remove('admin', 'can-manage', 'one');
    document.title = APP;
    $('#entraLogin').hidden = !methods.entra;
    if (methods.entra) $('#entraLogin').href = '/auth/entra/login';   // no address while it is off: browsers fetch links ahead
    $('#tokenBox').hidden = !methods.token;
    $('#loginLang').append($('#lang'));              // the language can be chosen before signing in, too
    $('#login').hidden = false;
    $('#loginName').focus();
  }
  function loginError(text) {
    $('#loginErr').textContent = text;
    $('#loginErr').hidden = false;
  }
  async function start() {
    try {
      const who = await api('GET', '/me');
      admin = who.admin;
      if (who.kind !== 'token') me = who.name;
      if (token) sessionStorage.setItem('c2admin', token);
      signedIn = true;
      document.body.classList.toggle('admin', admin);
      $('#whoami').textContent = token ? t('Admin token') : me;
      $('#foot').insertBefore($('#lang'), $('#logout'));
      $('#login').hidden = true;
      $('#app').hidden = false;
      $('#treeBox').open = !matchMedia('(max-width: 860px)').matches;
      selected = '';
      libAt = 0;
      await refresh();
      route();
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

  // ---- everything that changes by itself: asked for every few seconds
  async function refresh() {
    clearTimeout(timer);
    try {
      const calls = [api('GET', '/devices'), api('GET', '/groups')];
      if (admin) calls.push(api('GET', '/server'), api('GET', '/status'));
      const r = await Promise.all(calls);
      [devices, groups] = r;
      if (admin) [server, status] = r.slice(2);
      if (Date.now() - libAt > 15000) {             // the library changes rarely, and templates are large
        [templates, packages, images] = await Promise.all([api('GET', '/templates'), api('GET', '/firmware'), api('GET', '/images')]);
        libAt = Date.now();
      }
      loaded = true;
      render();
    } catch (e) { if (signedIn && e.status !== 401) toast(e.message); }
    if (signedIn) timer = setTimeout(refresh, 3000);
  }
  function render() {
    renderSide();
    renderOpen();
    if (admin) renderServer();
    if (selected) renderDevice(); else if (view === 'fleet') renderFleet();
  }

  // ---- the side: where the devices are, and which groups there are
  const where = (d) => [d.region || '', d.country || '', d.site || ''];
  const placeText = (d) => where(d).filter(Boolean).join(' › ');
  const inPlace = (d) => !place || place.every((p, i) => where(d)[i] === p);
  const inGroup = (d) => !groupFilter || (d.groups || []).includes(groupFilter);
  const groupName = (id) => (groups.find((g) => g.id === id) || {}).name || '';
  const same = (x, y) => JSON.stringify(x) === JSON.stringify(y);
  function renderSide() {
    const live = devices.filter((d) => d.state !== 'revoked');
    const tree = $('#tree');
    tree.textContent = '';
    const row = (box, cls, text, list, current, pickIt) => {
      const a = el('a', { className: cls, href: '#fleet', role: 'listitem' }, el('span', { textContent: text }),
        el('i', { textContent: `${list.filter((d) => d.online).length}/${list.length}`, title: t('online / all') }));
      if (current) a.setAttribute('aria-current', 'true');
      a.addEventListener('click', () => { pickIt(); filter = 'all'; if (location.hash === '#fleet') render(); });
      box.append(a);
    };
    const at = (path) => row(tree, path.filter((p, i) => i && (p || i < path.length - 1)).length > 1 ? 'l2' : path.length > 1 ? 'l1' : '', path[path.length - 1],
      live.filter((d) => path.every((p, i) => where(d)[i] === p)), !groupFilter && same(place, path), () => { place = path; groupFilter = 0; });
    row(tree, '', t('All devices'), live, !place && !groupFilter, () => { place = null; groupFilter = 0; });
    const uniq = (list) => [...new Set(list)].sort((x, y) => !x - !y || x.localeCompare(y));
    for (const region of uniq(live.map((d) => d.region || ''))) {
      const inR = live.filter((d) => (d.region || '') === region);
      if (region) at([region]);
      for (const country of uniq(inR.map((d) => d.country || ''))) {
        const inC = inR.filter((d) => (d.country || '') === country);
        if (country) at([region, country]);
        for (const site of uniq(inC.map((d) => d.site || ''))) if (site) at([region, country, site]);
      }
    }
    const nowhere = live.filter((d) => !placeText(d));
    if (nowhere.length && nowhere.length < live.length) row(tree, '', t('No location'), nowhere, !groupFilter && same(place, ['', '', '']), () => { place = ['', '', '']; groupFilter = 0; });
    const gn = $('#grpNav');
    gn.textContent = '';
    for (const g of groups) row(gn, '', g.name, live.filter((d) => (d.groups || []).includes(g.id)), groupFilter === g.id, () => { groupFilter = g.id; place = null; });
    $('#grpCap').hidden = !groups.length;
  }

  // ---- the fleet
  function uses(d) {
    const out = [], st = d.status || {};
    if (d.console) out.push(d.console.by && d.console.by !== 'token' ? t('port {n}: {who}', { n: d.console.port, who: d.console.by }) : t('port {n}: remote console', { n: d.console.port }));
    if (!d.online) return out;
    if (st.web) out.push(st.web === 1 ? t('1 browser at the device') : t('{n} browsers at the device', { n: st.web }));
    if (st.tcp) out.push(t('raw TCP'));
    if (st.bt) out.push(t('Bluetooth'));
    return out;
  }
  const kind = (d) => (d.state !== 'active' ? d.state : uses(d).length ? 'busy' : d.online ? 'on' : 'off');
  const WORD = { on: t('online'), busy: t('in use'), off: t('offline'), pending: t('waiting'), revoked: t('revoked') };
  const left = (s) => (s >= 172800 ? t('{n} days', { n: Math.round(s / 86400) }) : s >= 7200 ? `${Math.round(s / 3600)} h` : `${Math.max(1, Math.round(s / 60))} min`);
  function bars(rssi) {
    const n = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
    return el('span', { className: 'bars s' + n }, el('i'), el('i'), el('i'), el('i'));
  }
  // What a device carries with it, as small marks: groups, and what is unusual.
  function chips(d, box) {
    const st = d.status || {};
    for (const g of d.groups || []) if (groupName(g)) box.append(el('span', { className: 'chip', textContent: groupName(g) }));
    if (d.online && st.unatt) box.append(el('span', { className: 'chip warn', textContent: t('unattended, {left} left', { left: left(st.unatt) }) }));
    if (d.old_key) box.append(el('span', { className: 'chip warn', textContent: t('old server key') }));
    if (d.update) box.append(el('span', { className: 'chip acc', textContent: t('transfer {n} %', { n: Math.floor(d.update.sent * 100 / d.update.size) }) }));
    return box;
  }
  function renderFleet() {
    const find = $('#find').value.trim().toLowerCase();
    const scope = devices.filter((d) => inPlace(d) && inGroup(d) && (!find || [d.name, d.id, d.region, d.country, d.site].concat((d.groups || []).map(groupName)).some((x) => (x || '').toLowerCase().includes(find))));
    const count = (f) => scope.filter(f).length;
    $('#nAll').textContent = count((d) => d.state !== 'revoked');
    $('#nOnline').textContent = count((d) => d.state === 'active' && d.online);
    $('#nBusy').textContent = count((d) => kind(d) === 'busy');
    $('#nOffline').textContent = count((d) => kind(d) === 'off');
    $('#nPending').textContent = count((d) => d.state === 'pending');
    for (const b of $('#filters').children) b.setAttribute('aria-pressed', String(b.dataset.f === filter));
    const pass = { all: () => true, online: (d) => d.state === 'active' && d.online, busy: (d) => kind(d) === 'busy', offline: (d) => kind(d) === 'off', pending: (d) => d.state === 'pending' }[filter];
    const order = { pending: 0, busy: 1, on: 1, off: 2, revoked: 3 };
    const shown = scope.filter(pass).sort((x, y) => order[kind(x)] - order[kind(y)] || (x.name || x.id).localeCompare(y.name || y.id));
    $('#crumb').textContent = groupFilter ? t('Group {name}', { name: groupName(groupFilter) }) : place ? place.filter(Boolean).join(' › ') || t('No location') : '';
    const list = $('#cards');
    const focus = document.activeElement && document.activeElement.dataset && document.activeElement.dataset.id;
    list.textContent = '';
    for (const d of shown) {
      const st = d.status || {}, k = kind(d), use = uses(d);
      const a = el('a', { className: 'dev ' + k, href: `#fleet/${d.id}`, role: 'listitem' },
        el('div', { className: 'top' }, el('span', { className: 'dot ' + k }), el('b', { textContent: d.name || d.id.slice(0, 8) }), el('span', { className: 'pill ' + k, textContent: WORD[k] || k })));
      a.dataset.id = d.id;
      if (placeText(d)) a.append(el('div', { className: 'where', textContent: placeText(d) }));
      a.append(chips(d, el('div', { className: 'chips' })));
      const facts = el('div', { className: 'facts' });
      if (st.bat_pct != null) facts.append(el('span', { textContent: t('battery {n} %', { n: st.bat_pct }) }));
      if (st.rssi != null && d.online) facts.append(el('span', {}, bars(st.rssi), `${st.rssi} dBm`));
      const fw = st.fw || (d.info && d.info.fw);
      if (fw) facts.append(el('span', { textContent: t('firmware {v}', { v: fw }) }));
      if (st.cfgs) facts.append(el('span', { textContent: st.cfgs.length === 1 ? t('1 configuration') : t('{n} configurations', { n: st.cfgs.length }) }));
      a.append(facts);
      const foot = d.state === 'pending' ? t('Type in the code from its display') : d.state === 'revoked' ? '' : use.length ? use.join(', ')
        : d.online ? t('ports free') : d.last_seen ? t('last seen {ago}', { ago: ago(d.last_seen) }) : t('has not reported in yet');
      a.append(el('div', { className: 'use' + (use.length ? ' busy' : ''), textContent: foot }));
      list.append(a);
      if (d.id === focus) a.focus({ preventScroll: true });     // the list is rebuilt under the keyboard's feet
    }
    $('#noDevices').hidden = !!devices.length || !loaded;
    $('#noMatch').hidden = !devices.length || !!shown.length;
  }
  $('#find').addEventListener('input', renderFleet);
  $('#filters').addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (b) { filter = b.dataset.f; renderFleet(); }
  });

  // ---- one device
  function pretty(v) {
    return typeof v === 'string' ? t(v) : JSON.stringify(v, null, 1);   // a refusal comes in English: 'refused at the device'
  }
  let placeFor = '', histFor = '';
  const tile = (id, value, sub, state) => {
    $(id).textContent = '';
    $(id).append(value);
    $(id + 'Sub').textContent = sub || '';
    $(id).parentNode.className = state || '';
  };
  async function renderDevice() {
    const d = devices.find((x) => x.id === selected);
    if (!d) { if (loaded) { conDetach(); location.hash = '#fleet'; } return; }     // it is gone, or not ours to see
    const st = d.status || {}, k = kind(d), live = d.state === 'active', use = uses(d);
    document.body.classList.toggle('can-manage', d.right === 'manage');
    $('#dName').textContent = d.name || d.id;
    $('#dPlace').textContent = placeText(d);
    $('#dState').className = 'pill ' + k;
    $('#dState').textContent = d.state === 'pending' ? STATES.pending : WORD[k] || k;
    $('#dChips').textContent = '';
    chips(d, $('#dChips'));
    $('#dOldKey').hidden = !d.old_key || !admin;
    $('#approveForm').hidden = d.state !== 'pending' || !admin;
    $('#dTabs').hidden = !live;
    if (!live) tab = 'activity';
    if (tab === 'settings' && d.right !== 'manage') tab = 'overview';
    tabs('device', $('#dTabs'), (name) => `#fleet/${d.id}/${name}`);

    // overview: the state of things, in a few words
    tile('#oConn', d.online ? t('online') : t('offline'), d.last_seen ? t('reported in {ago}', { ago: ago(d.last_seen) }) : t('has not reported in yet'), d.online ? 'good' : 'attn');
    tile('#oUse', use.length ? t('in use') : d.online ? t('free') : '–', use.join(', '), use.length ? 'attn' : '');
    tile('#oUnatt', st.unatt ? t('on') : t('off'), st.unatt ? t('{left} left: a console opens without anybody at the device agreeing', { left: left(st.unatt) })
      : t('Somebody at the device has to allow every console. Granted only at the device.'), st.unatt ? 'attn' : '');
    tile('#oCtl', st.ctl == null ? '–' : st.ctl ? t('allowed') : t('read only'), st.ctl ? t('Commands, settings and templates are taken.') : t('Allowed only at the device itself.'));
    tile('#oBat', st.bat_pct != null ? `${st.bat_pct} %` : '–', st.bat_mv ? `${(st.bat_mv / 1000).toFixed(2)} V` : '');
    tile('#oSig', st.rssi != null ? el('span', {}, bars(st.rssi), ` ${st.rssi} dBm`) : '–', st.rssi == null ? '' : st.rssi >= -67 ? t('good') : st.rssi >= -78 ? t('fair') : t('weak – large transfers take long'));
    tile('#oFw', st.fw || (d.info && d.info.fw) || '–', [LANGS[st.lang] || st.lang, st.hw || (d.info && d.info.hw)].filter(Boolean).join(' · '));
    const sd = st.sd || {};
    const sdText = sd.stick ? t('SD card lent to the USB port as a memory stick') : sd.on ? t('SD card: {used} of {total} MB used', { used: sd.used, total: sd.mb }) : st.sd ? t('no SD card') : '';
    tile('#oStore', st.cfgs ? (st.cfgs.length === 1 ? t('1 configuration') : t('{n} configurations', { n: st.cfgs.length })) : '–', sdText);
    $('#oStore').className = 'text';
    $('#oConsole').href = `#fleet/${d.id}/console`;

    // console
    $('#conHint').textContent = st.unatt ? t('Unattended access is on at this device: the console opens without anybody there agreeing.') : t('Somebody at the device has to tap “Allow” on its display.');
    renderConsole(d);

    // what is on it
    const cb = $('#cfgs').tBodies[0], cfgs = st.cfgs || [], can = d.right === 'manage';
    cb.textContent = '';
    for (const c of cfgs) {
      const tpl = templates.find((x) => x.name === c.n);
      const mark = el('td');
      if (tpl) mark.append(el('span', { className: 'chip ' + (new Blob([tpl.text]).size === c.s ? 'ok' : 'warn'), textContent: new Blob([tpl.text]).size === c.s ? t('as in the library') : t('differs from the library') }));
      const acts = el('td', { className: 'acts' });
      if (can) {
        const b = (text, cls, fn) => { const x = el('button', { className: 'btn small ' + cls, type: 'button', textContent: text }); x.addEventListener('click', fn); acts.append(x); };
        b(t('View'), '', () => viewConfig(d, c.n));
        b(t('Play'), 'warn', () => playConfig(d, c.n));
        b(t('Delete'), 'danger', async () => {
          if (await ask(t('Delete “{name}” from the device?', { name: c.n }), t('The library keeps its copy.'), t('Delete'))) send('config_delete', { name: c.n });
        });
      }
      cb.append(el('tr', {}, el('td', { textContent: c.n }), el('td', { textContent: c.s < 1024 ? `${c.s} B` : bytes(c.s) }), mark, acts));
    }
    label($('#cfgs'));
    $('#cfgs').hidden = !cfgs.length;
    $('#noCfgs').hidden = !!cfgs.length;
    $('#noCfgs').textContent = st.cfgs ? t('Nothing is stored on this device.') : t('This device does not say what is stored on it (older firmware).');
    $('#cfgAge').textContent = st.cfgs && d.last_seen ? t('as reported {ago}', { ago: ago(d.last_seen) }) : '';
    $('#ctlHint').hidden = st.ctl !== false || !can;
    $('#sdText').textContent = sdText || '–';
    const sb = $('#sdFiles').tBodies[0], files = sd.files || [];
    sb.textContent = '';
    for (const f of files) {
      const acts = el('td', { className: 'acts' });
      if (can) {
        const del = el('button', { className: 'btn small danger', type: 'button', textContent: t('Delete') });
        del.addEventListener('click', async () => {
          if (await ask(t('Delete “{name}” from the SD card?', { name: f.n }), t('The file is gone from the card for good.'), t('Delete'))) send('sd_delete', { name: f.n });
        });
        acts.append(del);
      }
      // The green mark: this device has read the file back from its card and
      // found the MD5 sum of the image the library holds under that name.
      const lib = images.images.find((x) => x.name === f.n && x.size === f.s);
      const mark = el('td');
      if (lib && sdChecked[f.n] === lib.md5) mark.append(el('span', { className: 'chip ok', textContent: '✓ ' + t('MD5 ok'), title: lib.md5 }));
      sb.append(el('tr', {}, el('td', { textContent: f.n }), el('td', { textContent: f.s < 1024 ? `${f.s} B` : bytes(f.s) }), mark, acts));
    }
    label($('#sdFiles'));
    $('#sdFiles').hidden = !files.length;
    if (document.activeElement !== $('#sdPick')) {
      const was = $('#sdPick').value;
      $('#sdPick').textContent = '';
      for (const im of images.images) $('#sdPick').append(el('option', { value: im.id, textContent: `${im.folder ? im.folder + ' / ' : ''}${im.name} (${bytes(im.size)})` }));
      $('#sdPick').value = was;
      if ($('#sdPick').selectedIndex < 0) $('#sdPick').selectedIndex = 0;
    }
    $('#sdForm').hidden = !sd.on || !images.images.length;
    $('#sdNone').hidden = !sd.on || !!images.images.length || !can;
    $('#sdState').textContent = sdCmd && d.update && d.update.cmd === sdCmd ? t('On its way: {n} %', { n: Math.floor(d.update.sent * 100 / d.update.size) }) : sdNote;
    if (document.activeElement !== $('#tplPick')) {
      const was = $('#tplPick').value;
      $('#tplPick').textContent = '';
      for (const tpl of templates.slice().sort(byFolder)) $('#tplPick').append(el('option', { value: tpl.id, textContent: (tpl.folder ? tpl.folder + ' / ' : '') + tpl.name }));
      $('#tplPick').value = was;
      if ($('#tplPick').selectedIndex < 0) $('#tplPick').selectedIndex = 0;
    }
    $('#tplSendForm').hidden = !templates.length;
    renderFirmware(d);

    // settings: where it belongs. Filled once per device, so typing is not undone.
    if (placeFor !== d.id) {
      placeFor = d.id;
      for (const [id, key] of [['#dRegion', 'region'], ['#dCountry', 'country'], ['#dSite', 'site']]) {
        $(id).value = d[key] || '';
        $(id).list.textContent = '';
        for (const v of [...new Set(devices.map((x) => x[key]).filter(Boolean))].sort()) $(id).list.append(el('option', { value: v }));
      }
      $('#dGroups').textContent = '';
      for (const g of groups) $('#dGroups').append(el('label', { className: 'check' }, el('input', { type: 'checkbox', value: g.id, checked: (d.groups || []).includes(g.id) }), ' ' + g.name));
      $('#dNoGroups').hidden = !!groups.length;
    }
    if (setFor !== d.id) { setFor = d.id; setLoaded = null; $('#setForm').hidden = true; $('#setState').textContent = $('#bkState').textContent = $('#fwState').textContent = ''; }
    $('#revoke').hidden = d.state === 'revoked';

    // activity: fetched only when somebody looks at it
    $('#dId').textContent = d.id;
    $('#dFp').textContent = d.fingerprint;
    $('#dEnrolled').textContent = when(d.enrolled);
    $('#dSeen').textContent = d.last_seen ? `${when(d.last_seen)} (${ago(d.last_seen)})` : '–';
    $('#dInfo').textContent = d.info ? Object.entries(d.info).map(([key, v]) => `${key}: ${v}`).join(' · ') : '–';
    $('#dStatus').textContent = d.status ? Object.entries(d.status).map(([key, v]) => `${key}: ${typeof v === 'object' ? JSON.stringify(v) : v}`).join(' · ') : '–';
    if (tab !== 'activity' && tab !== 'content') return;
    let cmds = [];
    try { cmds = await api('GET', `/devices/${d.id}/commands`); } catch (e) { return; }
    if (selected !== d.id) return;                  // another device was picked meanwhile
    // which files this device has checked on its card: its own answers say so
    sdChecked = {};
    const bk = cmds.filter((c) => c.type === 'sd_backup').pop();
    $('#bkText').textContent = !bk ? t('No backup yet.') : bk.state === 'done' ? t('Last backup {ago}: {what}', { ago: ago(bk.done), what: t(String(bk.result)) })
      : bk.state === 'failed' ? t('The last backup failed: {why}', { why: t(String(bk.result)) }) : t('A backup is on its way to the device.');
    for (const c of cmds) {
      const m = c.type === 'sd_put' && c.state === 'done' && typeof c.result === 'string' && /^stored (.+) \(\d+ bytes, MD5 ([0-9a-f]{32}) checked on the card\)$/.exec(c.result);
      if (m) sdChecked[m[1]] = m[2];
      if (c.type === 'sd_delete' && c.state === 'done' && c.args) delete sdChecked[c.args.name];
    }
    if (tab !== 'activity') return;
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
    $('#history').hidden = !cmds.length;
    $('#noHistory').hidden = !!cmds.length;
  }

  $('#placeSave').addEventListener('click', async () => {
    const dev = selected;
    try {
      await api('POST', `/devices/${dev}/place`, { region: $('#dRegion').value, country: $('#dCountry').value, site: $('#dSite').value });
      await api('POST', `/devices/${dev}/groups`, { groups: [...$('#dGroups').querySelectorAll('input:checked')].map((x) => +x.value) });
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
  // An image from the library to the device's SD card. It travels through the
  // device's own channel in small pieces: fine for megabytes, hours for more.
  let sdCmd = 0, sdNote = '', sdChecked = {};
  $('#sdForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const im = images.images.find((x) => x.id === $('#sdPick').value), dev = selected;
    if (!im) return;
    const min = Math.max(1, Math.round(im.size / 30000 / 60));
    if (!await ask(t('Send “{name}” to the SD card?', { name: im.name }), t('{size} through the device\'s own connection: about {min} min on a good link, much longer on a weak one. The device does nothing else meanwhile. A file of the same name is replaced. If it is interrupted, it continues where it stopped; at the end the device reads the file back from the card and compares its MD5 sum.', { size: bytes(im.size), min }), t('Send'), 'warn')) return;
    $('#sdForm').querySelector('button').disabled = true;
    sdNote = t('Waiting for the device …');
    try {
      const c = await api('POST', `/devices/${dev}/commands`, { type: 'sd_put', args: { img: im.id } });
      sdCmd = c.id;
      let done = null;
      while (!done) {                               // as long as it takes: the page shows how far it is
        await new Promise((r) => setTimeout(r, 2000));
        const now = (await api('GET', `/devices/${dev}/commands`)).find((x) => x.id === c.id);
        if (!now) throw new Error(t('The device did not answer'));
        if (now.state === 'failed' || now.state === 'expired') throw new Error(typeof now.result === 'string' ? t(now.result) : CMD_STATES[now.state]);
        if (now.state === 'done') done = now;
      }
      sdCmd = 0;
      sdNote = '✓ ' + t('On the SD card: {name}', { name: im.name }) + ' – ' + t('MD5 ok');
      toast(t('On the SD card: {name}', { name: im.name }));
    } catch (err) { sdCmd = 0; sdNote = err.message; toast(err.message, 6000); }
    $('#sdForm').querySelector('button').disabled = false;
    refresh();
  });
  // A stored configuration: read it from the device, play it to a port.
  let cfgShown = null;
  async function viewConfig(d, name) {
    toast(t('Waiting for the device …'), 30000);
    try {
      cfgShown = await run(d.id, 'config_get', { name });
      $('#toast').hidden = true;
      $('#cfgTitle').textContent = cfgShown.name;
      $('#cfgText').textContent = cfgShown.text;
      $('#cfgDlg').showModal();
    } catch (e) { toast(e.message, 6000); }
  }
  $('#cfgClose').addEventListener('click', () => $('#cfgDlg').close());
  $('#cfgKeep').addEventListener('click', async () => {
    try {
      const have = templates.find((x) => x.name === cfgShown.name);
      await api(have ? 'PUT' : 'POST', have ? `/templates/${have.id}` : '/templates', { name: cfgShown.name, folder: have ? have.folder || '' : '', text: cfgShown.text });
      libAt = 0;
      toast(t('Kept as a template'));
      $('#cfgDlg').close();
    } catch (e) { toast(e.message, 6000); }
  });
  async function playConfig(d, name) {
    const port = await pick(t('Play “{name}”', { name }), t('Port'), t('Play'), [1, 2, 3, 4].map((n) => [String(n), t('Port {n}', { n })]));
    if (!port) return;
    if (!await ask(t('Play “{name}” on port {port}?', { name, port }), t('The lines go to the console of whatever is connected there.'), t('Play'))) return;
    send('play', { name, port: +port });
  }

  // ---- the library: templates, firmware and images, kept here for every device
  let templates = [], images = { images: [] }, imgBusy = false;
  const bytes = (n) => (n >= 1073741824 ? (n / 1073741824).toFixed(2) + ' GB' : n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' kB');
  const byFolder = (x, y) => (x.folder || '').localeCompare(y.folder || '');
  // One row per folder in front of what is in it.
  // A folder is nothing but a name on what is in it: renaming it renames that
  // name everywhere, removing it leaves its contents without a folder. move()
  // puts one item into a folder.
  function folderRows(body, items, cols, row, move) {
    let folder = null;
    for (const it of items.slice().sort(byFolder)) {
      if ((it.folder || '') !== folder && items.some((x) => x.folder)) {
        folder = it.folder || '';
        const cell = el('td', { colSpan: cols }, el('span', { textContent: folder || t('No folder') }));
        if (folder && admin && move) {
          const name = folder, inside = items.filter((x) => x.folder === name);
          const all = async (to) => {
            try { for (const x of inside) await move(x, to); } catch (e) { toast(e.message, 5000); }
            loadLibrary();
          };
          const ren = el('button', { className: 'btn small', type: 'button', textContent: t('Rename') });
          const del = el('button', { className: 'btn small danger', type: 'button', textContent: t('Remove folder') });
          ren.addEventListener('click', async () => {
            const to = await pick(t('Rename the folder “{name}”', { name }), t('Folder'), t('Save'), [...new Set(items.map((x) => x.folder).filter((f) => f && f !== name))], name);
            if (to && to !== name) all(to);
          });
          del.addEventListener('click', async () => {
            if (await ask(t('Remove the folder “{name}”?', { name }), t('What is in it stays, without a folder.'), t('Remove folder'))) all('');
          });
          cell.append(el('span', { className: 'acts' }, ren, del));
        }
        body.append(el('tr', { className: 'grp' }, cell));
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
    tabs('library', $('[data-tabs=library]'));
    if (admin) loadProfiles();
    try { [templates, packages, images] = await Promise.all([api('GET', '/templates'), api('GET', '/firmware'), api('GET', '/images')]); } catch (e) { return; }
    libAt = Date.now();
    const mine = devices.filter((d) => d.state === 'active' && d.right === 'manage').map((d) => [d.id, (d.name || d.id.slice(0, 8)) + (placeText(d) ? ' – ' + placeText(d) : '')]);
    const on = (list) => (list.length ? el('span', { className: 'chip', textContent: list.length === 1 ? list[0].name || list[0].id.slice(0, 8) : t('{n} devices', { n: list.length }), title: list.map((d) => d.name || d.id.slice(0, 8)).join(', ') }) : '');
    const act = (text, cls, fn, off) => { const b = el('button', { className: 'btn small ' + cls, type: 'button', textContent: text, disabled: !!off }); b.addEventListener('click', fn); return b; };
    const tb = $('#tpls').tBodies[0];
    tb.textContent = '';
    folderRows(tb, templates, 5, (tpl) => {
      const acts = el('td', { className: 'acts' }, act(t('Send'), '', async () => {
        const dev = await pick(t('Send “{name}”', { name: tpl.name }), t('Device'), t('Send'), mine);
        if (!dev) return;
        try { await api('POST', `/templates/${tpl.id}/send`, { device: dev }); toast(t('{cmd}: queued – the device fetches the command the next time it reports in', { cmd: tpl.name })); } catch (e) { toast(e.message, 5000); }
      }, !mine.length));
      if (admin) acts.append(act(t('Edit'), '', () => editTemplate(tpl)), act(t('Delete'), 'danger', async () => {
        if (!await ask(t('Delete this template?'), tpl.name, t('Delete'))) return;
        try { await api('DELETE', `/templates/${tpl.id}`); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      }));
      return el('tr', {}, el('td', { textContent: tpl.name }), el('td', {}, on(devices.filter((d) => ((d.status || {}).cfgs || []).some((c) => c.n === tpl.name)))),
        el('td', { textContent: ago(tpl.updated) }), el('td', { textContent: tpl.by && tpl.by !== 'token' ? tpl.by : '' }), acts);
    }, (tpl, folder) => api('PUT', `/templates/${tpl.id}`, { name: tpl.name, folder, text: tpl.text }));
    label($('#tpls'));
    $('#tpls').hidden = !templates.length;
    $('#noTpls').hidden = !!templates.length;

    const fb = $('#libFw').tBodies[0];
    fb.textContent = '';
    folderRows(fb, packages, 7, (p) => {
      const acts = el('td', { className: 'acts' });
      if (admin) acts.append(act(t('Folder'), '', async () => {
        const folder = await pick(t('Folder'), t('Folder'), t('Save'), [...new Set(packages.map((x) => x.folder).filter(Boolean))], p.folder || '');
        if (folder === null) return;
        try { await api('POST', `/firmware/${p.id}/folder`, { folder }); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      }), act(t('Delete'), 'danger', async () => {
        if (!await ask(t('Delete this package?'), t('Version {v} ({lang}), signed {when}. Devices that have it installed keep it.', { v: p.fw, lang: LANGS[p.lang] || p.lang, when: when(new Date(p.seq * 1000).toISOString()) }), t('Delete'))) return;
        try { await api('DELETE', `/firmware/${p.id}`); } catch (e) { toast(e.message, 5000); }
        loadLibrary();
      }));
      return el('tr', {}, el('td', { textContent: p.fw }), el('td', { textContent: LANGS[p.lang] || p.lang }), el('td', { textContent: p.board }),
        el('td', { textContent: when(new Date(p.seq * 1000).toISOString()) }), el('td', { textContent: (p.size / 1048576).toFixed(1) + ' MB' }),
        el('td', {}, on(devices.filter((d) => (d.status || {}).fw === p.fw && (d.status || {}).lang === p.lang))), acts);
    }, (p, folder) => api('POST', `/firmware/${p.id}/folder`, { folder }));
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
      const sum = el('button', { className: 'btn small mono', type: 'button', textContent: im.md5, title: 'SHA-256 ' + im.sha256 });
      const proof = im.checked ? el('span', { className: 'chip ok', textContent: '✓ ' + t('MD5 ok'), title: t('The file had the MD5 sum given with the upload.') }) : '';
      // The whole checksum is what one compares with the maker's; the cell shows its beginning.
      sum.addEventListener('click', async () => {
        try { await navigator.clipboard.writeText(im.md5); toast(t('MD5 sum copied')); } catch (e) { toast(im.md5, 15000); }
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
      return el('tr', {}, el('td', { textContent: im.name }), el('td', { textContent: bytes(im.size) }), el('td', {}, sum, ' ', proof),
        el('td', { textContent: ago(im.added) }), el('td', { textContent: im.by && im.by !== 'token' ? im.by : '' }), el('td', { className: 'acts' }, ...(admin ? [get, name, move, del] : [get])));
    }, (im, folder) => api('PUT', `/images/${im.id}`, { name: im.name, folder }));
    label($('#imgs'));
    $('#imgs').hidden = !list.length;
    $('#noImgs').hidden = !!list.length;
    if (!imgBusy) $('#imgState').textContent = t('{used} of {total} used, up to {max} per file', { used: bytes(images.used), total: bytes(images.total), max: bytes(images.max) });
  }
  // An image is large: it goes up as it is, and the bar of the browser is not
  // enough to know how far it has got.
  $('#imgAdd').addEventListener('click', () => { $('#imgFile').value = ''; $('#imgFile').click(); });
  $('#imgFile').addEventListener('change', async () => {
    const file = $('#imgFile').files[0];
    if (!file || imgBusy) return;
    if (file.size > images.max) { toast(t('the file is larger than this server takes'), 6000); return; }
    // The maker's MD5 sum, if there is one: the server then takes the file
    // only if it has it. Without one the server works it out and shows it.
    const want = await pick(t('Upload “{name}”', { name: file.name }), t('MD5 sum from the maker (leave empty to have it worked out)'), t('Upload'), [], '');
    if (want === null) return;
    const md5 = want.replace(/[^0-9a-f]/gi, '').toLowerCase();
    if (want.trim() && md5.length !== 32) { toast(t('an MD5 sum has 32 characters from 0-9 and a-f'), 6000); return; }
    const done = (text, bad) => { imgBusy = false; $('#imgAdd').disabled = false; if (bad) toast(text, 6000); loadLibrary().then(() => { if (bad) $('#imgState').textContent = text; }); };
    const x = new XMLHttpRequest();
    x.open('POST', '/admin/v1/images?name=' + encodeURIComponent(file.name) + (md5 ? '&md5=' + md5 : ''));
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
  $('#v-device').addEventListener('click', async (e) => {
    const type = e.target.dataset && e.target.dataset.cmd;
    if (!type) return;
    if (type === 'reboot' && !await ask(t('Restart the device?'), t('Console sessions that are running break off.'), t('Restart'))) return;
    send(type);
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
    // Only what fits this hardware (a device that does not say what it is
    // sees everything and decides alone). Of those, the one to take: its
    // language, signed later than what it runs, the newest.
    const hw = st.hw || (d.info && d.info.hw) || '';
    const fits = packages.filter((p) => !hw || p.board === hw).sort((a, b) => b.seq - a.seq);
    const best = hw && fits.find((p) => p.lang === st.lang && p.seq > (st.seq || 0));
    $('#fwOther').hidden = fits.length === packages.length;
    $('#fwOther').textContent = t('{n} packages for other hardware are not shown. This device is “{hw}”.', { n: packages.length - fits.length, hw });
    for (const p of fits) {
      const install = el('button', { className: 'btn small ' + (p === best ? 'primary' : 'warn'), type: 'button', textContent: t('Install'), disabled: fwBusy });
      install.addEventListener('click', () => installFirmware(d, p));
      const ver = el('td', { textContent: p.fw + ' ' });
      if (p === best) ver.append(el('span', { className: 'chip ok', textContent: t('fits this device') }));
      else if (hw && p.seq <= (st.seq || 0)) ver.append(el('span', { className: 'chip', textContent: p.seq === st.seq ? t('installed') : t('older') }));
      body.append(el('tr', {}, ver, el('td', { textContent: LANGS[p.lang] || p.lang }),
        el('td', { textContent: when(new Date(p.seq * 1000).toISOString()) }), el('td', { textContent: (p.size / 1048576).toFixed(1) + ' MB' }),
        el('td', { className: 'acts' }, install)));
    }
    label($('#fwList'));
    $('#fwList').hidden = !fits.length;
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
    libAt = 0;
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

  // End to end (firmware src/c2_e2e.h): the browser makes a one-time key for
  // each request it sends; the device answers with its own when the console
  // opens. Both derive the same keys - the server carries only ciphertext - and
  // the same six digits, shown here and on the device's display.
  const conKeys = {};                               // command id -> our key pair, until the console opens
  const SUBTLE = window.crypto && window.crypto.subtle;
  async function e2eSession(kp, devKey64) {
    const bpub = new Uint8Array(await SUBTLE.exportKey('raw', kp.publicKey)), dpub = unb64(devKey64);
    const dk = await SUBTLE.importKey('raw', dpub, { name: 'ECDH', namedCurve: 'P-256' }, false, []);
    const z = await SUBTLE.deriveBits({ name: 'ECDH', public: dk }, kp.privateKey, 256);
    const salt = new Uint8Array(130);
    salt.set(bpub, 0);
    salt.set(dpub, 65);
    const ikm = await SUBTLE.importKey('raw', z, 'HKDF', false, ['deriveBits']);
    const okm = new Uint8Array(await SUBTLE.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt, info: new TextEncoder().encode('rs232-console-e2e-v1') }, ikm, 68 * 8));
    const code = String(new DataView(okm.buffer, 64, 4).getUint32(0) % 1000000).padStart(6, '0');
    return {
      kOut: await SUBTLE.importKey('raw', okm.slice(0, 32), 'AES-GCM', false, ['encrypt']),
      kIn: await SUBTLE.importKey('raw', okm.slice(32, 64), 'AES-GCM', false, ['decrypt']),
      ctrOut: 0, ctrIn: 0, buf: new Uint8Array(0), broken: false, code: code.slice(0, 3) + ' ' + code.slice(3),
    };
  }
  const e2eIv = (n) => { const v = new Uint8Array(12); new DataView(v.buffer).setBigUint64(4, BigInt(n)); return v; };
  async function e2eSeal(s, bytes) {                // one frame: length | AES-GCM
    const ct = new Uint8Array(await SUBTLE.encrypt({ name: 'AES-GCM', iv: e2eIv(s.ctrOut++) }, s.kOut, bytes));
    const f = new Uint8Array(2 + ct.length);
    f[0] = ct.length >> 8;
    f[1] = ct.length & 255;
    f.set(ct, 2);
    return f;
  }
  async function e2eOpen(s, bytes) {                // what the frames that are complete say
    const all = new Uint8Array(s.buf.length + bytes.length);
    all.set(s.buf);
    all.set(bytes, s.buf.length);
    const out = [];
    let at = 0;
    while (all.length - at >= 2) {
      const len = (all[at] << 8) | all[at + 1];
      if (len < 16 || len > 528) { s.broken = true; break; }
      if (all.length - at < 2 + len) break;
      try {
        out.push(new Uint8Array(await SUBTLE.decrypt({ name: 'AES-GCM', iv: e2eIv(s.ctrIn) }, s.kIn, all.subarray(at + 2, at + 2 + len))));
      } catch (e) { s.broken = true; break; }
      s.ctrIn++;
      at += 2 + len;
    }
    s.buf = all.slice(at);
    return out;
  }
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
        // encrypted once and given its place in the stream once: a frame must
        // not be made twice, and the server takes a request sent again only once
        if (item.d && item.at == null) {
          if (c.e2e) item.d = b64(await e2eSeal(c.e2e, unb64(item.d)));
          item.at = c.inAt;
        }
        await api('POST', `/devices/${c.dev}/console/${c.id}/input`, item.d ? { d: item.d, at: item.at } : item);
        if (item.d) c.inAt += unb64(item.d).length;
        c.queue.shift();
      } catch (e) {
        if (e.status === 429) { await new Promise((r) => setTimeout(r, 300)); continue; }   // the device is slower than the typing
        // the connection dropped or the server restarts: the same request again, it is taken once
        if (item.d && (!e.status || e.status >= 500) && (item.tries = (item.tries || 0) + 1) <= 30) { await new Promise((r) => setTimeout(r, 1000)); continue; }
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
    if (item.d && last && last.d && last.at == null && last !== con.queue[0] && last.d.length + item.d.length < 680) last.d = b64([...unb64(last.d), ...unb64(item.d)]);
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
      if (o.lost) {
        c.term.write(`\r\n\x1b[90m[${t('{n} characters are missing', { n: o.lost })}]\x1b[0m\r\n`);
        if (c.e2e) c.e2e.broken = true;           // the frames do not line up any more
      }
      if (o.d && c.e2e && !c.e2e.broken) for (const part of await e2eOpen(c.e2e, unb64(o.d))) c.term.write(part);
      else if (o.d && !c.e2e) c.term.write(unb64(o.d));
      if (c.e2e && c.e2e.broken && !c.e2eGone) {
        c.e2eGone = true;
        c.term.write(`\r\n\x1b[31m[${t('The encrypted stream cannot be read any more – end the session and open a new one.')}]\x1b[0m\r\n`);
      }
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
  function conAttach(dev, id, left, devKey) {
    conDetach();
    $('#conBox').hidden = false;
    const term = new Terminal({ cursorBlink: true, scrollback: 5000, fontSize: 14, convertEol: false, rightClickSelectsWord: false });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open($('#term'));
    fit.fit();
    const c = con = { dev, id, term, fit, next: 0, abort: new AbortController(), ended: false, queue: [], sending: false, inAt: 0, left: left || 0, leftAt: Date.now() };
    const e2e = $('#conE2E');
    e2e.hidden = false;
    e2e.className = 'chip';
    e2e.textContent = t('not end to end');
    e2e.title = t('The server can read this session (no key from this browser, or older firmware on the device).');
    if (conKeys[id] && devKey) {
      c.ready = e2eSession(conKeys[id], devKey).then((s) => {
        c.e2e = s;
        delete conKeys[id];
        e2e.className = 'chip ok';
        e2e.textContent = '🔒 ' + t('end to end · code {code}', { code: s.code });
        e2e.title = t('Encrypted between this browser and the device. The device shows the same code - if it does, nobody sits in between, not even the server.');
      });
    } else c.ready = Promise.resolve();
    term.onData((data) => {                         // a pasted text arrives in one piece: hand it on in small ones
      const bytes = new TextEncoder().encode(data);
      for (let i = 0; i < bytes.length; i += 512) conInput({ d: b64(bytes.subarray(i, i + 512)) });
    });
    // as in PuTTY: what is selected is on the clipboard at once
    term.onSelectionChange(() => {
      const text = term.getSelection();
      if (text && navigator.clipboard) navigator.clipboard.writeText(text).catch(() => {});
    });
    term.focus();
    c.ready.then(() => conRead(c));
  }
  function renderConsole(d) {
    if (con && con.dev !== d.id) conDetach();
    api('GET', `/devices/${d.id}/console`).then((v) => {
      if (selected !== d.id) return;
      const cur = v.console, st = $('#conState');
      let text = '', cls = '';
      // why a session ended comes from the server in English, or from the device in the device's language
      if (cur && cur.state === 'requested') { text = (d.status || {}).unatt ? t('Requested (port {port}) – the device opens it the next time it reports in.', { port: cur.port }) : t('Requested (port {port}) – somebody at the device now has to tap “Allow” on its display.', { port: cur.port }); cls = 'wait'; }
      else if (cur && cur.state === 'granted') { text = t('Allowed at the device – the connection is coming up.'); cls = 'wait'; }
      else if (cur && cur.state === 'open') { text = t('Open on port {port}.', { port: cur.port }); cls = 'open'; }
      else if (cur && cur.state === 'refused') text = t('Did not come about: {why}', { why: t(cur.reason) });
      else if (cur && cur.state === 'ended') text = t('Last session ended: {why}', { why: t(cur.reason) });
      st.textContent = text;
      st.className = 'dim ' + cls;
      const busy = cur && ['requested', 'granted', 'open'].includes(cur.state);
      $('#conForm').hidden = !!busy || d.state !== 'active';
      // with our key for this request, wait until the device's key is there: a session opened without would stay unencrypted here
      // end to end with another browser (or with this page before it was loaded again): without
      // its key nothing here can be read, and what is typed would reach the device unsealed and end the session
      const elsewhere = cur && cur.state === 'open' && cur.e2e && !conKeys[cur.id] && !(con && con.id === cur.id);
      if (elsewhere) { st.textContent = t('Open on port {port}, end to end with another browser: only that one can read and type.', { port: cur.port }); st.className = 'dim wait'; }
      $('#conEndOther').hidden = !elsewhere;
      if (cur && cur.state === 'open' && !elsewhere && !(con && con.id === cur.id) && tab === 'console' && (!conKeys[cur.id] || cur.e2e || Date.now() - (conKeys[cur.id].openSeen || (conKeys[cur.id].openSeen = Date.now())) > 8000)) conAttach(d.id, cur.id, cur.left_s, cur.e2e);
      if (con) {
        const left = Math.max(0, con.left - Math.round((Date.now() - con.leftAt) / 1000));
        $('#conInfo').textContent = con.ended ? t('ended') : t('Port {port} · {left} min left', { port: cur ? cur.port : '', left: mmss(left) });
        $('#conBreak').hidden = $('#conClose').hidden = con.ended;
      }
      conScripts(d, cur && cur.state === 'open' ? cur.port : 0);
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
      $('#noConLog').hidden = !!v.sessions.length;
    }).catch(() => {});
  }
  // Scripts under the terminal: played by the device on the console's port
  // (Player in the firmware: line by line, waiting for the prompt), so what
  // they do shows up in the terminal while it happens. The device's own
  // configurations, and library templates that are not on it as they are -
  // those are written to it first. Asked twice: it cannot be taken back.
  let scriptsKey = '', scriptCmd = 0;
  function conScripts(d, port) {
    const box = $('#conScripts'), st = d.status || {};
    box.hidden = !con || con.ended || !port || d.right !== 'manage';
    if (box.hidden) { scriptsKey = ''; return; }
    const onDev = st.cfgs || [];
    const list = onDev.map((c) => ({ name: c.n, where: t('on the device') }));
    for (const tpl of templates) {
      const have = onDev.find((c) => c.n === tpl.name);
      if (!have || have.s !== new Blob([tpl.text]).size) list.push({ name: tpl.name, tpl, where: have ? t('library – differs from the one on the device') : t('library') });
    }
    const allowed = st.ctl !== false;
    $('#conScriptHint').textContent = !allowed ? t('Changes are not allowed at the device: scripts cannot be played from here.')
      : list.length ? t('Played on port {port} – watch it in the terminal above.', { port }) : t('No scripts on the device and none in the library.');
    const key = JSON.stringify([d.id, port, allowed, !!scriptCmd, list.map((x) => [x.name, x.where])]);
    if (key === scriptsKey) return;
    scriptsKey = key;
    const box2 = $('#conScriptList');
    box2.textContent = '';
    for (const x of list) {
      const b = el('button', { className: 'btn small warn', type: 'button', textContent: t('Play'), disabled: !allowed || !!scriptCmd });
      b.addEventListener('click', () => playScript(d, x, port));
      box2.append(el('div', { className: 'script' }, el('span', { textContent: x.name, title: x.name }, el('small', { textContent: x.where })), b));
    }
  }
  async function playScript(d, x, port) {
    if (!await ask(t('Play “{name}” on port {port}?', { name: x.name, port }), t('The lines go to the console of whatever is connected there.'), t('Continue'), 'warn')) return;
    if (!await ask(t('Really play “{name}” now?', { name: x.name }), x.tpl
      ? t('The template is written to the device first (a configuration of the same name there is replaced), then played line by line. This cannot be undone.')
      : t('The device plays it line by line. This cannot be undone.'), t('Play now'))) return;
    const note = (text) => { $('#conScriptState').textContent = text; };
    scriptCmd = -1;
    scriptsKey = '';
    try {
      if (x.tpl) {
        const args = { name: x.tpl.name, text: x.tpl.text };
        if (new Blob([JSON.stringify(args)]).size > 6000) throw new Error(t('Too large to write from here: {list}', { list: x.name }));
        note(t('Writing “{name}” …', { name: x.name }));
        await run(d.id, 'config_put', args);
      }
      const c = await api('POST', `/devices/${d.id}/commands`, { type: 'play', args: { name: x.name, port } });
      scriptCmd = c.id;
      note(t('Waiting for the device to fetch “{name}” …', { name: x.name }));
      const term = con;
      // a line of our own in the terminal, between what the port says (grey, on a line of its own)
      const mark = (text) => { if (con === term && !term.ended) term.term.write(`\r\n\x1b[90m[${text}]\x1b[0m\r\n`); };
      let started = false;
      for (;;) {                                    // as long as it takes: the terminal shows it line by line
        await new Promise((r) => setTimeout(r, 1000));
        const now = (await api('GET', `/devices/${d.id}/commands`)).find((y) => y.id === c.id);
        if (!now) throw new Error(t('The device did not answer'));
        if (!started && now.state !== 'queued') {
          started = true;
          note(t('“{name}” is being played …', { name: x.name }));
          mark(t('Script “{name}” starts', { name: x.name }));
        }
        // the player's own words come in the device's language
        if (now.state === 'failed' || now.state === 'expired') {
          const why = typeof now.result === 'string' ? t(now.result) : (now.result && now.result.result) || CMD_STATES[now.state];
          mark(t('Script “{name}”: {result}', { name: x.name, result: why }));
          throw new Error(why);
        }
        if (now.state === 'done') {
          const res = (now.result && now.result.result) || '';
          note('✓ ' + x.name + (res ? ': ' + res : ''));
          mark(t('Script “{name}”: {result}', { name: x.name, result: res || t('done') }));
          break;
        }
      }
    } catch (err) { note(err.message); toast(err.message, 6000); }
    scriptCmd = 0;
    scriptsKey = '';
    refresh();
  }
  $('#conForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const args = { port: +$('#conPort').value, minutes: +$('#conMinutes').value };
    let kp = null;
    if (SUBTLE) {
      try {
        kp = await SUBTLE.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, false, ['deriveBits']);
        args.e2e = b64(new Uint8Array(await SUBTLE.exportKey('raw', kp.publicKey)));
      } catch (err) { kp = null; }
    }
    try {
      const c = await api('POST', `/devices/${selected}/commands`, { type: 'console', args });
      if (kp) conKeys[c.id] = kp;
      toast(t('{cmd}: queued – the device fetches the command the next time it reports in', { cmd: CMD_NAMES.console }));
    } catch (err) { toast(err.message, 5000); }
    refresh();
  });
  // as in PuTTY: the right button pastes (the browser asks once whether the page may read the clipboard)
  $('#term').addEventListener('contextmenu', async (e) => {
    if (!con) return;
    e.preventDefault();
    const c = con;
    if (c.ended) return;
    try {
      const text = await navigator.clipboard.readText();
      if (text && con === c) c.term.paste(text);
    } catch (err) { toast(t('This browser does not let the page read the clipboard – paste with Ctrl+Shift+V.'), 5000); }
    c.term.focus();
  });
  $('#conBreak').addEventListener('click', () => { conInput({ brk: true }); if (con) con.term.focus(); });
  $('#conEndOther').addEventListener('click', async () => {
    const d = devices.find((x) => x.id === selected);
    if (!d || !await ask(t('End the session of the other browser?'), t('Whoever works in it is cut off.'), t('End'))) return;
    try {
      const v = await api('GET', `/devices/${d.id}/console`);
      if (v.console) await api('POST', `/devices/${d.id}/console/${v.console.id}/close`);
    } catch (err) { toast(err.message); }
    refresh();
  });
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
  // Registration without an invitation: closed unless opened here, for 15
  // minutes to 3 hours; the server closes it by itself.
  const mins = (min) => (min >= 60 ? `${Math.floor(min / 60)} h` : '') + (min >= 60 && min % 60 ? ' ' : '') + (min % 60 ? `${min % 60} min` : '');
  for (let m = 15; m <= 180; m += 15) $('#openFor').append(new Option(mins(m), m));
  async function openFor(minutes) {
    try {
      await api('POST', '/enrollment/open', { minutes });
      toast(minutes ? t('Registration is open for {time}', { time: mins(minutes) }) : t('Registration closed'));
      refresh();
    } catch (err) { toast(err.message); }
  }
  $('#openSet').addEventListener('click', () => { openFor(+$('#openFor').value); $('#inviteDlg').close(); });
  $('#openEnd').addEventListener('click', () => openFor(0));
  function renderOpen() {
    const s = admin && server ? server.open_s || 0 : 0;
    $('#openNote').hidden = !s;
    if (s) $('#openText').textContent = t('Registration without invitation is open for another {time}. Devices that register appear under “Waiting”.', { time: mins(Math.max(1, Math.ceil(s / 60))) });
    $('#openUrl').textContent = server ? server.public_url || '' : '';
  }
  // ---- profiles for new devices: kept at the server, named in an invitation
  let profiles = [], profEditing = 0;
  const AUTHS = ['WPA2', 'EAP-TLS', 'PEAP', 'EAP-TTLS'];
  async function loadProfiles() {
    try { profiles = await api('GET', '/profiles'); } catch (e) { return; }
    const tb = $('#profs').tBodies[0];
    tb.textContent = '';
    for (const p of profiles) {
      const asks = [p.control ? t('changes') : '', p.unattended_h ? t('unattended {h} h', { h: p.unattended_h }) : ''].filter(Boolean).join(', ') || '–';
      const edit = el('button', { className: 'btn small', type: 'button', textContent: t('Edit') });
      edit.addEventListener('click', () => editProfile(p));
      const del = el('button', { className: 'btn small danger', type: 'button', textContent: t('Delete') });
      del.addEventListener('click', async () => {
        if (!await ask(t('Delete this profile?'), p.name, t('Delete'))) return;
        try { await api('DELETE', `/profiles/${p.id}`); } catch (e) { toast(e.message, 5000); }
        loadProfiles();
      });
      tb.append(el('tr', {}, el('td', { textContent: p.name + (p.note ? ' – ' + p.note : '') }), el('td', { textContent: [p.region, p.country, p.site].filter(Boolean).join(' › ') || '–' }),
        el('td', { textContent: p.wifi ? `${p.wifi.ssid} (${AUTHS[p.wifi.auth] || ''})` : '–' }), el('td', { textContent: asks }),
        el('td', { textContent: ago(p.updated) }), el('td', { className: 'acts' }, edit, del)));
    }
    label($('#profs'));
    $('#profs').hidden = !profiles.length;
    $('#noProfs').hidden = !!profiles.length;
  }
  function profAuthShow() {
    const a = $('#profAuth').value;
    for (const n of $('#profForm').querySelectorAll('[data-prof]')) n.hidden = !n.dataset.prof.includes(a);
  }
  function editProfile(p) {
    profEditing = p ? p.id : 0;
    const w = (p && p.wifi) || {};
    $('#profTitle').textContent = p ? t('Edit profile') : t('New profile');
    $('#profName').value = p ? p.name : '';
    $('#profNote').value = (p && p.note) || '';
    $('#profRegion').value = (p && p.region) || '';
    $('#profCountry').value = (p && p.country) || '';
    $('#profSite').value = (p && p.site) || '';
    $('#profCtl').checked = !!(p && p.control);
    $('#profUn').value = String((p && p.unattended_h) || 0);
    $('#profSsid').value = w.ssid || '';
    $('#profAuth').value = String(w.auth || 0);
    $('#profUser').value = w.user || '';
    $('#profIdent').value = w.identity || '';
    $('#profPass').value = '';
    $('#profPass').placeholder = w.pass_set ? t('unchanged') : '';
    $('#profCaCheck').checked = !!w.ca_check;
    $('#profDns').value = w.dns || '';
    $('#profCa').value = $('#profClient').value = $('#profClientPass').value = '';
    $('#profHave').textContent = [w.ca_set ? t('CA certificate stored') : '', w.client_set ? t('client certificate stored') : ''].filter(Boolean).join(' · ');
    const box = $('#profGroups');
    box.textContent = '';
    for (const g of groups) {
      const cb = el('input', { type: 'checkbox', value: g.id, checked: !!(p && (p.groups || []).includes(g.id)) });
      box.append(el('label', { className: 'check' }, cb, ' ' + g.name));
    }
    $('#profErr').hidden = true;
    profAuthShow();
    $('#profDlg').showModal();
  }
  const fileText = (inp) => (inp.files[0] ? inp.files[0].text() : Promise.resolve(''));
  const fileB64 = async (inp) => {
    if (!inp.files[0]) return null;
    const b = new Uint8Array(await inp.files[0].arrayBuffer());
    let s = '';
    for (const x of b) s += String.fromCharCode(x);
    return btoa(s);
  };
  $('#profAuth').addEventListener('change', profAuthShow);
  $('#profNew').addEventListener('click', () => editProfile(null));
  $('#profCancel').addEventListener('click', () => $('#profDlg').close());
  $('#profForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const body = {
      name: $('#profName').value, note: $('#profNote').value, region: $('#profRegion').value, country: $('#profCountry').value, site: $('#profSite').value,
      groups: [...$('#profGroups').querySelectorAll('input:checked')].map((c) => Number(c.value)), control: $('#profCtl').checked, unattended_h: Number($('#profUn').value),
    };
    if ($('#profSsid').value.trim()) {
      body.wifi = {
        ssid: $('#profSsid').value.trim(), auth: Number($('#profAuth').value), user: $('#profUser').value.trim(), identity: $('#profIdent').value.trim(),
        pass: $('#profPass').value, ca_check: $('#profCaCheck').checked, dns: $('#profDns').value.trim(),
        ca: await fileText($('#profCa')), client_pass: $('#profClientPass').value,
      };
      const client = await fileB64($('#profClient'));
      if (client) body.wifi.client = client;
    }
    try {
      await api(profEditing ? 'PUT' : 'POST', profEditing ? `/profiles/${profEditing}` : '/profiles', body);
      $('#profDlg').close();
      loadProfiles();
    } catch (err) { $('#profErr').textContent = err.message; $('#profErr').hidden = false; }
  });
  $('#bkNow').addEventListener('click', async () => {
    try { await api('POST', `/devices/${selected}/backup`); toast(t('Backup queued – the device writes it the next time it reports in')); } catch (err) { toast(err.message, 6000); }
  });
  $('#bkRestore').addEventListener('click', async () => {
    const opts = [[selected, t('this device’s own backup')]].concat(devices.filter((x) => x.id !== selected && x.state !== 'pending').map((x) => [x.id, t('the backup of {name}', { name: x.name || x.id.slice(0, 8) })]));
    const from = await pick(t('Restore from the SD card'), t('Which backup is on the card'), t('Continue'), opts);
    if (!from) return;
    if (!await ask(t('Restore this backup?'), t('The device replaces its settings, configurations and certificates with those from the card and restarts. Changes allowed and unattended access stay as they are.'), t('Restore'), 'warn')) return;
    try { await api('POST', `/devices/${selected}/restore`, { from }); toast(t('Restore queued')); } catch (err) { toast(err.message, 6000); }
  });
  $('#oProfile').addEventListener('click', async () => {
    if (!profiles.length) try { profiles = await api('GET', '/profiles'); } catch (e) { toast(e.message); return; }
    if (!profiles.length) { toast(t('No profile yet - make one in the library')); return; }
    const id = await pick(t('Apply a profile'), t('Profile'), t('Apply'), profiles.map((p) => [p.id, p.name]));
    if (!id) return;
    try {
      await api('POST', `/devices/${selected}/profile`, { profile: Number(id) });
      toast(t('Profile applied: place and groups set, the rest is queued for the device'));
    } catch (err) { toast(err.message, 6000); }
  });

  let inviteSeq = 0;
  async function newInvite() {             // null also when a later one was asked for meanwhile
    let inv;
    const seq = ++inviteSeq;
    const profile = Number($('#inviteProf').value || 0);
    try { inv = await api('POST', '/enroll-tokens', { ttl_s: 600, profile }); } catch (err) { toast(err.message); return null; }
    return seq === inviteSeq ? inv : null;
  }
  $('#inviteProf').addEventListener('change', async () => {
    const inv = await newInvite();
    if (inv) showInvite(inv);
  });
  $('#invite').addEventListener('click', async () => {
    try { profiles = await api('GET', '/profiles'); } catch (e) { profiles = []; }
    const sel = $('#inviteProf');
    sel.textContent = '';
    sel.append(el('option', { value: '0', textContent: t('none') }));
    for (const p of profiles) sel.append(el('option', { value: p.id, textContent: p.name }));
    const inv = await newInvite();
    if (!inv) return;
    showInvite(inv);
    $('#inviteDlg').showModal();
  });
  function showInvite(inv) {
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
  }
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
    'POST /devices/{id}/place': t('Location set'), 'POST /devices/{id}/groups': t('Groups of a device set'), 'POST /groups': t('Group added'), 'PUT /groups/{id}': t('Group changed'),
    'DELETE /groups/{id}': t('Group deleted'), 'POST /users/{name}/role': t('Role changed'), 'POST /templates': t('Template added'), 'PUT /templates/{id}': t('Template changed'),
    'DELETE /templates/{id}': t('Template deleted'), 'POST /templates/{id}/send': t('Template sent'), 'POST /firmware': t('Firmware uploaded'), 'DELETE /firmware/{id}': t('Firmware deleted'),
    'POST /images': t('Image uploaded'), 'DELETE /images/{id}': t('Image deleted'), 'POST /images/{id}/link': t('Image downloaded'),
    'POST /profiles': t('Profile added'), 'PUT /profiles/{id}': t('Profile changed'), 'DELETE /profiles/{id}': t('Profile deleted'), 'POST /devices/{id}/profile': t('Profile applied'),
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
    loadAccess();
  }
  async function loadAccess() {
    tabs('access', $('[data-tabs=access]'));
    try { [users, groups] = await Promise.all([api('GET', '/users'), api('GET', '/groups')]); } catch (e) { return; }
    const ub = $('#users tbody');
    ub.textContent = '';
    for (const u of users) {
      const state = u.disabled ? t('disabled') : u.ready ? t('active') : u.setup_expires ? t('waiting to be set up (code valid until {until})', { until: when(u.setup_expires) }) : t('not set up, code expired');
      const how = KINDS[u.kind] + (u.kind === 'entra' ? ` (${u.entra_name || u.entra_oid}${u.entra_name && !u.entra_oid ? ', ' + t('never signed in') : ''})` : '');
      const acts = el('td', { className: 'acts' });
      const name = encodeURIComponent(u.name);
      const role = el('select', { ariaLabel: t('May'), disabled: u.name === me && !token });
      role.append(el('option', { value: 'admin', textContent: t('everything') }), el('option', { value: 'member', textContent: t('its groups only') }));
      role.value = u.role;
      role.addEventListener('change', async () => {
        try { await api('POST', `/users/${name}/role`, { role: role.value }); toast(t('Saved')); } catch (e) { toast(e.message, 5000); }
        loadAccess();
      });
      const mine = el('div', { className: 'chips' });
      for (const g of groups) if (g.members && g.members[u.name]) mine.append(el('span', { className: 'chip', textContent: `${g.name}: ${RIGHTS[g.members[u.name]]}` }));
      if (u.kind === 'local') acts.append(el('button', { className: 'btn small', textContent: t('Reset'),
        onclick: () => userAction(t('Reset the account “{name}”?', { name: u.name }), t('Password and authenticator app stop working; there will be a new setup code.'), t('Reset'), 'POST', `/users/${name}/reset`) }));
      acts.append(el('button', { className: 'btn small', textContent: u.disabled ? t('Enable') : t('Disable'),
        onclick: () => userAction(u.disabled ? t('Enable the account “{name}”?', { name: u.name }) : t('Disable the account “{name}”?', { name: u.name }),
          u.disabled ? t('The person can sign in again afterwards.') : t('The person is signed out and cannot get in until the account is enabled again.'),
          u.disabled ? t('Enable') : t('Disable'), 'POST', `/users/${name}/disable`, { disabled: !u.disabled }, u.disabled) }));
      acts.append(el('button', { className: 'btn small danger', textContent: t('Delete'),
        onclick: () => userAction(t('Delete the account “{name}”?', { name: u.name }), t('This cannot be undone.'), t('Delete'), 'DELETE', `/users/${name}`) }));
      ub.append(el('tr', null, el('td', { textContent: u.name }), el('td', { textContent: how }), el('td', {}, role), el('td', {}, mine), el('td', { textContent: state }),
        el('td', { textContent: u.last_login ? ago(u.last_login) : '–', title: when(u.last_login) }), acts));
    }
    label($('#users'));
    $('#users').hidden = !users.length;
    $('#noUsers').hidden = !!users.length;

    const box = $('#groups');
    box.textContent = '';
    for (const g of groups) {
      const people = Object.entries(g.members || {}).sort().map(([n, r]) => `${n} (${RIGHTS[r]})`);
      const devs = devices.filter((d) => (d.groups || []).includes(g.id));
      const edit = el('button', { className: 'btn small', type: 'button', textContent: t('Edit') });
      edit.addEventListener('click', () => editGroup(g));
      box.append(el('div', { className: 'dev grpcard' },
        el('div', { className: 'top' }, el('b', { textContent: g.name }), edit),
        el('div', { className: 'where', textContent: g.note || '' }),
        el('div', { className: 'facts' }, el('span', { textContent: devs.length === 1 ? t('1 device') : t('{n} devices', { n: devs.length }) }),
          el('span', { textContent: people.length === 1 ? t('1 person') : t('{n} people', { n: people.length }) })),
        el('div', { className: 'use people', textContent: people.join(', ') || t('Nobody yet – only administrators see these devices.') })));
    }
    $('#noGroups').hidden = !!groups.length;
  }
  // One group: who is in it with which right, and which devices belong to it.
  let grpEditing = null;
  function editGroup(g) {
    grpEditing = g;
    $('#grpTitle').textContent = g ? t('Edit group') : t('New group');
    $('#grpName').value = g ? g.name : '';
    $('#grpNote').value = g ? g.note || '' : '';
    $('#grpErr').hidden = true;
    $('#grpDelete').hidden = !g;
    const mb = $('#grpMembers');
    mb.textContent = '';
    for (const u of users) {
      const sel = el('select', { ariaLabel: u.name });
      sel.append(el('option', { value: '', textContent: t('not in it') }), el('option', { value: 'use', textContent: t('use: look and console') }), el('option', { value: 'manage', textContent: t('manage: also change') }));
      sel.value = (g && g.members && g.members[u.name]) || '';
      sel.dataset.user = u.name;
      mb.append(el('div', { className: 'line' }, el('span', { textContent: u.name + (u.role === 'admin' ? ' · ' + t('administrator') : '') }), sel));
    }
    $('#grpNoUsers').hidden = !!users.length;
    $('#grpMembers').hidden = !users.length;
    const db = $('#grpDevices');
    db.textContent = '';
    const live = devices.filter((d) => d.state !== 'revoked');
    for (const d of live) db.append(el('label', { className: 'check' }, el('input', { type: 'checkbox', value: d.id, checked: !!g && (d.groups || []).includes(g.id) }), ' ' + (d.name || d.id.slice(0, 8)) + (placeText(d) ? ' – ' + placeText(d) : '')));
    $('#grpNoDevs').hidden = !!live.length;
    $('#grpDevices').hidden = !live.length;
    $('#grpDlg').showModal();
  }
  $('#grpNew').addEventListener('click', () => editGroup(null));
  $('#grpCancel').addEventListener('click', () => $('#grpDlg').close());
  $('#grpForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const members = {};
    for (const s of $('#grpMembers').querySelectorAll('select')) if (s.value) members[s.dataset.user] = s.value;
    try {
      const g = await api(grpEditing ? 'PUT' : 'POST', grpEditing ? `/groups/${grpEditing.id}` : '/groups', { name: $('#grpName').value, note: $('#grpNote').value, members });
      for (const box of $('#grpDevices').querySelectorAll('input')) {
        const d = devices.find((x) => x.id === box.value), has = (d.groups || []).includes(g.id);
        if (has !== box.checked) await api('POST', `/devices/${d.id}/groups`, { groups: box.checked ? (d.groups || []).concat(g.id) : d.groups.filter((x) => x !== g.id) });
      }
      $('#grpDlg').close();
      await refresh();
      loadAccess();
    } catch (err) { $('#grpErr').textContent = err.message; $('#grpErr').hidden = false; }
  });
  $('#grpDelete').addEventListener('click', async () => {
    if (!await ask(t('Delete the group “{name}”?', { name: grpEditing.name }), t('Its members lose the devices they saw only through it. The devices stay.'), t('Delete'))) return;
    try { await api('DELETE', `/groups/${grpEditing.id}`); } catch (e) { toast(e.message, 5000); }
    $('#grpDlg').close();
    await refresh();
    loadAccess();
  });
  async function loadAudit() {
    let log;
    try { log = await api('GET', '/audit?n=200'); } catch (e) { return; }
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
    if (view === 'server' && !selected) tabs('server', $('[data-tabs=server]'));
    const st = status.state, kc = server.key_change, dv = status.devices;
    // what is wrong is said on every page, not only where the numbers are
    $('#alert').hidden = st.ok;
    if (!st.ok) $('#alert').textContent = t('The server cannot save its state (since {since}). Changes are lost if it restarts now.', { since: when(st.since) });
    const mark = $('#navMark');
    mark.hidden = st.ok && !kc;
    mark.className = 'mark' + (st.ok ? '' : ' bad');
    const b = $('#stState');
    b.className = 'pill ' + (st.ok ? 'ok' : 'failed');
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
  $('#auditReload').addEventListener('click', loadAudit);
  $('#addUser').addEventListener('click', () => {
    $('#userForm').reset();
    $('#userErr').hidden = true;
    $('#userEntraRow').hidden = true;
    $('#userDlg').showModal();
  });
  $('#userKind').addEventListener('change', () => { $('#userEntraRow').hidden = $('#userKind').value !== 'entra'; $('#userPassHint').hidden = $('#userKind').value === 'entra'; });
  $('#userCancel').addEventListener('click', () => $('#userDlg').close());
  $('#codeClose').addEventListener('click', () => { $('#codeText').textContent = ''; $('#codeDlg').close(); });
  // The administrator's own account: straight to the form that sets the password.
  $('#codeUse').addEventListener('click', async () => {
    const name = $('#codeFor').textContent, code = $('#codeText').textContent;
    $('#codeText').textContent = '';
    $('#codeDlg').close();
    if (!token) try { await auth('logout', {}); } catch (e) { /* signed out here in any case */ }
    showLogin();
    $('#setupBox').open = true;
    $('#setupName').value = name;
    $('#setupCode').value = code;
    $('#setupPass').focus();
  });
  $('#userForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    try {
      const r = await api('POST', '/users', { name: $('#userName').value, kind: $('#userKind').value, entra: $('#userEntra').value, role: $('#userRole').value });
      $('#userDlg').close();
      if (r.setup) showCode(r.name, r.setup); else toast(t('Added'));
      loadAccess();
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

