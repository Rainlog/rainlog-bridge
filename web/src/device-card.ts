import weatherStationIcon from './icons/weather-station.svg';

export function createDeviceCard(kind: string, identity: string): HTMLElement {
  const card = document.createElement('article');
  card.className = `device-card ${kind}`;
  card.dataset.device = identity;
  const heading = document.createElement('div');
  heading.className = 'sensor-heading';
  const icon = document.createElement('div');
  icon.className = 'sensor-icon';
  icon.setAttribute('aria-hidden', 'true');
  icon.innerHTML = weatherStationIcon;
  const text = document.createElement('div');
  text.className = 'sensor-heading-text';
  const title = document.createElement('h4');
  const label = document.createElement('p');
  label.className = 'sensor-identity';
  const nameRow = document.createElement('div');
  nameRow.className = 'device-name';
  const edit = document.createElement('button');
  edit.type = 'button';
  edit.className = 'name-edit';
  edit.setAttribute('aria-label', 'Edit device name');
  edit.title = 'Edit device name';
  edit.innerHTML =
    '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 16v4h4L20 8l-4-4L4 16Z M14 6l4 4"/></svg>';
  nameRow.append(title, edit);
  text.append(nameRow, label);
  const signal = document.createElement('span');
  signal.className = 'signal-strength radio-signal';
  signal.hidden = true;
  heading.append(icon, text, signal);
  const detail = document.createElement('p');
  detail.className = 'small sensor-detail';
  const stats = document.createElement('div');
  stats.className = 'device-stats small';
  const actions = document.createElement('div');
  actions.className = 'device-actions';
  const management = document.createElement('div');
  management.className = 'device-management row';
  card.append(heading, detail, stats, actions, management);
  return card;
}

export function renderSignal(
  card: HTMLElement,
  rssi: number | undefined,
  radio: boolean,
  age = 0,
): void {
  const signal = card.querySelector<HTMLElement>('.signal-strength')!;
  signal.hidden = rssi === undefined || !Number.isFinite(rssi) || rssi >= 0;
  if (signal.hidden) return;
  const thresholds = radio ? [-60, -75, -90, -105] : [-55, -65, -75, -85];
  const level = thresholds.filter((threshold) => rssi! >= threshold).length;
  signal.replaceChildren();
  const bars = document.createElement('span');
  bars.className = 'radio-signal-bars';
  bars.setAttribute('aria-hidden', 'true');
  for (let i = 0; i < 4; i++) {
    const bar = document.createElement('i');
    bar.className = i < level ? 'active' : '';
    bars.append(bar);
  }
  signal.append(bars, ` ${rssi!.toFixed(radio ? 1 : 0)} dBm`);
  signal.title = radio
    ? `Last received RSSI estimate (${age}s ago). Sampled after the burst; may include background noise.`
    : 'Current Wi-Fi signal strength.';
  signal.setAttribute(
    'aria-label',
    `${radio ? 'Last received signal estimate' : 'Wi-Fi signal'}: ${rssi} dBm, ${level} of 4 bars`,
  );
}

export function uploaderButton(
  label: string,
  action: () => void,
): HTMLButtonElement {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'mini uploader-button';
  button.textContent = label;
  button.addEventListener('click', action);
  return button;
}
