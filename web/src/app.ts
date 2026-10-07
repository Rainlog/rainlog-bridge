import { createRadio } from './radio';
import { createManualUpdate } from './manual-update';
declare const RADIO_MHZ: number;
// Rainlog Bridge config page behavior. Bundled + minified by build.mjs and
// inlined into a single index.html that the firmware embeds. The page is fully
// static: current settings are fetched from /config at load and written into
// the fields (so the embedded HTML needs no server-side templating). Wi-Fi
// passwords stay hidden; authenticated users can edit saved station keys.

const WU_MAP_MAX = 8;

// The shipped Bridge Wi-Fi password (printed on the LCD). It must be changed
// once configured; mirrors the firmware's ap_pass_is_default check.
const DEFAULT_AP_PASS = 'rainlog123';

// Whether the bridge's AP password is still the shipped default (from /config).
// When true, the user must set a new one before saving.
let apPassDefault = false;

interface ScannedNet {
  ssid: string;
  rssi: number;
}

interface WuMapping {
  gauge_id: number;
  wu_id: string;
  wu_key: string; // stored key; shown behind the row's Show toggle
}

interface BridgeConfig {
  sta_ssid: string;
  ap_ssid: string;
  ap_pass_default: boolean;
  ap_ip: string;
  wu_map: WuMapping[];
  radio_enabled?: boolean;
  wifi_interception_enabled?: boolean;
  wifi_interception_active?: boolean;
  radio_map?: Parameters<
    ReturnType<typeof createRadio>['load']
  >[0]['radio_map'];
}

interface TestStatus {
  s: 'idle' | 'running' | 'ok' | 'fail';
}

type OtaPhase =
  | 'idle'
  | 'checking'
  | 'uptodate'
  | 'available'
  | 'updating'
  | 'error';

interface OtaStatus {
  board?: string;
  max_image_size?: number;
  phase: OtaPhase;
  running: string;
  latest: string;
  available: boolean;
  progress: number;
  error: string;
}

interface ApClient {
  mac: string;
  vendor: string;
  name: string; // user-given name; '' if never named
  hostname: string; // hostname the device announced via DHCP; '' if none
  ip: string; // last known DHCP address; '' if never leased
  rssi: number; // only meaningful while connected
  connected: boolean;
  age_s: number; // seconds since last seen
  you: boolean; // this row is the device viewing the page
  gauge_id?: number;
  rx: number; // weather uploads captured from this device (this boot)
  rl: number; // of those, accepted by Rainlog
  wu: number; // of those, relayed to Weather Underground
  err: number; // sticky error flags; see deviceBadges
  rlrej: string; // last Rainlog reject reason (detail for the err&2 flag)
  wurej: string; // last WU reject reason (detail for the err&8 flag)
}

interface Badge {
  text: string;
  warn: boolean; // amber "expected, not great" vs red "something's wrong"
}

// Turn an upstream's raw reject code into concise English + a severity. The
// same *_REJECT flag covers both the benign (rate limiting, which the bridge
// already self-throttles) and the serious (bad credentials), so severity has
// to come from the reason, not the flag.
function rejectInfo(reason: string): Badge {
  const r = reason.toUpperCase();
  if (r === 'RATELIMIT') {
    return { text: 'uploading too often', warn: true };
  }
  if (r.includes('INVALIDPASSWORD') || r.includes('INVALIDID')) {
    return { text: 'wrong station ID or key', warn: false };
  }
  return { text: reason.toLowerCase(), warn: false }; // unknown: show as-is
}

// Badges for a device's sticky error flags (firmware AP_CLIENT_ERR_* bits): the
// unreachable flags are always errors; the reject flags defer to rejectInfo.
function deviceBadges(c: ApClient): Badge[] {
  const out: Badge[] = [];
  if (c.err & 1) {
    out.push({ text: 'Rainlog unreachable', warn: false });
  }
  if (c.err & 2) {
    const info = rejectInfo(c.rlrej);
    out.push({ text: `Rainlog: ${info.text}`, warn: info.warn });
  }
  if (c.err & 4) {
    out.push({ text: 'WU unreachable', warn: false });
  }
  if (c.err & 8) {
    const info = rejectInfo(c.wurej);
    out.push({ text: `WU: ${info.text}`, warn: info.warn });
  }
  return out;
}

const el = (id: string): HTMLElement => {
  const e = document.getElementById(id);
  if (e === null) {
    throw new Error(`missing #${id}`);
  }
  return e;
};

