import tippingBucketIcon from './icons/tipping-bucket.svg';
import weatherStationIcon from './icons/weather-station.svg';

interface RadioMapping {
  model: number;
  sensor_id: number;
  channel: number;
  gauge_id: number;
  rainlog_key: string;
}
interface RadioConfig {
  radio_enabled?: boolean;
  wifi_interception_enabled?: boolean;
  radio_map?: RadioMapping[];
}
interface RadioSensor extends Omit<RadioMapping, 'rainlog_key'> {
  age_s: number;
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
) {
  const rows = document.getElementById('radioDeviceList')!;
  const status = document.getElementById('radioStatus')!;
  const deviceStatus = document.getElementById('radioDeviceStatus')!;
  const enabled = document.getElementById('radioEnabled') as HTMLInputElement;
  const names = ['La Crosse TX5U', 'AcuRite Iris'];
  let sensors: RadioSensor[] = [];
  let receiving = false;
  // TX5U normally transmits about every 51 seconds. Five minutes misses
  // several expected reports without flagging a single lost packet.
  const TX5U_QUIET_SECONDS = 300;

  function addRow(mapping: RadioMapping): void {
    if (rows.querySelectorAll('.radio-row').length >= 8) return;
    const card = sensorCard(mapping);
    const row = document.createElement('fieldset');
    row.className = 'radio-row';
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
    remove.textContent = 'Remove mapping';
    remove.addEventListener('click', () => {
      row.remove();
      renderSensors();
    });
    row.append(remove);
    card.querySelector('.mapping-action')?.remove();
    card.append(row);
    numberRows();
    refreshSensorPickers();
  }
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
      card = document.createElement('article');
      card.className = 'radio-card';
      card.dataset.sensor = identity;
      const icon = document.createElement('div');
      icon.className = 'sensor-icon';
      icon.setAttribute('aria-hidden', 'true');
      icon.innerHTML =
        sensor.model === 0 ? tippingBucketIcon : weatherStationIcon;
      const title = document.createElement('h4');
      title.textContent = names[sensor.model] ?? 'Weather sensor';
      const identityLabel = document.createElement('p');
      identityLabel.className = 'sensor-identity';
      const detail = document.createElement('p');
      detail.className = 'small sensor-detail';
      const heading = document.createElement('div');
      heading.className = 'sensor-heading';
      const headingText = document.createElement('div');
      headingText.append(title, identityLabel);
      heading.append(icon, headingText);
      card.append(heading, detail);
      rows.append(card);
    }
    card.querySelector('.sensor-identity')!.textContent =
      `ID ${sensor.sensor_id}${sensor.channel ? `, channel ${String.fromCharCode(sensor.channel)}` : ''}`;
    return card;
  }
  function renderSensors(): void {
    // Keep mapping fields in place while polling so edits and focus survive.
    for (const card of Array.from(rows.children)) {
      if (!card.querySelector('.radio-row')) card.remove();
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
      card.querySelector('h4')!.textContent = names[sensor.model];
      card.querySelector('.sensor-identity')!.textContent =
        `ID ${sensor.sensor_id}${sensor.channel ? `, channel ${String.fromCharCode(sensor.channel)}` : ''}`;
      const seen = sensors.find(
        (seen) => sensorIdentity(seen) === sensorIdentity(sensor),
      );
      card.querySelector('.sensor-detail')!.textContent = seen
        ? sensorDetail(seen)
        : 'Not seen this boot';
    }
    sensors.forEach((sensor) => {
      const card = sensorCard(sensor);
      card.querySelector('.sensor-detail')!.textContent = sensorDetail(sensor);
      if (!card.querySelector('.radio-row')) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'mini mapping-action';
        button.textContent = 'Add mapping';
        button.addEventListener('click', () =>
          addRow({ ...sensor, gauge_id: 0, rainlog_key: '' }),
        );
        card.append(button);
      }
    });
    refreshSensorPickers();
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
      (
        document.getElementById('wifiInterceptionEnabled') as HTMLInputElement
      ).checked = config.wifi_interception_enabled ?? false;
      (config.radio_map ?? []).forEach(addRow);
      renderSensors();
    },
    numberRows,
  };
}
