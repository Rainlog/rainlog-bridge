import tippingBucketIcon from './icons/tipping-bucket.svg';
import { createDeviceCard, renderSignal, uploaderButton } from './device-card';

interface RadioMapping {
  model: number;
  sensor_id: number;
  channel: number;
  gauge_id: number;
  rainlog_key: string;
  name?: string;
}
interface RadioConfig {
  radio_enabled?: boolean;
  radio_map?: RadioMapping[];
}
interface RadioSensor extends Omit<RadioMapping, 'rainlog_key'> {
  age_s: number;
  rssi_dbm?: number;
  has_rain?: boolean;
  rain_raw?: number;
  packets?: number;
}
interface RadioStatus {
  available: boolean;
  receiving: boolean;
  error: number;
  sensors: RadioSensor[];
}

export function createRadio(
  attachPasswordToggle: (input: HTMLInputElement) => void,
  attachWuUploader: (card: HTMLElement, gauge: () => number) => void,
  refreshUploaders: () => void,
) {
  const rows = document.getElementById('radioDeviceList')!;
  const status = document.getElementById('radioStatus')!;
  const deviceStatus = document.getElementById('radioDeviceStatus')!;
  const enabled = document.getElementById('radioEnabled') as HTMLInputElement;
  const names = ['La Crosse TX5U', 'AcuRite Iris'];
  let sensors: RadioSensor[] = [];
  let receiving = false;
  const sensorNames = new Map<string, string>();
  // TX5U normally transmits about every 51 seconds. Five minutes misses
  // several expected reports without flagging a single lost packet.
  const TX5U_QUIET_SECONDS = 300;

  function addRow(mapping: RadioMapping): void {
    if (rows.querySelectorAll('.radio-row').length >= 8) return;
    const card = sensorCard(mapping);
    const row = document.createElement('fieldset');
    row.className = 'radio-row uploader-panel';
    const legend = document.createElement('legend');
    legend.textContent = 'Rainlog Uploader';
    row.append(legend);
    const identity = (name: string, value: string): HTMLInputElement => {
      const field = document.createElement('input');
      field.type = 'hidden';
      field.dataset.field = name;
      field.value = value;
      row.append(field);
      return field;
    };
    const model = identity('model', mapping ? String(mapping.model) : '');
    const id = identity('id', mapping ? String(mapping.sensor_id) : '');
    const channel = identity(
      'channel',
      mapping?.channel ? String.fromCharCode(mapping.channel) : '',
    );
    const caption = document.createElement('label');
    caption.textContent = 'Detected sensor';
    const picker = document.createElement('select');
    picker.className = 'sensor-picker';
    picker.required = true;
    picker.setAttribute('form', 'form');
    picker.setAttribute('aria-label', 'Choose a detected sensor');
    picker.dataset.selected = mapping ? sensorIdentity(mapping) : '';
    const replaceable = mapping?.model === 0;
    picker.dataset.tx5u = replaceable ? 'true' : 'false';
    picker.disabled = true;
    caption.hidden = true;
    picker.addEventListener('change', () => {
      const sensor = sensors.find(
        (sensor) => sensorIdentity(sensor) === picker.value,
      );
      if (!sensor) return;
      model.value = String(sensor.model);
      id.value = String(sensor.sensor_id);
      channel.value = sensor.channel ? String.fromCharCode(sensor.channel) : '';
      picker.dataset.selected = picker.value;
      if (replaceable) {
        picker.disabled = true;
        replace.textContent = 'Replace sensor';
        caption.hidden = true;
        card.dataset.sensor = sensorIdentity(sensor);
        renderSensors();
      }
      refreshSensorPickers();
    });
    caption.append(picker);
    row.append(caption);
    const warning = document.createElement('p');
    warning.className = 'hint tx5u-warning';
    warning.hidden = true;
    row.append(warning);
    const replace = document.createElement('button');
    replace.type = 'button';
    replace.className = 'mini sensor-replace';
    replace.textContent = 'Replace sensor';
    replace.hidden = !replaceable;
    replace.addEventListener('click', () => {
      picker.disabled = !picker.disabled;
      caption.hidden = picker.disabled;
      replace.textContent = picker.disabled
        ? 'Replace sensor'
        : 'Cancel replacement';
      refreshSensorPickers();
      if (!picker.disabled) picker.focus();
    });
    if (replaceable) row.append(replace);
    function input(
      label: string,
      name: string,
      value: string,
      type = 'text',
    ): HTMLInputElement {
      const caption = document.createElement('label');
      caption.textContent = label;
      const element = document.createElement('input');
      element.dataset.field = name;
      element.type = type;
      element.setAttribute('form', 'form');
      element.value = value;
      element.required = true;
      caption.append(element);
      row.append(caption);
      return element;
    }
    const gauge = input(
      'Rainlog station ID',
      'gauge',
      mapping?.gauge_id ? `Rainlog${mapping.gauge_id}` : '',
    );
    gauge.placeholder = 'Rainlog12345';
    gauge.pattern = '(?:[Rr][Aa][Ii][Nn][Ll][Oo][Gg])?[0-9]+';
    const key = input(
      'Rainlog PWS key',
      'key',
      mapping?.rainlog_key ?? '',
      'password',
    );
    key.maxLength = 64;
    attachPasswordToggle(key);
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'mini';
    remove.textContent = 'Remove Rainlog Uploader';
    remove.addEventListener('click', () => {
      row.remove();
      validateGaugeAssignments();
      renderSensors();
    });
    row.append(remove);
    card.querySelector('.mapping-action')?.remove();
    card.append(row);
    numberRows();
    refreshSensorPickers();
    refreshUploaders();
  }
  const wifiGauges = new Set<number>();
  function validateGaugeAssignments(): void {
    const gauges = Array.from(
      rows.querySelectorAll<HTMLInputElement>('[data-field=gauge]'),
    );
    const id = (input: HTMLInputElement) =>
      Number(input.value.replace(/^Rainlog/i, ''));
    gauges.forEach((input) => {
      const gauge = id(input);
      input.setCustomValidity(
        wifiGauges.has(gauge)
          ? 'This Rainlog gauge is already assigned to a Wi-Fi device.'
          : gauge &&
              gauges.some((other) => other !== input && id(other) === gauge)
            ? 'This Rainlog gauge is already assigned to another radio sensor.'
            : '',
      );
    });
  }
  rows.addEventListener('input', validateGaugeAssignments);
  function numberRows(): void {
    Array.from(rows.querySelectorAll('.radio-row')).forEach((row, index) => {
      row
        .querySelectorAll<HTMLInputElement | HTMLSelectElement>('[data-field]')
        .forEach((field) => {
          field.setAttribute('form', 'form');
          field.name = `radio_${field.dataset.field}${index}`;
        });
    });
    rows
      .querySelectorAll<HTMLButtonElement>('.mapping-action')
      .forEach((button) => {
        button.disabled = rows.querySelectorAll('.radio-row').length >= 8;
      });
  }
  function sensorIdentity(
    sensor: Pick<RadioMapping, 'model' | 'sensor_id' | 'channel'>,
  ): string {
    return `${sensor.model}:${sensor.sensor_id}:${sensor.channel}`;
  }
  function refreshSensorPickers(): void {
    rows
      .querySelectorAll<HTMLSelectElement>('.sensor-picker')
      .forEach((picker) => {
        const selected = picker.dataset.selected ?? '';
        picker.replaceChildren(new Option('Choose a detected sensor', ''));
        sensors.forEach((sensor) => {
          if (picker.dataset.tx5u === 'true' && sensor.model !== 0) return;
          const option = new Option(
            `${names[sensor.model] ?? 'Weather sensor'}: ID ${sensor.sensor_id}${sensor.channel ? `, channel ${String.fromCharCode(sensor.channel)}` : ''}`,
            sensorIdentity(sensor),
          );
          // Replacements must not take a sensor belonging to another mapping.
          if (
            picker.dataset.tx5u === 'true' &&
            sensorIdentity(sensor) !== selected
          ) {
            option.disabled =
              !!sensor.gauge_id ||
              Array.from(
                rows.querySelectorAll<HTMLSelectElement>('.sensor-picker'),
              ).some(
                (other) =>
                  other !== picker &&
                  other.dataset.selected === sensorIdentity(sensor),
              );
          }
          picker.add(option);
        });
        if (
          selected &&
          !sensors.some((sensor) => sensorIdentity(sensor) === selected)
        ) {
          const [model, id, channel] = selected.split(':').map(Number);
          const saved = new Option(
            `${names[model]}: ID ${id}${channel ? `, channel ${String.fromCharCode(channel)}` : ''} (not seen this boot)`,
            selected,
          );
          saved.disabled = true;
          picker.add(saved);
        }
        picker.value = selected;
        const replace = picker
          .closest('fieldset')!
          .querySelector<HTMLButtonElement>('.sensor-replace');
        if (replace) {
          const available = Array.from(picker.options).some(
            (option) =>
              option.value !== '' &&
              option.value !== selected &&
              !option.disabled,
          );
          replace.hidden = !available;
          if (!available) {
            picker.disabled = true;
            picker.parentElement!.hidden = true;
            replace.textContent = 'Replace sensor';
          }
        }
        const warning = picker
          .closest('fieldset')!
          .querySelector<HTMLElement>('.tx5u-warning')!;
        const sensor = sensors.find(
          (sensor) => sensorIdentity(sensor) === selected,
        );
        warning.hidden =
          picker.dataset.tx5u !== 'true' ||
          !receiving ||
          (!!sensor && sensor.age_s < TX5U_QUIET_SECONDS);
        warning.textContent = sensor
          ? 'This TX5U has not been heard for at least 5 minutes. Battery replacement can change its ID. Use Replace sensor to keep this Rainlog mapping.'
          : 'This TX5U has not been seen since the bridge restarted. If its batteries were replaced, use Replace sensor to select its new ID and keep this Rainlog mapping.';
      });
    numberRows();
  }
  function sensorCard(
    sensor: Pick<RadioMapping, 'model' | 'sensor_id' | 'channel'>,
  ): HTMLElement {
    const identity = sensorIdentity(sensor);
    let card = Array.from(rows.children).find(
      (card) => (card as HTMLElement).dataset.sensor === identity,
    ) as HTMLElement | undefined;
    if (!card) {
      card = createDeviceCard('radio-card', identity);
      card.dataset.sensor = identity;
      card.querySelector('h4')!.textContent =
        names[sensor.model] ?? 'Weather sensor';
      if (sensor.model === 0)
        card.querySelector('.sensor-icon')!.innerHTML = tippingBucketIcon;
      attachWuUploader(card, () =>
        Number(
          card!
            .querySelector<HTMLInputElement>('[data-field=gauge]')
            ?.value.replace(/^Rainlog/i, '') ?? 0,
        ),
      );
      card.querySelector<HTMLButtonElement>('.name-edit')!.onclick =
        async () => {
          const identity = card!.dataset.sensor!;
          const entered = prompt(
            'Name for this device (empty to clear):',
            sensorNames.get(identity) ?? '',
          );
          if (entered === null) return;
          try {
            const response = await fetch('/rename', {
              method: 'POST',
              headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
              body: new URLSearchParams({
                mac: `radio:${identity}`,
                name: entered.trim().slice(0, 32),
              }),
            });
            if (!response.ok) throw new Error('Unable to save device name.');
            sensorNames.set(identity, entered.trim().slice(0, 32));
            renderSensors();
          } catch {
            alert('Unable to save device name.');
          }
        };
      rows.append(card);
    }
    card.querySelector('.sensor-identity')!.textContent =
      `ID ${sensor.sensor_id}${sensor.channel ? `, channel ${String.fromCharCode(sensor.channel)}` : ''}`;
    return card;
  }
  function renderSensors(): void {
    // Keep mapping fields in place while polling so edits and focus survive.
    for (const card of Array.from(rows.children)) {
      if (!card.querySelector('.radio-row') && !card.querySelector('.wurow'))
        card.remove();
    }
    for (const row of Array.from(rows.querySelectorAll('.radio-row'))) {
      const value = (name: string) =>
        (row.querySelector(`[data-field=${name}]`) as HTMLInputElement).value;
      const sensor = {
        model: Number(value('model')),
        sensor_id: Number(value('id')),
        channel: value('channel').charCodeAt(0) || 0,
      };
      const card = row.parentElement!;
      card.dataset.sensor = sensorIdentity(sensor);
      card.querySelector('h4')!.textContent =
        sensorNames.get(sensorIdentity(sensor)) || names[sensor.model];
      card.querySelector('.sensor-identity')!.textContent =
        `ID ${sensor.sensor_id}${sensor.channel ? `, channel ${String.fromCharCode(sensor.channel)}` : ''}`;
      const seen = sensors.find(
        (seen) => sensorIdentity(seen) === sensorIdentity(sensor),
      );
      renderSignal(card, seen?.rssi_dbm, true, seen?.age_s);
      card.querySelector('.sensor-detail')!.textContent = seen
        ? sensorDetail(seen)
        : 'Not seen this boot';
    }
    sensors.forEach((sensor) => {
      const card = sensorCard(sensor);
      renderSignal(card, sensor.rssi_dbm, true, sensor.age_s);
      card.querySelector('.sensor-detail')!.textContent = sensorDetail(sensor);
      if (
        !card.querySelector('.radio-row') &&
        !card.querySelector('.mapping-action')
      ) {
        const button = uploaderButton('Add Rainlog Uploader', () =>
          addRow({ ...sensor, gauge_id: 0, rainlog_key: '' }),
        );
        button.classList.add('mapping-action');
        card.querySelector('.device-actions')!.prepend(button);
      }
    });
    for (const card of Array.from(
      rows.querySelectorAll<HTMLElement>('.radio-card'),
    )) {
      const identity = card.dataset.sensor!;
      const model = Number(identity.split(':')[0]);
      const name = sensorNames.get(identity);
      card.querySelector('h4')!.textContent = name || names[model];
      const parts = identity.split(':');
      card.querySelector('.sensor-identity')!.textContent =
        `${name ? names[model] + ' · ' : ''}ID ${parts[1]}${Number(parts[2]) ? ', channel ' + String.fromCharCode(Number(parts[2])) : ''}`;
    }
    refreshSensorPickers();
    refreshUploaders();
    if (!rows.children.length) {
      const empty = document.createElement('p');
      empty.className = 'radio-empty hint';
      empty.textContent = 'Waiting for a weather sensor transmission.';
      rows.append(empty);
    }
  }
  function sensorDetail(sensor: RadioSensor): string {
    return `Last seen ${sensor.age_s}s ago · ${sensor.packets ?? 0} packets${sensor.has_rain ? ` · Tip counter ${sensor.rain_raw}` : ''}`;
  }

  async function poll(): Promise<void> {
    if (document.hidden) return;
    try {
      const response = await fetch('/radio');
      if (!response.ok) throw new Error();
      const data = (await response.json()) as RadioStatus;
      status.textContent = !data.available
        ? 'Radio unavailable.'
        : data.receiving
          ? 'Radio reception enabled.'
          : 'Radio reception disabled.';
      status.textContent += ` ${data.sensors.length} ${data.sensors.length === 1 ? 'device' : 'devices'} seen.`;
      deviceStatus.textContent = status.textContent;
      receiving = data.available && data.receiving;
      sensors = data.sensors;
      sensors.forEach((sensor) =>
        sensorNames.set(sensorIdentity(sensor), sensor.name ?? ''),
      );
      renderSensors();
    } catch {
      status.textContent = 'Unable to read radio status.';
      deviceStatus.textContent = status.textContent;
    }
  }
  document.addEventListener('visibilitychange', () => void poll());
  window.setInterval(() => void poll(), 5000);
  void poll();
  return {
    load(config: RadioConfig): void {
      enabled.checked = config.radio_enabled ?? true;
      (config.radio_map ?? []).forEach((mapping) => {
        sensorNames.set(sensorIdentity(mapping), mapping.name ?? '');
        addRow(mapping);
      });
      renderSensors();
    },
    setWifiGauges(ids: number[]): void {
      ids.filter((id) => id > 0).forEach((id) => wifiGauges.add(id));
      validateGaugeAssignments();
    },
    numberRows,
  };
}