const field = (name: string): HTMLInputElement =>
  document.querySelector(`[name=${name}]`) as HTMLInputElement;

const setScanStatus = (html: string): void => {
  el('sstat').innerHTML = html;
};
const setTestStatus = (html: string): void => {
  el('tstat').innerHTML = html;
};

// Ascending signal bars (▂▄▆█) for an RSSI in dBm; more/taller bars = stronger.
function signalBars(rssi: number): string {
  const lvl =
    rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  return lvl > 0 ? '▂▄▆█'.slice(0, lvl) : '·';
}

// Populate the SSID combobox's chevron dropdown. The first option is a header
// placeholder (hidden in the closed box, which shows only the chevron); the
// rest are scanned networks with a signal-strength meter. The option value is
// the bare SSID, so picking one fills the text input cleanly (see the change
// handler below).
function fillNetworks(list: ScannedNet[]): void {
  const sel = el('nets') as HTMLSelectElement;
  sel.innerHTML = '';
  sel.add(new Option('Scanned networks', ''));
  for (const net of list) {
    sel.add(new Option(`${net.ssid}  ${signalBars(net.rssi)}`, net.ssid));
  }
}

// ---- live password validation --------------------------------------------
//
// Validate as the user types and disable Save while invalid, so we never POST
// a bad password and bounce to a server error page (which would wipe the whole
// form). Errors appear inline under the field once it has been touched.

const touched = new Set<HTMLInputElement>();

// Home Wi-Fi password: optional (blank keeps the saved one / open network), but
// a non-empty value must meet the WPA2 minimum.
function homePwError(): string | null {
  const v = field('sta_pass').value;
  if (v.length > 0 && v.length < 8) {
    return 'Wi-Fi passwords are at least 8 characters.';
  }
  return null;
}

// Bridge Wi-Fi password: must be set when still the default, and any new value
// must be 8+ chars and not the shipped default (mirrors the firmware).
function bridgePwError(): string | null {
  if ((el('bridgeWifiFields') as HTMLFieldSetElement).disabled) return null;
  const v = field('ap_pass').value;
  if (apPassDefault && v.length === 0) {
    return 'Choose a Bridge Wi-Fi password.';
  }
  if (v.length > 0) {
    if (v === DEFAULT_AP_PASS) {
      return 'Choose a password other than the default.';
    }
    if (v.length < 8) {
      return 'At least 8 characters.';
    }
  }
  return null;
}

function receptionError(): string | null {
  if (RADIO_MHZ === 0) return null;
  return !(el('radioEnabled') as HTMLInputElement).checked &&
    !(el('wifiInterceptionEnabled') as HTMLInputElement).checked
    ? 'Enable at least one reception source.'
    : null;
}

function revalidate(): void {
  const interception = document.getElementById(
    'wifiInterceptionEnabled',
  ) as HTMLInputElement | null;
  (el('bridgeWifiFields') as HTMLFieldSetElement).disabled =
    interception !== null && !interception.checked;
  if (RADIO_MHZ !== 0) {
    const radio = el('radioEnabled') as HTMLInputElement;
    radio.disabled = radio.checked && !interception!.checked;
    interception!.disabled = interception!.checked && !radio.checked;
    el('reception_err').textContent = receptionError() ?? '';
    // Disabled checkboxes do not submit, so preserve the locked-on source.
    el('form')
      .querySelectorAll('[data-reception]')
      .forEach((input) => input.remove());
    [radio, interception!]
      .filter((input) => input.disabled && input.checked)
      .forEach((input) => {
        const value = document.createElement('input');
        value.type = 'hidden';
        value.name = input.name;
        value.value = 'on';
        value.dataset.reception = '';
        el('form').append(value);
      });
  }
  const he = homePwError();
  const be = bridgePwError();
  el('sta_pass_err').textContent =
    touched.has(field('sta_pass')) && he ? he : '';
  el('ap_pass_err').textContent = touched.has(field('ap_pass')) && be ? be : '';
  document
    .querySelectorAll<HTMLButtonElement>('button[type=submit]')
    .forEach((button) => {
      button.disabled = Boolean(he || be || receptionError());
    });
}

// Insert an inline error <div> after a field's wrapper (.pw) or the field.
function addErrorSlot(input: HTMLInputElement, id: string): void {
  const div = document.createElement('div');
  div.className = 'err';
  div.id = id;
  (input.closest('.pw') ?? input).insertAdjacentElement('afterend', div);
  const mark = (): void => {
    touched.add(input);
    revalidate();
  };
  input.addEventListener('input', mark);
  input.addEventListener('blur', mark);
}

// Wrap a password input with a Show/Hide toggle that flips its type in place.
function attachPwToggle(input: HTMLInputElement): void {
  const wrap = document.createElement('div');
  wrap.className = 'pw';
  input.parentNode?.insertBefore(wrap, input);
  wrap.appendChild(input);

  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'toggle';
  btn.textContent = 'Show';
  btn.addEventListener('click', () => {
    const reveal = input.type === 'password';
    input.type = reveal ? 'text' : 'password';
    btn.textContent = reveal ? 'Hide' : 'Show';
  });
  wrap.appendChild(btn);
}

// ---- WU forwarding map rows ----------------------------------------------

// Append one gauge->WU mapping row. `m` pre-fills it; omit for a blank row.
function addRow(m?: WuMapping): void {
  const rows = el('wuRows');
  if (rows.children.length >= WU_MAP_MAX) {
    return;
  }
  const div = document.createElement('div');
  div.className = 'wurow';

  // The Rainlog station id is the string "Rainlog<gaugeId>" (what the user
  // sees on rainlog.org and typed into their console); the firmware accepts
  // that or the bare gauge number.
  const gauge = document.createElement('input');
  gauge.className = 'rl';

  gauge.setAttribute('form', 'form');
  gauge.setAttribute('aria-label', 'Rainlog station ID');
  gauge.placeholder = 'Rainlog station ID (Rainlog12345)';
  if (m) {
    gauge.value = `Rainlog${m.gauge_id}`;
  }

  const picker = document.createElement('select');
  picker.className = 'stationPicker';
  picker.setAttribute('aria-label', 'Choose a known Rainlog station');
  picker.addEventListener('change', () => {
    if (picker.value) gauge.value = picker.value;
  });
  const station = document.createElement('div');
  station.className = 'combo';
  station.append(gauge, picker);

  // Downward arrow between the Rainlog station and the WU credentials it
  // forwards to, making the row read as "this forwards down to this".
  const arrow = document.createElement('div');
  arrow.className = 'fwdarrow';
  arrow.textContent = '↓';

  const wuId = document.createElement('input');
  wuId.className = 'wu';
  wuId.setAttribute('form', 'form');
  wuId.placeholder = 'WU station ID';
  if (m) {
    wuId.value = m.wu_id;
  }

  const wuKey = document.createElement('input');
  wuKey.className = 'wk';
  wuKey.setAttribute('form', 'form');
  wuKey.type = 'password';
  wuKey.placeholder = 'WU key';
  if (m) {
    wuKey.value = m.wu_key; // hidden by default; the Show toggle reveals it
  }

  const rm = document.createElement('button');
  rm.type = 'button';
  rm.className = 'rm';
  rm.textContent = 'Remove';
  rm.addEventListener('click', () => {
    div.remove();
    refreshAddButton();
  });

  div.append(station, arrow, wuId, wuKey, rm);
  attachPwToggle(wuKey); // wraps wuKey in place (stays before rm)
  rows.appendChild(div);
  refreshAddButton();
  refreshKnownGauges();
}

function refreshAddButton(): void {
  (el('addBtn') as HTMLButtonElement).disabled =
    el('wuRows').children.length >= WU_MAP_MAX;
}

// Give each row's inputs contiguous indexed names (rl0/wu0/wk0, rl1/...) right
// before the browser serializes the form, so the firmware sees a dense list.
function numberRows(): void {
  const rows = el('wuRows').children;
  for (let i = 0; i < rows.length; i++) {
    const row = rows[i];
    (row.querySelector('.rl') as HTMLInputElement).name = `rl${i}`;
    (row.querySelector('.wu') as HTMLInputElement).name = `wu${i}`;
    (row.querySelector('.wk') as HTMLInputElement).name = `wk${i}`;
  }
}

// Saved mappings and station IDs observed in console uploads are suggestions.
// The station picker fills an editable input for stations not yet seen.
const knownGaugeIds = new Set<number>();
function refreshKnownGauges(ids: number[] = []): void {
  ids.forEach((id) => {
    if (Number.isInteger(id) && id > 0 && id <= 4294967295)
      knownGaugeIds.add(id);
  });
  document
    .querySelectorAll<HTMLInputElement>('#radioRows [data-field=gauge]')
    .forEach((input) => {
      const id = Number(input.value.replace(/^Rainlog/i, ''));
      if (Number.isInteger(id) && id > 0 && id <= 4294967295)
        knownGaugeIds.add(id);
    });
  const inputs = Array.from(
    document.querySelectorAll<HTMLInputElement>('#wuRows .rl'),
  );
  const used = new Set(
    inputs
      .filter((input) => input.value)
      .map((input) => Number(input.value.replace(/^Rainlog/i, ''))),
  );
  const unused = [...knownGaugeIds].filter((id) => !used.has(id));
  const blank = inputs.filter((input) => !input.value);
  if (unused.length === 1 && blank.length === 1)
    blank[0].value = `Rainlog${unused[0]}`;
  inputs.forEach((input) => {
    const picker = input.parentElement!.querySelector('select')!;
    picker.replaceChildren(new Option('Enter station ID manually', ''));
    [...knownGaugeIds]
      .sort((a, b) => a - b)
      .forEach((id) => picker.add(new Option(`Rainlog${id}`, `Rainlog${id}`)));
    picker.value = knownGaugeIds.has(
      Number(input.value.replace(/^Rainlog/i, '')),
    )
      ? input.value
      : '';
  });
}

// ---- scanning + WiFi test -------------------------------------------------

// Cached list (instant, never disrupts the AP).
async function scan(): Promise<void> {
  try {
    fillNetworks((await (await fetch('/scan')).json()) as ScannedNet[]);
  } catch {
    /* leave the picker hidden */
  }
}

// On-demand live scan (silences the radio ~1-2s; spinner covers it).
async function scanlive(): Promise<void> {
  setScanStatus('<span class=spin></span>Scanning…');
  try {
    const list = (await (await fetch('/scanlive')).json()) as ScannedNet[];
    fillNetworks(list);
    setScanStatus(`${list.length} found`);
  } catch {
    setScanStatus('Scan failed');
  }
}

// Test the entered home-WiFi creds. The phone briefly drops while the bridge
// connects (channel change), then reconnects; we poll for the result.
function test(): void {
  const ssid = field('sta_ssid').value;
  const pass = field('sta_pass').value;
  setTestStatus(
    '<span class=spin></span>Testing - your phone may drop and reconnect…',
  );
  void fetch('/test', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: `sta_ssid=${encodeURIComponent(ssid)}&sta_pass=${encodeURIComponent(pass)}`,
  }).catch(() => {});
  setTimeout(poll, 4000);
}

function poll(): void {
  fetch('/teststatus')
    .then((r) => r.json() as Promise<TestStatus>)
    .then((j) => {
      if (j.s === 'ok') {
        setTestStatus('Connection test passed');
      } else if (j.s === 'fail') {
        setTestStatus('Connection test failed - check name and password');
      } else {
        setTestStatus('<span class=spin></span>Testing…');
        setTimeout(poll, 2000);
      }
    })
    .catch(() => {
      setTestStatus('<span class=spin></span>Reconnecting…');
      setTimeout(poll, 2500);
    });
}

// ---- firmware (OTA) updates -----------------------------------------------
//
// The bridge checks for updates on its own (shortly after it connects, then
// every 12-24h). This panel shows the last known status and lets the user force
// a check or apply an available update. The actual download/install runs on the
// bridge; we just poll /ota/status for progress.

const otaStat = (html: string): void => {
  el('otaStat').innerHTML = html;
};

let otaPolling = false;
const manualUpdate = createManualUpdate();

function renderOta(s: OtaStatus): void {
  manualUpdate.setStatus(s);
  const apply = el('otaApplyBtn') as HTMLButtonElement;
  const check = el('otaCheckBtn') as HTMLButtonElement;
  apply.hidden = !s.available || s.phase === 'updating';
  check.disabled = s.phase === 'checking' || s.phase === 'updating';
  el('fwWarn').hidden = !s.available; // out-of-date marker on the tab itself
  switch (s.phase) {
    case 'checking':
      otaStat('<span class=spin></span>Checking for updates…');
      break;
    case 'available':
      otaStat(`Update available: v${s.running} &rarr; v${s.latest}`);
      break;
    case 'uptodate':
      otaStat(`Up to date (v${s.running}).`);
      break;
    case 'updating':
      otaStat(
        `<span class=spin></span>Updating to v${s.latest}… ${s.progress}%`,
      );
      break;
    case 'error':
      otaStat(`${s.error || 'Update error'}. Running v${s.running}.`);
      break;
    default:
      otaStat(`Firmware v${s.running}.`);
  }
}

async function fetchOta(): Promise<OtaStatus | null> {
  try {
    return (await (await fetch('/ota/status')).json()) as OtaStatus;
  } catch {
    return null;
  }
}

// Poll while a check/update is in flight. During an apply the bridge reboots,
// so a failed fetch after "updating" means the new image is coming up.
function pollOta(): void {
  if (otaPolling) {
    return;
  }
  otaPolling = true;
  const tick = async (): Promise<void> => {
    const s = await fetchOta();
    if (s === null) {
      otaStat('Bridge is rebooting into the new firmware. Reconnect shortly.');
      otaPolling = false;
      return;
    }
    renderOta(s);
    if (s.phase === 'checking' || s.phase === 'updating') {
      setTimeout(() => void tick(), 1500);
    } else {
      otaPolling = false;
    }
  };
  void tick();
}

function otaCheck(): void {
  otaStat('<span class=spin></span>Checking for updates…');
  void fetch('/ota/check', { method: 'POST' }).catch(() => {});
  setTimeout(pollOta, 800);
}

function otaApply(): void {
  if (!confirm('Install the new firmware and reboot the bridge?')) {
    return;
  }
  otaStat('<span class=spin></span>Starting update…');
  void fetch('/ota/apply', { method: 'POST' }).catch(() => {});
  setTimeout(pollOta, 800);
}

async function loadOta(): Promise<void> {
  const s = await fetchOta();
  if (s) {
    renderOta(s);
    if (s.phase === 'checking' || s.phase === 'updating') {
      pollOta();
    }
  }
}

// ---- devices tab ------------------------------------------------------------
//
// Lists the devices on the bridge's SoftAP from /clients, with a direct link
// to each one (the viewer is on the bridge's WiFi by definition, so plain
// http://10.41.0.x/ links work). Polls only while the tab is visible.

const DEV_POLL_MS = 5000;

// The bridge's AP SSID (from /config), for the devices empty-state text.
let apSsid = '';
let wifiInterceptionActive = RADIO_MHZ === 0;

// The bridge's AP IP (from /config). Device links only work for a viewer on
// the bridge's own WiFi (there is no routing from the home LAN into the AP
// subnet), so links are shown only when the page itself was loaded from the
// AP address. While apIp is still unknown ( /config not loaded yet, or its
// fetch failed) assume the AP side - the common case, and a dead link beats
// hiding a working one.
let apIp = '';

function onBridgeWifi(): boolean {
  return apIp === '' || location.hostname === apIp;
}

let devTimer: number | undefined;

function ageText(s: number): string {
  if (s < 60) {
    return 'just now';
  }
  const m = Math.floor(s / 60);
  if (m < 60) {
    return `${m}m ago`;
  }
  const h = Math.floor(m / 60);
  return h < 24 ? `${h}h ago` : `${Math.floor(h / 24)}d ago`;
}

function renderDevices(list: ApClient[]): void {
  const wrap = el('devList');
  const empty = el('devEmpty');
  wrap.innerHTML = '';
  empty.hidden = list.length > 0;
  empty.textContent = `No devices yet - connect your weather station to "${apSsid}".`;
  for (const c of list) {
    const div = document.createElement('div');
    div.className = 'dev';

    const main = document.createElement('div');
    main.className = 'devmain';
    const dot = document.createElement('span');
    dot.className = c.connected ? 'dot on' : 'dot';
    dot.title = c.connected ? 'Connected' : 'Disconnected';
    // Best available identity: what the user named it, else what the device
    // calls itself (DHCP hostname), else its maker.
    const title = c.name || c.hostname || c.vendor;
    const name = document.createElement('b');
    name.textContent = title;
    main.append(dot, name);
    if (c.connected && c.rssi !== 0) {
      const bars = document.createElement('span');
      bars.className = 'bars';
      bars.textContent = signalBars(c.rssi);
      main.append(bars);
    }
    if (c.you) {
      const you = document.createElement('span');
      you.className = 'you';
      you.textContent = 'this device';
      main.append(you);
    }

    const sub = document.createElement('div');
    sub.className = 'devsub small';
    const parts = [c.mac, c.ip || 'no address yet'];
    if (c.gauge_id) parts.push(`Rainlog${c.gauge_id}`);
    // Whichever identity facts the title doesn't already say.
    if (c.hostname && c.hostname !== title) {
      parts.push(c.hostname);
    }
    if (c.vendor !== title) {
      parts.push(c.vendor);
    }
    if (!c.connected) {
      parts.push(`last seen ${ageText(c.age_s)}`);
    }
    sub.textContent = parts.join(' · ');
    div.append(main, sub);

    // Upload stats + any latched errors, only once the device has uploaded
    // (or tried and failed): received, forwarded to Rainlog, relayed to WU.
    if (c.rx > 0 || c.err !== 0) {
      const stats = document.createElement('div');
      stats.className = 'devsub small';
      const bits = [`${c.rx} received`, `${c.rl} to Rainlog`];
      if (c.wu > 0) {
        bits.push(`${c.wu} to WU`);
      }
      stats.textContent = bits.join(' · ');
      for (const b of deviceBadges(c)) {
        const badge = document.createElement('span');
        badge.className = b.warn ? 'devwarn' : 'deverr';
        badge.textContent = b.text;
        stats.append(' ', badge);
      }
      div.append(stats);
    }

    const actions = document.createElement('div');
    actions.className = 'row';
    // No link for the viewer's own device (it would just open your phone) or
    // before the device has a lease. From the home LAN the AP subnet is
    // unroutable, so show how to reach it instead of a dead link.
    if (c.ip && !c.you) {
      if (onBridgeWifi()) {
        const open = document.createElement('a');
        open.className = 'mini open';
        open.href = `http://${c.ip}/`;
        open.target = '_blank';
        open.rel = 'noopener';
        open.textContent = 'Open';
        actions.append(open);
      } else {
        const note = document.createElement('div');
        note.className = 'small';
        note.textContent = `Join "${apSsid}" to open this device.`;
        actions.append(note);
      }
    }
    const ren = document.createElement('button');
    ren.type = 'button';
    ren.className = 'mini';
    ren.textContent = 'Rename';
    ren.addEventListener('click', () => void renameDevice(c));
    actions.append(ren);
    div.append(actions);
    wrap.appendChild(div);
  }
}

async function pollDevices(): Promise<void> {
  try {
    if (wifiInterceptionActive) {
      const clients = (await (await fetch('/clients')).json()) as ApClient[];
      refreshKnownGauges(clients.map((client) => client.gauge_id ?? 0));
      renderDevices(clients);
    }
  } catch {
    /* keep the last rendered list on a blip */
  }
}

// Ask for a name and save it on the bridge (empty clears, cancel aborts).
async function renameDevice(c: ApClient): Promise<void> {
  const entered = prompt('Name for this device (empty to clear):', c.name);
  if (entered === null) {
    return;
  }
  const name = entered.trim().slice(0, 32);
  try {
    await fetch('/rename', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `mac=${encodeURIComponent(c.mac)}&name=${encodeURIComponent(name)}`,
    });
  } catch {
    /* the refresh below shows whether it stuck */
  }
  void pollDevices();
}

function devicesVisible(): boolean {
  return !document.hidden && !el('viewDevices').hidden;
}

// Start/stop the 5s poll to match tab + page visibility.
function updateDevPolling(): void {
  if (devicesVisible() && devTimer === undefined) {
    void pollDevices();
    devTimer = window.setInterval(() => void pollDevices(), DEV_POLL_MS);
  } else if (!devicesVisible() && devTimer !== undefined) {
    clearInterval(devTimer);
    devTimer = undefined;
  }
}

// ---- load + wire ----------------------------------------------------------

// Fill the form from the bridge's current settings. Passwords/keys are never
// returned; their placeholders reflect only whether a value is already set.
async function loadConfig(): Promise<void> {
  let cfg: BridgeConfig | null = null;
  try {
    cfg = (await (await fetch('/config')).json()) as BridgeConfig;
  } catch {
    /* leave fields blank if the bridge is unreachable */
  }
  if (cfg) {
    radio?.load(cfg);
    refreshKnownGauges([
      ...cfg.wu_map.map((mapping) => mapping.gauge_id),
      ...(cfg.radio_map ?? []).map((mapping) => mapping.gauge_id),
    ]);
    wifiInterceptionActive =
      cfg.wifi_interception_active ?? cfg.wifi_interception_enabled ?? true;
    el('wifiDevices').hidden = !wifiInterceptionActive;
    apPassDefault = cfg.ap_pass_default;
    apSsid = cfg.ap_ssid;
    apIp = cfg.ap_ip;
    // Sign-out only makes sense for a LAN-side session; the bridge's own
    // WiFi side is always open.
    el('signOut').hidden = onBridgeWifi();
    field('sta_ssid').value = cfg.sta_ssid;
    field('ap_ssid').value = cfg.ap_ssid;
    field('ap_pass').placeholder = cfg.ap_pass_default
      ? 'Set a password (required)'
      : '(unchanged)';
    for (const m of cfg.wu_map) {
      addRow(m);
    }
  }
  if (el('wuRows').children.length === 0) {
    addRow(); // always show one empty row to invite a first mapping
  }
  revalidate(); // reflect the loaded state on the Save button
}

const radio = RADIO_MHZ === 433 ? createRadio(attachPwToggle) : null;

// Add a Show/Hide toggle to the static password fields (the WU key fields get
// one as their rows are created).
document
  .querySelectorAll<HTMLInputElement>('input[type=password]')
  .forEach(attachPwToggle);

// Live validation slots for the two Wi-Fi passwords (after toggles wrap them).
addErrorSlot(field('sta_pass'), 'sta_pass_err');
addErrorSlot(field('ap_pass'), 'ap_pass_err');

// Routes preserve unsaved fields and keep polling in sync with browser history.
const tabs = document.querySelectorAll<HTMLAnchorElement>('.tab');
function renderRoute(): void {
  let active = Array.from(tabs).find((tab) => tab.pathname === location.pathname);
  if (!active) {
    active = tabs[0];
    history.replaceState(null, '', active.pathname);
  }
  tabs.forEach((tab) => {
    tab.classList.toggle('active', tab === active);
    if (tab === active) tab.setAttribute('aria-current', 'page');
    else tab.removeAttribute('aria-current');
    if (tab.dataset.view) el(tab.dataset.view).hidden = tab !== active;
  });
  updateDevPolling();
}
tabs.forEach((tab) => {
  tab.addEventListener('click', (event) => {
    if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    event.preventDefault();
    if (location.pathname !== tab.pathname) history.pushState(null, '', tab.pathname);
    renderRoute();
  });
});
window.addEventListener('popstate', renderRoute);
renderRoute();
document.addEventListener('visibilitychange', updateDevPolling);

el('signOut').addEventListener('click', () => {
  // The server drops the session and expires the cookie; the reload then
  // lands on the sign-in page.
  void fetch('/logout', { method: 'POST' }).finally(() => location.reload());
});
el('scanBtn').addEventListener('click', () => void scanlive());
el('testBtn').addEventListener('click', test);
el('addBtn').addEventListener('click', () => addRow());
el('otaCheckBtn').addEventListener('click', otaCheck);
el('otaApplyBtn').addEventListener('click', otaApply);
el('nets').addEventListener('change', (e) => {
  const sel = e.target as HTMLSelectElement;
  if (sel.value) {
    field('sta_ssid').value = sel.value;
    sel.selectedIndex = 0; // back to the chevron label
  }
});
// Associated controls live in both tabs. Reveal the invalid field before
// native form validation tries to focus it.
el('form').addEventListener(
  'invalid',
  (event) => {
    const input = event.target as HTMLElement;
    const view = input.closest('#viewSetup') ? 'viewSetup' : 'viewDevices';
    (
      document.querySelector(`[data-view=${view}]`) as HTMLAnchorElement
    ).click();
  },
  true,
);

el('form').addEventListener('submit', (e) => {
  // Belt-and-suspenders: Save is disabled while invalid, but guard the submit
  // too so a stray Enter can't POST a bad password and lose the page state.
  if (homePwError() || bridgePwError() || receptionError()) {
    touched.add(field('sta_pass'));
    touched.add(field('ap_pass'));
    revalidate();
    e.preventDefault();
    return;
  }
  numberRows();
  radio?.numberRows();
});

document
  .getElementById('radioRows')
  ?.addEventListener('input', () => refreshKnownGauges());

document
  .getElementById('wifiInterceptionEnabled')
  ?.addEventListener('change', revalidate);
document.getElementById('radioEnabled')?.addEventListener('change', revalidate);

void loadConfig();
void scan();
void loadOta();
